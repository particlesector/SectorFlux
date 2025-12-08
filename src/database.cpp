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

#include "database.hpp"

#include <iostream>

namespace sectorflux
{

Database::Database()
{
}

Database::~Database()
{
    // Signal shutdown and wait for worker to finish
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_requested_ = true;
    }
    queue_cv_.notify_one();

    // jthread automatically joins on destruction, but we need to ensure
    // the worker exits its wait loop first

    if (db_)
    {
        sqlite3_close(db_);
    }
}

std::optional<std::string> Database::init(const std::string& db_path)
{
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc)
    {
        std::string err = "Can't open database: " + std::string(sqlite3_errmsg(db_));
        return err;
    }

    // Enable WAL mode for concurrent write support
    char* wal_err = nullptr;
    rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &wal_err);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Warning: Failed to enable WAL mode: "
                  << (wal_err ? wal_err : "unknown error") << std::endl;
        if (wal_err)
        {
            sqlite3_free(wal_err);
        }
    }

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS requests (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            method TEXT,
            endpoint TEXT,
            model TEXT,
            request_body TEXT,
            response_status INTEGER,
            response_body TEXT,
            duration_ms INTEGER,
            prompt_tokens INTEGER DEFAULT 0,
            completion_tokens INTEGER DEFAULT 0,
            prompt_eval_duration_ms INTEGER DEFAULT 0,
            eval_duration_ms INTEGER DEFAULT 0,
            ttft_ms INTEGER DEFAULT 0,
            is_starred INTEGER DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS cache (
            request_body TEXT PRIMARY KEY,
            response_status INTEGER,
            response_body TEXT
        );
    )";

    char* err_msg = nullptr;
    rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK)
    {
        std::string err = "SQL error: " + std::string(err_msg);
        sqlite3_free(err_msg);
        return err;
    }

    // Start the async write worker thread
    write_worker_ = std::jthread([this](std::stop_token stop_token)
    {
        while (!stop_token.stop_requested())
        {
            processWriteQueue();
        }
        // Process any remaining items before exiting
        processWriteQueue();
    });

    return std::nullopt;
}

void Database::processWriteQueue()
{
    std::function<void()> task;

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this]
        {
            return !write_queue_.empty() || shutdown_requested_;
        });

        if (write_queue_.empty())
        {
            return;
        }

        task = std::move(write_queue_.front());
        write_queue_.pop();
    }

    // Execute the task outside the lock
    if (task)
    {
        task();
    }
}

void Database::logInteractionAsync(
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
    long long ttft_ms)
{
    // Capture all parameters by value for the async task
    auto task = [this,
                 method,
                 endpoint,
                 model,
                 request_body,
                 response_status,
                 response_body,
                 duration_ms,
                 prompt_tokens,
                 completion_tokens,
                 prompt_eval_duration_ms,
                 eval_duration_ms,
                 ttft_ms]()
    {
        auto result = logInteractionSync(
            method,
            endpoint,
            model,
            request_body,
            response_status,
            response_body,
            duration_ms,
            prompt_tokens,
            completion_tokens,
            prompt_eval_duration_ms,
            eval_duration_ms,
            ttft_ms);

        if (result)
        {
            std::cerr << "Async log failed: " << *result << std::endl;
        }
    };

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        write_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

std::optional<std::string> Database::logInteractionSync(
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
    long long ttft_ms)
{
    if (!db_)
    {
        return "Database not initialized";
    }

    const char* sql =
        "INSERT INTO requests (method, endpoint, model, request_body, "
        "response_status, response_body, duration_ms, prompt_tokens, "
        "completion_tokens, prompt_eval_duration_ms, eval_duration_ms, ttft_ms) VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_));
    }

    sqlite3_bind_text(stmt, 1, method.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, endpoint.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, model.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, request_body.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, response_status);
    sqlite3_bind_text(stmt, 6, response_body.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 7, duration_ms);
    sqlite3_bind_int(stmt, 8, prompt_tokens);
    sqlite3_bind_int(stmt, 9, completion_tokens);
    sqlite3_bind_int64(stmt, 10, prompt_eval_duration_ms);
    sqlite3_bind_int64(stmt, 11, eval_duration_ms);
    sqlite3_bind_int64(stmt, 12, ttft_ms);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        std::string err = "Execution failed: " + std::string(sqlite3_errmsg(db_));
        sqlite3_finalize(stmt);
        return err;
    }
    sqlite3_finalize(stmt);

    // Enforce History Limit (Keep last kMaxHistoryEntries)
    const char* delete_sql =
        "DELETE FROM requests WHERE id NOT IN ("
        "SELECT id FROM requests ORDER BY id DESC LIMIT ?)";

    sqlite3_stmt* delete_stmt;
    rc = sqlite3_prepare_v2(db_, delete_sql, -1, &delete_stmt, nullptr);
    if (rc == SQLITE_OK)
    {
        sqlite3_bind_int(delete_stmt, 1, kMaxHistoryEntries);
        rc = sqlite3_step(delete_stmt);
        if (rc != SQLITE_DONE)
        {
            std::cerr << "Failed to enforce history limit: "
                      << sqlite3_errmsg(db_) << std::endl;
        }
        sqlite3_finalize(delete_stmt);
    }

    return std::nullopt;
}

