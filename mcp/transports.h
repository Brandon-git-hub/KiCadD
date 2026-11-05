/*
 * KiCadD MCP - stdio and Streamable HTTP transports.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_TRANSPORTS_H
#define KICADD_MCP_TRANSPORTS_H

#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>

namespace kicadd::mcp
{

class MCP_SERVER;
class WEB_HUB;

/// Newline-delimited JSON-RPC on stdin/stdout until EOF. Logs go to stderr.
int RunStdio( MCP_SERVER& aServer );

struct HTTP_OPTIONS
{
    std::string              host = "127.0.0.1";
    unsigned short           port = 8765;
    std::string              path = "/mcp";
    std::string              token;            ///< if set, require "Authorization: Bearer <token>"
    bool                     trustLoopback = false;   ///< no token needed from 127.0.0.1 / ::1 peers
    std::vector<std::string> allowedOrigins;   ///< besides localhost; "*" disables the check

    bool        web = true;   ///< serve the 2D view at / and its WebSocket at /ws
    std::string webRoot;      ///< serve index.html from here instead of the embedded copy
};

/// MCP Streamable HTTP (POST JSON-RPC, JSON responses; no server-initiated stream), plus the
/// 2D view (GET /, WebSocket /ws) when aHub is set. Sockets are created on aIoc, which the
/// hub runs for WebSocket sessions. Blocks.
int RunHttp( MCP_SERVER& aServer, const HTTP_OPTIONS& aOptions, WEB_HUB* aHub,
             boost::asio::io_context& aIoc );

} // namespace kicadd::mcp

#endif // KICADD_MCP_TRANSPORTS_H
