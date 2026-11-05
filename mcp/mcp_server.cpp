/*
 * KiCadD MCP - transport-independent MCP (JSON-RPC 2.0) message handling.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "mcp_server.h"

#include <algorithm>
#include <exception>

using json = nlohmann::json;

namespace kicadd::mcp
{

namespace
{

constexpr int JSONRPC_PARSE_ERROR      = -32700;
constexpr int JSONRPC_INVALID_REQUEST  = -32600;
constexpr int JSONRPC_METHOD_NOT_FOUND = -32601;
constexpr int JSONRPC_INVALID_PARAMS   = -32602;
constexpr int JSONRPC_INTERNAL_ERROR   = -32603;

json rpcError( const json& aId, int aCode, const std::string& aMessage )
{
    return { { "jsonrpc", "2.0" },
             { "id", aId },
             { "error", { { "code", aCode }, { "message", aMessage } } } };
}


json rpcResult( const json& aId, json aResult )
{
    return { { "jsonrpc", "2.0" }, { "id", aId }, { "result", std::move( aResult ) } };
}

} // namespace


const std::vector<std::string>& MCP_SERVER::SupportedProtocolVersions()
{
    // Newest first. Nothing here needs the newer features (sampling, elicitation, tasks),
    // so older revisions are served with the same behaviour.
    static const std::vector<std::string> versions = { "2025-11-25", "2025-06-18", "2025-03-26",
                                                       "2024-11-05" };
    return versions;
}


MCP_SERVER::MCP_SERVER( std::vector<TOOL> aTools ) :
        m_tools( std::move( aTools ) )
{
}


json MCP_SERVER::handleInitialize( const json& aParams )
{
    const auto& supported = SupportedProtocolVersions();
    std::string version = supported.front();

    if( aParams.contains( "protocolVersion" ) && aParams["protocolVersion"].is_string() )
    {
        std::string requested = aParams["protocolVersion"].get<std::string>();

        if( std::find( supported.begin(), supported.end(), requested ) != supported.end() )
            version = requested;
    }

    return { { "protocolVersion", version },
             { "capabilities", { { "tools", { { "listChanged", false } } } } },
             { "serverInfo", { { "name", "kicadd" }, { "version", "0.2.0" } } },
             { "instructions",
               "KiCadD: a KiCad PCB held in memory on the server. Coordinates in mm, Y axis down. "
               "Footprints are addressed by reference (U5), pads by REF.PAD (U5.3). Prefer "
               "query_area / query_net over broad queries; move_footprints returns a local DRC, so "
               "iterate with it and call flush_ratsnest / save_board when done." } };
}


json MCP_SERVER::handleToolsList() const
{
    json tools = json::array();

    for( const TOOL& t : m_tools )
    {
        tools.push_back( { { "name", t.name },
                           { "description", t.description },
                           { "inputSchema", t.inputSchema } } );
    }

    return { { "tools", tools } };
}


json MCP_SERVER::handleToolsCall( const json& aParams )
{
    std::string name = aParams.value( "name", "" );
    json        args = aParams.contains( "arguments" ) && aParams["arguments"].is_object()
                               ? aParams["arguments"]
                               : json::object();

    auto it = std::find_if( m_tools.begin(), m_tools.end(),
                            [&]( const TOOL& t ) { return t.name == name; } );

    if( it == m_tools.end() )
        throw std::invalid_argument( "unknown tool: " + name );

    TOOL_RESULT r;

    {
        std::lock_guard<std::mutex> lock( m_callMutex );

        try
        {
            r = it->run( args );
        }
        catch( const std::exception& e )
        {
            r = { std::string( "tool failed: " ) + e.what(), true };
        }
    }

    return { { "content", json::array( { { { "type", "text" }, { "text", r.text } } } ) },
             { "isError", r.isError } };
}


std::optional<json> MCP_SERVER::HandleMessage( const json& aMessage )
{
    if( !aMessage.is_object() )
        return rpcError( nullptr, JSONRPC_INVALID_REQUEST, "message must be an object" );

    const bool hasId = aMessage.contains( "id" );
    const json id = hasId ? aMessage["id"] : json( nullptr );

    // A response from the client (we never send requests, but tolerate them) or a notification.
    if( !aMessage.contains( "method" ) )
        return std::nullopt;

    if( !aMessage["method"].is_string() )
        return hasId ? std::optional<json>( rpcError( id, JSONRPC_INVALID_REQUEST, "method must be a string" ) )
                     : std::nullopt;

    const std::string method = aMessage["method"].get<std::string>();
    const json        params = aMessage.contains( "params" ) ? aMessage["params"] : json::object();

    if( !hasId )
        return std::nullopt;   // notifications/initialized, notifications/cancelled, ...

    try
    {
        if( method == "initialize" )
            return rpcResult( id, handleInitialize( params ) );

        if( method == "ping" )
            return rpcResult( id, json::object() );

        if( method == "tools/list" )
            return rpcResult( id, handleToolsList() );

        if( method == "tools/call" )
            return rpcResult( id, handleToolsCall( params ) );

        return rpcError( id, JSONRPC_METHOD_NOT_FOUND, "method not found: " + method );
    }
    catch( const std::invalid_argument& e )
    {
        return rpcError( id, JSONRPC_INVALID_PARAMS, e.what() );
    }
    catch( const std::exception& e )
    {
        return rpcError( id, JSONRPC_INTERNAL_ERROR, e.what() );
    }
}


std::optional<json> MCP_SERVER::HandleBody( const json& aBody )
{
    if( aBody.is_array() )
    {
        if( aBody.empty() )
            return rpcError( nullptr, JSONRPC_INVALID_REQUEST, "empty batch" );

        json out = json::array();

        for( const json& msg : aBody )
        {
            if( std::optional<json> r = HandleMessage( msg ) )
                out.push_back( std::move( *r ) );
        }

        return out.empty() ? std::nullopt : std::optional<json>( out );
    }

    return HandleMessage( aBody );
}

} // namespace kicadd::mcp
