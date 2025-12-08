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

#include <cstdlib>
#include <string>

namespace sectorflux
{

namespace detail
{

/**
 * @brief Cross-platform safe getenv wrapper.
 * @param name Environment variable name.
 * @return std::string The value or empty string if not found.
 */
inline std::string safeGetenv(const char* name)
{
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr)
    {
        std::string result(buffer);
        free(buffer);
        return result;
    }
    return "";
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : "";
#endif
}

}  // namespace detail

/**
 * @brief Configuration management class for SectorFlux.
 *
 * Provides environment-based configuration with sensible defaults.
 */
class Config
{
public:
    /**
     * @brief Get the Ollama host URL.
     * @return std::string The Ollama host URL (default: "http://localhost:11434").
     */
    static std::string getOllamaHost()
    {
        std::string env_host = detail::safeGetenv("OLLAMA_HOST");
        if (!env_host.empty())
        {
            return env_host;
        }
        return "http://localhost:11434";
    }

    /**
     * @brief Get the database file path.
     * @return std::string The database path (default: "sectorflux.db").
     */
    static std::string getDatabasePath()
    {
        std::string env_db = detail::safeGetenv("SECTORFLUX_DB");
        if (!env_db.empty())
        {
            return env_db;
        }
        return "sectorflux.db";
    }

    /**
     * @brief Get the SectorFlux listening port.
     * @return int The port number (default: 8888).
     */
    static int getPort()
    {
        std::string env_port = detail::safeGetenv("SECTORFLUX_PORT");
        if (!env_port.empty())
        {
            try
            {
                int port = std::stoi(env_port);
                if (port > 0 && port <= 65535)
                {
                    return port;
                }
            }
            catch (...)
            {
                // Invalid port, use default
            }
        }
        return kDefaultPort;
    }

    // Configuration constants
    static constexpr int kDefaultPort = 8888;
    static constexpr int kDefaultTimeout = 60;
    static constexpr int kMaxHistoryEntries = 100;
};

} // namespace sectorflux
