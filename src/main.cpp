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

#include "config.hpp"
#include "database.hpp"
#include "embedded_ui.hpp"
#include "proxy.hpp"
#include "version.hpp"

#include <crow.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{

/**
 * @brief State for managing WebSocket connections.
 */
struct ConnectionState
{
    std::atomic<bool> active{true};
    std::jthread worker;
};

// Map to store connection state: connection* -> shared_ptr<ConnectionState>
std::mutex g_connection_mutex;
std::unordered_map<void*, std::shared_ptr<ConnectionState>> g_connections;

// Constants
constexpr int kProxyTimeoutSec = 5;

/**
 * @brief Broadcaster for real-time dashboard updates via WebSocket.
 */
class DashboardBroadcaster
{
public:
    explicit DashboardBroadcaster(sectorflux::Database& db) : db_(db)
    {
        worker_ = std::jthread([this](std::stop_token stop_token)
        {
            broadcastLoop(stop_token);
        });
    }

    ~DashboardBroadcaster()
    {
        // jthread automatically requests stop and joins
    }

    void addConnection(crow::websocket::connection* conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.insert(conn);
    }

    void removeConnection(crow::websocket::connection* conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(conn);
    }

private:
    void broadcastLoop(std::stop_token stop_token)
    {
        while (!stop_token.stop_requested())
        {
            std::this_thread::sleep_for(std::chrono::seconds(kBroadcastIntervalSec));
            if (!stop_token.stop_requested())
            {
                broadcast();
            }
        }
    }

    void broadcast()
    {
        // 1. Fetch Logs
        auto logs_opt = db_.getLogs();
        if (!logs_opt)
        {
            return;
        }

        crow::json::wvalue data;
        const auto& logs = *logs_opt;

        for (size_t i = 0; i < logs.size(); ++i)
        {
            auto idx = static_cast<unsigned int>(i);
            data["logs"][idx]["id"] = logs[i].id;
            data["logs"][idx]["timestamp"] = logs[i].timestamp;
            data["logs"][idx]["method"] = logs[i].method;
            data["logs"][idx]["endpoint"] = logs[i].endpoint;
            data["logs"][idx]["model"] = logs[i].model;
            data["logs"][idx]["response_status"] = logs[i].response_status;
            data["logs"][idx]["duration_ms"] = logs[i].duration_ms;
            data["logs"][idx]["prompt_tokens"] = logs[i].prompt_tokens;
            data["logs"][idx]["completion_tokens"] = logs[i].completion_tokens;
            data["logs"][idx]["prompt_eval_duration_ms"] = logs[i].prompt_eval_duration_ms;
            data["logs"][idx]["eval_duration_ms"] = logs[i].eval_duration_ms;
            data["logs"][idx]["is_starred"] = logs[i].is_starred;
        }

        // 2. Fetch Metrics
        auto metrics = db_.getMetrics();
        data["metrics"]["total_requests"] = metrics.total_requests;
        data["metrics"]["avg_latency_ms"] = metrics.avg_latency_ms;
        data["metrics"]["cache_hit_rate"] = metrics.cache_hit_rate;

        // 3. Fetch Running Model (Ollama)
        httplib::Client cli(sectorflux::Config::getOllamaHost());
        cli.set_connection_timeout(kOllamaTimeoutSec, 0);
        cli.set_read_timeout(kOllamaTimeoutSec, 0);

        auto res = cli.Get("/api/ps");
        if (res && res->status == 200)
        {
            auto json = crow::json::load(res->body);
            if (json && json.has("models") && json["models"].size() > 0)
            {
                data["running_model"] = json["models"][0]["name"];
            }
            else
            {
                data["running_model"] = "None";
            }
        }
        else
        {
            data["running_model"] = "Ollama Offline";
        }

        std::string message = data.dump();

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* conn : connections_)
        {
            conn->send_text(message);
        }
    }

    sectorflux::Database& db_;
    std::jthread worker_;
    std::mutex mutex_;
    std::unordered_set<crow::websocket::connection*> connections_;

    static constexpr int kBroadcastIntervalSec = 1;
    static constexpr int kOllamaTimeoutSec = 1;
};

