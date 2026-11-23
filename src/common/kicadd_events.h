/*
 * KiCadD roadmap phase 4 item 2: change notifications pushed to kicadd-mcp.
 *
 * The API is request / reply only, so a viewer (kicadd-mcp's web hub) used to ask for changes
 * every 50 ms, each request a turn of KiCad's main loop. Now KiCad also publishes short topics
 * on an NNG PUB socket next to its API socket ("<api socket>.ev"), and the hub asks only when
 * told (keeping a slow poll as a fallback):
 *
 *   "board"    the board changed (its render revision moved, or another board was opened)
 *   "sch"      the schematic changed
 *   "copilot"  the Co-pilot state changed (an agent edit, a refusal, the pause switch)
 *
 * Publish() may be called many times inside one commit: the topics are collected and sent
 * once, after the current event (wxTheApp->CallAfter). Main thread only, one per process (in
 * kicommon), opened by KICAD_API_SERVER::Start.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicommon.h>

#include <set>
#include <string>

namespace kicadd
{

class KICOMMON_API EVENTS
{
public:
    static EVENTS& Get();

    /// Listen on aApiUrl + ".ev" (aApiUrl: the API server's "ipc://..." URL)
    void Open( const std::string& aApiUrl );
    void Close();
    bool IsOpen() const { return m_open; }

    /// Queue aTopic; sent once after the current event
    void Publish( const std::string& aTopic );

    static std::string UrlFor( const std::string& aApiUrl ) { return aApiUrl + ".ev"; }

private:
    EVENTS() = default;
    void flush();

    bool                  m_open = false;
    bool                  m_scheduled = false;
    std::set<std::string> m_pending;
    void*                 m_socket = nullptr;   ///< nng_socket*, kept out of the header
};

} // namespace kicadd
