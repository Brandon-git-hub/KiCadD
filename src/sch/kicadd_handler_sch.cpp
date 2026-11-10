/*
 * KiCadD M5a: schematic generation for agents. See kicadd_handler_sch.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_handler_sch.h"

#include <api/sch_context.h>
#include <base_units.h>
#include <kiface_ids.h>
#include <kiface_services.h>
#include <kiway.h>
#include <ki_exception.h>
#include <lib_id.h>
#include <lib_symbol.h>
#include <libraries/symbol_library_adapter.h>
#include <project.h>
#include <project_sch.h>
#include <pgm_base.h>
#include <libraries/library_manager.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <tool/tool_manager.h>
#include <sch_field.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_pin.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <sch_sheet_path.h>
#include <wildcards_and_files_ext.h>
#include <wx/filename.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <connectivity/conn_facade.h>

#include <json_common.h>

#include <algorithm>
#include <map>
#include <set>

using namespace kicadd::schematic;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;
using json = nlohmann::json;


struct KICADD_HANDLER_SCH::PIN_AT
{
    SCH_SYMBOL*    symbol = nullptr;
    SCH_PIN*       pin = nullptr;
    SCH_SHEET_PIN* sheetPin = nullptr;   ///< a sheet's pin instead of a symbol's
    SCH_ITEM*      item = nullptr;       ///< what carries the net: pin or sheet pin
    SCH_SHEET_PATH sheet;
    SCH_SCREEN*    screen = nullptr;
    VECTOR2I       pos;
    VECTOR2I       out;      ///< unit step away from the body, in IU
    const char*    side = "";
};


namespace
{

const int GRID = schIUScale.MilsToIU( 50 );     // 1.27 mm connection grid
const int STUB = schIUScale.MilsToIU( 100 );    // 2.54 mm wire stub

ApiResponseStatus makeError( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}

double mm( int aIU )
{
    return schIUScale.IUTomm( aIU );
}

int iu( double aMm )
{
    return schIUScale.mmToIU( aMm );
}

int snap( int aIU )
{
    return KiROUND( (double) aIU / GRID ) * GRID;
}

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

wxString wx( const std::string& s )
{
    return wxString::FromUTF8( s );
}

std::string pinType( const SCH_PIN* aPin )
{
    std::string t = str( aPin->GetCanonicalElectricalTypeName() );
    std::transform( t.begin(), t.end(), t.begin(), ::tolower );
    return t;
}

/// Labels (any kind) sitting exactly at @a aPos on @a aScreen.
std::vector<SCH_LABEL_BASE*> labelsAt( SCH_SCREEN* aScreen, const VECTOR2I& aPos )
{
    std::vector<SCH_LABEL_BASE*> out;

    for( SCH_ITEM* item : aScreen->Items().Overlapping( aPos ) )
    {
        if( auto* label = dynamic_cast<SCH_LABEL_BASE*>( item ); label && label->GetPosition() == aPos )
            out.push_back( label );
    }

    return out;
}

std::vector<SCH_LINE*> wiresEndingAt( SCH_SCREEN* aScreen, const VECTOR2I& aPos )
{
    std::vector<SCH_LINE*> out;

    for( SCH_ITEM* item : aScreen->Items().Overlapping( SCH_LINE_T, aPos ) )
    {
        auto* line = static_cast<SCH_LINE*>( item );

        if( line->IsWire() && ( line->GetStartPoint() == aPos || line->GetEndPoint() == aPos ) )
            out.push_back( line );
    }

    return out;
}

SCH_NO_CONNECT* noConnectAt( SCH_SCREEN* aScreen, const VECTOR2I& aPos )
{
    for( SCH_ITEM* item : aScreen->Items().Overlapping( SCH_NO_CONNECT_T, aPos ) )
    {
        if( item->GetPosition() == aPos )
            return static_cast<SCH_NO_CONNECT*>( item );
    }

    return nullptr;
}

/// Something at @a aPos a new wire end or label would connect to.
bool occupied( SCH_SCREEN* aScreen, const VECTOR2I& aPos )
{
    if( !labelsAt( aScreen, aPos ).empty() || !wiresEndingAt( aScreen, aPos ).empty() || noConnectAt( aScreen, aPos ) )
        return true;

    for( SCH_ITEM* item : aScreen->Items().Overlapping( SCH_SYMBOL_T, aPos ) )
    {
        for( SCH_PIN* pin : static_cast<SCH_SYMBOL*>( item )->GetPins( nullptr ) )
        {
            if( pin->GetPosition() == aPos )
                return true;
        }
    }

    return false;
}

/// "input" -> the label shape; false when unknown.
bool shapeOf( const std::string& aName, LABEL_FLAG_SHAPE& aOut )
{
    static const std::map<std::string, LABEL_FLAG_SHAPE> shapes = {
        { "input", LABEL_FLAG_SHAPE::L_INPUT },         { "output", LABEL_FLAG_SHAPE::L_OUTPUT },
        { "bidirectional", LABEL_FLAG_SHAPE::L_BIDI },  { "bidi", LABEL_FLAG_SHAPE::L_BIDI },
        { "tri_state", LABEL_FLAG_SHAPE::L_TRISTATE },  { "passive", LABEL_FLAG_SHAPE::L_UNSPECIFIED } };

    auto it = shapes.find( aName );

    if( it == shapes.end() )
        return false;

    aOut = it->second;
    return true;
}

} // namespace


std::string KICADD_HANDLER_SCH::shapeName( LABEL_FLAG_SHAPE aShape )
{
    switch( aShape )
    {
    case LABEL_FLAG_SHAPE::L_INPUT:    return "input";
    case LABEL_FLAG_SHAPE::L_OUTPUT:   return "output";
    case LABEL_FLAG_SHAPE::L_BIDI:     return "bidirectional";
    case LABEL_FLAG_SHAPE::L_TRISTATE: return "tri_state";
    default:                           return "passive";
    }
}


KICADD_HANDLER_SCH::KICADD_HANDLER_SCH( std::shared_ptr<SCH_CONTEXT> aContext ) :
        API_HANDLER(),
        m_context( std::move( aContext ) )
{
    registerHandler<GetSchSummary, SchSummary>( &KICADD_HANDLER_SCH::handleGetSummary );
    registerHandler<SchAddPart, SchPartResponse>( &KICADD_HANDLER_SCH::handleAddPart );
    registerHandler<SchGetPart, SchPartResponse>( &KICADD_HANDLER_SCH::handleGetPart );
    registerHandler<SchConnect, SchConnectResponse>( &KICADD_HANDLER_SCH::handleConnect );
    registerHandler<SchDisconnect, SchConnectResponse>( &KICADD_HANDLER_SCH::handleDisconnect );
    registerHandler<GetSchNetlist, SchNetlist>( &KICADD_HANDLER_SCH::handleGetNetlist );
    registerHandler<SchDeleteParts, SchConnectResponse>( &KICADD_HANDLER_SCH::handleDeleteParts );
    registerHandler<SchSave, SchConnectResponse>( &KICADD_HANDLER_SCH::handleSave );
    registerHandler<SchAddSheet, SchSheetResponse>( &KICADD_HANDLER_SCH::handleAddSheet );
}


KICADD_HANDLER_SCH::~KICADD_HANDLER_SCH() = default;


SCHEMATIC* KICADD_HANDLER_SCH::schematic() const
{
    return m_context ? m_context->GetSchematic() : nullptr;
}


void KICADD_HANDLER_SCH::rebuildConnectivity()
{
    if( SCHEMATIC* sch = schematic() )
        sch->RebuildConnectivity();
}


SCH_EDIT_FRAME* KICADD_HANDLER_SCH::editor() const
{
    TOOL_MANAGER* mgr = m_context ? m_context->GetToolManager() : nullptr;
    return mgr ? dynamic_cast<SCH_EDIT_FRAME*>( mgr->GetToolHolder() ) : nullptr;
}


std::optional<ApiResponseStatus> KICADD_HANDLER_SCH::busy() const
{
    if( !m_context || m_context->CanAcceptApiCommands() )
        return std::nullopt;

    return makeError( ApiStatusCode::AS_BUSY,
                      "the Schematic Editor is busy: the user is in an interactive tool (drawing a "
                      "wire, moving, a dialog); the edit was not applied" );
}


void KICADD_HANDLER_SCH::announce( const wxString& aWhat ) const
{
    // The commit has updated the view; an API request (unlike a tool event) does not repaint
    // the canvas by itself.
    if( SCH_EDIT_FRAME* frame = editor() )
    {
        frame->SetStatusText( aWhat );
        frame->GetCanvas()->Refresh();
    }
}


std::vector<std::pair<SCH_SYMBOL*, SCH_SHEET_PATH>> KICADD_HANDLER_SCH::findSymbols( const std::string& aRef )
{
    std::vector<std::pair<SCH_SYMBOL*, SCH_SHEET_PATH>> out;
    wxString                                            ref = wx( aRef );

    for( const SCH_SHEET_PATH& sheet : schematic()->Hierarchy() )
    {
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->GetRef( &sheet ).CmpNoCase( ref ) == 0 )
                out.emplace_back( symbol, sheet );
        }
    }

    return out;
}


std::vector<std::pair<SCH_SHEET*, SCH_SHEET_PATH>> KICADD_HANDLER_SCH::findSheetSymbols( const std::string& aName )
{
    std::vector<std::pair<SCH_SHEET*, SCH_SHEET_PATH>> out;

    for( const SCH_SHEET_PATH& sheet : schematic()->Hierarchy() )
    {
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SHEET_T ) )
        {
            auto*          child = static_cast<SCH_SHEET*>( item );
            SCH_SHEET_PATH path = sheet;
            path.push_back( child );

            if( str( child->GetName() ) == aName || str( path.PathHumanReadable() ) == aName
                || str( path.PathHumanReadable( true, true ) ) == aName )
                out.emplace_back( child, sheet );
        }
    }

    return out;
}


std::optional<SCH_SHEET_PATH> KICADD_HANDLER_SCH::findSheet( const std::string& aNameOrPath, std::string& aError )
{
    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();

    if( aNameOrPath.empty() || aNameOrPath == "/" )
        return hierarchy.empty() ? std::nullopt : std::optional<SCH_SHEET_PATH>( hierarchy[0] );

    std::vector<SCH_SHEET_PATH> found;

    for( const SCH_SHEET_PATH& path : hierarchy )
    {
        if( path.size() < 2 )
            continue;

        if( str( path.Last()->GetName() ) == aNameOrPath || str( path.PathHumanReadable() ) == aNameOrPath
            || str( path.PathHumanReadable( true, true ) ) == aNameOrPath )
            found.push_back( path );
    }

    if( found.size() == 1 )
        return found[0];

    aError = found.empty() ? "no sheet " + aNameOrPath
                           : "sheet " + aNameOrPath + " is used more than once; give its path (\"/Parent/"
                                     + aNameOrPath + "/\")";
    return std::nullopt;
}


void KICADD_HANDLER_SCH::packSheet( SCH_SHEET* aSheet, const SCH_SHEET_PATH& aParent, SchSheet& aOut )
{
    SCH_SHEET_PATH path = aParent;
    path.push_back( aSheet );

    wxFileName file( aSheet->GetFileName() );

    aOut.set_name( str( aSheet->GetName() ) );
    aOut.set_file( str( file.GetFullPath() ) );
    aOut.set_path( str( path.PathHumanReadable() ) );
    aOut.set_parent( str( aParent.PathHumanReadable() ) );
    aOut.set_x( mm( aSheet->GetPosition().x ) );
    aOut.set_y( mm( aSheet->GetPosition().y ) );
    aOut.set_width( mm( aSheet->GetSize().x ) );
    aOut.set_height( mm( aSheet->GetSize().y ) );

    int parts = 0;

    if( SCH_SCREEN* screen = aSheet->GetScreen() )
    {
        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            if( !static_cast<SCH_SYMBOL*>( item )->IsPower() )
                parts++;
        }
    }

    aOut.set_parts( parts );

    for( SCH_SHEET_PIN* sp : aSheet->GetPins() )
    {
        SchSheetPin* p = aOut.add_pins();
        p->set_name( str( sp->GetText() ) );
        p->set_direction( shapeName( sp->GetShape() ) );
        p->set_side( sp->GetSide() == SHEET_SIDE::LEFT    ? "left"
                     : sp->GetSide() == SHEET_SIDE::RIGHT ? "right"
                     : sp->GetSide() == SHEET_SIDE::TOP   ? "up"
                                                          : "down" );
        // its net on the parent: only when something is attached there (otherwise connectivity
        // reports the net inside the sheet)
        if( !wiresEndingAt( aParent.LastScreen(), sp->GetPosition() ).empty()
            || !labelsAt( aParent.LastScreen(), sp->GetPosition() ).empty() )
            p->set_net( pinNet( sp, aParent ) );
        p->set_x( mm( sp->GetPosition().x ) );
        p->set_y( mm( sp->GetPosition().y ) );
    }
}


void KICADD_HANDLER_SCH::hierarchyChanged()
{
    schematic()->RefreshHierarchy();

    if( SCH_EDIT_FRAME* frame = editor() )
        frame->UpdateHierarchyNavigator();
}


bool KICADD_HANDLER_SCH::findPin( const std::string& aRef, const std::string& aPin, PIN_AT& aOut,
                                  std::string& aError )
{
    auto symbols = findSymbols( aRef );

    // no such part: a sheet of that name, and its pin
    if( symbols.empty() )
    {
        std::vector<std::pair<SCH_SHEET*, SCH_SHEET_PATH>> sheets = findSheetSymbols( aRef );

        if( sheets.empty() )
        {
            aError = "no part or sheet " + aRef;
            return false;
        }

        if( sheets.size() > 1 )
        {
            aError = "sheet name " + aRef + " is used more than once; give its path";
            return false;
        }

        auto& [sheet, parent] = sheets[0];

        for( SCH_SHEET_PIN* sp : sheet->GetPins() )
        {
            if( str( sp->GetText() ) != aPin )
                continue;

            aOut = PIN_AT();
            aOut.sheetPin = sp;
            aOut.item = sp;
            aOut.sheet = parent;
            aOut.screen = parent.LastScreen();
            aOut.pos = sp->GetPosition();

            switch( sp->GetSide() )
            {
            case SHEET_SIDE::LEFT:   aOut.out = { -1, 0 }; aOut.side = "left";  break;
            case SHEET_SIDE::RIGHT:  aOut.out = { 1, 0 };  aOut.side = "right"; break;
            case SHEET_SIDE::TOP:    aOut.out = { 0, -1 }; aOut.side = "up";    break;
            default:                 aOut.out = { 0, 1 };  aOut.side = "down";  break;
            }

            return true;
        }

        aError = "sheet " + aRef + " has no pin " + aPin + " (a port of the sheet: sch_connect ports)";
        return false;
    }

    std::vector<PIN_AT> byNumber, byName;

    for( auto& [symbol, sheet] : symbols )
    {
        for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
        {
            PIN_AT at;
            at.symbol = symbol;
            at.pin = pin;
            at.item = pin;
            at.sheet = sheet;
            at.screen = sheet.LastScreen();
            at.pos = pin->GetPosition();

            // The pin's orientation is the direction from its connection point to the body;
            // a wire leaves the other way.
            switch( pin->PinDrawOrient( symbol->GetTransform() ) )
            {
            case PIN_ORIENTATION::PIN_RIGHT: at.out = { -1, 0 }; at.side = "left";  break;
            case PIN_ORIENTATION::PIN_LEFT:  at.out = { 1, 0 };  at.side = "right"; break;
            case PIN_ORIENTATION::PIN_UP:    at.out = { 0, 1 };  at.side = "down";  break;
            default:                         at.out = { 0, -1 }; at.side = "up";    break;
            }

            if( str( pin->GetNumber() ) == aPin )
                byNumber.push_back( at );
            else if( str( pin->GetShownName() ) == aPin || str( pin->GetName() ) == aPin )
                byName.push_back( at );
        }
    }

    if( byNumber.size() == 1 || ( byNumber.empty() && byName.size() == 1 ) )
    {
        aOut = byNumber.empty() ? byName[0] : byNumber[0];
        return true;
    }

    if( byNumber.size() > 1 || byName.size() > 1 )
        aError = aRef + " pin " + aPin + " is ambiguous (" + std::to_string( std::max( byNumber.size(), byName.size() ) )
                 + " pins); use the pin number";
    else
        aError = aRef + " has no pin " + aPin + ( symbols.size() == 1 && symbols[0].first->IsMultiUnit()
                                                          ? " on the units placed so far"
                                                          : "" );

    return false;
}


std::string KICADD_HANDLER_SCH::pinNet( SCH_ITEM* aPin, const SCH_SHEET_PATH& aSheet )
{
    std::optional<SCH_CONNECTIVITY::ITEM_VIEW> view =
            schematic()->Connectivity().Connection( aPin->m_Uuid, aSheet.Path() );

    if( !view || !view->IsNet() || view->IsUnconnected() )
        return {};

    // a pin on its own still gets a net, named "unconnected-(U1-VOUT-Pad2)"
    std::string name = str( view->Name() );
    return name.rfind( "unconnected-", 0 ) == 0 ? std::string() : name;
}


void KICADD_HANDLER_SCH::packSymbol( SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aSheet, bool aWithPins,
                                     SchSymbol& aOut )
{
    aOut.set_ref( str( aSymbol->GetRef( &aSheet ) ) );
    aOut.set_value( str( aSymbol->GetField( FIELD_T::VALUE )->GetText() ) );
    aOut.set_lib_id( str( aSymbol->GetLibId().Format().wx_str() ) );
    aOut.set_footprint( str( aSymbol->GetField( FIELD_T::FOOTPRINT )->GetText() ) );
    aOut.set_x( mm( aSymbol->GetPosition().x ) );
    aOut.set_y( mm( aSymbol->GetPosition().y ) );

    switch( aSymbol->GetOrientationProp() )
    {
    case SYMBOL_ANGLE_90:  aOut.set_rotation( 90 );  break;
    case SYMBOL_ANGLE_180: aOut.set_rotation( 180 ); break;
    case SYMBOL_ANGLE_270: aOut.set_rotation( 270 ); break;
    default:               aOut.set_rotation( 0 );   break;
    }

    aOut.set_unit( aSymbol->GetUnit() );
    aOut.set_units( aSymbol->GetUnitCount() );
    aOut.set_sheet( str( aSheet.PathHumanReadable() ) );

    if( const SCH_FIELD* lcsc = aSymbol->GetField( wxS( "LCSC" ) ) )
        aOut.set_lcsc( str( lcsc->GetText() ) );

    if( !aWithPins )
        return;

    for( SCH_PIN* pin : aSymbol->GetPins( &aSheet ) )
    {
        PIN_AT      at;
        std::string err;
        SchPin*     p = aOut.add_pins();
        p->set_number( str( pin->GetNumber() ) );
        p->set_name( str( pin->GetShownName() ) );
        p->set_type( pinType( pin ) );
        p->set_x( mm( pin->GetPosition().x ) );
        p->set_y( mm( pin->GetPosition().y ) );
        p->set_unit( pin->GetUnit() ? pin->GetUnit() : aSymbol->GetUnit() );

        switch( pin->PinDrawOrient( aSymbol->GetTransform() ) )
        {
        case PIN_ORIENTATION::PIN_RIGHT: p->set_side( "left" );  break;
        case PIN_ORIENTATION::PIN_LEFT:  p->set_side( "right" ); break;
        case PIN_ORIENTATION::PIN_UP:    p->set_side( "down" );  break;
        default:                         p->set_side( "up" );    break;
        }

        p->set_net( pinNet( pin, aSheet ) );
    }
}


HANDLER_RESULT<SchSummary> KICADD_HANDLER_SCH::handleGetSummary( const HANDLER_CONTEXT<GetSchSummary>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    rebuildConnectivity();

    SchSummary            out;
    std::set<std::string> nets;
    int                   unconnected = 0;

    out.set_file( str( m_context->GetCurrentFileName() ) );
    out.set_sheets( (int) schematic()->Hierarchy().size() );

    for( const SCH_SHEET_PATH& sheet : schematic()->Hierarchy() )
    {
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->IsPower() )
                continue;

            packSymbol( symbol, sheet, aCtx.Request.with_pins(), *out.add_symbols() );

            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                std::string net = pinNet( pin, sheet );

                if( !net.empty() )
                    nets.insert( net );
                else if( !noConnectAt( sheet.LastScreen(), pin->GetPosition() ) )
                    unconnected++;
            }
        }
    }

    out.set_nets( (int) nets.size() );
    out.set_unconnected_pins( unconnected );

    for( const SCH_SHEET_PATH& sheet : schematic()->Hierarchy() )
    {
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SHEET_T ) )
            packSheet( static_cast<SCH_SHEET*>( item ), sheet, *out.add_sheet_list() );
    }

    return out;
}


HANDLER_RESULT<SchPartResponse> KICADD_HANDLER_SCH::handleAddPart( const HANDLER_CONTEXT<SchAddPart>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    const SchAddPart& req = aCtx.Request;
    std::string       note;
    LIB_ID            libId;

    if( !req.lcsc().empty() )
    {
        // Fetching and converting is board code: pcbnew's export, which also saves the
        // footprint, so the symbol's Footprint field resolves.
        KIFACE_KICADD_LCSC_FETCH_SERVICE::Type fetch = nullptr;

        try
        {
            if( KIWAY* kiway = m_context->GetKiway() )
                fetch = GetKifaceService<KIFACE_KICADD_LCSC_FETCH_SERVICE>( kiway->KiFACE( KIWAY::FACE_PCB ) );
        }
        catch( const IO_ERROR& e )
        {
            return tl::unexpected( makeError( ApiStatusCode::AS_UNIMPLEMENTED,
                                              "the PCB module could not be loaded: " + str( e.What() ) ) );
        }

        if( !fetch )
            return tl::unexpected( makeError( ApiStatusCode::AS_UNIMPLEMENTED, "no LCSC support in this build" ) );

        json ask = { { "lcsc", req.lcsc() }, { "project_dir", str( m_context->Prj().GetProjectPath() ) } };
        json ans = json::parse( str( fetch( wx( ask.dump() ), m_context->GetKiway() ) ), nullptr, false );

        if( ans.is_discarded() || !ans.value( "ok", false ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              ans.is_discarded() ? "no answer from the LCSC fetch"
                                                                 : ans.value( "error", std::string() ) ) );

        if( ans.value( "symbol", std::string() ).empty() )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "the LCSC part has no symbol: "
                                                                                     + ans.value( "note", std::string() ) ) );

        libId.Parse( ans.value( "symbol", std::string() ) );
        note = ans.value( "summary", std::string() );

        if( !ans.value( "note", std::string() ).empty() )
            note += " [" + ans.value( "note", std::string() ) + "]";
    }
    else if( libId.Parse( req.lib_id() ) >= 0 || !libId.IsValid() )
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "lib_id (\"Library:Symbol\", e.g. \"Device:R\") or lcsc is required" ) );
    }

    // Headless, nothing loads the project's symbol libraries in the background: load the one
    // asked for now. LCSC may have just been written by pcbnew: reload it if the part is new.
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &m_context->Prj() );
    adapter->LoadOne( libId.GetLibNickname() );
    LIB_SYMBOL* libSymbol = adapter->LoadSymbol( libId );

    if( !libSymbol && !req.lcsc().empty() )
    {
        Pgm().GetLibraryManager().ReloadLibraryEntry( LIBRARY_TABLE_TYPE::SYMBOL, libId.GetLibNickname(),
                                                      LIBRARY_TABLE_SCOPE::PROJECT );
        adapter->LoadOne( libId.GetLibNickname() );
        libSymbol = adapter->LoadSymbol( libId );
    }

    if( !libSymbol )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "symbol " + str( libId.Format().wx_str() ) + " not found" ) );

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    SCH_SHEET_PATH sheet = *hierarchy.begin();
    std::string    sheetErr;

    if( !req.sheet().empty() )
    {
        std::optional<SCH_SHEET_PATH> asked = findSheet( req.sheet(), sheetErr );

        if( !asked )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, sheetErr ) );

        sheet = *asked;
    }

    SCH_SCREEN* screen = sheet.LastScreen();

    if( screen->GetRefCount() > 1 )
        note += std::string( note.empty() ? "" : "; " ) + "the sheet's file is used more than once: "
                "the part is on every copy";
    int            unit = req.unit() > 0 ? req.unit() : 1;

    if( libSymbol->GetUnitCount() > 0 && unit > libSymbol->GetUnitCount() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "unit " + std::to_string( unit ) + " out of range ("
                                                  + std::to_string( libSymbol->GetUnitCount() ) + " units)" ) );

    auto symbol = std::make_unique<SCH_SYMBOL>( *libSymbol, libId, &sheet, unit, 0, VECTOR2I( 0, 0 ) );

    switch( ( ( req.rotation() % 360 ) + 360 ) % 360 )
    {
    case 90:  symbol->SetOrientationProp( SYMBOL_ANGLE_90 );  break;
    case 180: symbol->SetOrientationProp( SYMBOL_ANGLE_180 ); break;
    case 270: symbol->SetOrientationProp( SYMBOL_ANGLE_270 ); break;
    default: break;
    }

    if( !req.value().empty() )
        symbol->SetValueFieldText( wx( req.value() ) );

    if( !req.footprint().empty() )
        symbol->SetFootprintFieldText( wx( req.footprint() ) );

    // The functional block: a hidden field, carried to the footprint by sync_pcb, read by
    // auto_place to keep the block's parts together.
    if( !req.block().empty() )
    {
        SCH_FIELD* field = symbol->GetField( wxS( "Block" ) );

        if( !field )
        {
            field = symbol->AddField( SCH_FIELD( symbol.get(), FIELD_T::USER, wxS( "Block" ) ) );
            field->SetPosition( symbol->GetPosition() );
        }

        field->SetText( wx( req.block() ) );
        field->SetVisible( false );
    }

    // Reference: "R5" as given; "", "R?" or a bare prefix annotated with the next free number.
    wxString ref = wx( req.ref() );

    if( !ref.IsEmpty() && !ref.EndsWith( wxS( "?" ) ) && wxIsdigit( ref.Last() ) )
    {
        if( !findSymbols( req.ref() ).empty() )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, req.ref() + " already exists" ) );

        wxString base;
        wxString error = symbol->BaseRefFromUser( sheet, ref, base, schematic() );

        if( !error.IsEmpty() )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, str( error ) ) );

        symbol->SetRef( &sheet, base );
    }
    else
    {
        if( !ref.IsEmpty() )
            symbol->SetRef( &sheet, ref.EndsWith( wxS( "?" ) ) ? ref : ref + wxS( "?" ) );

        SCH_REFERENCE_LIST existing;
        hierarchy.GetSymbols( existing, SYMBOL_FILTER_NON_POWER );
        existing.SortByReferenceOnly();
        AnnotateNewSymbol( symbol.get(), { sheet }, existing, schematic()->Settings(), &hierarchy );
    }

    // Position: as asked (snapped to the grid), or the first free slot on the sheet, leaving
    // room around the body for stubs and labels.
    if( req.has_x() && req.has_y() )
    {
        symbol->SetPosition( VECTOR2I( snap( iu( req.x() ) ), snap( iu( req.y() ) ) ) );
    }
    else
    {
        std::vector<BOX2I> taken;

        for( SCH_ITEM* item : screen->Items() )
            taken.push_back( item->GetBoundingBox() );

        BOX2I body = symbol->GetBodyAndPinsBoundingBox();     // at the origin
        body.Inflate( iu( 12.7 ), iu( 7.62 ) );

        const int left = iu( 25.4 ), top = iu( 25.4 );
        const int right = screen->GetPageSettings().GetWidthIU( schIUScale.IU_PER_MILS ) - iu( 25.4 );
        const int bottom = screen->GetPageSettings().GetHeightIU( schIUScale.IU_PER_MILS ) - iu( 40 );
        const int step = iu( 2.54 );
        bool      found = false;
        VECTOR2I  at;

        for( int y = top; y + body.GetHeight() < bottom && !found; y += step )
        {
            for( int x = left; x + body.GetWidth() < right && !found; x += step )
            {
                VECTOR2I anchor( snap( x - body.GetLeft() ), snap( y - body.GetTop() ) );
                BOX2I    box = body;
                box.Move( anchor );

                if( std::none_of( taken.begin(), taken.end(), [&]( const BOX2I& b ) { return b.Intersects( box ); } ) )
                {
                    at = anchor;
                    found = true;
                }
            }
        }

        if( !found )
        {
            // Sheet full: below everything else
            int lowest = top;

            for( const BOX2I& b : taken )
                lowest = std::max( lowest, b.GetBottom() );

            at = VECTOR2I( snap( left - body.GetLeft() ), snap( lowest + iu( 5.08 ) - body.GetTop() ) );
            note += std::string( note.empty() ? "" : "; " ) + "the sheet is full: placed below it";
        }

        symbol->SetPosition( at );
    }

    symbol->AutoplaceFields( screen, AUTOPLACE_AUTO );

    SCH_SYMBOL* placed = symbol.release();
    SCH_COMMIT  commit( m_context->GetToolManager() );
    commit.Add( placed, screen );
    wxString what = wxString::Format( wxS( "add %s" ), placed->GetRef( &sheet ) );
    commit.Push( wxS( "Agent: " ) + what );

    rebuildConnectivity();
    announce( wxS( "Agent: " ) + what );

    SchPartResponse out;
    packSymbol( placed, sheet, true, *out.mutable_symbol() );
    out.set_note( note );
    return out;
}


HANDLER_RESULT<SchPartResponse> KICADD_HANDLER_SCH::handleGetPart( const HANDLER_CONTEXT<SchGetPart>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    auto symbols = findSymbols( aCtx.Request.ref() );

    if( symbols.empty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no part " + aCtx.Request.ref() ) );

    rebuildConnectivity();

    SchPartResponse out;
    packSymbol( symbols[0].first, symbols[0].second, true, *out.mutable_symbol() );

    // other units of the same part: their pins too
    for( size_t i = 1; i < symbols.size(); ++i )
    {
        SchSymbol unit;
        packSymbol( symbols[i].first, symbols[i].second, true, unit );

        for( const SchPin& p : unit.pins() )
            *out.mutable_symbol()->add_pins() = p;
    }

    return out;
}


int KICADD_HANDLER_SCH::disconnectPin( const PIN_AT& aPin, SCH_COMMIT& aCommit )
{
    std::set<SCH_ITEM*> gone;

    for( SCH_LINE* wire : wiresEndingAt( aPin.screen, aPin.pos ) )
    {
        gone.insert( wire );
        VECTOR2I farEnd = wire->GetStartPoint() == aPin.pos ? wire->GetEndPoint() : wire->GetStartPoint();

        // the stub's label, unless something else still uses that point
        if( wiresEndingAt( aPin.screen, farEnd ).size() == 1 )
        {
            for( SCH_LABEL_BASE* label : labelsAt( aPin.screen, farEnd ) )
                gone.insert( label );
        }
    }

    for( SCH_LABEL_BASE* label : labelsAt( aPin.screen, aPin.pos ) )
        gone.insert( label );

    if( SCH_NO_CONNECT* nc = noConnectAt( aPin.screen, aPin.pos ) )
        gone.insert( nc );

    for( SCH_ITEM* item : gone )
        aCommit.Remove( item, aPin.screen );

    return (int) gone.size();
}


HANDLER_RESULT<SchConnectResponse> KICADD_HANDLER_SCH::handleConnect( const HANDLER_CONTEXT<SchConnect>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    rebuildConnectivity();

    SchConnectResponse out;
    SCH_COMMIT         commit( m_context->GetToolManager() );
    int                added = 0;

    // ports: "VOUT" or "VOUT:output"
    std::map<std::string, LABEL_FLAG_SHAPE> ports;

    for( const std::string& port : aCtx.Request.ports() )
    {
        size_t           colon = port.find( ':' );
        std::string      name = port.substr( 0, colon );
        LABEL_FLAG_SHAPE shape = LABEL_FLAG_SHAPE::L_BIDI;

        if( colon != std::string::npos && !shapeOf( port.substr( colon + 1 ), shape ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "port " + port + ": direction is input, output, bidirectional, "
                                              "tri_state or passive" ) );

        ports[name] = shape;
    }

    std::set<SCH_SHEET*> modifiedSheets;

    for( const SchPinNet& link : aCtx.Request.links() )
    {
        PIN_AT      at;
        std::string err;
        std::string label = link.ref() + "." + link.pin();

        if( link.net().empty() )
        {
            out.add_errors( label + ": no net given" );
            continue;
        }

        if( !findPin( link.ref(), link.pin(), at, err ) )
        {
            out.add_errors( err );
            continue;
        }

        bool        wantNc = link.net() == "NC";
        std::string current = pinNet( at.item, at.sheet );

        if( wantNc )
        {
            if( noConnectAt( at.screen, at.pos ) )
            {
                out.set_done( out.done() + 1 );     // already
                continue;
            }

            if( !current.empty() || occupied( at.screen, at.pos ) && !wiresEndingAt( at.screen, at.pos ).empty() )
            {
                out.add_errors( label + " is on net " + ( current.empty() ? "?" : current )
                                + "; sch_disconnect it first" );
                continue;
            }

            commit.Add( new SCH_NO_CONNECT( at.pos ), at.screen );
            out.set_done( out.done() + 1 );
            added++;
            continue;
        }

        if( noConnectAt( at.screen, at.pos ) )
        {
            out.add_errors( label + " has a no-connect flag; sch_disconnect it first" );
            continue;
        }

        if( !wiresEndingAt( at.screen, at.pos ).empty() || !labelsAt( at.screen, at.pos ).empty() )
        {
            if( current == link.net() || current == "/" + link.net() )
                out.set_done( out.done() + 1 );     // same net again: nothing to do
            else
                out.add_errors( label + " is already on net " + ( current.empty() ? "?" : current )
                                + "; sch_disconnect it first" );

            continue;
        }

        // Stub end: 2.54 mm out, further if that point is taken (another pin's stub, a label)
        VECTOR2I end;
        bool     free = false;

        for( int k = 1; k <= 4 && !free; ++k )
        {
            end = at.pos + at.out * ( STUB * k );
            free = !occupied( at.screen, end );
        }

        if( !free )
        {
            out.add_errors( label + ": no free spot for a label next to the pin" );
            continue;
        }

        auto            port = ports.find( link.net() );
        SCH_LABEL_BASE* netLabel = nullptr;

        if( port != ports.end() )
        {
            // A port of the pin's sheet: a hierarchical label here, a sheet pin of the same name
            // on the sheet's symbol on the parent
            if( at.sheet.size() < 2 )
            {
                out.add_errors( label + ": " + link.net() + " can't be a port: the pin is on the root sheet" );
                continue;
            }

            SCH_SHEET_PATH parent = at.sheet;
            parent.pop_back();
            SCH_SHEET* owner = at.sheet.Last();

            if( !owner->HasPin( wx( link.net() ) ) )
            {
                if( modifiedSheets.insert( owner ).second )
                    commit.Modify( owner, parent.LastScreen() );

                addSheetPin( owner, wx( link.net() ), port->second );
            }

            auto* hier = new SCH_HIERLABEL( end, wx( link.net() ) );
            hier->SetShape( port->second );
            netLabel = hier;
        }
        else if( aCtx.Request.global() )
        {
            netLabel = new SCH_GLOBALLABEL( end, wx( link.net() ) );
        }
        else
        {
            netLabel = new SCH_LABEL( end, wx( link.net() ) );
        }

        auto* wire = new SCH_LINE( at.pos, LAYER_WIRE );
        wire->SetEndPoint( end );
        commit.Add( wire, at.screen );

        // text runs away from the pin
        if( at.out.x < 0 )
            netLabel->SetSpinStyle( SPIN_STYLE::LEFT );
        else if( at.out.x > 0 )
            netLabel->SetSpinStyle( SPIN_STYLE::RIGHT );
        else if( at.out.y < 0 )
            netLabel->SetSpinStyle( SPIN_STYLE::UP );
        else
            netLabel->SetSpinStyle( SPIN_STYLE::BOTTOM );

        commit.Add( netLabel, at.screen );
        out.set_done( out.done() + 1 );
        added++;
    }

    if( added )
    {
        out.set_history_label( "connect " + std::to_string( added ) + " pin" + ( added > 1 ? "s" : "" ) );
        commit.Push( wxS( "Agent: " ) + wx( out.history_label() ) );
        rebuildConnectivity();
        announce( wxS( "Agent: " ) + wx( out.history_label() ) );
    }
    else
    {
        commit.Revert();
    }

    return out;
}


HANDLER_RESULT<SchConnectResponse> KICADD_HANDLER_SCH::handleDisconnect( const HANDLER_CONTEXT<SchDisconnect>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    SchConnectResponse out;
    SCH_COMMIT         commit( m_context->GetToolManager() );
    int                removed = 0;

    for( const SchPinNet& p : aCtx.Request.pins() )
    {
        PIN_AT      at;
        std::string err;

        if( !findPin( p.ref(), p.pin(), at, err ) )
        {
            out.add_errors( err );
            continue;
        }

        int n = disconnectPin( at, commit );
        removed += n;

        if( n )
            out.set_done( out.done() + 1 );
    }

    if( removed )
    {
        out.set_history_label( "disconnect " + std::to_string( out.done() ) + " pin(s)" );
        commit.Push( wxS( "Agent: " ) + wx( out.history_label() ) );
        rebuildConnectivity();
        announce( wxS( "Agent: " ) + wx( out.history_label() ) );
    }
    else
    {
        commit.Revert();
    }

    return out;
}


HANDLER_RESULT<SchNetlist> KICADD_HANDLER_SCH::handleGetNetlist( const HANDLER_CONTEXT<GetSchNetlist>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    rebuildConnectivity();

    // Sets: a pin shared by the units of a symbol (the supply pins of a 74HC125) comes back
    // from every unit; it is one pin on the net, as in KiCad's own netlist.
    std::map<std::string, std::set<std::string>> nets;
    std::set<std::string>                        unconnected, noConnect;
    SchNetlist                                   out;

    for( const SCH_SHEET_PATH& sheet : schematic()->Hierarchy() )
    {
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->IsPower() )
                continue;

            std::string ref = str( symbol->GetRef( &sheet ) );

            // virtual symbols (#PWR, #FLG) are no parts, also when the power flag is missing
            if( ref.empty() || ref[0] == '#' )
                continue;

            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                std::string net = pinNet( pin, sheet );
                std::string name = ref + "." + str( pin->GetNumber() );

                if( !net.empty() )
                    nets[net].insert( name );
                else if( noConnectAt( sheet.LastScreen(), pin->GetPosition() ) )
                    noConnect.insert( name );
                else
                    unconnected.insert( name );
            }
        }
    }

    for( auto& [name, pins] : nets )
    {
        if( !aCtx.Request.net().empty() && name != aCtx.Request.net() && name != "/" + aCtx.Request.net() )
            continue;

        SchNet* n = out.add_nets();
        n->set_name( name );

        for( const std::string& p : pins )
            n->add_pins( p );
    }

    if( aCtx.Request.net().empty() )
    {
        for( const std::string& p : unconnected )
            out.add_unconnected( p );

        for( const std::string& p : noConnect )
            out.add_no_connect( p );
    }

    return out;
}


HANDLER_RESULT<SchConnectResponse> KICADD_HANDLER_SCH::handleDeleteParts( const HANDLER_CONTEXT<SchDeleteParts>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    SchConnectResponse out;
    SCH_COMMIT         commit( m_context->GetToolManager() );
    bool               sheetsGone = false;

    for( const std::string& ref : aCtx.Request.refs() )
    {
        auto symbols = findSymbols( ref );

        if( symbols.empty() )
        {
            // a sheet: its symbol on the parent (the stubs on its pins with it); what is on the
            // sheet goes with it, its file stays on disk
            auto sheets = findSheetSymbols( ref );

            if( sheets.size() == 1 )
            {
                auto& [sheet, parent] = sheets[0];

                for( SCH_SHEET_PIN* sp : sheet->GetPins() )
                {
                    PIN_AT at;
                    at.sheetPin = sp;
                    at.item = sp;
                    at.sheet = parent;
                    at.screen = parent.LastScreen();
                    at.pos = sp->GetPosition();
                    disconnectPin( at, commit );
                }

                commit.Remove( sheet, parent.LastScreen() );
                sheetsGone = true;
                out.set_done( out.done() + 1 );
            }
            else
            {
                out.add_errors( sheets.empty() ? "no part or sheet " + ref
                                               : "sheet name " + ref + " is used more than once; give its path" );
            }

            continue;
        }

        for( auto& [symbol, sheet] : symbols )
        {
            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                PIN_AT      at;
                std::string err;

                if( findPin( ref, str( pin->GetNumber() ), at, err ) )
                    disconnectPin( at, commit );
            }

            commit.Remove( symbol, sheet.LastScreen() );
        }

        out.set_done( out.done() + 1 );
    }

    if( out.done() )
    {
        out.set_history_label( "delete " + std::to_string( out.done() ) + " part(s)" );
        commit.Push( wxS( "Agent: " ) + wx( out.history_label() ) );

        if( sheetsGone )
            hierarchyChanged();

        rebuildConnectivity();
        announce( wxS( "Agent: " ) + wx( out.history_label() ) );
    }
    else
    {
        commit.Revert();
    }

    return out;
}


void KICADD_HANDLER_SCH::addSheetPin( SCH_SHEET* aSheet, const wxString& aName, LABEL_FLAG_SHAPE aShape )
{
    // inputs on the left, everything else on the right, one every 2.54 mm from the top; the
    // sheet grows when its side is full
    SHEET_SIDE side = aShape == LABEL_FLAG_SHAPE::L_INPUT ? SHEET_SIDE::LEFT : SHEET_SIDE::RIGHT;
    int        n = 0;

    for( SCH_SHEET_PIN* sp : aSheet->GetPins() )
    {
        if( sp->GetSide() == side )
            n++;
    }

    const VECTOR2I pos = aSheet->GetPosition();
    VECTOR2I       size = aSheet->GetSize();
    int            y = pos.y + STUB * ( n + 1 );

    if( y > pos.y + size.y - STUB )
    {
        size.y = STUB * ( n + 2 );
        aSheet->Resize( size );
    }

    auto* pin = new SCH_SHEET_PIN( aSheet, VECTOR2I( side == SHEET_SIDE::LEFT ? pos.x : pos.x + size.x, y ), aName );
    pin->SetShape( aShape );
    pin->SetSide( side );
    pin->SetPosition( VECTOR2I( side == SHEET_SIDE::LEFT ? pos.x : pos.x + size.x, y ) );
    aSheet->AddPin( pin );
}


HANDLER_RESULT<SchSheetResponse> KICADD_HANDLER_SCH::handleAddSheet( const HANDLER_CONTEXT<SchAddSheet>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    const SchAddSheet& req = aCtx.Request;
    wxString           name = wx( req.name() );

    if( name.IsEmpty() || name.Contains( wxS( "/" ) ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "a sheet needs a name (no \"/\")" ) );

    std::string                   err;
    std::optional<SCH_SHEET_PATH> parent = findSheet( req.parent(), err );

    if( !parent )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    SCH_SCREEN* parentScreen = parent->LastScreen();

    for( SCH_ITEM* item : parentScreen->Items().OfType( SCH_SHEET_T ) )
    {
        if( static_cast<SCH_SHEET*>( item )->GetName() == name )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "sheet " + req.name() + " already exists there" ) );
    }

    // Its file: as asked or "<name>.kicad_sch", next to the root schematic. A new file only:
    // reusing a sheet file (several instances) is not supported here.
    const wxString ext = wxS( "." ) + wxString( FILEEXT::KiCadSchematicFileExtension );
    wxString       file = req.file().empty() ? name + ext : wx( req.file() );
    file.Replace( wxS( " " ), wxS( "_" ) );

    if( !file.EndsWith( ext ) )
        file += ext;

    wxFileName root( schematic()->RootScreen()->GetFileName() );
    wxFileName abs( file );

    if( !abs.IsAbsolute() )
        abs.MakeAbsolute( root.GetPath() );

    if( abs.FileExists() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          str( abs.GetFullName() ) + " already exists; give another file name" ) );

    SCH_SCREENS screens( schematic()->Root() );

    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        if( wxFileName( screen->GetFileName() ) == abs )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              str( abs.GetFullName() ) + " is already a sheet of this schematic" ) );
    }

    VECTOR2I size( snap( iu( req.width() > 0 ? req.width() : 50.8 ) ), snap( iu( req.height() > 0 ? req.height() : 25.4 ) ) );
    VECTOR2I pos;
    std::string note;

    if( req.has_x() && req.has_y() )
    {
        pos = VECTOR2I( snap( iu( req.x() ) ), snap( iu( req.y() ) ) );
    }
    else
    {
        // the first free spot on the parent, with room around for the pins' stubs and labels
        std::vector<BOX2I> taken;

        for( SCH_ITEM* item : parentScreen->Items() )
            taken.push_back( item->GetBoundingBox() );

        BOX2I body( VECTOR2I( 0, 0 ), size );
        body.Inflate( iu( 20.32 ), iu( 7.62 ) );

        const int left = iu( 25.4 ), top = iu( 25.4 );
        const int right = parentScreen->GetPageSettings().GetWidthIU( schIUScale.IU_PER_MILS ) - iu( 25.4 );
        const int bottom = parentScreen->GetPageSettings().GetHeightIU( schIUScale.IU_PER_MILS ) - iu( 40 );
        const int step = iu( 2.54 );
        bool      found = false;

        for( int y = top; y + body.GetHeight() < bottom && !found; y += step )
        {
            for( int x = left; x + body.GetWidth() < right && !found; x += step )
            {
                VECTOR2I anchor( snap( x - body.GetLeft() ), snap( y - body.GetTop() ) );
                BOX2I    box = body;
                box.Move( anchor );

                if( std::none_of( taken.begin(), taken.end(), [&]( const BOX2I& b ) { return b.Intersects( box ); } ) )
                {
                    pos = anchor;
                    found = true;
                }
            }
        }

        if( !found )
        {
            int lowest = top;

            for( const BOX2I& b : taken )
                lowest = std::max( lowest, b.GetBottom() );

            pos = VECTOR2I( snap( left + iu( 20.32 ) ), snap( lowest + iu( 10.16 ) ) );
            note = "the sheet is full: placed below it";
        }
    }

    // No parent item: SCH_COMMIT turns adding an item whose parent is a sheet into modifying
    // that sheet; the screen becomes its parent when it is appended.
    auto* sheet = new SCH_SHEET( nullptr, pos, size );
    sheet->GetField( FIELD_T::SHEET_NAME )->SetText( name );

    wxFileName rel( abs );
    rel.MakeRelativeTo( wxFileName( parentScreen->GetFileName() ).GetPath() );
    sheet->GetField( FIELD_T::SHEET_FILENAME )->SetText( rel.GetFullPath( wxPATH_UNIX ) );

    auto* screen = new SCH_SCREEN( schematic() );
    screen->SetFileName( abs.GetFullPath() );
    screen->SetPageSettings( parentScreen->GetPageSettings() );
    screen->SetContentModified();
    sheet->SetScreen( screen );

    SCH_SHEET_LIST hierarchy = schematic()->Hierarchy();
    SCH_SHEET_PATH instance = *parent;
    instance.push_back( sheet );
    instance.SetPageNumber( hierarchy.GetNextPageNumber() );

    sheet->AutoplaceFields( parentScreen, AUTOPLACE_AUTO );

    SCH_COMMIT commit( m_context->GetToolManager() );
    commit.Add( sheet, parentScreen );
    wxString what = wxS( "add sheet " ) + name;
    commit.Push( wxS( "Agent: " ) + what );

    hierarchyChanged();
    rebuildConnectivity();
    announce( wxS( "Agent: " ) + what );

    SchSheetResponse out;
    packSheet( sheet, *parent, *out.mutable_sheet() );
    out.set_note( note );
    return out;
}


HANDLER_RESULT<SchConnectResponse> KICADD_HANDLER_SCH::handleSave( const HANDLER_CONTEXT<SchSave>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    SchConnectResponse out;

    if( SCH_EDIT_FRAME* frame = editor() )
    {
        // In the Schematic Editor the file is the user's: sync_pcb reads the editor's schematic
        // and needs no save; an explicit save goes through the editor (title, modified flags).
        if( aCtx.Request.only_headless() )
        {
            out.set_history_label( "in the Schematic Editor: not saved (read from the editor)" );
            return out;
        }

        if( auto err = busy() )
            return tl::unexpected( *err );

        if( !frame->SaveProject() )
            return tl::unexpected( makeError( ApiStatusCode::AS_UNKNOWN, "saving the schematic failed" ) );

        announce( wxS( "Agent: saved the schematic" ) );
    }
    else if( !m_context->SaveSchematic() )
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_UNKNOWN, "saving the schematic failed" ) );
    }

    out.set_done( 1 );
    out.set_history_label( "saved " + str( m_context->GetCurrentFileName() ) );
    return out;
}
