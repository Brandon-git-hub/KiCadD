/*
 * KiCadD MCP - stdio and Streamable HTTP transports.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_TRANSPORTS_H
#define KICADD_MCP_TRANSPORTS_H

#include <string>
#include <vector>

namespace kicadd::mcp
{

class MCP_SERVER;

/// Newline-delimited JSON-RPC on stdin/stdout until EOF. Logs go to stderr.
int RunStdio( MCP_SERVER& aServer );

struct HTTP_OPTIONS
{
    std::string              host = "127.0.0.1";
    unsigned short           port = 8765;
    std::string              path = "/mcp";
    std::string              token;            ///< if set, require "Authorization: Bearer <token>"
    std::vector<std::string> allowedOrigins;   ///< besides localhost; "*" disables the check
};

/// MCP Streamable HTTP (POST JSON-RPC, JSON responses; no server-initiated stream). Blocks.
int RunHttp( MCP_SERVER& aServer, const HTTP_OPTIONS& aOptions );

} // namespace kicadd::mcp

#endif // KICADD_MCP_TRANSPORTS_H
