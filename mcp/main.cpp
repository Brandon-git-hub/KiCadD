/*
 * kicadd-mcp: MCP server for a running KiCadD (kicad-cli api-server).
 *
 *   kicadd-mcp --socket /path/kicadd.sock --stdio
 *   kicadd-mcp --socket /path/kicadd.sock --http 127.0.0.1:8765 [--token T] [--allow-origin O]
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ipc_client.h"
#include "mcp_server.h"
#include "tools.h"
#include "transports.h"
#include "web.h"

#include <boost/asio/io_context.hpp>

using namespace kicadd::mcp;

namespace
{

void usage()
{
    std::fprintf( stderr,
                  "usage: kicadd-mcp --socket PATH (--stdio | --http [HOST:]PORT)\n"
                  "                  [--path /mcp] [--token TOKEN] [--allow-origin ORIGIN]...\n"
                  "                  [--timeout-ms MS]\n"
                  "  --socket   KiCadD API socket (plain path or ipc:// URL); env KICADD_SOCK\n"
                  "  --stdio    MCP over stdin/stdout (spawned by the client, e.g. via ssh)\n"
                  "  --http     MCP Streamable HTTP; binds 127.0.0.1 unless HOST is given\n"
                  "  --token    require Authorization: Bearer TOKEN; env KICADD_MCP_TOKEN\n"
                  "  --no-web   don't serve the 2D view (/ and /ws) in --http mode\n"
                  "  --web-root DIR  serve DIR/index.html instead of the embedded page\n" );
}

} // namespace


int main( int argc, char** argv )
{
    std::string  socket = std::getenv( "KICADD_SOCK" ) ? std::getenv( "KICADD_SOCK" ) : "";
    bool         stdio = false, httpMode = false;
    int          timeoutMs = 120000;
    HTTP_OPTIONS http;

    if( const char* t = std::getenv( "KICADD_MCP_TOKEN" ) )
        http.token = t;

    for( int i = 1; i < argc; ++i )
    {
        std::string a = argv[i];
        auto        next = [&]() -> std::string
        {
            if( i + 1 >= argc )
            {
                usage();
                std::exit( 2 );
            }

            return argv[++i];
        };

        if( a == "--socket" )
            socket = next();
        else if( a == "--stdio" )
            stdio = true;
        else if( a == "--http" )
        {
            httpMode = true;
            std::string hp = next();

            if( size_t colon = hp.rfind( ':' ); colon != std::string::npos )
            {
                http.host = hp.substr( 0, colon );
                hp = hp.substr( colon + 1 );
            }

            http.port = static_cast<unsigned short>( std::stoi( hp ) );
        }
        else if( a == "--path" )
            http.path = next();
        else if( a == "--token" )
            http.token = next();
        else if( a == "--allow-origin" )
            http.allowedOrigins.push_back( next() );
        else if( a == "--no-web" )
            http.web = false;
        else if( a == "--web-root" )
            http.webRoot = next();
        else if( a == "--timeout-ms" )
            timeoutMs = std::stoi( next() );
        else if( a == "-h" || a == "--help" )
        {
            usage();
            return 0;
        }
        else
        {
            std::fprintf( stderr, "kicadd-mcp: unknown argument %s\n", a.c_str() );
            usage();
            return 2;
        }
    }

    if( socket.empty() || stdio == httpMode )
    {
        usage();
        return 2;
    }

    if( socket.rfind( "ipc://", 0 ) != 0 && socket.rfind( "tcp://", 0 ) != 0 )
        socket = "ipc://" + socket;

    if( httpMode && http.host != "127.0.0.1" && http.host != "localhost" && http.host != "::1"
        && http.token.empty() )
    {
        std::fprintf( stderr, "kicadd-mcp: refusing to listen on %s without --token\n",
                      http.host.c_str() );
        return 2;
    }

    IPC_CLIENT  ipc( socket, timeoutMs );
    std::string err;

    if( !ipc.Connect( err ) )
    {
        std::fprintf( stderr, "kicadd-mcp: %s\n", err.c_str() );
        return 1;
    }

    std::vector<TOOL> tools = MakeTools( ipc );

    if( stdio )
    {
        MCP_SERVER server( std::move( tools ) );
        return RunStdio( server );
    }

    boost::asio::io_context  ioc;
    std::unique_ptr<WEB_HUB> hub;

    if( http.web )
    {
        hub = std::make_unique<WEB_HUB>( ipc, ioc );

        // Push an agent's edits to the browsers right away instead of at the next poll.
        for( TOOL& t : tools )
        {
            t.run = [run = std::move( t.run ), h = hub.get()]( const nlohmann::json& aArgs )
            {
                TOOL_RESULT r = run( aArgs );
                h->Kick();
                return r;
            };
        }
    }

    MCP_SERVER server( std::move( tools ) );
    return RunHttp( server, http, hub.get(), ioc );
}
