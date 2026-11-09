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
#include <sch_field.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_pin.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
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

} // namespace


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


bool KICADD_HANDLER_SCH::findPin( const std::string& aRef, const std::string& aPin, PIN_AT& aOut,
                                  std::string& aError )
{
    auto symbols = findSymbols( aRef );

    if( symbols.empty() )
    {
        aError = "no part " + aRef;
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


std::string KICADD_HANDLER_SCH::pinNet( SCH_PIN* aPin, const SCH_SHEET_PATH& aSheet )
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
    return out;
}


HANDLER_RESULT<SchPartResponse> KICADD_HANDLER_SCH::handleAddPart( const HANDLER_CONTEXT<SchAddPart>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

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
    SCH_SHEET_PATH sheet = m_context->GetCurrentSheet().value_or( *hierarchy.begin() );
    SCH_SCREEN*    screen = sheet.LastScreen();
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
    commit.Push( wxString::Format( wxS( "add %s" ), placed->GetRef( &sheet ) ) );

    rebuildConnectivity();

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
        VECTOR2I far = wire->GetStartPoint() == aPin.pos ? wire->GetEndPoint() : wire->GetStartPoint();

        // the stub's label, unless something else still uses that point
        if( wiresEndingAt( aPin.screen, far ).size() == 1 )
        {
            for( SCH_LABEL_BASE* label : labelsAt( aPin.screen, far ) )
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

    rebuildConnectivity();

    SchConnectResponse out;
    SCH_COMMIT         commit( m_context->GetToolManager() );
    int                added = 0;

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
        std::string current = pinNet( at.pin, at.sheet );

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

        auto* wire = new SCH_LINE( at.pos, LAYER_WIRE );
        wire->SetEndPoint( end );
        commit.Add( wire, at.screen );

        SCH_LABEL_BASE* netLabel = aCtx.Request.global()
                                           ? static_cast<SCH_LABEL_BASE*>( new SCH_GLOBALLABEL( end, wx( link.net() ) ) )
                                           : static_cast<SCH_LABEL_BASE*>( new SCH_LABEL( end, wx( link.net() ) ) );

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
        commit.Push( wx( out.history_label() ) );
        rebuildConnectivity();
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
        commit.Push( wx( out.history_label() ) );
        rebuildConnectivity();
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

    std::map<std::string, std::vector<std::string>> nets;
    SchNetlist                                      out;

    for( const SCH_SHEET_PATH& sheet : schematic()->Hierarchy() )
    {
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->IsPower() )
                continue;

            std::string ref = str( symbol->GetRef( &sheet ) );

            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                std::string net = pinNet( pin, sheet );
                std::string name = ref + "." + str( pin->GetNumber() );

                if( !net.empty() )
                    nets[net].push_back( name );
                else if( !noConnectAt( sheet.LastScreen(), pin->GetPosition() ) && aCtx.Request.net().empty() )
                    out.add_unconnected( name );
            }
        }
    }

    for( auto& [name, pins] : nets )
    {
        if( !aCtx.Request.net().empty() && name != aCtx.Request.net() && name != "/" + aCtx.Request.net() )
            continue;

        std::sort( pins.begin(), pins.end() );
        SchNet* n = out.add_nets();
        n->set_name( name );

        for( const std::string& p : pins )
            n->add_pins( p );
    }

    return out;
}


HANDLER_RESULT<SchConnectResponse> KICADD_HANDLER_SCH::handleDeleteParts( const HANDLER_CONTEXT<SchDeleteParts>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    SchConnectResponse out;
    SCH_COMMIT         commit( m_context->GetToolManager() );

    for( const std::string& ref : aCtx.Request.refs() )
    {
        auto symbols = findSymbols( ref );

        if( symbols.empty() )
        {
            out.add_errors( "no part " + ref );
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
        commit.Push( wx( out.history_label() ) );
        rebuildConnectivity();
    }
    else
    {
        commit.Revert();
    }

    return out;
}


HANDLER_RESULT<SchConnectResponse> KICADD_HANDLER_SCH::handleSave( const HANDLER_CONTEXT<SchSave>& aCtx )
{
    if( !schematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( !m_context->SaveSchematic() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNKNOWN, "saving the schematic failed" ) );

    SchConnectResponse out;
    out.set_done( 1 );
    out.set_history_label( "saved " + str( m_context->GetCurrentFileName() ) );
    return out;
}
