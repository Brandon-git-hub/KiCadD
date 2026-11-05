/*
 * KiCadD MCP - stdio and Streamable HTTP transports.
 *
 * HTTP follows the MCP Streamable HTTP transport: the client POSTs JSON-RPC to one endpoint;
 * requests get an application/json response, notifications get 202. The server never
 * initiates messages, so GET (SSE stream) answers 405, which the spec allows.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "transports.h"
#include "mcp_server.h"
#include "web.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace kicadd::mcp
{

int RunStdio( MCP_SERVER& aServer )
{
    std::ios::sync_with_stdio( false );
    std::string line;

    while( std::getline( std::cin, line ) )
    {
        if( line.find_first_not_of( " \t\r" ) == std::string::npos )
            continue;

        std::optional<json> reply;

        try
        {
            reply = aServer.HandleBody( json::parse( line ) );
        }
        catch( const json::parse_error& e )
        {
            reply = json{ { "jsonrpc", "2.0" },
                          { "id", nullptr },
                          { "error", { { "code", -32700 }, { "message", e.what() } } } };
        }

        if( reply )
        {
            std::cout << reply->dump() << '\n';
            std::cout.flush();
        }
    }

    return 0;
}


/// Embedded web/index.html (generated at build time, see cmake/embed_web.cmake).
extern const char* const KICADD_WEB_INDEX_HTML;


namespace
{

std::string newSessionId()
{
    static std::mt19937_64 rng( std::random_device{}() );
    static std::mutex      mutex;
    std::lock_guard<std::mutex> lock( mutex );

    char buf[33];
    std::snprintf( buf, sizeof( buf ), "%016llx%016llx", (unsigned long long) rng(),
                   (unsigned long long) rng() );
    return buf;
}


bool isLocalOrigin( const std::string& aOrigin )
{
    for( const char* prefix : { "http://localhost", "https://localhost", "http://127.0.0.1",
                                "https://127.0.0.1", "http://[::1]", "https://[::1]" } )
    {
        if( aOrigin.rfind( prefix, 0 ) == 0 )
        {
            char next = aOrigin.size() > strlen( prefix ) ? aOrigin[strlen( prefix )] : '\0';

            if( next == '\0' || next == ':' || next == '/' )
                return true;
        }
    }

    return false;
}


std::string queryParam( const std::string& aTarget, const std::string& aKey )
{
    size_t q = aTarget.find( '?' );

    while( q != std::string::npos )
    {
        size_t start = q + 1;
        size_t end = aTarget.find( '&', start );
        std::string kv = aTarget.substr( start, end == std::string::npos ? std::string::npos : end - start );

        if( kv.rfind( aKey + "=", 0 ) == 0 )
            return kv.substr( aKey.size() + 1 );

        q = end;
    }

    return {};
}


using REQUEST = http::request<http::string_body>;
using RESPONSE = http::response<http::string_body>;


RESPONSE makeResponse( const REQUEST& aReq, http::status aStatus, std::string aBody = {},
                       const char* aContentType = "application/json" )
{
    RESPONSE res{ aStatus, aReq.version() };
    res.set( http::field::server, "kicadd-mcp" );
    res.keep_alive( aReq.keep_alive() );

    if( !aBody.empty() )
        res.set( http::field::content_type, aContentType );

    res.body() = std::move( aBody );
    res.prepare_payload();
    return res;
}


/// DNS-rebinding protection: browsers send Origin; accept local, same-origin (the page we
/// served ourselves) or explicitly allowed origins.
bool originAllowed( const HTTP_OPTIONS& aOpt, const REQUEST& aReq )
{
    auto origin = aReq.find( http::field::origin );

    if( origin == aReq.end() )
        return true;   // not a browser

    std::string o( origin->value() );

    if( isLocalOrigin( o ) )
        return true;

    if( auto host = aReq.find( http::field::host ); host != aReq.end() )
    {
        std::string h( host->value() );

        if( o == "http://" + h || o == "https://" + h )
            return true;
    }

    for( const std::string& allowed : aOpt.allowedOrigins )
    {
        if( allowed == "*" || allowed == o )
            return true;
    }

    return false;
}


bool authorized( const HTTP_OPTIONS& aOpt, const REQUEST& aReq )
{
    if( aOpt.token.empty() )
        return true;

    auto auth = aReq.find( http::field::authorization );

    if( auth != aReq.end() && auth->value() == "Bearer " + aOpt.token )
        return true;

    // Browsers cannot set headers on a WebSocket: accept ?token= as well.
    return queryParam( std::string( aReq.target() ), "token" ) == aOpt.token;
}


std::string loadPage( const HTTP_OPTIONS& aOpt )
{
    if( aOpt.webRoot.empty() )
        return KICADD_WEB_INDEX_HTML;

    std::ifstream     in( aOpt.webRoot + "/index.html", std::ios::binary );
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}


RESPONSE handle( MCP_SERVER& aServer, const HTTP_OPTIONS& aOpt, const REQUEST& aReq )
{
    // Strip the query string for matching.
    std::string target( aReq.target() );

    if( size_t q = target.find( '?' ); q != std::string::npos )
        target.resize( q );

    // The page is static and holds no data; the WebSocket behind it is what's protected.
    if( aOpt.web && aReq.method() == http::verb::get && ( target == "/" || target == "/index.html" ) )
    {
        RESPONSE res = makeResponse( aReq, http::status::ok, loadPage( aOpt ), "text/html; charset=utf-8" );
        res.set( http::field::cache_control, "no-cache" );
        return res;
    }

    if( target != aOpt.path )
        return makeResponse( aReq, http::status::not_found, R"({"error":"not found"})" );

    if( !originAllowed( aOpt, aReq ) )
        return makeResponse( aReq, http::status::forbidden, R"({"error":"origin not allowed"})" );

    if( !aOpt.token.empty() )
    {
        auto auth = aReq.find( http::field::authorization );

        if( auth == aReq.end() || auth->value() != "Bearer " + aOpt.token )
        {
            RESPONSE res = makeResponse( aReq, http::status::unauthorized, R"({"error":"unauthorized"})" );
            res.set( http::field::www_authenticate, "Bearer" );
            return res;
        }
    }

    if( aReq.method() == http::verb::get )
    {
        RESPONSE res = makeResponse( aReq, http::status::method_not_allowed );
        res.set( http::field::allow, "POST, DELETE" );
        return res;
    }

    if( aReq.method() == http::verb::delete_ )
        return makeResponse( aReq, http::status::ok );   // sessions hold no server state

    if( aReq.method() != http::verb::post )
        return makeResponse( aReq, http::status::method_not_allowed );

    json body;

    try
    {
        body = json::parse( aReq.body() );
    }
    catch( const json::parse_error& e )
    {
        json err = { { "jsonrpc", "2.0" },
                     { "id", nullptr },
                     { "error", { { "code", -32700 }, { "message", e.what() } } } };
        return makeResponse( aReq, http::status::bad_request, err.dump() );
    }

    const bool isInitialize = body.is_object() && body.value( "method", "" ) == "initialize";

    std::optional<json> reply = aServer.HandleBody( body );

    if( !reply )
        return makeResponse( aReq, http::status::accepted );

    RESPONSE res = makeResponse( aReq, http::status::ok, reply->dump() );

    if( isInitialize )
        res.set( "Mcp-Session-Id", newSessionId() );

    return res;
}


void serveConnection( tcp::socket aSocket, MCP_SERVER& aServer, const HTTP_OPTIONS& aOpt, WEB_HUB* aHub )
{
    beast::flat_buffer buffer;
    beast::error_code  ec;

    for( ;; )
    {
        http::request_parser<http::string_body> parser;
        parser.body_limit( 8 * 1024 * 1024 );
        http::read( aSocket, buffer, parser, ec );

        if( ec == http::error::end_of_stream || ec == asio::error::connection_reset
            || ec == asio::error::eof )
            break;

        if( ec )
        {
            std::fprintf( stderr, "kicadd-mcp: read error: %s\n", ec.message().c_str() );
            break;
        }

        REQUEST req = parser.release();

        if( beast::websocket::is_upgrade( req ) )
        {
            std::string target( req.target() );
            target = target.substr( 0, target.find( '?' ) );

            RESPONSE refuse;
            bool     refused = true;

            if( !aHub || target != "/ws" )
                refuse = makeResponse( req, http::status::not_found, R"({"error":"not found"})" );
            else if( !originAllowed( aOpt, req ) )
                refuse = makeResponse( req, http::status::forbidden, R"({"error":"origin not allowed"})" );
            else if( !authorized( aOpt, req ) )
                refuse = makeResponse( req, http::status::unauthorized, R"({"error":"unauthorized"})" );
            else
                refused = false;

            if( refused )
            {
                refuse.keep_alive( false );
                http::write( aSocket, refuse, ec );
                break;
            }

            aHub->Accept( std::move( aSocket ), std::move( req ) );
            return;   // the hub owns the socket now
        }

        RESPONSE res = handle( aServer, aOpt, req );
        bool     keepAlive = res.keep_alive();

        http::write( aSocket, res, ec );

        if( ec || !keepAlive )
            break;
    }

    aSocket.shutdown( tcp::socket::shutdown_send, ec );
}

} // namespace


int RunHttp( MCP_SERVER& aServer, const HTTP_OPTIONS& aOpt, WEB_HUB* aHub, asio::io_context& aIoc )
{
    try
    {
        tcp::endpoint endpoint{ asio::ip::make_address( aOpt.host ), aOpt.port };
        tcp::acceptor acceptor{ aIoc, endpoint };

        std::fprintf( stderr, "kicadd-mcp: listening on http://%s:%u%s%s\n", aOpt.host.c_str(),
                      (unsigned) aOpt.port, aOpt.path.c_str(),
                      aOpt.token.empty() ? "" : " (bearer token required)" );

        if( aHub )
            std::fprintf( stderr, "kicadd-mcp: 2D view at http://%s:%u/%s\n", aOpt.host.c_str(),
                          (unsigned) aOpt.port, aOpt.token.empty() ? "" : "?token=..." );

        for( ;; )
        {
            tcp::socket socket{ aIoc };
            acceptor.accept( socket );
            std::thread( serveConnection, std::move( socket ), std::ref( aServer ), std::cref( aOpt ), aHub )
                    .detach();
        }
    }
    catch( const std::exception& e )
    {
        std::fprintf( stderr, "kicadd-mcp: http server error: %s\n", e.what() );
        return 1;
    }
}

} // namespace kicadd::mcp
