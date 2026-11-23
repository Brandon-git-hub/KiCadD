/*
 * KiCadD: the desktop co-pilot's shared state. See kicadd_copilot.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "common/kicadd_copilot.h"
#include "common/kicadd_events.h"

namespace kicadd
{

COPILOT& COPILOT::Get()
{
    static COPILOT s_copilot;
    return s_copilot;
}


std::string COPILOT::PausedMessage()
{
    return "paused: the user paused the agent's edits (Co-pilot panel in the editor, or the web view); nothing was "
           "changed. Reads still work; ask the user to resume";
}


void COPILOT::SetPaused( bool aPaused, const wxString& aWho )
{
    if( aPaused == m_paused )
        return;

    m_paused = aPaused;
    add( COPILOT_EVENT::NOTE, aWho + ( aPaused ? wxS( " paused the agent's edits" ) : wxS( " resumed the agent's edits" ) ) );
}


void COPILOT::Edit( const wxString& aWhat )
{
    wxString what = aWhat;
    what.StartsWith( wxS( "Agent: " ), &what );
    m_edits++;
    add( COPILOT_EVENT::EDIT, what );
}


void COPILOT::Request( const std::string& aType, int aStatus, const std::string& aError, const std::string& aClient )
{
    // a viewer's requests (kicadd-mcp's web view; its render feed; this state itself): not the agent's
    if( aClient == "kicadd-web" || aType.starts_with( "kicadd.render." ) || aType.ends_with( "Copilot" )
        || aType.ends_with( "CopilotPaused" ) )
        return;

    std::string type = aType.substr( aType.rfind( '.' ) == std::string::npos ? 0 : aType.rfind( '.' ) + 1 );
    m_requests++;
    m_last = wxDateTime::Now();
    m_lastType = wxString::FromUTF8( type );

    // 1 = AS_OK; 5 = AS_UNHANDLED (another editor's request, e.g. a schematic one in the PCB
    // Editor alone): nothing for the user. Counted only: the panel's status line follows on its
    // timer (a notification per read would wake every viewer for nothing)
    if( aStatus == 1 || aStatus == 5 )
        return;

    std::string first = aError.substr( 0, aError.find( '\n' ) );

    if( first.size() > 200 )
        first = first.substr( 0, 200 ) + "...";

    // the user's own pause: said short (the switch is right there)
    wxString text = first.starts_with( "paused:" ) ? wxString::FromUTF8( type + " (paused)" )
                                                   : wxString::FromUTF8( type + ": " + first );

    // kicadd-mcp retries a busy editor for ~10 s: one row, counted
    if( !m_events.empty() && m_events.back().kind == COPILOT_EVENT::REFUSED && m_events.back().text == text
        && ( m_last - m_events.back().when ).GetSeconds() < 15 )
    {
        m_events.back().repeat++;
        m_events.back().when = m_last;
        notify();
        return;
    }

    m_refused++;
    add( COPILOT_EVENT::REFUSED, text );
}


void COPILOT::Clear()
{
    m_events.clear();
    m_clears++;
    notify();
}


void COPILOT::add( COPILOT_EVENT::KIND aKind, const wxString& aText )
{
    if( m_events.size() >= MAX_EVENTS )
        m_events.erase( m_events.begin(), m_events.begin() + MAX_EVENTS / 10 );

    m_events.push_back( { wxDateTime::Now(), aKind, aText } );
    m_added++;
    notify();
}


void COPILOT::notify()
{
    EVENTS::Get().Publish( "copilot" );

    // a copy: a listener may unlisten (a panel closed) while being called
    std::map<int, std::function<void()>> listeners = m_listeners;

    for( auto& [id, fn] : listeners )
    {
        if( fn )
            fn();
    }
}


int COPILOT::Listen( std::function<void()> aOnChange )
{
    m_listeners[m_nextId] = std::move( aOnChange );
    return m_nextId++;
}


void COPILOT::Unlisten( int aId )
{
    m_listeners.erase( aId );
}

} // namespace kicadd
