/*
 * KiCadD MCP - synchronous IPC client for the KiCad API server.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "ipc_client.h"

#include <algorithm>

#include <chrono>
#include <thread>

#include <nng/protocol/reqrep0/req.h>

#include <api/common/envelope.pb.h>

using kiapi::common::ApiRequest;
using kiapi::common::ApiResponse;
using kiapi::common::ApiStatusCode;

namespace kicadd::mcp
{

IPC_CLIENT::IPC_CLIENT( std::string aUrl, int aTimeoutMs ) :
        m_url( std::move( aUrl ) ),
        m_timeoutMs( aTimeoutMs )
{
}


IPC_CLIENT::~IPC_CLIENT()
{
    if( m_open )
        nng_close( m_socket );
}


bool IPC_CLIENT::Connect( std::string& aError )
{
    std::lock_guard<std::mutex> lock( m_mutex );

    if( m_open )
        return true;

    if( int rv = nng_req0_open( &m_socket ); rv != 0 )
    {
        aError = std::string( "nng_req0_open: " ) + nng_strerror( rv );
        return false;
    }

    m_open = true;
    nng_socket_set_ms( m_socket, NNG_OPT_RECVTIMEO, m_timeoutMs );
    // A send only waits for a connection (the request itself is tiny): with no KiCadD up
    // (desktop editor not open yet), fail in seconds instead of the long reply timeout.
    nng_socket_set_ms( m_socket, NNG_OPT_SENDTIMEO, std::min( m_timeoutMs, 5000 ) );

    // Render snapshots of large boards are several MB; NNG's default cap is 1 MB.
    nng_socket_set_size( m_socket, NNG_OPT_RECVMAXSZ, 0 );

    // Non-blocking dial: the daemon may start after us; NNG keeps retrying in the background.
    if( int rv = nng_dial( m_socket, m_url.c_str(), nullptr, NNG_FLAG_NONBLOCK ); rv != 0 )
    {
        aError = "nng_dial " + m_url + ": " + nng_strerror( rv );
        return false;
    }

    return true;
}


bool IPC_CLIENT::Call( const google::protobuf::Message& aRequest, google::protobuf::Message& aResponse,
                       std::string& aError )
{
    ApiRequest envelope;
    envelope.mutable_header()->set_client_name( "kicadd-mcp" );
    envelope.mutable_message()->PackFrom( aRequest );
    const std::string wire = envelope.SerializeAsString();

    std::lock_guard<std::mutex> lock( m_mutex );

    if( !m_open )
    {
        aError = "IPC client not connected";
        return false;
    }

    constexpr int MAX_ATTEMPTS = 50;   // ~10 s of NOT_READY/BUSY (board still loading, task)

    std::string lastBusy;

    for( int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt )
    {
        if( int rv = nng_send( m_socket, const_cast<char*>( wire.data() ), wire.size(), 0 ); rv != 0 )
        {
            aError = std::string( "send to KiCad failed: " ) + nng_strerror( rv )
                     + " (is KiCadD running? desktop: pcbnew with a board open; server: kicad-cli api-server; socket " + m_url + ")";
            return false;
        }

        char*  buf = nullptr;
        size_t size = 0;

        if( int rv = nng_recv( m_socket, &buf, &size, NNG_FLAG_ALLOC ); rv != 0 )
        {
            aError = std::string( "receive from KiCad failed: " ) + nng_strerror( rv );
            return false;
        }

        ApiResponse reply;
        bool        parsed = reply.ParseFromArray( buf, static_cast<int>( size ) );
        nng_free( buf, size );

        if( !parsed )
        {
            aError = "could not parse KiCad API response";
            return false;
        }

        ApiStatusCode status = reply.status().status();

        if( status == ApiStatusCode::AS_NOT_READY || status == ApiStatusCode::AS_BUSY )
        {
            lastBusy = reply.status().error_message();
            std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
            continue;
        }

        if( status != ApiStatusCode::AS_OK )
        {
            aError = kiapi::common::ApiStatusCode_Name( status );

            if( !reply.status().error_message().empty() )
                aError += ": " + reply.status().error_message();

            if( status == ApiStatusCode::AS_UNHANDLED
                && aRequest.GetDescriptor()->full_name().starts_with( "kicadd." ) )
            {
                aError += " (this KiCad has no KiCadD handler: a stock KiCad, or a KiCadD pcbnew "
                          "older than the M2 desktop co-pilot, e.g. v0.1.0-alpha.1 with the API "
                          "enabled by hand; use a newer KiCadD pcbnew or kicad-cli api-server)";
            }

            return false;
        }

        if( !reply.message().UnpackTo( &aResponse ) )
        {
            aError = "unexpected response type " + reply.message().type_url();
            return false;
        }

        return true;
    }

    aError = "KiCad stayed busy / not ready for 10 s"
             + ( lastBusy.empty() ? std::string() : " (" + lastBusy + ")" )
             + "; try again once the user has finished";
    return false;
}

} // namespace kicadd::mcp
