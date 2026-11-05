/*
 * KiCadD MCP - transport-independent MCP (JSON-RPC 2.0) message handling.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_SERVER_H
#define KICADD_MCP_SERVER_H

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools.h"

namespace kicadd::mcp
{

class MCP_SERVER
{
public:
    explicit MCP_SERVER( std::vector<TOOL> aTools );

    /**
     * Handle one JSON-RPC message (request or notification). Returns the response, or
     * nullopt for notifications and client responses.
     */
    std::optional<nlohmann::json> HandleMessage( const nlohmann::json& aMessage );

    /// Handle a raw body: a single message or (pre-2025-06-18 clients) a batch array.
    /// Returns nullopt when nothing needs to be sent back.
    std::optional<nlohmann::json> HandleBody( const nlohmann::json& aBody );

    static const std::vector<std::string>& SupportedProtocolVersions();

private:
    nlohmann::json handleInitialize( const nlohmann::json& aParams );
    nlohmann::json handleToolsList() const;
    nlohmann::json handleToolsCall( const nlohmann::json& aParams );

    std::vector<TOOL> m_tools;
    std::mutex        m_callMutex;   // tool calls touch one shared board; run them one at a time
};

} // namespace kicadd::mcp

#endif // KICADD_MCP_SERVER_H
