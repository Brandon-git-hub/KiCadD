/*
 * KiCadD roadmap phase 4 item 1: the Co-pilot panel, docked in the PCB Editor.
 *
 * Shows what the agent does in this KiCad (kicadd::COPILOT): a status line (requests, how long
 * since the last one, paused or not), a log of its edits and refused requests, newest first,
 * and the "Pause agent edits" switch. Plain wx, no editor types: any frame can dock it.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <functional>

#include <wx/panel.h>
#include <wx/timer.h>

class wxCheckBox;
class wxListCtrl;
class wxStaticText;
class wxButton;

class KICADD_COPILOT_PANEL : public wxPanel
{
public:
    /// aFocusCanvas gives the keyboard back to the editor's canvas: the panel keeps no focus,
    /// so the user's hotkeys (Ctrl+Z ...) still reach the editor after a click here
    KICADD_COPILOT_PANEL( wxWindow* aParent, std::function<void()> aFocusCanvas );
    ~KICADD_COPILOT_PANEL() override;

    static wxString PaneName() { return wxS( "KicaddCopilot" ); }

private:
    void rebuild();         ///< the list from the model (after a change)
    void updateStatus();    ///< the status line (also every second: "last request 3 s ago")

    wxCheckBox*   m_pause = nullptr;
    wxStaticText* m_status = nullptr;
    wxListCtrl*   m_list = nullptr;
    wxButton*     m_clear = nullptr;
    wxTimer       m_timer;
    int           m_listenId = 0;
    long long     m_added = 0;       ///< the model's EventsAdded() when the list was last filled
    int           m_clears = 0;
    bool          m_dirty = false;
    std::function<void()> m_focusCanvas;
};
