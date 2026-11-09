/*
 * KiCadD M4d: "Add LCSC Symbol..." in the Schematic Editor. See kicadd_lcsc_sch_tool.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_lcsc_sch_tool.h"

#include <bitmaps.h>
#include <confirm.h>
#include <kiface_base.h>
#include <kiface_ids.h>
#include <kiface_services.h>
#include <kiway.h>
#include <ki_exception.h>
#include <project.h>
#include <remote_symbol_import_utils.h>
#include <sch_edit_frame.h>

#include <json_common.h>

#include <wx/textdlg.h>
#include <wx/utils.h>

using json = nlohmann::json;


TOOL_ACTION KICADD_LCSC_SCH_TOOL::placeLcscSymbol( TOOL_ACTION_ARGS()
        .Name( "kicadd.LcscSchTool.placeLcscSymbol" )
        .Scope( AS_GLOBAL )
        .FriendlyName( "Add LCSC Symbol..." )
        .Tooltip( "Fetch a part by its LCSC number (e.g. C8734) from the JLCEDA/EasyEDA Official "
                  "Library into the project's LCSC libraries (symbol and footprint) and place its symbol" )
        .Icon( BITMAPS::add_component ) );


int KICADD_LCSC_SCH_TOOL::PlaceLcscSymbol( const TOOL_EVENT& aEvent )
{
    static wxString last;
    wxTextEntryDialog dlg( m_frame, _( "LCSC part number (e.g. C8734):" ), _( "Add LCSC Symbol" ), last );

    if( dlg.ShowModal() != wxID_OK || dlg.GetValue().Trim().Trim( false ).IsEmpty() )
        return 0;

    last = dlg.GetValue().Trim().Trim( false );

    if( m_frame->Prj().GetProjectPath().IsEmpty() )
    {
        DisplayErrorMessage( m_frame, _( "Save the schematic in a project first: LCSC parts go into "
                                         "the project's own libraries." ) );
        return 0;
    }

    KIFACE_KICADD_LCSC_FETCH_SERVICE::Type fetch = nullptr;

    try
    {
        // the fetch and footprint conversion are board code: ask the PCB kiface
        fetch = GetKifaceService<KIFACE_KICADD_LCSC_FETCH_SERVICE>( m_frame->Kiway().KiFACE( KIWAY::FACE_PCB ) );
    }
    catch( const IO_ERROR& e )
    {
        DisplayErrorMessage( m_frame, _( "The PCB module could not be loaded." ), e.What() );
        return 0;
    }

    if( !fetch )
    {
        DisplayErrorMessage( m_frame, _( "This build has no LCSC support in the PCB module." ) );
        return 0;
    }

    json req = { { "lcsc", last.ToStdString( wxConvUTF8 ) },
                 { "project_dir", m_frame->Prj().GetProjectPath().ToStdString( wxConvUTF8 ) } };
    json ans;

    {
        wxBusyCursor busy;
        ans = json::parse( fetch( wxString::FromUTF8( req.dump() ), &m_frame->Kiway() ).ToStdString( wxConvUTF8 ),
                           nullptr, false );
    }

    if( ans.is_discarded() || !ans.value( "ok", false ) )
    {
        DisplayErrorMessage( m_frame, _( "Could not get the LCSC part." ),
                             wxString::FromUTF8( ans.is_discarded() ? std::string( "no answer" )
                                                                    : ans.value( "error", std::string() ) ) );
        return 0;
    }

    std::string symbol = ans.value( "symbol", std::string() );
    wxString    status = wxString::FromUTF8( ans.value( "summary", std::string() ) );
    wxString    note = wxString::FromUTF8( ans.value( "note", std::string() ) );

    if( symbol.empty() )
    {
        DisplayErrorMessage( m_frame, _( "The LCSC part has no symbol that could be saved." ), note );
        return 0;
    }

    if( !note.IsEmpty() )
        status << wxS( "  [" ) << note << wxS( "]" );

    m_frame->ShowInfoBarMsg( status, true );

    wxString name = wxString::FromUTF8( symbol.substr( symbol.find( ':' ) + 1 ) );
    wxString err;

    if( !PlaceRemoteDownloadedSymbol( m_frame, wxS( "LCSC" ), name, err ) )
        DisplayErrorMessage( m_frame, _( "The LCSC symbol could not be placed." ), err );

    return 0;
}


void KICADD_LCSC_SCH_TOOL::setTransitions()
{
    Go( &KICADD_LCSC_SCH_TOOL::PlaceLcscSymbol, placeLcscSymbol.MakeEvent() );
}
