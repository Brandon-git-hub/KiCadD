/*
 * KiCadD roadmap phase 4 item 1: the desktop co-pilot's shared state.
 *
 * One per process (in kicommon, so the PCB and Schematic Editors of one KiCad share it): what
 * the agent did (its edits, as the handlers announce them, and its refused or failed requests,
 * as the API server answered them), how many requests it sent and when the last one came, and
 * the user's "pause agent edits" switch, which the KiCadD handlers' busy() honour.
 *
 * Main thread only: API requests are handled on the UI thread (KICAD_API_SERVER queues them as
 * events), and the panels live there.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicommon.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <wx/datetime.h>
#include <wx/string.h>

namespace kicadd
{

struct COPILOT_EVENT
{
    enum KIND
    {
        EDIT,       ///< an agent edit that went through (the handler's announce text)
        REFUSED,    ///< a request answered with an error (busy, paused, bad arguments ...)
        NOTE        ///< the user's own actions on the switch (paused, resumed)
    };

    wxDateTime when;
    KIND       kind = EDIT;
    wxString   text;
    int        repeat = 1;    ///< the same refusal again within seconds (the client retries a busy editor)
};


class KICOMMON_API COPILOT
{
public:
    static COPILOT& Get();

    bool Paused() const { return m_paused; }
    /// aWho for the log ("you": the switch in this KiCad; "a web viewer")
    void SetPaused( bool aPaused, const wxString& aWho = wxS( "you" ) );

    /// An edit went through: the handlers' announce text ("Agent: moved R1" -> "moved R1")
    void Edit( const wxString& aWhat );

    /// Every API request the server answered: aType the message's full name
    /// ("kicadd.board.MoveFootprints"), aStatus its ApiStatusCode, aError the message if not OK,
    /// aClient the header's client name ("kicadd-web": a viewer, left out)
    void Request( const std::string& aType, int aStatus, const std::string& aError, const std::string& aClient = {} );

    const std::vector<COPILOT_EVENT>& Events() const { return m_events; }
    void                              Clear();
    long long                         EventsAdded() const { return m_added; }   ///< ever, Clear included
    int                               Clears() const { return m_clears; }

    long long         Requests() const { return m_requests; }
    long long         EditCount() const { return m_edits; }
    long long         RefusedCount() const { return m_refused; }
    const wxDateTime& LastRequest() const { return m_last; }
    const wxString&   LastType() const { return m_lastType; }   ///< short: "MoveFootprints"

    /// Called after every change (a new event, the switch, Clear); returns an id for Unlisten
    int  Listen( std::function<void()> aOnChange );
    void Unlisten( int aId );

    /// The refusal text busy() returns while paused
    static std::string PausedMessage();

    /// Start the API server for the agent if it is not running: the PCB and Schematic Editors call
    /// it when they open, whichever comes first. The transport, so started even when the user
    /// preference is off; KICADD_NO_API=1 opts out, KICADD_API_SOCKET (launch_kicad) sets the socket.
    static void StartApiServer();

    static constexpr size_t MAX_EVENTS = 500;

private:
    COPILOT() = default;
    void add( COPILOT_EVENT::KIND aKind, const wxString& aText );
    void notify();

    bool                                 m_paused = false;
    std::vector<COPILOT_EVENT>           m_events;
    long long                            m_requests = 0, m_edits = 0, m_refused = 0, m_added = 0;
    int                                  m_clears = 0;
    wxDateTime                           m_last;
    wxString                             m_lastType;
    std::map<int, std::function<void()>> m_listeners;
    int                                  m_nextId = 1;
};

} // namespace kicadd
