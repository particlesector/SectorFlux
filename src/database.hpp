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

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <vector>

namespace sectorflux
{

/**
 * @brief Represents a single logged request/response interaction.
 */
struct LogEntry
{
    int id;
    std::string timestamp;
    std::string method;
    std::string endpoint;
    std::string model;
    std::string request_body;
    int response_status;
    std::string response_body;
    long long duration_ms;
    int prompt_tokens;
    int completion_tokens;
    long long prompt_eval_duration_ms;
    long long eval_duration_ms;
    long long ttft_ms;
    bool is_starred;
};

/**
 * @brief Aggregated metrics for the dashboard.
 */
struct Metrics
{
    int total_requests;
    double avg_latency_ms;
    double cache_hit_rate;
};

/**
 * @brief Database manager for SQLite persistence.
 *
 * Handles all database operations including logging, caching, and metrics.
 * Uses WAL mode for concurrent write support and async writes to avoid
 * blocking the HTTP response stream.
 */
class Database
{
public:
    Database();
    ~Database();

    // Delete copy operations to prevent accidental duplication
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /**
     * @brief Initialize the database (open connection, create tables, enable WAL).
     * @param db_path Path to the SQLite database file.
     * @return std::optional<std::string> std::nullopt on success, error message on failure.
     */
    std::optional<std::string> init(const std::string& db_path = "sectorflux.db");

    /**
     * @brief Log a request/response interaction asynchronously.
     *
     * This method queues the log entry for async write to avoid blocking
     * the HTTP response stream.
     *
     * @param method HTTP method (GET, POST, etc.).
     * @param endpoint API endpoint.
     * @param model The LLM model used (e.g., "llama3:latest").
     * @param request_body Body of the request.
     * @param response_status HTTP status code of the response.
     * @param response_body Body of the response.
     * @param duration_ms Duration of the request in milliseconds.
     * @param prompt_tokens Number of prompt tokens.
     * @param completion_tokens Number of completion tokens.
     * @param prompt_eval_duration_ms Prompt evaluation duration in ms.
     * @param eval_duration_ms Evaluation duration in ms.
     * @param ttft_ms Time to first token in ms.
     */
    void logInteractionAsync(
        const std::string& method,
        const std::string& endpoint,
        const std::string& model,
        const std::string& request_body,
        int response_status,
        const std::string& response_body,
        long long duration_ms,
        int prompt_tokens,
        int completion_tokens,
        long long prompt_eval_duration_ms,
        long long eval_duration_ms,
        long long ttft_ms);

    /**
     * @brief Retrieve recent logs.
     * @param limit Maximum number of logs to retrieve.
     * @return std::optional<std::vector<LogEntry>> Vector of logs on success,
     *         std::nullopt on failure.
     */
    [[nodiscard]] std::optional<std::vector<LogEntry>> getLogs(int limit = 50);

    /**
     * @brief Get a cached response for a given request body.
     * @param request_body The request body to look up.
     * @return std::optional<std::pair<int, std::string>> Status code and response
     *         body if found, nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::pair<int, std::string>>
    getCachedResponse(const std::string& request_body);

    /**
     * @brief Cache a response for a given request body.
     * @param request_body The request body to cache.
     * @param response_status The status code to cache.
     * @param response_body The response body to cache.
     * @return std::optional<std::string> Error message on failure, nullopt on success.
     */
    std::optional<std::string> cacheResponse(
        const std::string& request_body,
        int response_status,
        const std::string& response_body);

    /**
     * @brief Get current metrics.
     * @return Metrics struct containing total requests, avg latency, and cache hit rate.
     */
    [[nodiscard]] Metrics getMetrics();

    /**
     * @brief Get a specific log entry by ID (for replay).
     * @param id The ID of the log entry.
     * @return std::optional<LogEntry> The log entry if found, nullopt otherwise.
     */
    [[nodiscard]] std::optional<LogEntry> getLog(int id);

    /**
     * @brief Set the starred status of a log entry.
     * @param id The ID of the log entry.
     * @param is_starred The new starred status.
     * @return std::optional<std::string> Error message on failure, nullopt on success.
     */
    std::optional<std::string> setStarred(int id, bool is_starred);

private:
    /**
     * @brief Internal synchronous log interaction (called by worker thread).
     */
    std::optional<std::string> logInteractionSync(
        const std::string& method,
        const std::string& endpoint,
        const std::string& model,
        const std::string& request_body,
        int response_status,
        const std::string& response_body,
        long long duration_ms,
        int prompt_tokens,
        int completion_tokens,
        long long prompt_eval_duration_ms,
        long long eval_duration_ms,
        long long ttft_ms);

    /**
     * @brief Worker thread function for async database writes.
     */
    void processWriteQueue();

    sqlite3* db_ = nullptr;

    // Async write queue
    std::queue<std::function<void()>> write_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::jthread write_worker_;
    std::atomic<bool> shutdown_requested_{false};

    // Constants
    static constexpr int kMaxHistoryEntries = 100;
};

} // namespace sectorflux
