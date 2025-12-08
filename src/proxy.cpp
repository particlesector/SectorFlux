/*
 * SectorFlux - LLM Proxy and Analytics
 * Copyright (c) 2025 ParticleSector.com
 *
 * This software is dual-licensed:
 * - GPL-3.0 for open source use
 * - Commercial license for proprietary use
 *
 * See LICENSE and LICENSING.md for details.
 */

#include "proxy.hpp"

#include <httplib.h>

#include <chrono>
#include <iostream>

namespace sectorflux
{

ProxyHandler::ProxyHandler(Database& db) : db_(db)
{
}

std::string ProxyHandler::extractModelFromRequest(const std::string& request_body)
{
    try
    {
        auto json = crow::json::load(request_body);
        if (json && json.has("model"))
        {
            return json["model"].s();
        }
    }
    catch (...)
    {
        // Ignore parsing errors
    }
    return "unknown";
}

ProxyHandler::ResponseMetrics ProxyHandler::extractMetrics(const std::string& response)
{
    ResponseMetrics metrics;

    try
    {
        // For NDJSON streaming responses, split by newlines and parse each line
        // The metrics are in the last JSON object with "done": true
        size_t pos = response.length();

        // Search backwards through the response for complete JSON objects
        while (pos > 0)
        {
            // Find the end of a potential JSON line (look for newline or end)
            size_t line_end = pos;
            size_t line_start = response.rfind('\n', pos - 1);
            if (line_start == std::string::npos)
            {
                line_start = 0;
            }
            else
            {
                line_start++;  // Move past the newline
            }

            // Extract the line and trim whitespace
            std::string line = response.substr(line_start, line_end - line_start);

            // Trim whitespace
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first != std::string::npos)
            {
                size_t last = line.find_last_not_of(" \t\r\n");
                line = line.substr(first, last - first + 1);
            }

            if (!line.empty() && line.front() == '{')
            {
                auto json = crow::json::load(line);
                if (json)
                {
                    bool found_metrics = false;

                    if (json.has("prompt_eval_count"))
                    {
                        metrics.prompt_tokens = static_cast<int>(json["prompt_eval_count"].i());
                        found_metrics = true;
                    }
                    if (json.has("eval_count"))
                    {
                        metrics.completion_tokens = static_cast<int>(json["eval_count"].i());
                        found_metrics = true;
                    }
                    if (json.has("prompt_eval_duration"))
                    {
                        metrics.prompt_eval_duration_ms =
                            json["prompt_eval_duration"].i() / kNanosecondsPerMillisecond;
                        found_metrics = true;
                    }
                    if (json.has("eval_duration"))
                    {
                        metrics.eval_duration_ms =
                            json["eval_duration"].i() / kNanosecondsPerMillisecond;
                        found_metrics = true;
                    }

                    // Found the summary object with metrics or done flag
                    if (found_metrics || (json.has("done") && json["done"].b()))
                    {
                        break;
                    }
                }
            }

            // Move to previous line
            if (line_start == 0)
            {
                break;
            }
            pos = line_start - 1;
        }
    }
    catch (...)
    {
        // Ignore parsing errors
    }

    return metrics;
}

void ProxyHandler::handleRequest(
    const crow::request& req,
    crow::response& res,
    const std::string& target_endpoint)
{
    auto start_time = std::chrono::steady_clock::now();

    // Capture request body early (before res.end() which may invalidate req)
    std::string request_body_copy = req.body;

    // Extract model from request for logging
    std::string model = extractModelFromRequest(request_body_copy);

    // 1. Check Cache (Smart Caching)
    // Skip cache if X-SectorFlux-No-Cache header is present OR cache is globally disabled
    bool skip_cache = !cache_enabled_ ||
                      req.get_header_value("X-SectorFlux-No-Cache") == "true";

    if (!skip_cache)
    {
        auto cached = db_.getCachedResponse(request_body_copy);
        if (cached)
        {
            std::cout << "Cache Hit for: " << target_endpoint << std::endl;
            res.code = cached->first;
            res.body = cached->second;
            res.add_header("X-SectorFlux-Cache", "HIT");
            res.end();

            // Extract metrics from cached response for logging
            auto metrics = extractMetrics(cached->second);

            // Log the interaction asynchronously (duration 0 indicates cache hit)
            db_.logInteractionAsync(
                "POST", target_endpoint, model, request_body_copy,
                res.code, res.body, 0, metrics.prompt_tokens, metrics.completion_tokens,
                0, 0, 0);
            return;
        }
    }

    // Log the request
    std::cout << "Forwarding request to: " << ollama_host_ << target_endpoint << std::endl;

    httplib::Client cli(ollama_host_);
    cli.set_connection_timeout(kConnectionTimeoutSec, 0);
    cli.set_read_timeout(kConnectionTimeoutSec, 0);

    // Capture the full response for logging
    std::string accumulated_response;
    long long ttft_ms = 0;

    // Prepare response headers for streaming
    res.add_header("Content-Type", "application/json");
    res.add_header("X-SectorFlux-Cache", "MISS");

    auto* res_ptr = &res;

    // Construct httplib Request manually to support content receiver
    httplib::Request req_http;
    req_http.method = "POST";
    req_http.path = target_endpoint;
    req_http.body = request_body_copy;
    req_http.set_header("Content-Type", "application/json");

    // Set content receiver to handle streaming response
    req_http.content_receiver = [&](const char* data,
                                    size_t data_length,
                                    uint64_t /*offset*/,
                                    uint64_t /*total_length*/)
    {
        if (ttft_ms == 0)
        {
            auto now = std::chrono::steady_clock::now();
            ttft_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_time).count();
        }

        // Receive chunk
        std::string chunk(data, data_length);

        // Accumulate for DB
        accumulated_response += chunk;

        // Forward to client
        res_ptr->body += chunk;

        return true;  // Continue processing
    };

    auto result = cli.send(req_http);

    if (result)
    {
        res.code = result->status;

        // 2. Cache the response if successful and not empty
        if (res.code == 200 && !accumulated_response.empty())
        {
            db_.cacheResponse(request_body_copy, res.code, accumulated_response);
        }
    }
    else
    {
        res.code = 500;
        res.body = "Error forwarding request to Ollama: " + to_string(result.error());
        accumulated_response = res.body;
    }
    res.end();

    auto end_time = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    // Extract metrics from response
    auto metrics = extractMetrics(accumulated_response);

    // Log to DB asynchronously
    db_.logInteractionAsync(
        "POST", target_endpoint, model, request_body_copy, res.code, accumulated_response,
        duration_ms, metrics.prompt_tokens, metrics.completion_tokens,
        metrics.prompt_eval_duration_ms, metrics.eval_duration_ms, ttft_ms);
}