std::optional<std::vector<LogEntry>> Database::getLogs(int limit)
{
    if (!db_)
    {
        return std::nullopt;
    }

    std::vector<LogEntry> logs;
    const char* sql =
        "SELECT id, timestamp, method, endpoint, model, request_body, "
        "response_status, response_body, duration_ms, prompt_tokens, "
        "completion_tokens, prompt_eval_duration_ms, eval_duration_ms, "
        "ttft_ms, is_starred FROM requests ORDER BY id DESC LIMIT ?";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return std::nullopt;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        LogEntry entry;
        entry.id = sqlite3_column_int(stmt, 0);

        const unsigned char* ts = sqlite3_column_text(stmt, 1);
        entry.timestamp = ts ? reinterpret_cast<const char*>(ts) : "";

        const unsigned char* m = sqlite3_column_text(stmt, 2);
        entry.method = m ? reinterpret_cast<const char*>(m) : "";

        const unsigned char* ep = sqlite3_column_text(stmt, 3);
        entry.endpoint = ep ? reinterpret_cast<const char*>(ep) : "";

        const unsigned char* model = sqlite3_column_text(stmt, 4);
        entry.model = model ? reinterpret_cast<const char*>(model) : "";

        const unsigned char* req = sqlite3_column_text(stmt, 5);
        entry.request_body = req ? reinterpret_cast<const char*>(req) : "";

        entry.response_status = sqlite3_column_int(stmt, 6);

        const unsigned char* res = sqlite3_column_text(stmt, 7);
        entry.response_body = res ? reinterpret_cast<const char*>(res) : "";

        entry.duration_ms = sqlite3_column_int64(stmt, 8);
        entry.prompt_tokens = sqlite3_column_int(stmt, 9);
        entry.completion_tokens = sqlite3_column_int(stmt, 10);
        entry.prompt_eval_duration_ms = sqlite3_column_int64(stmt, 11);
        entry.eval_duration_ms = sqlite3_column_int64(stmt, 12);
        entry.ttft_ms = sqlite3_column_int64(stmt, 13);
        entry.is_starred = sqlite3_column_int(stmt, 14) != 0;

        logs.push_back(entry);
    }

    sqlite3_finalize(stmt);
    return logs;
}

