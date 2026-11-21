/*
 * KiCadD: a region of the schematic for the simulator (ROADMAP phase 3, item 2).
 *
 * The parts in the region (refs, a sheet, or around parts / nets through some hops) with each
 * pin's net, and for the nets whether they are ground or a supply and how many pins of parts
 * outside the region they have (the region's boundary). kicadd-mcp makes the models (from the
 * parts index's parameters) and the netlist, and runs ngspice: nothing here simulates.
 *
 * A part with its own SPICE model (Sim.* fields) gets KiCad's netlist lines for it, from
 * NETLIST_EXPORTER_SPICE on the whole schematic, net names as the exporter makes them (the same
 * ConvertToSpiceMarkup the nets' "spice" names come from).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_handler_sch.h"

#include <api/sch_context.h>
#include <connection_graph.h>
#include <netlist_exporters/netlist_exporter_spice.h>
#include <reporter.h>
#include <richio.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>

#include <wx/filename.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>

using namespace kicadd::schematic;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;

namespace
{

ApiResponseStatus simError( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}

std::string utf8( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

std::string upperOf( std::string s )
{
    std::transform( s.begin(), s.end(), s.begin(), ::toupper );
    return s;
}

std::string bareOf( const std::string& aNet )
{
    size_t slash = aNet.rfind( '/' );
    return slash == std::string::npos ? aNet : aNet.substr( slash + 1 );
}

bool isGroundName( const std::string& aNet )
{
    std::string n = upperOf( bareOf( aNet ) );
    return n.find( "GND" ) != std::string::npos || n.find( "VSS" ) != std::string::npos || n == "0V" || n == "0"
           || n.rfind( "EARTH", 0 ) == 0;
}

std::string spiceName( const std::string& aNet )
{
    wxString n = wxString::FromUTF8( aNet );
    NETLIST_EXPORTER_SPICE::ConvertToSpiceMarkup( &n );
    return utf8( n );
}

bool hasOwnModel( SCH_SYMBOL* aSymbol )
{
    for( const wchar_t* f : { L"Sim.Device", L"Sim.Library", L"Sim.Name", L"Spice_Primitive", L"Spice_Model" } )
    {
        if( const SCH_FIELD* field = aSymbol->GetField( wxString( f ) ); field && !field->GetText().IsEmpty() )
            return true;
    }

    return false;
}

struct SIM_PIN
{
    std::string ref, number, name, net, type, unit;
};

} // namespace


HANDLER_RESULT<SchSimRegionResponse> KICADD_HANDLER_SCH::handleSimRegion( const HANDLER_CONTEXT<SchSimRegion>& aCtx )
{
    SCHEMATIC* sch = schematic();

    if( !sch )
        return tl::unexpected( simError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    const SchSimRegion& req = aCtx.Request;
    int                 modes = ( req.refs_size() > 0 ) + !req.sheet().empty() + ( req.around_size() > 0 );

    if( modes != 1 )
        return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, "give one of refs, sheet or around" ) );

    rebuildConnectivity();

    // --- every part's pins, and the nets' pins
    struct PART_AT
    {
        SCH_SYMBOL*    symbol = nullptr;
        SCH_SHEET_PATH sheet;
        std::vector<SIM_PIN> pins;
    };

    std::map<std::string, PART_AT>                  parts;
    std::map<std::string, std::vector<std::string>> netRefs;   // net -> refs on it (once each)
    std::set<std::string>                           supplies;

    std::optional<SCH_SHEET_PATH> top;

    if( !req.sheet().empty() )
    {
        std::string err;
        top = findSheet( req.sheet(), err );

        if( !top )
            return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, err ) );
    }

    std::set<std::string> onSheet;

    for( const SCH_SHEET_PATH& sheet : sch->Hierarchy() )
    {
        const bool inScope = top && ( sheet == *top || sheet.IsContainedWithin( *top ) );

        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto*       symbol = static_cast<SCH_SYMBOL*>( item );
            std::string ref = utf8( symbol->GetRef( &sheet ) );

            if( symbol->IsPower() || ( !ref.empty() && ref[0] == '#' ) )
            {
                for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
                {
                    std::string net = pinNet( pin, sheet );

                    if( !net.empty() && upperOf( utf8( symbol->GetLibId().GetLibItemName().wx_str() ) ).find( "PWR_FLAG" ) == std::string::npos )
                        supplies.insert( net );
                }

                continue;
            }

            if( symbol->GetExcludedFromSim() )
                continue;

            PART_AT& part = parts[ref];

            if( !part.symbol )
            {
                part.symbol = symbol;
                part.sheet = sheet;
            }

            if( inScope )
                onSheet.insert( ref );

            const std::string unit = symbol->GetUnitCount() > 1 ? utf8( symbol->SubReference( symbol->GetUnit(), false ) ) : "";

            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                std::string net = pinNet( pin, sheet );
                std::string type = utf8( pin->GetCanonicalElectricalTypeName() );
                std::transform( type.begin(), type.end(), type.begin(), ::tolower );
                part.pins.push_back( { ref, utf8( pin->GetNumber() ), utf8( pin->GetShownName() ), net, type, unit } );

                if( net.empty() )
                    continue;

                auto& on = netRefs[net];

                if( std::find( on.begin(), on.end(), ref ) == on.end() )
                    on.push_back( ref );

                if( type == "power output" || type == "power_out" || type == "power input" || type == "power_in" )
                    supplies.insert( net );
            }
        }
    }

    auto ground = [&]( const std::string& aNet ) { return isGroundName( aNet ); };
    auto supply = [&]( const std::string& aNet ) { return !ground( aNet ) && supplies.count( aNet ) > 0; };

    // --- the region
    std::set<std::string> region;
    SchSimRegionResponse  out;

    auto findNet = [&]( const std::string& aName ) -> std::string
    {
        if( netRefs.count( aName ) )
            return aName;

        std::string hit;

        for( const auto& [net, refs] : netRefs )
        {
            if( bareOf( net ) == aName || net == "/" + aName )
            {
                if( !hit.empty() )
                    return "?" + hit + " or " + net;

                hit = net;
            }
        }

        return hit;
    };

    if( req.refs_size() > 0 )
    {
        for( const std::string& ref : req.refs() )
        {
            if( !parts.count( ref ) )
                return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, "no part " + ref + " (or it is excluded from simulation)" ) );

            region.insert( ref );
        }
    }
    else if( top )
    {
        region = onSheet;

        if( region.empty() )
            return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, "no parts on sheet " + req.sheet() ) );
    }
    else
    {
        // a hop goes through a net to the parts on it. Never through ground; through a supply
        // only to its two-pin parts (decoupling, a pull-up, a filter's inductor), not to the
        // other ICs it feeds. The parts named in around start with all their nets but ground.
        std::set<std::string> frontier;
        std::set<std::string> seenNets;

        for( const std::string& a : req.around() )
        {
            if( parts.count( a ) )
            {
                region.insert( a );

                for( const SIM_PIN& p : parts[a].pins )
                {
                    if( !p.net.empty() && !ground( p.net ) )
                        frontier.insert( p.net );
                }

                continue;
            }

            std::string net = findNet( a );

            if( net.empty() )
                return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, "around: no part or net " + a ) );

            if( net[0] == '?' )
                return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, "around " + a + ": which, " + net.substr( 1 ) + "?" ) );

            frontier.insert( net );
        }

        auto twoPin = [&]( const std::string& aRef )
        {
            std::set<std::string> numbers;

            for( const SIM_PIN& p : parts[aRef].pins )
                numbers.insert( p.number );

            return numbers.size() <= 2;
        };

        const int hops = req.hops() > 0 ? std::min( req.hops(), 20 ) : 1;

        for( int h = 0; h < hops && !frontier.empty(); ++h )
        {
            std::set<std::string> next;

            for( const std::string& net : frontier )
            {
                if( !seenNets.insert( net ).second )
                    continue;

                const bool viaSupply = supply( net ) || ground( net );

                for( const std::string& ref : netRefs[net] )
                {
                    if( region.count( ref ) || ( viaSupply && !twoPin( ref ) ) )
                        continue;

                    region.insert( ref );

                    for( const SIM_PIN& p : parts[ref].pins )
                    {
                        if( !p.net.empty() && !ground( p.net ) && !supply( p.net ) && !seenNets.count( p.net ) )
                            next.insert( p.net );
                    }
                }
            }

            frontier = next;
        }

        if( region.empty() )
            return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, "around: no parts there" ) );
    }

    if( region.size() > 400 )
        return tl::unexpected( simError( ApiStatusCode::AS_BAD_REQUEST, std::to_string( region.size() )
                                                                            + " parts in the region: too many for a local simulation (fewer hops, or refs)" ) );

    // --- KiCad's lines for the parts with their own model
    std::map<std::string, std::string> ownItem, ownError;
    std::set<std::string>              ownModels;   // the model names their item lines use
    std::vector<std::string>           modelBlocks;

    bool anyOwn = std::any_of( region.begin(), region.end(), [&]( const std::string& r ) { return hasOwnModel( parts[r].symbol ); } );

    if( anyOwn )
    {
        NETLIST_EXPORTER_SPICE exporter( sch, nullptr );
        STRING_FORMATTER       formatter;
        WX_STRING_REPORTER     reporter;
        exporter.DoWriteNetlist( wxEmptyString, NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS, formatter, reporter );

        if( reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR ) )
            out.add_notes( "KiCad's SPICE models: " + utf8( reporter.GetMessages() ).substr( 0, 300 ) );

        // the netlist as statements (a '+' line continues the one before)
        std::vector<std::string> stmts;
        std::istringstream       in( formatter.GetString() );
        std::string              line;

        while( std::getline( in, line ) )
        {
            if( !line.empty() && line.back() == '\r' )
                line.pop_back();

            if( !line.empty() && line[0] == '+' && !stmts.empty() )
                stmts.back() += "\n" + line;
            else
                stmts.push_back( line );
        }

        std::map<std::string, std::string> byName;   // item name (upper) -> statement

        for( size_t i = 0; i < stmts.size(); ++i )
        {
            const std::string& s = stmts[i];
            std::string        head = upperOf( s.substr( 0, s.find_first_of( " \t\n" ) ) );

            if( head == ".INCLUDE" || head == ".LIB" )
            {
                out.add_includes( s );
            }
            else if( head == ".MODEL" )
            {
                modelBlocks.push_back( s );
            }
            else if( head == ".SUBCKT" )
            {
                std::string block = s;

                while( ++i < stmts.size() )
                {
                    block += "\n" + stmts[i];

                    if( upperOf( stmts[i].substr( 0, 5 ) ) == ".ENDS" )
                        break;
                }

                modelBlocks.push_back( block );
            }
            else if( !head.empty() && head[0] != '.' && head[0] != '*' )
            {
                byName[head] = s;
            }
        }

        for( const std::string& ref : region )
        {
            if( !hasOwnModel( parts[ref].symbol ) )
                continue;

            std::string name = upperOf( utf8( exporter.GetItemName( wxString::FromUTF8( ref ) ) ) );
            auto        it = byName.find( name );

            if( name.empty() || it == byName.end() )
            {
                ownError[ref] = "KiCad could not make its SPICE model";
                continue;
            }

            ownItem[ref] = it->second;
            std::istringstream tok( it->second );
            std::string        t;

            while( tok >> t )
                ownModels.insert( upperOf( t ) );
        }
    }

    // --- the parts
    std::map<std::string, int> netOutside;
    std::map<std::string, std::vector<std::string>> netOutsideRefs;

    for( const auto& [net, refs] : netRefs )
    {
        for( const std::string& ref : refs )
        {
            if( region.count( ref ) )
                continue;

            for( const SIM_PIN& p : parts[ref].pins )
                netOutside[net] += p.net == net;

            if( netOutsideRefs[net].size() < 4 )
                netOutsideRefs[net].push_back( ref );
        }
    }

    std::set<std::string> regionNets;

    for( const std::string& ref : region )
    {
        PART_AT&    at = parts[ref];
        SCH_SYMBOL* symbol = at.symbol;
        SchSimPart* p = out.add_parts();
        p->set_ref( ref );
        p->set_value( utf8( symbol->GetField( FIELD_T::VALUE )->GetShownText( &at.sheet, FOR_GUI ) ) );
        p->set_footprint( utf8( symbol->GetField( FIELD_T::FOOTPRINT )->GetText() ) );
        p->set_lib_id( utf8( symbol->GetLibId().Format().wx_str() ) );

        if( const SCH_FIELD* d = symbol->GetField( FIELD_T::DESCRIPTION ) )
            p->set_description( utf8( d->GetText() ) );

        for( const SCH_FIELD& field : symbol->GetFields() )
        {
            if( field.GetId() != FIELD_T::USER )
                continue;

            if( field.GetName().IsSameAs( wxS( "LCSC" ), false ) || field.GetName().IsSameAs( wxS( "LCSC Part" ), false )
                || field.GetName().IsSameAs( wxS( "JLCPCB" ), false ) )
            {
                if( p->lcsc().empty() )
                    p->set_lcsc( utf8( field.GetText() ) );
            }
            else
            {
                ( *p->mutable_fields() )[utf8( field.GetName() )] = utf8( field.GetText() );
            }
        }

        for( const SIM_PIN& pin : at.pins )
        {
            SchSimPin* sp = p->add_pins();
            sp->set_number( pin.number );
            sp->set_name( pin.name );
            sp->set_net( pin.net );
            sp->set_type( pin.type );
            sp->set_unit( pin.unit );

            if( !pin.net.empty() )
                regionNets.insert( pin.net );
        }

        if( ownItem.count( ref ) )
            p->set_spice_item( ownItem[ref] );

        if( ownError.count( ref ) )
            p->set_spice_error( ownError[ref] );
    }

    // the .model / .subckt blocks the own-model items use
    for( const std::string& block : modelBlocks )
    {
        std::istringstream tok( block );
        std::string        kw, name;
        tok >> kw >> name;

        if( ownModels.count( upperOf( name ) ) )
        {
            for( SchSimPart& p : *out.mutable_parts() )
            {
                if( !p.spice_item().empty() )
                {
                    p.set_spice_model( p.spice_model() + block + "\n" );
                    break;   // once in the netlist is enough
                }
            }
        }
    }

    for( const std::string& net : regionNets )
    {
        SchSimNet* n = out.add_nets();
        n->set_name( net );
        n->set_spice( spiceName( net ) );
        n->set_ground( ground( net ) );
        n->set_supply( supply( net ) );
        n->set_outside( netOutside[net] );

        for( const std::string& r : netOutsideRefs[net] )
            n->add_outside_refs( r );
    }

    out.set_project_dir( utf8( wxFileName( sch->Project().GetProjectFullName() ).GetPath() ) );
    return out;
}
