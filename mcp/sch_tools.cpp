/*
 * KiCadD MCP - M5a schematic tools (proto/kicadd/schematic.proto).
 *
 * Text answers like the board tools: one line per part / net, mm, "REF.PIN" pin names.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "ipc_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include <kicadd/schematic.pb.h>
#include <kicadd/board.pb.h>

using json = nlohmann::json;
namespace ks = kicadd::schematic;
namespace kb = kicadd::board;

namespace kicadd::mcp
{

namespace
{

TOOL_RESULT fail( const std::string& aMsg ) { return { aMsg, true }; }

std::string num( double aMm )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.2f", aMm );
    std::string s = buf;

    while( s.size() > 1 && s.back() == '0' )
        s.pop_back();

    if( s.back() == '.' )
        s.pop_back();

    return s == "-0" ? "0" : s;
}

json obj( json aProps, std::vector<std::string> aRequired = {} )
{
    json s = { { "type", "object" }, { "properties", std::move( aProps ) } };

    if( !aRequired.empty() )
        s["required"] = aRequired;

    return s;
}

std::string str( const json& a, const char* k )
{
    auto it = a.find( k );
    return it != a.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// "U1 STM32F103C8T6 LCSC:STM32F103C8T6 [LCSC:LQFP-48...] @50.8,63.5 r90 (C8734)"
std::string symbolLine( const ks::SchSymbol& s )
{
    std::string t = s.ref() + " " + s.value() + " " + s.lib_id();

    if( !s.footprint().empty() )
        t += " [" + s.footprint() + "]";

    t += " @" + num( s.x() ) + "," + num( s.y() );

    if( s.rotation() )
        t += " r" + std::to_string( s.rotation() );

    if( s.units() > 1 )
        t += " unit " + std::to_string( s.unit() ) + "/" + std::to_string( s.units() );

    if( !s.lcsc().empty() )
        t += " (" + s.lcsc() + ")";

    if( !s.sheet().empty() && s.sheet() != "/" )
        t += " on " + s.sheet();

    for( const std::string& attr : s.attributes() )
        t += " " + attr;

    if( !s.fields().empty() )
    {
        // sorted: the map's order is not stable
        std::map<std::string, std::string> fields( s.fields().begin(), s.fields().end() );
        t += "\n  fields:";

        for( const auto& [k, v] : fields )
            t += " " + k + "=" + ( v.size() > 60 ? v.substr( 0, 57 ) + "..." : v ) + ";";

        t.pop_back();
    }

    return t;
}

/// "SHEET Power /Power/ (Power.kicad_sch) on / @x,y 50.8x25.4, 3 parts, pins: VOUT output right -> +5V"
std::string sheetLine( const ks::SchSheet& s )
{
    std::string t = "SHEET " + s.name() + " " + s.path() + " (" + s.file() + ") on " + s.parent() + " @"
                    + num( s.x() ) + "," + num( s.y() ) + " " + num( s.width() ) + "x" + num( s.height() ) + ", "
                    + std::to_string( s.parts() ) + " part(s)";

    for( const ks::SchSheetPin& p : s.pins() )
        t += "\n  pin " + p.name() + " " + p.direction() + " " + p.side() + " @" + num( p.x() ) + "," + num( p.y() )
             + ( p.net().empty() ? " (not connected on " + s.parent() + ")" : " -> " + p.net() );

    return t;
}

/// "  1 VBAT power_in left @x,y -> +3V3"
std::string pinLines( const ks::SchSymbol& s )
{
    std::string t;

    for( const ks::SchPin& p : s.pins() )
    {
        t += "\n  " + p.number();

        if( !p.name().empty() && p.name() != p.number() && p.name() != "~" )
            t += " " + p.name();

        t += " " + p.type() + " " + p.side() + " @" + num( p.x() ) + "," + num( p.y() );
        t += p.net().empty() ? std::string( " (unconnected)" ) : " -> " + p.net();
    }

    return t;
}

TOOL_RESULT connectResult( const ks::SchConnectResponse& r, const std::string& aWhat )
{
    std::string t = std::to_string( r.done() ) + " " + aWhat;

    for( const std::string& e : r.errors() )
        t += "\n  error: " + e;

    return { t, r.done() == 0 && r.errors_size() > 0 };
}

/// "R1.2" -> ("R1", "2")
bool splitPin( const std::string& aText, ks::SchPinNet& aOut )
{
    size_t dot = aText.find( '.' );

    if( dot == std::string::npos || dot == 0 || dot + 1 >= aText.size() )
        return false;

    aOut.set_ref( aText.substr( 0, dot ) );
    aOut.set_pin( aText.substr( dot + 1 ) );
    return true;
}

std::string lowerOf( std::string s )
{
    for( char& c : s )
        c = (char) std::tolower( static_cast<unsigned char>( c ) );

    return s;
}

/// The chip size in a footprint name: "Resistor_SMD:R_0603_1608Metric" -> "0603"
std::string chipSize( const std::string& aFootprint )
{
    std::string name = aFootprint.substr( aFootprint.find( ':' ) + 1 );

    for( const char* size : { "01005", "0201", "0402", "0603", "0805", "1206", "1210", "1812", "2010", "2512" } )
    {
        size_t at = 0;

        while( ( at = name.find( size, at ) ) != std::string::npos )
        {
            size_t end = at + std::strlen( size );
            bool   before = at == 0 || !std::isalnum( static_cast<unsigned char>( name[at - 1] ) );
            bool   after = end == name.size() || !std::isalnum( static_cast<unsigned char>( name[end] ) );

            // "R_0603_1608Metric", "LED_0805", "C_0402"; not "1608Metric"
            if( before && after )
                return size;

            at = end;
        }
    }

    return {};
}


/// lcsc_match: parts without an LCSC number, grouped by value + footprint, matched in the parts
/// index (resistors, capacitors, inductors, ferrite beads, LEDs in chip packages)
TOOL_RESULT lcscMatch( IPC_CLIENT& aIpc, const json& aArgs )
{
    ks::GetSchSummary sreq;
    sreq.set_with_pins( true );     // the attributes come with the pins
    ks::SchSummary    sum;
    std::string       err;

    if( !aIpc.Call( sreq, sum, err ) )
        return fail( err );

    const bool apply = aArgs.value( "apply", false );
    const bool replace = aArgs.value( "replace", false );
    const bool live = aArgs.value( "live", true );
    const bool basicOnly = aArgs.value( "basic_only", false );

    std::set<std::string> only;

    for( const json& r : aArgs.value( "refs", json::array() ) )
    {
        if( r.is_string() )
            only.insert( r.get<std::string>() );
    }

    std::map<std::string, std::string> pick;   // ref -> LCSC number for its group

    if( aArgs.contains( "pick" ) && aArgs["pick"].is_object() )
    {
        for( const auto& [ref, v] : aArgs["pick"].items() )
        {
            if( v.is_string() )
                pick[ref] = v.get<std::string>();
        }
    }

    struct GROUP
    {
        std::string              value, footprint;
        std::vector<std::string> refs;
        std::string              lcsc, line, why;   // the match, its description, or why none
        bool                     picked = false;
    };

    std::map<std::pair<std::string, std::string>, GROUP> groups;
    std::set<std::string>                                seen;
    int                                                  have = 0, skipped = 0;

    for( const ks::SchSymbol& sym : sum.symbols() )
    {
        const std::string& ref = sym.ref();

        if( ref.empty() || ref[0] == '#' || ref.back() == '?' || !seen.insert( ref ).second )
            continue;

        if( !only.empty() && !only.count( ref ) )
            continue;

        bool out = false;

        for( const std::string& attr : sym.attributes() )
            out |= attr == "dnp" || attr == "exclude_from_bom" || attr == "exclude_from_board";

        if( out )
        {
            skipped++;
            continue;
        }

        if( !sym.lcsc().empty() && !replace && !pick.count( ref ) )
        {
            have++;
            continue;
        }

        GROUP& g = groups[{ sym.value(), sym.footprint() }];
        g.value = sym.value();
        g.footprint = sym.footprint();
        g.refs.push_back( ref );
    }

    for( const std::string& r : std::vector<std::string>( only.begin(), only.end() ) )
    {
        if( !seen.count( r ) )
            return fail( "not in the schematic: " + r );
    }

    int matched = 0;

    for( auto& [key, g] : groups )
    {
        std::sort( g.refs.begin(), g.refs.end(),
                   []( const std::string& a, const std::string& b )
                   {
                       size_t da = a.find_first_of( "0123456789" ), db = b.find_first_of( "0123456789" );
                       std::string pa = a.substr( 0, da ), pb = b.substr( 0, db );

                       if( pa != pb )
                           return pa < pb;

                       return std::atol( a.c_str() + std::min( da, a.size() ) )
                              < std::atol( b.c_str() + std::min( db, b.size() ) );
                   } );

        for( const std::string& r : g.refs )
        {
            auto it = pick.find( r );

            if( it != pick.end() )
            {
                g.lcsc = it->second;
                g.picked = true;
            }
        }

        if( g.picked )
        {
            g.line = "your pick";
            matched++;
            continue;
        }

        // what kind of part: the reference's letters, the footprint for LEDs
        std::string prefix;

        for( char c : g.refs[0] )
        {
            if( !std::isalpha( static_cast<unsigned char>( c ) ) )
                break;

            prefix += (char) std::toupper( static_cast<unsigned char>( c ) );
        }

        const std::string fpLow = lowerOf( g.footprint );
        const std::string valLow = lowerOf( g.value );
        std::string       category;

        if( prefix == "R" )
            category = "resistor";
        else if( prefix == "C" )
            category = "capacitor";
        else if( prefix == "L" )
            category = "inductor";
        else if( prefix == "FB" )
            category = "ferrite";
        else if( prefix == "LED" || ( prefix == "D" && ( fpLow.find( "led" ) != std::string::npos
                                                          || valLow.find( "led" ) != std::string::npos ) ) )
            category = "led";

        if( category.empty() )
        {
            g.why = "only resistors, capacitors, inductors, ferrite beads and LEDs are matched: lcsc_search, then "
                    "pick";
            continue;
        }

        if( g.footprint.empty() )
        {
            g.why = "no footprint: the package is not known (sch_set_fields Footprint first)";
            continue;
        }

        const std::string size = chipSize( g.footprint );

        if( size.empty() )
        {
            g.why = "not a chip package (0402, 0603, ...): lcsc_search package=..., then pick";
            continue;
        }

        kb::LcscSearch q;
        q.set_package( size );
        q.set_basic_only( basicOnly );
        q.set_limit( 10 );
        q.set_live( -1 );

        std::string colour;

        if( category == "led" )
        {
            for( const char* c : { "red", "green", "blue", "yellow", "white", "orange", "amber", "pink", "purple" } )
            {
                if( valLow.find( c ) != std::string::npos )
                    colour = c;
            }

            if( colour.empty() )
            {
                g.why = "the colour is not in the value (\"LED red\", \"green\"): set the value, or pick";
                continue;
            }

            q.set_category( "LED Indication" );
            q.set_keyword( colour );
        }
        else if( category == "ferrite" )
        {
            // "600R", "600R@100MHz", "600 ohm": the description says "600Ω@100MHz"
            std::string z;

            for( char c : g.value )
            {
                if( !std::isdigit( static_cast<unsigned char>( c ) ) && c != '.' )
                    break;

                z += c;
            }

            if( z.empty() )
            {
                g.why = "the impedance at 100 MHz is not in the value (\"600R\"): set the value, or pick";
                continue;
            }

            q.set_category( "Ferrite Beads" );
            q.set_keyword( z + "Ω@100MHz" );
        }
        else
        {
            // "100nF 50V X7R", "10k 1%", "4.7uF/25V"
            std::string v = g.value;

            for( char& c : v )
            {
                if( c == '/' || c == ',' )
                    c = ' ';
            }

            std::string tok, first;
            v += " ";

            for( char c : v )
            {
                if( c != ' ' )
                {
                    tok += c;
                    continue;
                }

                if( tok.empty() )
                    continue;

                std::string up = tok;
                std::transform( up.begin(), up.end(), up.begin(), ::toupper );

                if( first.empty() )
                    first = tok;
                else if( up.back() == 'V' && std::isdigit( static_cast<unsigned char>( up[0] ) ) )
                    q.set_voltage_min( std::atof( up.c_str() ) );
                else if( up.back() == '%' )
                    q.set_tolerance( std::atof( up.c_str() ) );
                else if( up == "X7R" || up == "X5R" || up == "C0G" || up == "NP0" || up == "X7S" || up == "Y5V" )
                    q.set_dielectric( up );

                tok.clear();
            }

            q.set_category( category );
            q.set_value( first );
        }

        kb::LcscSearchResponse r;
        std::string            serr;

        if( !aIpc.Call( q, r, serr ) )
        {
            g.why = serr;
            continue;
        }

        const kb::LcscHit* best = nullptr;

        for( const kb::LcscHit& h : r.hits() )
        {
            std::string d = lowerOf( h.description() );

            // a white LED with a yellow lens is not a yellow LED
            if( !colour.empty() && colour != "white" && d.find( "white" ) != std::string::npos )
                continue;

            best = &h;
            break;
        }

        if( !best )
        {
            g.why = "nothing in stock at JLCPCB for " + g.value + " " + size
                    + ( basicOnly ? " (basic only)" : "" ) + ": lcsc_search with fewer conditions, then pick";
            continue;
        }

        kb::LcscHit chosen = *best;

        // today's stock and assembly for the chosen one (one fetch); an empty shelf: the next two
        if( live )
        {
            const int at = (int) ( best - &r.hits( 0 ) );
            bool      ok = false;

            for( int width : { at + 1, at + 3 } )
            {
                kb::LcscSearch one = q;
                one.set_limit( width );
                one.set_live( width );
                kb::LcscSearchResponse lr;

                if( !aIpc.Call( one, lr, serr ) )
                    break;

                for( int k = width == at + 1 ? at : at + 1; k < lr.hits_size(); ++k )
                {
                    const kb::LcscHit& h = lr.hits( k );
                    std::string        d = lowerOf( h.description() );

                    if( !colour.empty() && colour != "white" && d.find( "white" ) != std::string::npos )
                        continue;

                    if( h.lcsc() == chosen.lcsc() )
                        chosen = h;      // with its live numbers, good or not

                    // JLCPCB assembles from its own stock: LCSC's retail stock may be 0 for a basic part
                    if( h.live_smt() )
                    {
                        chosen = h;
                        ok = true;
                        break;
                    }
                }

                if( ok || lr.hits_size() <= at + 1 )
                    break;
            }
        }

        g.lcsc = chosen.lcsc();
        g.line = std::string( chosen.basic() ? "BASIC" : chosen.preferred() ? "preferred" : "extended" ) + " "
                 + chosen.mfr() + "; JLCPCB assembly stock " + std::to_string( chosen.stock() ) + " (index "
                 + r.index_date() + ")";

        if( !chosen.live_class().empty() || chosen.live_stock() >= 0 )
            g.line += std::string( chosen.live_smt() ? ", JLCPCB assembles it now" : ", NOT assembled by JLCPCB now" )
                      + ", LCSC retail stock now " + std::to_string( std::max<int64_t>( 0, chosen.live_stock() ) );

        g.line += "; " + ( chosen.description().size() > 110 ? chosen.description().substr( 0, 107 ) + "..."
                                                              : chosen.description() );

        if( r.total() > 1 )
            g.line += "; first of " + std::to_string( r.total() ) + " matching parts";

        matched++;
    }

    for( const auto& [ref, num] : pick )
    {
        if( !seen.count( ref ) )
            return fail( "pick: not in the schematic: " + ref );
    }

    std::string t;

    if( groups.empty() )
    {
        t = "nothing to match: " + std::to_string( have ) + " part(s) have an LCSC number already";

        if( skipped )
            t += ", " + std::to_string( skipped ) + " not assembled (DNP / not in the BOM)";

        return TOOL_RESULT{ t + " (replace=true matches them again)", false };
    }

    t = std::to_string( groups.size() ) + " group(s) of parts without an LCSC number (same value and footprint): "
        + std::to_string( matched ) + " matched, " + std::to_string( groups.size() - matched ) + " not";

    if( have )
        t += "; " + std::to_string( have ) + " part(s) have one already";

    if( skipped )
        t += "; " + std::to_string( skipped ) + " not assembled (DNP / not in the BOM)";

    t += "\nfirst basic, then preferred, then the most in stock (jlcparts index; check the voltage and "
         "dielectric of the capacitors)";

    // in reference order: C1 C2 ... R1 R2 ... R10
    auto refKey = []( const std::string& aRef )
    {
        size_t d = aRef.find_first_of( "0123456789" );
        return std::make_pair( aRef.substr( 0, d ), d == std::string::npos ? 0L : std::atol( aRef.c_str() + d ) );
    };

    std::vector<const GROUP*> order;

    for( auto& [key, g] : groups )
        order.push_back( &g );

    std::sort( order.begin(), order.end(),
               [&]( const GROUP* a, const GROUP* b ) { return refKey( a->refs[0] ) < refKey( b->refs[0] ); } );

    ks::SchSetFields set;

    for( const GROUP* gp : order )
    {
        const GROUP& g = *gp;
        std::string  refs;

        for( size_t i = 0; i < g.refs.size(); ++i )
        {
            if( i == 8 )
            {
                refs += " +" + std::to_string( g.refs.size() - 8 );
                break;
            }

            refs += ( i ? " " : "" ) + g.refs[i];
        }

        std::string fp = g.footprint.substr( g.footprint.find( ':' ) + 1 );
        t += "\n" + refs + " (" + g.value + ", " + ( fp.empty() ? "no footprint" : fp ) + ")";

        if( g.lcsc.empty() )
        {
            t += ": not matched: " + g.why;
            continue;
        }

        t += " -> " + g.lcsc + " " + g.line;

        for( const std::string& r : g.refs )
        {
            ks::SchFieldEdit* e = set.add_parts();
            e->set_ref( r );
            ( *e->mutable_fields() )["LCSC"] = g.lcsc;
        }
    }

    if( !apply )
    {
        t += "\ndry run: apply=true writes these LCSC numbers (one sch_undo); pick {\"R1\": \"C25804\"} uses "
             "another part for R1's group (or one for a group not matched)";
        return TOOL_RESULT{ t, false };
    }

    if( set.parts().empty() )
        return TOOL_RESULT{ t + "\nnothing written", false };

    ks::SchConnectResponse r;

    if( !aIpc.Call( set, r, err ) )
        return fail( t + "\nwriting them failed: " + err );

    t += "\nLCSC number set on " + std::to_string( r.done() ) + " part(s) (sch_undo takes it back; sync_pcb "
         "carries it to the board)";

    for( const std::string& e : r.errors() )
        t += "\n  error: " + e;

    return TOOL_RESULT{ t, false };
}

} // namespace


void AddSchematicTools( std::vector<TOOL>& aTools, IPC_CLIENT& aIpc )
{
    const json pinList = { { "type", "array" }, { "items", { { "type", "string" } } },
                           { "description", "Pins as \"REF.PIN\", e.g. [\"R1.2\", \"U1.VDD\"]" } };

    aTools.push_back( { "sch_summary",
            "The open schematic: its hierarchical sheets (with their pins and the nets on them), its "
            "parts (ref, value, symbol, footprint, position, sheet), net count and unconnected pins. "
            "with_pins lists every pin with its net.",
            obj( { { "with_pins", { { "type", "boolean" } } } } ),
            [&aIpc]( const json& a )
            {
                ks::GetSchSummary req;
                req.set_with_pins( a.value( "with_pins", false ) );
                ks::SchSummary r;
                std::string    err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = r.file() + ": " + std::to_string( r.symbols_size() ) + " parts, "
                                + std::to_string( r.nets() ) + " nets, " + std::to_string( r.unconnected_pins() )
                                + " unconnected pins, " + std::to_string( r.sheets() ) + " sheet(s)";

                for( const ks::SchSheet& s : r.sheet_list() )
                    t += "\n" + sheetLine( s );

                for( const ks::SchSymbol& s : r.symbols() )
                    t += "\n" + symbolLine( s ) + ( req.with_pins() ? pinLines( s ) : "" );

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_add_part",
            "Place a symbol on the schematic: from a library (lib_id \"Device:R\", "
            "\"Device:C\", ...) or an LCSC part (lcsc \"C25804\": fetched from the JLCEDA/EasyEDA "
            "Official Library, symbol + footprint saved to the project's LCSC libraries). ref \"R?\" "
            "or empty is numbered automatically. Without x/y it goes to the next free spot. Returns "
            "its pins (number, name, type, side) to connect with sch_connect. block names the "
            "functional block the part belongs to (\"LDO\", \"MCU\", \"USB\"): auto_place keeps "
            "each block's parts together on the board.",
            obj( { { "lib_id", { { "type", "string" } } },
                   { "lcsc", { { "type", "string" } } },
                   { "ref", { { "type", "string" } } },
                   { "value", { { "type", "string" }, { "description", "e.g. 10k, 100nF" } } },
                   { "footprint", { { "type", "string" }, { "description", "Library:Footprint, overrides the symbol's" } } },
                   { "x", { { "type", "number" }, { "description", "mm" } } },
                   { "y", { { "type", "number" }, { "description", "mm, down is +" } } },
                   { "rotation", { { "type", "integer" }, { "enum", { 0, 90, 180, 270 } } } },
                   { "unit", { { "type", "integer" }, { "description", "Multi-unit parts: which unit (default 1)" } } },
                   { "block", { { "type", "string" }, { "description", "functional block, e.g. LDO" } } },
                   { "sheet", { { "type", "string" }, { "description", "sheet to put it on (name or path; default the root)" } } } } ),
            [&aIpc]( const json& a )
            {
                ks::SchAddPart req;
                req.set_lib_id( str( a, "lib_id" ) );
                req.set_lcsc( str( a, "lcsc" ) );
                req.set_ref( str( a, "ref" ) );
                req.set_value( str( a, "value" ) );
                req.set_footprint( str( a, "footprint" ) );

                if( a.contains( "x" ) && a["x"].is_number() && a.contains( "y" ) && a["y"].is_number() )
                {
                    req.set_x( a["x"].get<double>() );
                    req.set_y( a["y"].get<double>() );
                }

                req.set_rotation( a.value( "rotation", 0 ) );
                req.set_unit( a.value( "unit", 0 ) );
                req.set_block( str( a, "block" ) );
                req.set_sheet( str( a, "sheet" ) );

                ks::SchPartResponse r;
                std::string         err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = "added " + symbolLine( r.symbol() ) + pinLines( r.symbol() );

                if( !r.note().empty() )
                    t += "\n  " + r.note();

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_part",
            "One part of the schematic with all its pins and their nets.",
            obj( { { "ref", { { "type", "string" } } } }, { "ref" } ),
            [&aIpc]( const json& a )
            {
                ks::SchGetPart req;
                req.set_ref( str( a, "ref" ) );
                ks::SchPartResponse r;
                std::string         err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                return TOOL_RESULT{ symbolLine( r.symbol() ) + pinLines( r.symbol() ), false };
            } } );

    aTools.push_back( { "sch_connect",
            "Connect pins to nets, as {\"nets\": {\"GND\": [\"R1.2\", \"C1.2\"], \"VCC\": [\"U1.VDD\"]}}. "
            "Each pin gets a short wire and a net label, so pins on the same net name are connected; "
            "net \"NC\" marks pins as not connected. A pin already on another net is refused "
            "(sch_disconnect it first). Hierarchy: a net named in ports is a port of the pins' sheet "
            "(a hierarchical label inside, a sheet pin of that name on the sheet's symbol: inputs on "
            "the left, the rest on the right); on the parent the sheet pin is SHEET.PIN, like a part's, "
            "e.g. {\"+5V\": [\"Power.VOUT\", \"U2.3\"]}. global uses global labels instead (every "
            "sheet sees the net by name). Nets in power get a power symbol instead of a label (GND, "
            "+3V3, VCC: global on every sheet, the usual way for supplies; ground-like names get a "
            "ground symbol).",
            obj( { { "nets", { { "type", "object" },
                               { "additionalProperties", pinList },
                               { "description", "net name -> pins" } } },
                   { "global", { { "type", "boolean" } } },
                   { "ports", { { "type", "array" }, { "items", { { "type", "string" } } },
                                { "description", "nets that are ports of their sheet: \"VOUT\" or \"VOUT:output\" "
                                                 "(input, output, bidirectional, tri_state, passive)" } } },
                   { "power", { { "type", "array" }, { "items", { { "type", "string" } } },
                                { "description", "nets connected with a power symbol, e.g. [\"GND\", \"+3V3\"]" } } } },
                 { "nets" } ),
            [&aIpc]( const json& a )
            {
                ks::SchConnect req;
                req.set_global( a.value( "global", false ) );

                for( const json& p : a.value( "power", json::array() ) )
                    if( p.is_string() )
                        req.add_power( p.get<std::string>() );

                for( const json& p : a.value( "ports", json::array() ) )
                    if( p.is_string() )
                        req.add_ports( p.get<std::string>() );
                std::string bad;

                if( !a.contains( "nets" ) || !a["nets"].is_object() )
                    return fail( "nets must be an object: net name -> [\"REF.PIN\", ...]" );

                for( auto& [net, pins] : a["nets"].items() )
                {
                    for( const json& p : pins )
                    {
                        ks::SchPinNet link;

                        if( !p.is_string() || !splitPin( p.get<std::string>(), link ) )
                        {
                            bad += " " + p.dump();
                            continue;
                        }

                        link.set_net( net );
                        *req.add_links() = link;
                    }
                }

                if( !bad.empty() )
                    return fail( "pins must be \"REF.PIN\":" + bad );

                ks::SchConnectResponse r;
                std::string            err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                return connectResult( r, "pin(s) connected" );
            } } );

    aTools.push_back( { "sch_disconnect",
            "Take pins off their nets: removes the wire from the pin (through its bends, up to where it "
            "meets another pin, a junction, a label or another wire) with a label left on nothing, "
            "or the no-connect flag.",
            obj( { { "pins", pinList } }, { "pins" } ),
            [&aIpc]( const json& a )
            {
                ks::SchDisconnect req;

                for( const json& p : a.value( "pins", json::array() ) )
                {
                    ks::SchPinNet pin;

                    if( p.is_string() && splitPin( p.get<std::string>(), pin ) )
                        *req.add_pins() = pin;
                }

                ks::SchConnectResponse r;
                std::string            err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                return connectResult( r, "pin(s) disconnected" );
            } } );

    aTools.push_back( { "sch_netlist",
            "Nets of the schematic with their pins (\"REF.PIN\"), the pins on no net (unconnected) and "
            "the ones marked no-connect (X) on purpose. net filters to one net.",
            obj( { { "net", { { "type", "string" } } } } ),
            [&aIpc]( const json& a )
            {
                ks::GetSchNetlist req;
                req.set_net( str( a, "net" ) );
                ks::SchNetlist r;
                std::string    err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = std::to_string( r.nets_size() ) + " nets";

                for( const ks::SchNet& n : r.nets() )
                {
                    t += "\n" + n.name() + ":";

                    for( const std::string& p : n.pins() )
                        t += " " + p;
                }

                if( r.unconnected_size() )
                {
                    t += "\nunconnected (" + std::to_string( r.unconnected_size() ) + "):";

                    for( const std::string& p : r.unconnected() )
                        t += " " + p;
                }

                if( r.no_connect_size() )
                {
                    t += "\nno-connect (" + std::to_string( r.no_connect_size() ) + "):";

                    for( const std::string& p : r.no_connect() )
                        t += " " + p;
                }

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_add_sheet",
            "Add a hierarchical sheet (a new .kicad_sch file) on a sheet (default the root). Then put "
            "parts on it with sch_add_part sheet, connect them there with sch_connect, and make the "
            "nets that leave the sheet ports (sch_connect ports): they become sheet pins, wired on "
            "the parent as SHEET.PIN.",
            obj( { { "name", { { "type", "string" }, { "description", "e.g. Power; unique among its siblings" } } },
                   { "file", { { "type", "string" }, { "description", "default <name>.kicad_sch" } } },
                   { "parent", { { "type", "string" }, { "description", "sheet name or path (default the root)" } } },
                   { "x", { { "type", "number" }, { "description", "top left, mm (default: a free spot)" } } },
                   { "y", { { "type", "number" } } },
                   { "width", { { "type", "number" }, { "description", "mm, default 50.8" } } },
                   { "height", { { "type", "number" }, { "description", "mm, default 25.4; grows with its pins" } } } },
                 { "name" } ),
            [&aIpc]( const json& a )
            {
                ks::SchAddSheet req;
                req.set_name( str( a, "name" ) );
                req.set_file( str( a, "file" ) );
                req.set_parent( str( a, "parent" ) );

                if( a.contains( "x" ) && a["x"].is_number() && a.contains( "y" ) && a["y"].is_number() )
                {
                    req.set_x( a["x"].get<double>() );
                    req.set_y( a["y"].get<double>() );
                }

                if( a.contains( "width" ) && a["width"].is_number() )
                    req.set_width( a["width"].get<double>() );

                if( a.contains( "height" ) && a["height"].is_number() )
                    req.set_height( a["height"].get<double>() );

                ks::SchSheetResponse r;
                std::string          err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = "added " + sheetLine( r.sheet() );

                if( !r.note().empty() )
                    t += "\n  " + r.note();

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_delete",
            "Delete from the schematic: refs (parts, with their wire stubs and labels; a sheet's "
            "name deletes the sheet with everything on it, its file stays on disk), wires (a point "
            "[x, y] on a wire deletes that wire: its segments through bends, up to where it meets a "
            "pin, junction, label or another wire; labels and junctions left on nothing go with it), "
            "dangling=true (every wire with an end on nothing). sheet: for wires / dangling.",
            obj( { { "refs", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                   { "wires", { { "type", "array" },
                                { "items", { { "type", "array" }, { "items", { { "type", "number" } } } } } } },
                   { "dangling", { { "type", "boolean" } } },
                   { "sheet", { { "type", "string" } } } },
                 {} ),
            [&aIpc]( const json& a )
            {
                ks::SchDeleteParts req;

                for( const json& r : a.value( "refs", json::array() ) )
                {
                    if( r.is_string() )
                        req.add_refs( r.get<std::string>() );
                }

                for( const json& p : a.value( "wires", json::array() ) )
                {
                    if( !p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number() )
                        return fail( "wires: [[x, y], ...] in mm, a point on each wire" );

                    ks::SchPoint* pt = req.add_wires();
                    pt->set_x( p[0].get<double>() );
                    pt->set_y( p[1].get<double>() );
                }

                req.set_dangling( a.value( "dangling", false ) );
                req.set_sheet( str( a, "sheet" ) );

                if( req.refs().empty() && req.wires().empty() && !req.dangling() )
                    return fail( "give refs, wires or dangling=true" );

                ks::SchConnectResponse r;
                std::string            err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                // "delete 1 part(s), 2 wire(s)" -> "deleted 1 part(s), 2 wire(s)"
                const std::string& label = r.history_label();
                return connectResult( r, label.rfind( "delete ", 0 ) == 0 ? "deleted: " + label.substr( 7 ) : "deleted" );
            } } );

    aTools.push_back( { "sync_pcb",
            "Update PCB from Schematic: brings the board in line with the schematic (the open "
            "Schematic Editor's, unsaved edits included; headless: saved first) "
            "(new footprints from each symbol's Footprint field, nets on the pads, changed values). "
            "New footprints land outside the board outline until placed (auto_place). dry_run "
            "reports without changing anything; delete_extra removes footprints the schematic no "
            "longer has. A reference already on the board but not linked to its symbol (placed "
            "by hand) is warned about: the sync would add it a second time. One undoable edit.",
            obj( { { "dry_run", { { "type", "boolean" } } },
                   { "delete_extra", { { "type", "boolean" } } },
                   { "detail", { { "type", "boolean" }, { "description", "list every change line (default: the first 25)" } } },
                   { "update_footprints", { { "type", "boolean" },
                                            { "description", "Replace footprints whose library link changed" } } } } ),
            [&aIpc]( const json& a )
            {
                std::string err;
                // Headless: save, the board side netlists the file. In the Schematic Editor
                // nothing is saved (the board side asks the editor). No schematic handler (the
                // PCB Editor on its own): the saved file is what there is.
                ks::SchSave save;
                ks::SchConnectResponse saved;
                save.set_only_headless( true );

                if( !aIpc.Call( save, saved, err ) && err.rfind( "AS_UNHANDLED", 0 ) != 0 )
                    return fail( "saving the schematic first: " + err );

                err.clear();

                kicadd::board::SyncFromSchematic req;
                req.set_dry_run( a.value( "dry_run", false ) );
                req.set_delete_extra( a.value( "delete_extra", false ) );
                req.set_update_footprints( a.value( "update_footprints", false ) );
                kicadd::board::SyncFromSchematicResponse r;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = std::string( req.dry_run() ? "dry run: " : "" ) + std::to_string( r.added() )
                                + " footprint(s) added";

                if( r.new_refs_size() )
                {
                    t += ":";

                    for( const std::string& ref : r.new_refs() )
                        t += " " + ref;
                }

                t += std::string( req.dry_run() ? "; board has " : "; board now " ) + std::to_string( r.footprints() )
                     + " footprints, " + std::to_string( r.nets() ) + " nets"
                     + ( req.dry_run() ? " (unchanged: dry run)" : "" ) + "; " + std::to_string( r.errors() )
                     + " errors, " + std::to_string( r.warnings() ) + " warnings";

                if( r.from_editor() )
                    t += " (from the open Schematic Editor)";

                // The updater's report without its routine lines: errors and warnings first,
                // then the changes; "Update R1 fields." lines are only counted.
                std::vector<std::string> problems, changes;
                int                      fieldUpdates = 0;
                size_t                   from = 0;

                while( from < r.report().size() )
                {
                    size_t      to = r.report().find( '\n', from );
                    std::string line = r.report().substr( from, to == std::string::npos ? std::string::npos : to - from );
                    from = to == std::string::npos ? r.report().size() : to + 1;

                    // routine bookkeeping of new parts: the summary line above says it
                    static const char* routine[] = { "Processing symbol", "Updated ", "Changed REF**",
                                                     "Connected ", "Add net ", "Added ", "Total warnings" };

                    if( line.empty() || std::any_of( std::begin( routine ), std::end( routine ),
                                                     [&]( const char* r ) { return line.rfind( r, 0 ) == 0; } ) )
                        continue;

                    if( line.rfind( "error: ", 0 ) == 0 || line.rfind( "warning: ", 0 ) == 0 )
                        problems.push_back( line );
                    else if( line.rfind( "Update ", 0 ) == 0 && line.size() > 8
                             && line.compare( line.size() - 8, 8, " fields." ) == 0 )
                        fieldUpdates++;
                    else
                        changes.push_back( line );
                }

                const size_t limit = a.value( "detail", false ) ? SIZE_MAX : 25;
                size_t       shown = 0, more = 0;

                for( const std::vector<std::string>* list : { &problems, &changes } )
                {
                    for( const std::string& line : *list )
                    {
                        if( shown < limit || list == &problems )
                        {
                            t += "\n  " + line;
                            shown++;
                        }
                        else
                        {
                            more++;
                        }
                    }
                }

                if( fieldUpdates )
                    t += "\n  " + std::string( req.dry_run() ? "would update" : "updated" ) + " the fields of "
                         + std::to_string( fieldUpdates ) + " footprint(s)";

                if( more )
                    t += "\n  ... " + std::to_string( more ) + " more line(s) (detail=true lists all)";

                if( !r.history_label().empty() )
                    t += "\nundo takes all of it back: \"" + r.history_label() + "\"";

                // a dry run's errors are what it found, not a failure of the call
                return TOOL_RESULT{ t, r.errors() > 0 && !req.dry_run() };
            } } );

    aTools.push_back( { "auto_place",
            "Place parts automatically inside the board outline: each part goes near what it "
            "connects to (decoupling caps and pull-ups next to their IC pin), courtyards kept apart, "
            "rotations tried, as one undoable edit. Default: the parts outside the board (new from "
            "sync_pcb); refs for chosen parts, all for every unlocked part; fixed keeps parts where "
            "they are (place connectors first, then lock or fix them). Functional blocks are kept "
            "together: from groups, else the parts' Block field (sch_add_part block); groups "
            "\"sheets\" uses the schematic sheets, false none. Keeping blocks together costs some "
            "wire length (group_weight trades one for the other). Bottom side: back lists parts that "
            "may go there, back_decaps lets decoupling caps go under their IC pin (SMD only; off by "
            "default: two-sided assembly costs more). Reports wire length before/after, how mixed "
            "the blocks are and how far the small passives (decaps, pull-ups) are from their IC pin. "
            "Existing tracks and vias are obstacles: on a routed board fewer parts find room, so "
            "place before routing (right after sync_pcb), or delete_tracks first. Runs beside "
            "the editor (it stays responsive); if the board is changed meanwhile nothing is moved. "
            "When every part is already placed (no overlaps, no rules), a result that is not better "
            "than the current placement is not applied (force applies it anyway). Moving parts "
            "leaves the board's tracks behind and copper zones in their old shape: refill_zones "
            "before run_drc.",
            obj( { { "refs", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                   { "all", { { "type", "boolean" } } },
                   { "fixed", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                   { "spacing", { { "type", "number" }, { "description", "courtyard gap, mm (default 0.25)" } } },
                   { "edge", { { "type", "number" }, { "description", "courtyard to board edge, mm (default 0.5)" } } },
                   { "rotate", { { "type", "boolean" }, { "description", "try 90/180/270 (default true)" } } },
                   { "groups", { { "description", "{\"LDO\": [\"U2\", \"C3\"], ...} blocks to keep together, "
                                                   "\"sheets\", or false: no blocks" } } },
                   { "group_weight", { { "type", "number" },
                                       { "description", "pull to the block's centre, 1 = like a two-pin net (default 3)" } } },
                   { "back", { { "type", "array" }, { "items", { { "type", "string" } } },
                               { "description", "parts that may go to the bottom side, e.g. [\"C*\", \"R12\"]" } } },
                   { "back_decaps", { { "type", "boolean" },
                                      { "description", "decoupling caps may go to the bottom, under their IC pin" } } },
                   { "time_limit", { { "type", "number" },
                                     { "description", "seconds for improving the wire length after every part "
                                                      "has a legal spot (default 10, -1 no limit); the "
                                                      "placement is legal either way" } } },
                   { "force", { { "type", "boolean" },
                                { "description", "apply the result even when the current placement is better" } } },
                   { "measure_only", { { "type", "boolean" },
                                       { "description", "move nothing, report the numbers for the parts as they are" } } },
                   { "detail", { { "type", "boolean" }, { "description", "list every moved part (default: the first 15)" } } },
                   { "rules", { { "type", "object" },
                                { "description",
                                  "placement rules; refs may be patterns (\"J*\"). edge: {\"J1\": \"left\"} "
                                  "(left/right/top/bottom/any: the courtyard at that board edge); region: "
                                  "{\"U1\": [x0, y0, x1, y1]} (mm, the courtyard inside); rotation: "
                                  "{\"J1\": 90}; near: [{\"refs\": [\"Y1\", \"U1\"], \"max\": 5}] (mm "
                                  "between the centres of their pads). edge/region/rotation are hard, near is reported when "
                                  "it can't be met" } } } } ),
            [&aIpc]( const json& a )
            {
                kicadd::board::AutoPlace req;

                if( a.contains( "groups" ) )
                {
                    const json& g = a["groups"];

                    if( g.is_boolean() )
                    {
                        req.set_no_groups( !g.get<bool>() );
                    }
                    else if( g.is_string() && g.get<std::string>() == "sheets" )
                    {
                        req.set_groups_from_sheets( true );
                    }
                    else if( g.is_object() )
                    {
                        for( const auto& [name, refs] : g.items() )
                        {
                            kicadd::board::PartGroup* pg = req.add_groups();
                            pg->set_name( name );

                            for( const json& r : refs )
                                if( r.is_string() )
                                    pg->add_refs( r.get<std::string>() );
                        }
                    }
                    else
                    {
                        return fail( "groups: an object {block: [refs]}, \"sheets\" or false" );
                    }
                }

                if( a.contains( "group_weight" ) && a["group_weight"].is_number() )
                    req.set_group_weight( a["group_weight"].get<double>() );

                for( const json& r : a.value( "back", json::array() ) )
                    if( r.is_string() )
                        req.add_back( r.get<std::string>() );

                req.set_back_decaps( a.value( "back_decaps", false ) );
                req.set_measure_only( a.value( "measure_only", false ) );
                req.set_force( a.value( "force", false ) );

                if( a.contains( "time_limit" ) && a["time_limit"].is_number() )
                    req.set_time_limit( a["time_limit"].get<double>() );

                if( a.contains( "rules" ) )
                {
                    const json& rules = a["rules"];

                    if( !rules.is_object() )
                        return fail( "rules: an object with edge, region, rotation, near" );

                    // (named: items() of a temporary would dangle in the loop)
                    const json edge = rules.value( "edge", json::object() );
                    const json region = rules.value( "region", json::object() );
                    const json rotation = rules.value( "rotation", json::object() );
                    const json nearRules = rules.value( "near", json::array() );

                    if( !edge.is_object() || !region.is_object() || !rotation.is_object() || !nearRules.is_array() )
                        return fail( "rules: edge, region, rotation are objects (ref -> ...), near a list" );

                    for( const auto& [ref, side] : edge.items() )
                    {
                        kicadd::board::EdgeRule* r = req.add_edge_rules();
                        r->set_ref( ref );
                        r->set_side( side.is_string() ? side.get<std::string>() : "" );
                    }

                    for( const auto& [ref, box] : region.items() )
                    {
                        if( !box.is_array() || box.size() != 4 || !std::all_of( box.begin(), box.end(),
                                                                               []( const json& v ) { return v.is_number(); } ) )
                            return fail( "region rule " + ref + ": [x0, y0, x1, y1] in mm" );

                        kicadd::board::RegionRule* r = req.add_region_rules();
                        r->set_ref( ref );
                        r->mutable_min()->set_x( std::llround( box[0].get<double>() * 1e6 ) );
                        r->mutable_min()->set_y( std::llround( box[1].get<double>() * 1e6 ) );
                        r->mutable_max()->set_x( std::llround( box[2].get<double>() * 1e6 ) );
                        r->mutable_max()->set_y( std::llround( box[3].get<double>() * 1e6 ) );
                    }

                    for( const auto& [ref, deg] : rotation.items() )
                    {
                        kicadd::board::RotationRule* r = req.add_rotation_rules();
                        r->set_ref( ref );
                        r->set_degrees( deg.is_number() ? deg.get<int>() : 1 );
                    }

                    for( const json& near : nearRules )
                    {
                        if( !near.is_object() || !near.contains( "refs" ) || !near.contains( "max" ) || !near["max"].is_number() )
                            return fail( "near rule: {\"refs\": [...], \"max\": mm}" );

                        kicadd::board::NearRule* r = req.add_near_rules();
                        r->set_max_distance( std::llround( near["max"].get<double>() * 1e6 ) );

                        for( const json& ref : near["refs"] )
                            if( ref.is_string() )
                                r->add_refs( ref.get<std::string>() );
                    }
                }

                for( const json& r : a.value( "refs", json::array() ) )
                    if( r.is_string() )
                        req.add_refs( r.get<std::string>() );

                for( const json& r : a.value( "fixed", json::array() ) )
                    if( r.is_string() )
                        req.add_fixed( r.get<std::string>() );

                req.set_all( a.value( "all", false ) );
                req.set_no_rotate( !a.value( "rotate", true ) );

                if( a.contains( "spacing" ) && a["spacing"].is_number() )
                    req.set_spacing( (int64_t) std::llround( a["spacing"].get<double>() * 1e6 ) );

                if( a.contains( "edge" ) && a["edge"].is_number() )
                    req.set_edge( (int64_t) std::llround( a["edge"].get<double>() * 1e6 ) );

                kicadd::board::AutoPlaceResponse r;
                std::string                      err;

                // On another thread in the editor (it stays responsive; a long run used to freeze
                // it and time out every call queued behind it); ask until it's done.
                req.set_background( true );

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                const auto t0 = std::chrono::steady_clock::now();
                const double limit = req.time_limit() > 0 ? req.time_limit() : 10.0;
                const double giveUp = ( req.time_limit() < 0 ? 600.0 : limit + 300.0 );
                bool         cancelSent = false;
                std::string  lastProgress;

                while( r.running() )
                {
                    lastProgress = r.progress();
                    std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
                    const double waited = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();

                    kicadd::board::AutoPlaceStatus st;

                    if( waited > giveUp && !cancelSent )
                    {
                        st.set_cancel( true );
                        cancelSent = true;
                    }

                    kicadd::board::AutoPlaceResponse next;

                    if( !aIpc.Call( st, next, err ) )
                    {
                        // done, but the user is routing / in a dialog: it is applied when they finish
                        if( err.find( "interactive tool" ) != std::string::npos && waited < giveUp )
                            continue;

                        return fail( err );
                    }

                    r = std::move( next );
                }

                if( r.cancelled() )
                    return fail( "auto_place gave up after " + num( giveUp ) + " s (" + lastProgress
                                 + "); nothing was moved" );

                std::string t = a.value( "measure_only", false ) ? std::string( "measured (nothing moved)" )
                                : r.kept_current() ? std::string( "kept the current placement (nothing moved): the new one "
                                                                  "was not better" )
                                                   : "placed " + std::to_string( r.moved() ) + " part(s)";

                if( r.moved() )
                {
                    t += ":";
                    const bool detail = a.value( "detail", false );
                    int        n = 0;

                    for( const std::string& ref : r.refs() )
                    {
                        if( !detail && n++ >= 15 )
                        {
                            t += " ... (" + std::to_string( r.refs_size() ) + " in all; detail=true lists them)";
                            break;
                        }

                        t += " " + ref;
                    }
                }

                t += std::string( r.kept_current() ? "; wire length (half-perimeter) now " : "; wire length (half-perimeter) " )
                     + num( r.hpwl_before_mm() ) + ( r.kept_current() ? ", new one " : " -> " ) + num( r.hpwl_after_mm() )
                     + " mm";

                if( r.decaps() > 0 )
                    t += "; " + std::to_string( r.decaps() ) + " passive(s) tied to an IC pin "
                         + num( r.decap_after_mm() ) + " mm from it on average"
                         + ( r.decap_before_mm() > 0 ? " (before " + num( r.decap_before_mm() ) + ")" : "" );

                if( r.on_back() > 0 || r.flipped() > 0 )
                    t += "; " + std::to_string( r.on_back() ) + " on the bottom side ("
                         + std::to_string( r.flipped() ) + " changed side)";

                if( r.groups() > 0 )
                    t += "; " + std::to_string( r.groups() ) + " blocks (" + r.group_source() + "), "
                         + std::to_string( r.mixed() ) + " part(s) with their nearest neighbour in another block (before "
                         + std::to_string( r.mixed_before() ) + ")";

                if( !r.edit().history_label().empty() )
                    t += "; undo: \"" + r.edit().history_label() + "\"";

                if( r.timed_out() )
                    t += "\n  stopped improving at the time limit (" + num( r.seconds() ) + " s, "
                         + std::to_string( r.refine_passes() ) + " pass(es)): the placement is legal; "
                         "time_limit gives it longer";

                if( r.unplaced_size() )
                {
                    t += "\n  no room for:";

                    for( const std::string& ref : r.unplaced() )
                        t += " " + ref;

                    t += r.kept_current() ? " (in the new one)" : " (left where they were; the others keep clear of them)";

                    if( r.track_obstacles() > 0 )
                        t += "\n  the board's " + std::to_string( r.track_obstacles() )
                             + " tracks/vias are obstacles: delete_tracks all=true first, or place right "
                               "after sync_pcb";
                }

                if( r.kept_current() )
                    t += "\n  force=true applies it anyway";

                if( r.moved() > 0 && ( r.track_obstacles() > 0 || r.zones() > 0 ) )
                {
                    t += "\n  the moved parts left";

                    if( r.track_obstacles() > 0 )
                        t += " their tracks behind (reroute, or delete_tracks refs=[...])";

                    if( r.zones() > 0 )
                        t += std::string( r.track_obstacles() > 0 ? " and" : "" ) + " the "
                             + std::to_string( r.zones() ) + " copper zone(s) in their old shape: "
                             "refill_zones before run_drc";
                }

                if( r.rule_violations_size() )
                {
                    t += "\n  rules not met:";

                    for( const std::string& v : r.rule_violations() )
                        t += "\n    " + v;
                }

                if( r.skipped_size() )
                {
                    t += "\n  kept:";

                    for( const std::string& ref : r.skipped() )
                        t += " " + ref;
                }

                return TOOL_RESULT{ t, r.moved() == 0 && r.unplaced_size() > 0 && !r.kept_current()
                                               && !a.value( "measure_only", false ) };
            } } );

    aTools.push_back( { "sch_move",
            "Move and/or rotate a placed part on the schematic: x/y (mm, the new anchor) or dx/dy, "
            "rotation 0/90/180/270. Its wire stubs, net labels, power symbols and no-connect flags go "
            "with it (re-drawn on the side each pin now faces), so its connections stay; other wires "
            "ending on its pins are dragged along. unit picks one unit of a multi-unit part (needed "
            "with x/y; dx/dy moves all units). Returns the part's pins at their new place.",
            obj( { { "ref", { { "type", "string" } } },
                   { "x", { { "type", "number" } } },
                   { "y", { { "type", "number" } } },
                   { "dx", { { "type", "number" } } },
                   { "dy", { { "type", "number" } } },
                   { "rotation", { { "type", "integer" }, { "enum", { 0, 90, 180, 270 } } } },
                   { "unit", { { "type", "integer" } } } },
                 { "ref" } ),
            [&aIpc]( const json& a )
            {
                ks::SchMove req;
                req.set_ref( str( a, "ref" ) );
                req.set_unit( a.value( "unit", 0 ) );

                if( a.contains( "x" ) && a["x"].is_number() )
                    req.set_x( a["x"].get<double>() );

                if( a.contains( "y" ) && a["y"].is_number() )
                    req.set_y( a["y"].get<double>() );

                if( a.contains( "dx" ) && a["dx"].is_number() )
                    req.set_dx( a["dx"].get<double>() );

                if( a.contains( "dy" ) && a["dy"].is_number() )
                    req.set_dy( a["dy"].get<double>() );

                if( a.contains( "rotation" ) && a["rotation"].is_number() )
                    req.set_rotation( a["rotation"].get<int>() );

                if( !req.has_x() && !req.has_y() && req.dx() == 0 && req.dy() == 0 && !req.has_rotation() )
                    return fail( "say where: x/y, dx/dy or rotation" );

                ks::SchPartResponse r;
                std::string         err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = "moved " + symbolLine( r.symbol() ) + pinLines( r.symbol() );

                if( !r.note().empty() )
                    t += "\n  " + r.note();

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_wire",
            "Draw a wire through points [[x, y], ...] (mm, snapped to the 1.27 mm grid), one segment "
            "per pair; give corner points for right angles. A point on a pin, label or wire connects "
            "there: junction dots are added where three or more meet and wires it lands on are split. "
            "Use it for readable schematics; sch_connect (stub + label) needs no routing. Returns the "
            "net the wire is on and its pins.",
            obj( { { "points", { { "type", "array" },
                                 { "items", { { "type", "array" }, { "items", { { "type", "number" } } } } } } },
                   { "sheet", { { "type", "string" } } } },
                 { "points" } ),
            [&aIpc]( const json& a )
            {
                ks::SchWire req;
                req.set_sheet( str( a, "sheet" ) );

                for( const json& p : a.value( "points", json::array() ) )
                {
                    if( !p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number() )
                        return fail( "points: [[x, y], [x, y], ...] in mm" );

                    ks::SchPoint* pt = req.add_points();
                    pt->set_x( p[0].get<double>() );
                    pt->set_y( p[1].get<double>() );
                }

                ks::SchWireResponse r;
                std::string         err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = std::to_string( r.segments() ) + " segment(s), " + std::to_string( r.junctions() )
                                + " junction(s)";

                if( !r.net().empty() )
                {
                    t += "; net " + r.net() + ":";

                    for( const std::string& p : r.pins() )
                        t += " " + p;
                }

                if( !r.note().empty() )
                    t += "\n  " + r.note();

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "run_erc",
            "Electrical Rules Check on the schematic as it is now (unsaved edits included): pins of "
            "different drivers on one net, power inputs not driven (a PWR_FLAG or a regulator output "
            "is missing), unconnected pins, labels on one pin only, duplicate references... Errors "
            "listed, warnings counted (warnings=true lists them too).",
            obj( { { "warnings", { { "type", "boolean" } } } } ),
            [&aIpc]( const json& a )
            {
                ks::SchRunErc req;
                req.set_include_warnings( a.value( "warnings", false ) );

                ks::SchErcResponse r;
                std::string        err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = "ERC: " + std::to_string( r.errors() ) + " error(s), " + std::to_string( r.warnings() )
                                + " warning(s)";

                std::map<std::string, int> byType;

                for( const ks::SchErcViolation& v : r.violations() )
                    byType[v.type()]++;

                if( !byType.empty() )
                {
                    t += " (";
                    bool first = true;

                    for( const auto& [type, n] : byType )
                    {
                        t += std::string( first ? "" : ", " ) + type + " " + std::to_string( n );
                        first = false;
                    }

                    t += ")";
                }

                int shown = 0;

                for( const ks::SchErcViolation& v : r.violations() )
                {
                    if( ++shown > 40 )
                    {
                        t += "\n  ... " + std::to_string( r.violations_size() - 40 ) + " more";
                        break;
                    }

                    t += "\n  " + v.severity() + " " + v.type() + ": " + v.description();

                    if( !v.sheet().empty() && v.sheet() != "/" )
                        t += " [" + v.sheet() + "]";

                    for( const std::string& item : v.items() )
                        t += "\n    " + item;
                }

                if( !r.ignored().empty() )
                    t += "\n  ignored by the project's ERC settings: " + std::to_string( r.ignored_size() ) + " check(s)";

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "audit",
            "Design review by rules, for you to judge (not a sign-off): decoupling (each IC power pin "
            "with a capacitor from its net to ground; on the board, how far the nearest one is, "
            "flagged beyond max_decap_mm, default 5), power (every supply net has a source: power "
            "output, regulator or connector, else a PWR_FLAG), ESD (connector nets without a TVS / ESD "
            "part) and nets with one pin only. Run run_erc too.",
            obj( { { "max_decap_mm", { { "type", "number" } } } } ),
            [&aIpc]( const json& a )
            {
                ks::SchAudit           req;
                ks::SchAuditResponse   r;
                std::string            err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                const double limit = a.contains( "max_decap_mm" ) && a["max_decap_mm"].is_number()
                                             ? a["max_decap_mm"].get<double>()
                                             : 5.0;

                // On the board: the nearest decoupling cap of each IC power pin
                std::map<std::string, std::pair<double, double>> pads;
                std::set<std::string>                            asked;
                bool                                             board = true;

                for( const ks::SchDecapPair& d : r.decaps() )
                {
                    if( !board || !asked.insert( d.net() ).second )
                        continue;

                    kicadd::board::QueryNet q;
                    q.set_net( d.net() );
                    q.set_limit( 2000 );
                    kicadd::board::QueryResponse qr;

                    if( !aIpc.Call( q, qr, err ) )
                    {
                        board = false;
                        break;
                    }

                    for( const auto& item : qr.items() )
                    {
                        if( item.kind() == kicadd::board::IK_PAD )
                            pads[item.handle()] = { item.pos().x() / 1e6, item.pos().y() / 1e6 };
                    }
                }

                std::map<std::string, std::pair<double, std::string>> nearest;   // IC pin -> mm, cap

                for( const ks::SchDecapPair& d : r.decaps() )
                {
                    auto a1 = pads.find( d.ic() ), a2 = pads.find( d.cap() );

                    if( a1 == pads.end() || a2 == pads.end() )
                        continue;

                    double dist = std::hypot( a1->second.first - a2->second.first, a1->second.second - a2->second.second );
                    auto   it = nearest.find( d.ic() );

                    if( it == nearest.end() || dist < it->second.first )
                        nearest[d.ic()] = { dist, d.cap().substr( 0, d.cap().find( '.' ) ) };
                }

                int warnings = 0, infos = 0;
                std::string body;
                std::map<std::string, std::string> byCat;

                for( const ks::SchAuditFinding& f : r.findings() )
                {
                    ( f.severity() == "warning" ? warnings : infos )++;
                    byCat[f.category()] += "\n  " + f.severity() + ": " + f.message();
                }

                std::string far;
                int         farCount = 0;

                for( const auto& [pin, d] : nearest )
                {
                    if( d.first > limit )
                    {
                        farCount++;
                        far += "\n  warning: " + pin + ": nearest decoupling cap " + d.second + " is "
                               + num( d.first ) + " mm away (pad to pad; max " + num( limit ) + ")";
                    }
                }

                warnings += farCount;

                std::string t = "audit: " + std::to_string( warnings ) + " warning(s), " + std::to_string( infos )
                                + " note(s); " + std::to_string( r.ics() ) + " IC(s), " + std::to_string( r.power_nets() )
                                + " supply net(s), " + std::to_string( r.connectors() ) + " connector(s)";

                for( const char* cat : { "decoupling", "power", "esd", "net" } )
                {
                    std::string extra = std::string( cat ) == "decoupling" ? far : "";

                    if( byCat.count( cat ) || !extra.empty() )
                        t += "\n" + std::string( cat ) + ":" + byCat[cat] + extra;
                }

                if( !nearest.empty() )
                {
                    double worst = 0;

                    for( const auto& [pin, d] : nearest )
                        worst = std::max( worst, d.first );

                    t += "\non the board: " + std::to_string( nearest.size() ) + " IC power pin(s) measured, farthest "
                         "decoupling cap " + num( worst ) + " mm";
                }
                else if( r.decaps_size() > 0 )
                {
                    t += "\nnot on a board yet (sync_pcb) or no board open: cap distances not measured";
                }

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "lib_create_symbol",
            "Make a box symbol from a pin list and save it to the project library \"kicadd\" "
            "(kicadd.kicad_sym, added to sym-lib-table on first use); then sch_add_part lib_id "
            "\"kicadd:<name>\". pins: [{number, name, type, side}], type input / output / "
            "bidirectional / tri_state / passive / power_in / power_out / open_collector / no_connect "
            "(default passive), side left / right / top / bottom (default: inputs and power inputs left, "
            "the rest right; in the order given, top to bottom). Prefer lcsc_fetch / sch_add_part lcsc "
            "when the part is at LCSC.",
            obj( { { "name", { { "type", "string" } } },
                   { "reference", { { "type", "string" }, { "description", "prefix, default U" } } },
                   { "value", { { "type", "string" } } },
                   { "footprint", { { "type", "string" } } },
                   { "datasheet", { { "type", "string" } } },
                   { "description", { { "type", "string" } } },
                   { "lcsc", { { "type", "string" } } },
                   { "pins", { { "type", "array" },
                               { "items", obj( { { "number", { { "type", "string" } } },
                                                 { "name", { { "type", "string" } } },
                                                 { "type", { { "type", "string" } } },
                                                 { "side", { { "type", "string" } } },
                                                 { "hidden", { { "type", "boolean" } } } },
                                               { "number" } ) } } },
                   { "replace", { { "type", "boolean" } } } },
                 { "name", "pins" } ),
            [&aIpc]( const json& a )
            {
                ks::SchCreateSymbol req;
                req.set_name( str( a, "name" ) );
                req.set_reference( str( a, "reference" ) );
                req.set_value( str( a, "value" ) );
                req.set_footprint( str( a, "footprint" ) );
                req.set_datasheet( str( a, "datasheet" ) );
                req.set_description( str( a, "description" ) );
                req.set_lcsc( str( a, "lcsc" ) );
                req.set_replace( a.value( "replace", false ) );

                for( const json& p : a.value( "pins", json::array() ) )
                {
                    if( !p.is_object() )
                        return fail( "pins: [{\"number\": \"1\", \"name\": \"VCC\", \"type\": \"power_in\"}, ...]" );

                    ks::SchSymbolPin* pin = req.add_pins();
                    pin->set_number( p.contains( "number" ) && p["number"].is_number() ? std::to_string( p["number"].get<int>() )
                                                                                       : str( p, "number" ) );
                    pin->set_name( str( p, "name" ) );
                    pin->set_type( str( p, "type" ) );
                    pin->set_side( str( p, "side" ) );
                    pin->set_hidden( p.value( "hidden", false ) );
                }

                ks::SchCreateSymbolResponse r;
                std::string                 err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = "made " + r.lib_id() + ": " + std::to_string( r.pins() ) + " pins, body "
                                + num( r.width() ) + " x " + num( r.height() ) + " mm, in " + r.file()
                                + ( r.registered() ? " (kicadd added to sym-lib-table)" : "" );

                if( !r.note().empty() )
                    t += "\n  " + r.note();

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "lib_create_footprint",
            "Make a footprint for a common package from a few numbers, saved to the project library "
            "\"kicadd\" (kicadd.pretty, added to fp-lib-table on first use): kind chip (size 0201..2512, "
            "prefix R/C/L/LED), sot23 (pins 3/5/6), soic (pins, pitch 1.27, body 3.9 or 7.5), so (pins, "
            "pitch, body, span: TSSOP / MSOP), qfn (pins, pitch, body, ep), qfp (pins, pitch, body, span), "
            "header (pins, rows 1/2, pitch). Pads by IPC-7351-like margins on nominal leads; check them "
            "against the datasheet's land pattern. Odd shapes: the Footprint Editor.",
            obj( { { "kind", { { "type", "string" } } },
                   { "size", { { "type", "string" } } },
                   { "prefix", { { "type", "string" } } },
                   { "pins", { { "type", "integer" } } },
                   { "pitch", { { "type", "number" } } },
                   { "body", { { "type", "number" } } },
                   { "span", { { "type", "number" } } },
                   { "ep", { { "type", "number" } } },
                   { "rows", { { "type", "integer" } } },
                   { "name", { { "type", "string" } } },
                   { "replace", { { "type", "boolean" } } } },
                 { "kind" } ),
            [&aIpc]( const json& a )
            {
                kicadd::board::FpCreate req;
                req.set_kind( str( a, "kind" ) );
                req.set_size( a.contains( "size" ) && a["size"].is_number() ? std::to_string( a["size"].get<int>() )
                                                                            : str( a, "size" ) );

                if( req.size().size() == 3 )     // 402 -> 0402
                    req.set_size( "0" + req.size() );

                req.set_prefix( str( a, "prefix" ) );
                req.set_pins( a.value( "pins", 0 ) );
                req.set_pitch( a.value( "pitch", 0.0 ) );
                req.set_body( a.value( "body", 0.0 ) );
                req.set_span( a.value( "span", 0.0 ) );
                req.set_ep( a.value( "ep", 0.0 ) );
                req.set_rows( a.value( "rows", 1 ) );
                req.set_name( str( a, "name" ) );
                req.set_replace( a.value( "replace", false ) );

                kicadd::board::FpCreateResponse r;
                std::string                     err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = "made " + r.lib_id() + ": " + std::to_string( r.pads() ) + " pads, courtyard "
                                + num( r.courtyard().x() / 1e6 ) + " x " + num( r.courtyard().y() / 1e6 ) + " mm"
                                + ( r.registered() ? " (kicadd added to fp-lib-table)" : "" )
                                + "; use it as footprint \"" + r.lib_id() + "\"";

                if( !r.note().empty() )
                    t += "\n  " + r.note();

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "lib_register",
            "Add a library that is already on disk to the project: a symbol library (.kicad_sym) to "
            "sym-lib-table, a footprint library (.pretty folder) to fp-lib-table, so sch_add_part / "
            "add_footprint can use \"Nickname:Item\". path absolute or relative to the project; nickname "
            "defaults to the file name.",
            obj( { { "path", { { "type", "string" } } }, { "nickname", { { "type", "string" } } } }, { "path" } ),
            [&aIpc]( const json& a )
            {
                ks::RegisterLibrary req;
                req.set_path( str( a, "path" ) );
                req.set_nickname( str( a, "nickname" ) );

                ks::RegisterLibraryResponse r;
                std::string                 err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = std::string( r.added() ? "added " : "" ) + r.nickname() + " -> " + r.uri() + " in "
                                + r.table() + " (" + std::to_string( r.items() )
                                + ( r.table() == "sym-lib-table" ? " symbol(s))" : " footprint(s))" );

                if( !r.note().empty() )
                    t += "\n  " + r.note();

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_set_fields",
            "Change parts' fields and attributes as one edit (all units; sch_undo takes it back; "
            "sync_pcb carries it to the board). parts: {\"R1\": {\"Value\": \"10k\", \"LCSC\": "
            "\"C25804\", \"Footprint\": \"Lib:Name\", \"dnp\": true}}, or refs [...] with set {...} "
            "for the same change on many. Value, Footprint, Datasheet, Description are the symbol's own "
            "fields; any other name is a user field (added hidden when missing; null removes it). "
            "Attributes: dnp, exclude_from_bom, exclude_from_board, exclude_from_sim, exclude_from_pos "
            "(true / false). Refs that are not found are listed; the other parts are changed anyway.",
            obj( { { "parts", { { "type", "object" }, { "additionalProperties", { { "type", "object" } } } } },
                   { "refs", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                   { "set", { { "type", "object" } } } },
                 {} ),
            [&aIpc]( const json& a )
            {
                ks::SchSetFields req;
                std::string      bad;

                auto add = [&]( const std::string& aRef, const json& aSet )
                {
                    ks::SchFieldEdit* e = req.add_parts();
                    e->set_ref( aRef );

                    for( const auto& [key, v] : aSet.items() )
                    {
                        if( key == "dnp" || key.rfind( "exclude_from_", 0 ) == 0 )
                        {
                            if( !v.is_boolean() )
                            {
                                bad = key + " is true or false";
                                continue;
                            }

                            bool b = v.get<bool>();

                            if( key == "dnp" )                          e->set_dnp( b );
                            else if( key == "exclude_from_bom" )        e->set_exclude_from_bom( b );
                            else if( key == "exclude_from_board" )      e->set_exclude_from_board( b );
                            else if( key == "exclude_from_sim" )        e->set_exclude_from_sim( b );
                            else if( key == "exclude_from_pos" )        e->set_exclude_from_pos( b );
                            else                                        bad = "unknown attribute " + key;
                        }
                        else if( v.is_null() )
                        {
                            e->add_remove( key );
                        }
                        else if( v.is_string() )
                        {
                            ( *e->mutable_fields() )[key] = v.get<std::string>();
                        }
                        else if( v.is_number() )
                        {
                            ( *e->mutable_fields() )[key] = v.dump();
                        }
                        else
                        {
                            bad = key + ": a field's text is a string (null removes it)";
                        }
                    }
                };

                if( a.contains( "parts" ) && a["parts"].is_object() )
                {
                    for( const auto& [ref, set] : a["parts"].items() )
                    {
                        if( set.is_object() )
                            add( ref, set );
                    }
                }

                if( a.contains( "set" ) && a["set"].is_object() )
                {
                    for( const json& r : a.value( "refs", json::array() ) )
                    {
                        if( r.is_string() )
                            add( r.get<std::string>(), a["set"] );
                    }
                }

                if( !bad.empty() )
                    return fail( bad );

                if( req.parts().empty() )
                    return fail( "give parts {ref: {field: text}} or refs [...] with set {...}" );

                ks::SchConnectResponse r;
                std::string            err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                return connectResult( r, "part(s) changed" );
            } } );

    aTools.push_back( { "sch_render",
            "A picture (PNG) of one schematic sheet as it is now, to see what the coordinates do not "
            "show: overlapping parts or text, crossing wires, labels away from their wires, how the "
            "sheet reads. KiCad's own plot. refs are framed and labelled, nets drawn bright (wires, "
            "labels, pins); with either it zooms to them (zoom=false keeps the sheet) and picks the "
            "sheet of the first ref. sheet: a name or path (default the root); area [x0, y0, x1, y1] "
            "mm; frame=true the whole page with its title block. mm rulers on the top and left. size: "
            "pixels on the long side (default 1600). path also saves the PNG (relative: the project "
            "folder).",
            obj( { { "sheet", { { "type", "string" } } },
                   { "refs", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                   { "nets", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                   { "area", { { "type", "array" }, { "items", { { "type", "number" } } },
                               { "description", "[x0, y0, x1, y1] mm" } } },
                   { "zoom", { { "type", "boolean" } } },
                   { "frame", { { "type", "boolean" } } },
                   { "size", { { "type", "integer" } } },
                   { "margin", { { "type", "number" }, { "description", "mm around a zoom (5)" } } },
                   { "path", { { "type", "string" } } } } ),
            [&aIpc]( const json& a )
            {
                ks::SchRender req;
                req.set_sheet( str( a, "sheet" ) );

                for( const char* key : { "refs", "nets" } )
                {
                    for( const json& v : a.value( key, json::array() ) )
                    {
                        if( v.is_string() )
                            ( std::string( key ) == "refs" ? req.add_refs( v.get<std::string>() )
                                                           : req.add_nets( v.get<std::string>() ) );
                    }
                }

                if( a.contains( "area" ) )
                {
                    const json& b = a["area"];

                    if( !b.is_array() || b.size() != 4
                        || !std::all_of( b.begin(), b.end(), []( const json& v ) { return v.is_number(); } ) )
                        return fail( "area is [x0, y0, x1, y1] in mm" );

                    for( const json& v : b )
                        req.add_area( v.get<double>() );
                }

                req.set_zoom( a.value( "zoom", req.refs_size() + req.nets_size() > 0 ) );
                req.set_frame( a.value( "frame", false ) );
                req.set_size( a.value( "size", 0 ) );
                req.set_margin( a.value( "margin", 0.0 ) );
                req.set_path( str( a, "path" ) );

                ks::SchRenderResponse r;
                std::string           err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                std::string t = "picture of " + r.sheet() + " " + std::to_string( r.width() ) + "x"
                                + std::to_string( r.height() ) + " px, " + num( r.mm_per_px() * 1000 ) + " um/px; x "
                                + num( r.area( 0 ) ) + " .. " + num( r.area( 2 ) ) + ", y " + num( r.area( 1 ) )
                                + " .. " + num( r.area( 3 ) ) + " mm (rulers on the top and left)";

                if( req.refs_size() )
                    t += "\nmagenta frames: the parts asked for";

                if( req.nets_size() )
                    t += "\nblue: the nets asked for (wires, labels, pins)";

                for( const std::string& e : r.elsewhere() )
                    t += "\nelsewhere: " + e;

                if( !r.missing().empty() )
                {
                    t += "\nnot found:";

                    for( const std::string& m : r.missing() )
                        t += " " + m;
                }

                if( !r.path().empty() )
                    t += "\nsaved: " + r.path();

                TOOL_RESULT out{ t, false };
                out.image = r.png();
                return out;
            } } );

    aTools.push_back( { "lcsc_match",
            "Find LCSC / JLCPCB numbers for the schematic's parts that have none, many at once: parts of "
            "the same value and footprint are one group; resistors, capacitors, inductors, ferrite beads "
            "(FB) and LEDs (colour in the value) in chip packages (0402, 0603, ...) are matched in the "
            "parts index (basic first, then preferred, then stock; the chosen one checked live), the "
            "others are listed. Dry run first: apply=true writes the LCSC fields as one sch_undo. pick "
            "{\"R1\": \"C25804\"} sets the part for R1's group instead (also for a group not matched). "
            "The value may carry the rating: \"100nF 50V X7R\", \"10k 1%\". refs limits it to some "
            "parts; replace=true matches parts that have a number already. Then sync_pcb.",
            obj( { { "apply", { { "type", "boolean" }, { "description", "write them (default: dry run)" } } },
                   { "pick", { { "type", "object" }, { "additionalProperties", { { "type", "string" } } },
                               { "description", "ref -> LCSC number for that ref's group" } } },
                   { "refs", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                   { "basic_only", { { "type", "boolean" } } },
                   { "replace", { { "type", "boolean" } } },
                   { "live", { { "type", "boolean" }, { "description", "check today's stock (default true)" } } } } ),
            [&aIpc]( const json& a ) { return lcscMatch( aIpc, a ); } } );

    for( bool redo : { false, true } )
    {
        aTools.push_back( { redo ? "sch_redo" : "sch_undo",
                redo ? "Redo the agent's last undone schematic edit."
                     : "Undo the agent's last schematic edit (sch_add_part, sch_connect, sch_wire, sch_move, "
                       "sch_delete, ...; not the user's). What the user changed since is left as it is and "
                       "counted; an edit the user already undid in the editor (Ctrl+Z) is said so. The "
                       "agent keeps its last 50 edits; PCB edits have undo / redo.",
                obj( json::object() ),
                [&aIpc, redo]( const json& )
                {
                    ks::SchUndo req;
                    req.set_redo( redo );
                    ks::SchHistoryResponse r;
                    std::string            err;

                    if( !aIpc.Call( req, r, err ) )
                        return fail( err );

                    const std::string verb = redo ? "redo" : "undo";
                    std::string       t;

                    if( r.label().empty() )
                        t = "nothing to " + verb;
                    else if( r.ok() )
                        t = std::string( redo ? "redid: " : "undid: " ) + r.label() + " ("
                            + std::to_string( r.items() ) + " item(s))";
                    else if( r.changed() == 0 )
                        t = "\"" + r.label() + "\" was already " + ( redo ? "redone" : "undone" )
                            + " (in the editor); nothing changed";
                    else
                        t = "did not " + verb + " \"" + r.label() + "\": its items were changed since";

                    if( r.ok() && r.changed() )
                        t += "; " + std::to_string( r.changed() ) + " item(s) changed since, left as they are";

                    if( r.ok() && r.already() )
                        t += "; " + std::to_string( r.already() ) + " already as they were";

                    t += "\nschematic history: " + std::to_string( r.undo_size() ) + " undo, "
                         + std::to_string( r.redo_size() ) + " redo";

                    if( !r.next_undo().empty() )
                        t += "; next undo: " + r.next_undo();

                    return TOOL_RESULT{ t, false };
                } } );
    }

    aTools.push_back( { "sch_save",
            "Write the schematic to its .kicad_sch file(s), every sheet. They are written in this "
            "build's file format (KiCad nightly, 10.99): files from KiCad 9 / 10 are upgraded and "
            "those versions may not open them any more (as when KiCad itself saves them).",
            obj( json::object() ),
            [&aIpc]( const json& )
            {
                ks::SchSave            req;
                ks::SchConnectResponse r;
                std::string            err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                return TOOL_RESULT{ r.history_label(), false };
            } } );
}

} // namespace kicadd::mcp