void ProxyHandler::handleWebSocketRequest(
    crow::websocket::connection& conn,
    const std::string& message,
    std::atomic<bool>& is_active)
{
    // Parse incoming message to get model and prompt
    auto json_req = crow::json::load(message);
    if (!json_req)
    {
        conn.send_text("{\"error\": \"Invalid JSON\"}");
        return;
    }

    // Extract model for logging
    std::string model = "unknown";
    if (json_req.has("model"))
    {
        model = json_req["model"].s();
    }

    // 1. Check Cache (Smart Caching)
    if (cache_enabled_)
    {
        auto cached = db_.getCachedResponse(message);
        if (cached)
        {
            std::cout << "Cache Hit for WebSocket Chat" << std::endl;
            conn.send_text(cached->second);

            // Extract metrics from cached response for logging
            auto metrics = extractMetrics(cached->second);

            // Log the interaction asynchronously (duration 0 indicates cache hit)
            db_.logInteractionAsync(
                "POST", "/api/chat", model, message,
                cached->first, cached->second, 0, metrics.prompt_tokens, metrics.completion_tokens,
                0, 0, 0);
            return;
        }
    }

    // Run synchronously in the worker thread managed by main.cpp
    try
    {
        auto start_time = std::chrono::steady_clock::now();

        // Construct request to Ollama
        httplib::Client cli(ollama_host_);
        cli.set_connection_timeout(kWebSocketTimeoutSec, 0);
        cli.set_read_timeout(kWebSocketTimeoutSec, 0);

        httplib::Request req_http;
        req_http.method = "POST";
        req_http.path = "/api/chat";
        req_http.set_header("Content-Type", "application/json");

        // Force stream: true
        crow::json::wvalue body;
        body["model"] = json_req["model"].s();
        body["messages"] = json_req["messages"];
        body["stream"] = true;
        req_http.body = body.dump();

        std::string full_response;
        long long ttft_ms = 0;

        // Stream response back to client
        req_http.content_receiver = [&](const char* data,
                                        size_t data_length,
                                        uint64_t /*offset*/,
                                        uint64_t /*total_length*/)
        {
            if (ttft_ms == 0)
            {
                auto now = std::chrono::steady_clock::now();
                ttft_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - start_time).count();
            }

            // Check if connection is still active
            if (!is_active)
            {
                return false;  // Stop httplib request
            }

            std::string chunk(data, data_length);
            full_response += chunk;
            conn.send_text(chunk);
            return true;
        };

        auto result = cli.send(req_http);

        // Only log if we finished successfully and weren't aborted
        if (is_active)
        {
            if (!result || result->status != 200)
            {
                conn.send_text("{\"error\": \"Failed to connect to Ollama\"}");
            }
            else
            {
                // Log the interaction
                auto end_time = std::chrono::steady_clock::now();
                long long duration_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        end_time - start_time).count();

                // Extract metrics from response
                auto metrics = extractMetrics(full_response);

                db_.logInteractionAsync(
                    "POST", "/api/chat", model, message, 200, full_response,
                    duration_ms, metrics.prompt_tokens, metrics.completion_tokens,
                    metrics.prompt_eval_duration_ms, metrics.eval_duration_ms, ttft_ms);

                // Cache the response if enabled and valid
                if (cache_enabled_ && !full_response.empty())
                {
                    db_.cacheResponse(message, 200, full_response);
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception in handleWebSocketRequest: " << e.what() << std::endl;
        if (is_active)
        {
            conn.send_text("{\"error\": \"Internal Server Error\"}");
        }
    }
    catch (...)
    {
        std::cerr << "Unknown exception in handleWebSocketRequest" << std::endl;
    }
}

} // namespace sectorflux
