/*
 * KiCadD MCP - M5a schematic tools (proto/kicadd/schematic.proto).
 *
 * Text answers like the board tools: one line per part / net, mm, "REF.PIN" pin names.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "ipc_client.h"

#include <cstdio>

#include <kicadd/schematic.pb.h>

using json = nlohmann::json;
namespace ks = kicadd::schematic;

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

} // namespace


void AddSchematicTools( std::vector<TOOL>& aTools, IPC_CLIENT& aIpc )
{
    const json pinList = { { "type", "array" }, { "items", { { "type", "string" } } },
                           { "description", "Pins as \"REF.PIN\", e.g. [\"R1.2\", \"U1.VDD\"]" } };

    aTools.push_back( { "sch_summary",
            "The open schematic: its parts (ref, value, symbol, footprint, position), net count and "
            "unconnected pins. with_pins lists every pin with its net.",
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

                for( const ks::SchSymbol& s : r.symbols() )
                    t += "\n" + symbolLine( s ) + ( req.with_pins() ? pinLines( s ) : "" );

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_add_part",
            "Place a symbol on the schematic: from a library (lib_id \"Device:R\", "
            "\"Device:C\", ...) or an LCSC part (lcsc \"C25804\": fetched from the JLCEDA/EasyEDA "
            "Official Library, symbol + footprint saved to the project's LCSC libraries). ref \"R?\" "
            "or empty is numbered automatically. Without x/y it goes to the next free spot. Returns "
            "its pins (number, name, type, side) to connect with sch_connect.",
            obj( { { "lib_id", { { "type", "string" } } },
                   { "lcsc", { { "type", "string" } } },
                   { "ref", { { "type", "string" } } },
                   { "value", { { "type", "string" }, { "description", "e.g. 10k, 100nF" } } },
                   { "footprint", { { "type", "string" }, { "description", "Library:Footprint, overrides the symbol's" } } },
                   { "x", { { "type", "number" }, { "description", "mm" } } },
                   { "y", { { "type", "number" }, { "description", "mm, down is +" } } },
                   { "rotation", { { "type", "integer" }, { "enum", { 0, 90, 180, 270 } } } },
                   { "unit", { { "type", "integer" }, { "description", "Multi-unit parts: which unit (default 1)" } } } } ),
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
            "(sch_disconnect it first). global uses global labels (for multi-sheet designs).",
            obj( { { "nets", { { "type", "object" },
                               { "additionalProperties", pinList },
                               { "description", "net name -> pins" } } },
                   { "global", { { "type", "boolean" } } } },
                 { "nets" } ),
            [&aIpc]( const json& a )
            {
                ks::SchConnect req;
                req.set_global( a.value( "global", false ) );
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
            "Take pins off their nets (removes the wire stub and label, or the no-connect flag).",
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
            "Nets of the schematic with their pins (\"REF.PIN\"), and the pins on no net. net filters "
            "to one net.",
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

                return TOOL_RESULT{ t, false };
            } } );

    aTools.push_back( { "sch_delete",
            "Delete parts from the schematic, with their wire stubs and labels.",
            obj( { { "refs", { { "type", "array" }, { "items", { { "type", "string" } } } } } }, { "refs" } ),
            [&aIpc]( const json& a )
            {
                ks::SchDeleteParts req;

                for( const json& r : a.value( "refs", json::array() ) )
                {
                    if( r.is_string() )
                        req.add_refs( r.get<std::string>() );
                }

                ks::SchConnectResponse r;
                std::string            err;

                if( !aIpc.Call( req, r, err ) )
                    return fail( err );

                return connectResult( r, "part(s) deleted" );
            } } );

    aTools.push_back( { "sch_save",
            "Write the schematic to its .kicad_sch file.",
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
