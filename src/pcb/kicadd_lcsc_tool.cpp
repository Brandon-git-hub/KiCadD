/*
 * KiCadD M4d: "Add LCSC Part..." in the PCB Editor. See kicadd_lcsc_tool.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_lcsc_tool.h"
#include "lcsc/kicadd_lcsc.h"

#include <bitmaps.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <confirm.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <project.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <view/view_controls.h>

#include <wx/busyinfo.h>
#include <wx/filename.h>
#include <wx/textdlg.h>
#include <wx/utils.h>


TOOL_ACTION KICADD_LCSC_TOOL::placeLcscPart( TOOL_ACTION_ARGS()
        .Name( "kicadd.LcscTool.placeLcscPart" )
        .Scope( AS_GLOBAL )
        .FriendlyName( "Add LCSC Part..." )
        .Tooltip( "Fetch a part by its LCSC number (e.g. C8734) from the JLCEDA/EasyEDA "
                     "Official Library into the project's LCSC libraries and place its footprint" )
        .Icon( BITMAPS::module ) );


namespace
{

/// Next free "<prefix><n>" designator on the board.
wxString nextReference( BOARD* aBoard, const wxString& aPrefix )
{
    long highest = 0;

    for( const FOOTPRINT* fp : aBoard->Footprints() )
    {
        long n = 0;
        const wxString& ref = fp->GetReference();

        if( ref.StartsWith( aPrefix ) && ref.Mid( aPrefix.length() ).ToLong( &n ) && n > highest )
            highest = n;
    }

    return aPrefix + wxString::Format( wxS( "%ld" ), highest + 1 );
}

} // namespace


KICADD_LCSC_TOOL::KICADD_LCSC_TOOL() :
        PCB_TOOL_BASE( "kicadd.LcscTool" )
{
}


int KICADD_LCSC_TOOL::PlaceLcscPart( const TOOL_EVENT& aEvent )
{
    PCB_EDIT_FRAME* editFrame = frame<PCB_EDIT_FRAME>();
    BOARD*          brd = board();

    static wxString last;
    wxTextEntryDialog dlg( editFrame, _( "LCSC part number (e.g. C8734):" ), _( "Add LCSC Part" ), last );

    if( dlg.ShowModal() != wxID_OK || dlg.GetValue().Trim().Trim( false ).IsEmpty() )
        return 0;

    last = dlg.GetValue().Trim().Trim( false );

    wxString dir;

    if( !brd->GetFileName().IsEmpty() )
        dir = wxFileName( brd->GetFileName() ).GetPath();
    else if( !editFrame->Prj().GetProjectPath().IsEmpty() )
        dir = editFrame->Prj().GetProjectPath();

    kicadd::lcsc::FETCHED got;
    std::string           err;
    bool                  ok;

    {
        wxBusyCursor busy;
        ok = kicadd::lcsc::FetchAndSave( dir, &editFrame->Kiway(), last.ToStdString( wxConvUTF8 ), false,
                                         got, err );
    }

    if( !ok )
    {
        DisplayErrorMessage( editFrame, _( "Could not get the LCSC part." ), wxString::FromUTF8( err ) );
        return 0;
    }

    // What the part is, where it went, and anything that didn't work out
    wxString status = wxString::FromUTF8( kicadd::lcsc::Summary( got.part ) );

    if( !got.note.empty() )
        status << wxS( "  [" ) << wxString::FromUTF8( got.note ) << wxS( "]" );

    // the status bar's first field is too narrow for this; the infobar shows it whole
    editFrame->ShowInfoBarMsg( status, true );

    // The same hand-over as the footprint browser's "Insert Footprint": an inserted footprint
    // that the placement tool then moves with the cursor (Esc takes the insertion back).
    FOOTPRINT* fp = static_cast<FOOTPRINT*>( got.part.footprint->Duplicate( IGNORE_PARENT_GROUP ) );
    fp->SetFPID( got.part.footprint->GetFPID() );
    fp->SetParent( brd );

    BOARD_DESIGN_SETTINGS& bds = brd->GetDesignSettings();
    fp->ApplyDefaultSettings( *brd, bds.m_StyleFPFields, bds.m_StyleFPText, bds.m_StyleFPShapes,
                              bds.m_StyleFPDimensions, bds.m_StyleFPBarcodes );

    wxString prefix = wxString::FromUTF8( got.symbolSaved ? got.symbol.reference : std::string() );

    if( prefix.IsEmpty() )
        prefix = wxS( "U" );

    fp->SetReference( nextReference( brd, prefix ) );

    for( PAD* pad : fp->Pads() )
        pad->SetNetCode( 0 );

    KIGFX::VIEW_CONTROLS* controls = getViewControls();
    VECTOR2D              cursorPos = controls->GetCursorPosition();
    BOARD_COMMIT          commit( editFrame );

    commit.Add( fp );
    controls->SetCrossHairCursorPosition( VECTOR2D( 0, 0 ), false );
    editFrame->PlaceFootprint( fp );
    fp->SetPosition( VECTOR2I( 0, 0 ) );
    controls->SetCrossHairCursorPosition( cursorPos, false );
    commit.Push( wxString::Format( _( "Add LCSC part %s" ), wxString::FromUTF8( got.part.lcsc ) ) );

    m_toolMgr->RunAction( PCB_ACTIONS::placeFootprint, fp );
    return 0;
}


void KICADD_LCSC_TOOL::setTransitions()
{
    Go( &KICADD_LCSC_TOOL::PlaceLcscPart, placeLcscPart.MakeEvent() );
}