std::optional<std::pair<int, std::string>>
Database::getCachedResponse(const std::string& request_body)
{
    if (!db_)
    {
        return std::nullopt;
    }

    const char* sql =
        "SELECT response_status, response_body FROM cache WHERE request_body = ?";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, request_body.c_str(), -1, SQLITE_STATIC);

    std::optional<std::pair<int, std::string>> result = std::nullopt;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int status = sqlite3_column_int(stmt, 0);
        const unsigned char* body = sqlite3_column_text(stmt, 1);
        result = {status, body ? reinterpret_cast<const char*>(body) : ""};
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<std::string> Database::cacheResponse(
    const std::string& request_body,
    int response_status,
    const std::string& response_body)
{
    if (!db_)
    {
        return "Database not initialized";
    }

    const char* sql =
        "INSERT OR REPLACE INTO cache (request_body, response_status, response_body) "
        "VALUES (?, ?, ?)";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return std::string(sqlite3_errmsg(db_));
    }

    sqlite3_bind_text(stmt, 1, request_body.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, response_status);
    sqlite3_bind_text(stmt, 3, response_body.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return err;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

Metrics Database::getMetrics()
{
    Metrics m = {0, 0.0, 0.0};
    if (!db_)
    {
        return m;
    }

    // Total Requests
    const char* sql_total = "SELECT COUNT(*) FROM requests";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql_total, -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            m.total_requests = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Avg Latency
    const char* sql_latency = "SELECT AVG(duration_ms) FROM requests";
    if (sqlite3_prepare_v2(db_, sql_latency, -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            m.avg_latency_ms = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Cache Hit Rate (cache hits are marked with duration_ms = 0)
    const char* hit_sql = "SELECT COUNT(*) FROM requests WHERE duration_ms = 0";
    sqlite3_stmt* hit_stmt;
    if (sqlite3_prepare_v2(db_, hit_sql, -1, &hit_stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(hit_stmt) == SQLITE_ROW)
        {
            int hits = sqlite3_column_int(hit_stmt, 0);
            if (m.total_requests > 0)
            {
                m.cache_hit_rate = static_cast<double>(hits) / m.total_requests;
            }
        }
        sqlite3_finalize(hit_stmt);
    }

    return m;
}

std::optional<LogEntry> Database::getLog(int id)
{
    if (!db_)
    {
        return std::nullopt;
    }

    const char* sql =
        "SELECT id, timestamp, method, endpoint, model, request_body, "
        "response_status, response_body, duration_ms, prompt_tokens, "
        "completion_tokens, prompt_eval_duration_ms, eval_duration_ms, "
        "ttft_ms, is_starred FROM requests WHERE id = ?";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return std::nullopt;
    }

    sqlite3_bind_int(stmt, 1, id);

    std::optional<LogEntry> result = std::nullopt;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        LogEntry entry;
        entry.id = sqlite3_column_int(stmt, 0);

        const unsigned char* ts = sqlite3_column_text(stmt, 1);
        entry.timestamp = ts ? reinterpret_cast<const char*>(ts) : "";

        const unsigned char* m = sqlite3_column_text(stmt, 2);
        entry.method = m ? reinterpret_cast<const char*>(m) : "";

        const unsigned char* ep = sqlite3_column_text(stmt, 3);
        entry.endpoint = ep ? reinterpret_cast<const char*>(ep) : "";

        const unsigned char* model = sqlite3_column_text(stmt, 4);
        entry.model = model ? reinterpret_cast<const char*>(model) : "";

        const unsigned char* req = sqlite3_column_text(stmt, 5);
        entry.request_body = req ? reinterpret_cast<const char*>(req) : "";

        entry.response_status = sqlite3_column_int(stmt, 6);

        const unsigned char* res = sqlite3_column_text(stmt, 7);
        entry.response_body = res ? reinterpret_cast<const char*>(res) : "";

        entry.duration_ms = sqlite3_column_int64(stmt, 8);
        entry.prompt_tokens = sqlite3_column_int(stmt, 9);
        entry.completion_tokens = sqlite3_column_int(stmt, 10);
        entry.prompt_eval_duration_ms = sqlite3_column_int64(stmt, 11);
        entry.eval_duration_ms = sqlite3_column_int64(stmt, 12);
        entry.ttft_ms = sqlite3_column_int64(stmt, 13);
        entry.is_starred = sqlite3_column_int(stmt, 14) != 0;

        result = entry;
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<std::string> Database::setStarred(int id, bool is_starred)
{
    if (!db_)
    {
        return "Database not initialized";
    }

    const char* sql = "UPDATE requests SET is_starred = ? WHERE id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_));
    }

    sqlite3_bind_int(stmt, 1, is_starred ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        std::string err = "Execution failed: " + std::string(sqlite3_errmsg(db_));
        sqlite3_finalize(stmt);
        return err;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

} // namespace sectorflux
