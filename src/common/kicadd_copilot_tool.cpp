/*
 * KiCadD: the Co-pilot panel's actions. See kicadd_copilot_tool.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "common/kicadd_copilot_tool.h"
#include "common/kicadd_copilot.h"
#include "common/kicadd_copilot_panel.h"

#include <eda_base_frame.h>
#include <tool/action_manager.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>

#include <wx/aui/aui.h>
#include <wx/utils.h>


// one name per editor: both kifaces can be loaded in one KiCad, and action names are global there
#ifdef PCBNEW
#define COPILOT_TOOL_NAME "kicadd.PcbCopilotTool"
#else
#define COPILOT_TOOL_NAME "kicadd.SchCopilotTool"
#endif

TOOL_ACTION KICADD_COPILOT_TOOL::showCopilot( TOOL_ACTION_ARGS()
        .Name( COPILOT_TOOL_NAME ".showCopilot" )
        .Scope( AS_GLOBAL )
        .FriendlyName( "Co-pilot" )
        .Tooltip( "Show or hide the Co-pilot panel: what the agent does in this editor, and the switch "
                  "to pause its edits" )
        .ToolbarState( TOOLBAR_STATE::TOGGLE ) );

TOOL_ACTION KICADD_COPILOT_TOOL::pauseAgent( TOOL_ACTION_ARGS()
        .Name( COPILOT_TOOL_NAME ".pauseAgent" )
        .Scope( AS_GLOBAL )
        .DefaultHotkey( MD_CTRL + MD_ALT + 'P' )
        .FriendlyName( "Pause Agent Edits" )
        .Tooltip( "Refuse the agent's edits until resumed (it is told why); it can still read the design" )
        .ToolbarState( TOOLBAR_STATE::TOGGLE ) );


namespace
{

wxWindow* findPanel( wxWindow* aFrame )
{
    return aFrame ? aFrame->FindWindowByName( KICADD_COPILOT_PANEL::PaneName() ) : nullptr;
}

} // namespace


KICADD_COPILOT_TOOL::KICADD_COPILOT_TOOL() :
        TOOL_INTERACTIVE( COPILOT_TOOL_NAME )
{
}


void KICADD_COPILOT_TOOL::AddPane( wxWindow* aFrame, wxAuiManager& aAui, int aLayer, int aPosition,
                                   std::function<void()> aFocusCanvas )
{
    auto* panel = new KICADD_COPILOT_PANEL( aFrame, std::move( aFocusCanvas ) );
    panel->SetName( KICADD_COPILOT_PANEL::PaneName() );

    wxString show;
    bool     shown = !( wxGetEnv( wxS( "KICADD_COPILOT" ), &show ) && show == wxS( "0" ) );

    aAui.AddPane( panel, EDA_PANE().Name( KICADD_COPILOT_PANEL::PaneName() )
                                 .Right().Layer( aLayer ).Position( aPosition )
                                 .Caption( _( "Co-pilot" ) ).PaneBorder( false )
                                 .MinSize( panel->FromDIP( wxSize( 200, 120 ) ) )
                                 .BestSize( panel->FromDIP( wxSize( 260, 260 ) ) )
                                 .FloatingSize( panel->FromDIP( wxSize( 360, 300 ) ) )
                                 .DestroyOnClose( false )
                                 .CloseButton( true )
                                 .Show( shown ) );
}


bool KICADD_COPILOT_TOOL::Init()
{
    // the menu items' check marks
    if( ACTION_MANAGER* mgr = m_toolMgr->GetActionManager() )
    {
        wxWindow* frame = dynamic_cast<wxWindow*>( m_toolMgr->GetToolHolder() );

        mgr->SetConditions( showCopilot, ACTION_CONDITIONS().Check(
                [frame]( const SELECTION& )
                {
                    wxWindow* panel = findPanel( frame );
                    wxAuiManager* aui = panel ? wxAuiManager::GetManager( panel ) : nullptr;
                    return aui && aui->GetPane( panel ).IsShown();
                } ) );
        mgr->SetConditions( pauseAgent, ACTION_CONDITIONS().Check(
                []( const SELECTION& ) { return kicadd::COPILOT::Get().Paused(); } ) );
    }

    return true;
}


int KICADD_COPILOT_TOOL::TogglePanel( const TOOL_EVENT& aEvent )
{
    wxWindow*     panel = findPanel( dynamic_cast<wxWindow*>( m_toolMgr->GetToolHolder() ) );
    wxAuiManager* aui = panel ? wxAuiManager::GetManager( panel ) : nullptr;

    if( aui )
    {
        wxAuiPaneInfo& pane = aui->GetPane( panel );
        pane.Show( !pane.IsShown() );
        aui->Update();
    }

    return 0;
}


int KICADD_COPILOT_TOOL::TogglePause( const TOOL_EVENT& aEvent )
{
    kicadd::COPILOT& c = kicadd::COPILOT::Get();
    c.SetPaused( !c.Paused() );

    if( auto* frame = dynamic_cast<EDA_BASE_FRAME*>( m_toolMgr->GetToolHolder() ) )
        frame->SetStatusText( c.Paused() ? _( "Agent edits paused (Ctrl+Alt+P resumes)" ) : _( "Agent edits resumed" ) );

    return 0;
}


void KICADD_COPILOT_TOOL::setTransitions()
{
    Go( &KICADD_COPILOT_TOOL::TogglePanel, showCopilot.MakeEvent() );
    Go( &KICADD_COPILOT_TOOL::TogglePause, pauseAgent.MakeEvent() );
}