/**
 * @brief Open the default browser to the given URL.
 * @param url The URL to open.
 */
void openBrowser(const std::string& url)
{
#ifdef _WIN32
    std::string cmd = "start " + url;
#elif __APPLE__
    std::string cmd = "open " + url;
#else
    std::string cmd = "xdg-open " + url;
#endif
    std::system(cmd.c_str());
}

/**
 * @brief Create a lambda for proxying GET requests to Ollama.
 * @param endpoint The Ollama endpoint to proxy.
 * @return crow::response The proxied response.
 */
crow::response proxyGetRequest(const std::string& endpoint)
{
    httplib::Client cli(sectorflux::Config::getOllamaHost());
    cli.set_connection_timeout(kProxyTimeoutSec, 0);
    cli.set_read_timeout(kProxyTimeoutSec, 0);

    auto res = cli.Get(endpoint.c_str());
    if (res && res->status == 200)
    {
        crow::response c_res(200, res->body);
        c_res.set_header("Content-Type", "application/json");
        return c_res;
    }
    return crow::response(500, "Failed to fetch from Ollama");
}

}  // namespace

int main()
{
    crow::SimpleApp app;

    sectorflux::Database db;
    if (auto err = db.init(sectorflux::Config::getDatabasePath()))
    {
        std::cerr << "Failed to init DB: " << *err << std::endl;
        return 1;
    }

    sectorflux::ProxyHandler proxy_handler(db);
    DashboardBroadcaster dashboard_broadcaster(db);

    // API Routes - Proxy to Ollama
    CROW_ROUTE(app, "/api/generate")
        .methods(crow::HTTPMethod::POST)(
            [&proxy_handler](const crow::request& req, crow::response& res)
            {
                proxy_handler.handleRequest(req, res, "/api/generate");
            });

    CROW_ROUTE(app, "/api/chat")
        .methods(crow::HTTPMethod::POST)(
            [&proxy_handler](const crow::request& req, crow::response& res)
            {
                proxy_handler.handleRequest(req, res, "/api/chat");
            });

    // WebSocket Route - Chat with streaming
    CROW_WEBSOCKET_ROUTE(app, "/ws/chat")
        .onopen([&](crow::websocket::connection& conn)
        {
            std::lock_guard<std::mutex> lock(g_connection_mutex);
            g_connections[&conn] = std::make_shared<ConnectionState>();
        })
        .onclose([&](crow::websocket::connection& conn,
                     const std::string& /*reason*/,
                     uint16_t /*code*/)
        {
            std::shared_ptr<ConnectionState> state;
            {
                std::lock_guard<std::mutex> lock(g_connection_mutex);
                auto it = g_connections.find(&conn);
                if (it != g_connections.end())
                {
                    state = it->second;
                    g_connections.erase(it);
                }
            }

            if (state)
            {
                // Signal worker to stop
                state->active = false;
                // jthread will automatically join when state is destroyed
            }
        })
        .onmessage([&](crow::websocket::connection& conn,
                       const std::string& data,
                       bool is_binary)
        {
            if (!is_binary)
            {
                std::shared_ptr<ConnectionState> state;
                {
                    std::lock_guard<std::mutex> lock(g_connection_mutex);
                    auto it = g_connections.find(&conn);
                    if (it != g_connections.end())
                    {
                        state = it->second;
                    }
                }

                if (state)
                {
                    // Reset active flag for new request
                    state->active = true;

                    // Start new worker using jthread
                    state->worker = std::jthread(
                        [&proxy_handler, &conn, data, state](std::stop_token /*stop_token*/)
                        {
                            proxy_handler.handleWebSocketRequest(conn, data, state->active);
                        });
                }
            }
        });

    // WebSocket Route - Dashboard real-time updates
    std::cout << "Registering /ws/dashboard route..." << std::endl;
    CROW_WEBSOCKET_ROUTE(app, "/ws/dashboard")
        .onopen([&](crow::websocket::connection& conn)
        {
            std::cout << "Dashboard WebSocket connected" << std::endl;
            dashboard_broadcaster.addConnection(&conn);
        })
        .onclose([&](crow::websocket::connection& conn,
                     const std::string& /*reason*/,
                     uint16_t /*code*/)
        {
            std::cout << "Dashboard WebSocket disconnected" << std::endl;
            dashboard_broadcaster.removeConnection(&conn);
        })
        .onmessage([&](crow::websocket::connection& /*conn*/,
                       const std::string& /*data*/,
                       bool /*is_binary*/)
        {
            // Client doesn't send anything, just listens
        });

    // API Routes - Proxy Ollama info endpoints
    CROW_ROUTE(app, "/api/tags")([]()
    {
        return proxyGetRequest("/api/tags");
    });

    CROW_ROUTE(app, "/api/ps")([]()
    {
        return proxyGetRequest("/api/ps");
    });

    // API Routes - Logs
    CROW_ROUTE(app, "/api/logs")([&db]()
    {
        auto logs_opt = db.getLogs();
        if (!logs_opt)
        {
            return crow::response(500);
        }

        std::vector<crow::json::wvalue> log_list;
        const auto& logs = *logs_opt;

        for (const auto& log : logs)
        {
            crow::json::wvalue entry;
            entry["id"] = log.id;
            entry["timestamp"] = log.timestamp;
            entry["method"] = log.method;
            entry["endpoint"] = log.endpoint;
            entry["model"] = log.model;
            entry["response_status"] = log.response_status;
            entry["duration_ms"] = log.duration_ms;
            entry["prompt_tokens"] = log.prompt_tokens;
            entry["completion_tokens"] = log.completion_tokens;
            entry["prompt_eval_duration_ms"] = log.prompt_eval_duration_ms;
            entry["eval_duration_ms"] = log.eval_duration_ms;
            entry["request_body"] = log.request_body;
            entry["response_body"] = log.response_body;
            entry["is_starred"] = log.is_starred;
            log_list.push_back(std::move(entry));
        }

        crow::json::wvalue json_response = std::move(log_list);
        return crow::response(json_response);
    });

    CROW_ROUTE(app, "/api/logs/<int>")([&db](int id)
    {
        auto log_opt = db.getLog(id);
        if (!log_opt)
        {
            return crow::response(404, "Log not found");
        }

        const auto& log = *log_opt;
        crow::json::wvalue json_response;
        json_response["id"] = log.id;
        json_response["timestamp"] = log.timestamp;
        json_response["method"] = log.method;
        json_response["endpoint"] = log.endpoint;
        json_response["model"] = log.model;
        json_response["response_status"] = log.response_status;
        json_response["duration_ms"] = log.duration_ms;
        json_response["prompt_tokens"] = log.prompt_tokens;
        json_response["completion_tokens"] = log.completion_tokens;
        json_response["prompt_eval_duration_ms"] = log.prompt_eval_duration_ms;
        json_response["eval_duration_ms"] = log.eval_duration_ms;
        json_response["request_body"] = log.request_body;
        json_response["response_body"] = log.response_body;
        json_response["is_starred"] = log.is_starred;
        return crow::response(json_response);
    });

    // API Route - Set Starred Status
    CROW_ROUTE(app, "/api/logs/<int>/starred")
        .methods(crow::HTTPMethod::PUT)([&db](const crow::request& req, int id)
        {
            auto json = crow::json::load(req.body);
            if (!json)
            {
                return crow::response(400, "Invalid JSON");
            }

            if (!json.has("starred"))
            {
                return crow::response(400, "Missing 'starred' field");
            }

            bool is_starred = json["starred"].b();
            auto err = db.setStarred(id, is_starred);

            if (err)
            {
                return crow::response(500, *err);
            }

            crow::json::wvalue json_response;
            json_response["id"] = id;
            json_response["is_starred"] = is_starred;
            return crow::response(json_response);
        });

    // API Routes - Metrics
    CROW_ROUTE(app, "/api/metrics")([&db]()
    {
        auto metrics = db.getMetrics();
        crow::json::wvalue json_response;
        json_response["total_requests"] = metrics.total_requests;
        json_response["avg_latency_ms"] = metrics.avg_latency_ms;
        json_response["cache_hit_rate"] = metrics.cache_hit_rate;
        return crow::response(json_response);
    });

    // API Routes - Version
    CROW_ROUTE(app, "/api/version")([]()
    {
        crow::json::wvalue json_response;
        json_response["version"] = sectorflux::Version::kString;
        json_response["major"] = sectorflux::Version::kMajor;
        json_response["minor"] = sectorflux::Version::kMinor;
        json_response["patch"] = sectorflux::Version::kPatch;
        return crow::response(json_response);
    });

    // API Routes - Cache Configuration
    CROW_ROUTE(app, "/api/config/cache")
        .methods(crow::HTTPMethod::GET)([&proxy_handler]()
        {
            crow::json::wvalue json_response;
            json_response["enabled"] = proxy_handler.isCacheEnabled();
            return crow::response(json_response);
        });

    CROW_ROUTE(app, "/api/config/cache")
        .methods(crow::HTTPMethod::POST)([&proxy_handler](const crow::request& req)
        {
            auto json = crow::json::load(req.body);
            if (!json)
            {
                return crow::response(400, "Invalid JSON");
            }

            if (json.has("enabled"))
            {
                proxy_handler.setCacheEnabled(json["enabled"].b());
                return crow::response(200, "Cache configuration updated");
            }
            return crow::response(400, "Missing 'enabled' field");
        });

    // API Routes - Replay
    CROW_ROUTE(app, "/api/replay/<int>")
        .methods(crow::HTTPMethod::POST)(
            [&db, &proxy_handler](const crow::request& /*req*/,
                                  crow::response& res,
                                  int id)
            {
                auto log_opt = db.getLog(id);
                if (!log_opt)
                {
                    res.code = 404;
                    res.body = "Log entry not found";
                    res.end();
                    return;
                }

                const auto& log = *log_opt;

                // Construct a request to replay
                crow::request replay_req;
                replay_req.body = log.request_body;
                replay_req.method = crow::HTTPMethod::POST;
                // Skip cache for replays to get fresh response from Ollama
                replay_req.add_header("X-SectorFlux-No-Cache", "true");

                // Forward to proxy handler using the original endpoint
                proxy_handler.handleRequest(replay_req, res, log.endpoint);
            });

    // Static File Helper
    auto serve_embedded_file = [](crow::response& res,
                                  const unsigned char* data,
                                  size_t len,
                                  const std::string& content_type)
    {
        res.body = std::string(reinterpret_cast<const char*>(data), len);
        res.set_header("Content-Type", content_type);
        res.end();
    };

    // Static Files - Embedded UI
    CROW_ROUTE(app, "/")(
        [serve_embedded_file](const crow::request& /*req*/, crow::response& res)
        {
            serve_embedded_file(res, index_html, index_html_len, "text/html");
        });

    CROW_ROUTE(app, "/style.css")(
        [serve_embedded_file](const crow::request& /*req*/, crow::response& res)
        {
            serve_embedded_file(res, style_css, style_css_len, "text/css");
        });

    CROW_ROUTE(app, "/app.js")(
        [serve_embedded_file](const crow::request& /*req*/, crow::response& res)
        {
            serve_embedded_file(res, app_js, app_js_len, "application/javascript");
        });

    CROW_ROUTE(app, "/api.js")(
        [serve_embedded_file](const crow::request& /*req*/, crow::response& res)
        {
            serve_embedded_file(res, api_js, api_js_len, "application/javascript");
        });

    CROW_ROUTE(app, "/favicon.ico")([]()
    {
        return crow::response(204);
    });

    // API Route - Shutdown
    CROW_ROUTE(app, "/api/shutdown")
        .methods(crow::HTTPMethod::POST)([&app]()
        {
            std::cout << "Shutdown requested via API" << std::endl;
            app.stop();
            return crow::response(200, "Server shutting down");
        });

    int port = sectorflux::Config::getPort();
    std::cout << "SectorFlux v" << sectorflux::Version::kString
              << " starting on port " << port << "..." << std::endl;

    // Run browser opener in a separate thread
    std::jthread browser_thread([port](std::stop_token /*stop_token*/)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        openBrowser("http://localhost:" + std::to_string(port));
    });
    browser_thread.detach();

    app.port(static_cast<uint16_t>(port)).multithreaded().run();
    return 0;
}
