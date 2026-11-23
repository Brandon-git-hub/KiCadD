/*
 * KiCadD roadmap phase 4 item 1: the Co-pilot panel's actions in the PCB and Schematic Editors.
 *
 * View > Panels > Co-pilot (show / hide the docked panel) and View > Pause Agent Edits
 * (Ctrl+Alt+P; the same switch as the panel's check box). No editor types: compiled into each
 * kiface (the actions are named per editor: one process can hold both editors).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <functional>

#include <tool/tool_interactive.h>

class wxAuiManager;
class wxWindow;

class KICADD_COPILOT_TOOL : public TOOL_INTERACTIVE
{
public:
    KICADD_COPILOT_TOOL();

    bool Init() override;
    void Reset( RESET_REASON aReason ) override {}

    int TogglePanel( const TOOL_EVENT& aEvent );
    int TogglePause( const TOOL_EVENT& aEvent );

    static TOOL_ACTION showCopilot;
    static TOOL_ACTION pauseAgent;

    /// Dock the panel in aFrame's AUI manager on the right, at aLayer / aPosition (shown;
    /// KICADD_COPILOT=0 hides it at start).
    static void AddPane( wxWindow* aFrame, wxAuiManager& aAui, int aLayer, int aPosition,
                         std::function<void()> aFocusCanvas );

private:
    void setTransitions() override;
};
