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

#pragma once

#include "config.hpp"
#include "database.hpp"

#include <crow.h>

#include <atomic>
#include <string>

namespace sectorflux
{

/**
 * @brief Handles proxying requests to Ollama and streaming responses.
 *
 * This class manages the core proxy functionality, including:
 * - HTTP request forwarding to Ollama
 * - WebSocket chat streaming
 * - Response caching
 * - Metrics extraction from Ollama responses
 */
class ProxyHandler
{
public:
    /**
     * @brief Construct a new Proxy Handler object.
     * @param db Reference to the Database object for logging.
     */
    explicit ProxyHandler(Database& db);
    ~ProxyHandler() = default;

    // Delete copy operations
    ProxyHandler(const ProxyHandler&) = delete;
    ProxyHandler& operator=(const ProxyHandler&) = delete;

    /**
     * @brief Handle an incoming HTTP request and forward it to Ollama.
     * @param req The incoming Crow request.
     * @param res The Crow response object to populate.
     * @param target_endpoint The Ollama endpoint to forward to (e.g., "/api/generate").
     */
    void handleRequest(
        const crow::request& req,
        crow::response& res,
        const std::string& target_endpoint);

    /**
     * @brief Handle a WebSocket chat request and stream the response from Ollama.
     * @param conn The WebSocket connection.
     * @param message The incoming JSON message string.
     * @param is_active Atomic flag to check if the connection is still active.
     */
    void handleWebSocketRequest(
        crow::websocket::connection& conn,
        const std::string& message,
        std::atomic<bool>& is_active);

    /**
     * @brief Enable or disable response caching.
     * @param enabled True to enable caching, false to disable.
     */
    void setCacheEnabled(bool enabled)
    {
        cache_enabled_ = enabled;
    }

    /**
     * @brief Check if caching is enabled.
     * @return bool True if caching is enabled.
     */
    [[nodiscard]] bool isCacheEnabled() const
    {
        return cache_enabled_;
    }

    /**
     * @brief Metrics extracted from Ollama response.
     */
    struct ResponseMetrics
    {
        int prompt_tokens = 0;
        int completion_tokens = 0;
        long long prompt_eval_duration_ms = 0;
        long long eval_duration_ms = 0;
    };

    /**
     * @brief Extract metrics from Ollama response (handles both single JSON and NDJSON).
     * @param response The response body string.
     * @return ResponseMetrics The extracted metrics.
     */
    [[nodiscard]] ResponseMetrics extractMetrics(const std::string& response);

private:
    /**
     * @brief Extract model name from request JSON.
     * @param request_body The raw JSON request body.
     * @return std::string The model name, or "unknown" if not found.
     */
    [[nodiscard]] std::string extractModelFromRequest(const std::string& request_body);

    const std::string ollama_host_ = Config::getOllamaHost();
    Database& db_;
    bool cache_enabled_ = true;

    // Constants
    static constexpr int kConnectionTimeoutSec = 60;
    static constexpr int kWebSocketTimeoutSec = 300;
    static constexpr long long kNanosecondsPerMillisecond = 1000000;
};

} // namespace sectorflux
