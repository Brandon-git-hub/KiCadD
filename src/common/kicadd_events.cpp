/*
 * KiCadD: change notifications pushed to kicadd-mcp. See kicadd_events.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "common/kicadd_events.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

#include <wx/app.h>

namespace kicadd
{

EVENTS& EVENTS::Get()
{
    static EVENTS s_events;
    return s_events;
}


void EVENTS::Open( const std::string& aApiUrl )
{
    Close();

    auto* sock = new nng_socket;

    if( nng_pub0_open( sock ) != 0 )
    {
        delete sock;
        return;
    }

    // a leftover socket file of a killed KiCad is removed by NNG's ipc listener when nobody
    // answers on it
    if( nng_listen( *sock, UrlFor( aApiUrl ).c_str(), nullptr, 0 ) != 0 )
    {
        nng_close( *sock );
        delete sock;
        return;
    }

    m_socket = sock;
    m_open = true;
}


void EVENTS::Close()
{
    if( m_socket )
    {
        nng_close( *static_cast<nng_socket*>( m_socket ) );
        delete static_cast<nng_socket*>( m_socket );
    }

    m_socket = nullptr;
    m_open = false;
    m_pending.clear();
}


void EVENTS::Publish( const std::string& aTopic )
{
    if( !m_open )
        return;

    m_pending.insert( aTopic );

    if( m_scheduled )
        return;

    if( wxTheApp )
    {
        m_scheduled = true;
        wxTheApp->CallAfter( [this]() { flush(); } );
    }
    else
    {
        flush();
    }
}


void EVENTS::flush()
{
    m_scheduled = false;

    if( !m_socket )
    {
        m_pending.clear();
        return;
    }

    nng_socket sock = *static_cast<nng_socket*>( m_socket );

    for( const std::string& topic : m_pending )
        nng_send( sock, const_cast<char*>( topic.data() ), topic.size(), NNG_FLAG_NONBLOCK );

    m_pending.clear();
}

} // namespace kicadd
