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
#include <cstdio>
#include <map>
#include <thread>

#include <kicadd/schematic.pb.h>
#include <kicadd/board.pb.h>

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

    if( !s.sheet().empty() && s.sheet() != "/" )
        t += " on " + s.sheet();

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
            "Delete parts from the schematic, with their wire stubs and labels. A sheet's name "
            "deletes the sheet with everything on it (its file stays on disk).",
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

    aTools.push_back( { "sync_pcb",
            "Update PCB from Schematic: brings the board in line with the schematic (the open "
            "Schematic Editor's, unsaved edits included; headless: saved first) "
            "(new footprints from each symbol's Footprint field, nets on the pads, changed values). "
            "New footprints land outside the board outline until placed (auto_place). dry_run "
            "reports without changing anything; delete_extra removes footprints the schematic no "
            "longer has.",
            obj( { { "dry_run", { { "type", "boolean" } } },
                   { "delete_extra", { { "type", "boolean" } } },
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

                t += "; board now " + std::to_string( r.footprints() ) + " footprints, " + std::to_string( r.nets() )
                     + " nets; " + std::to_string( r.errors() ) + " errors, " + std::to_string( r.warnings() )
                     + " warnings";

                if( r.from_editor() )
                    t += " (from the open Schematic Editor)";

                // the updater's report, without its routine lines
                int shown = 0;
                size_t from = 0;

                while( from < r.report().size() && shown < 25 )
                {
                    size_t      to = r.report().find( '\n', from );
                    std::string line = r.report().substr( from, to == std::string::npos ? std::string::npos : to - from );
                    from = to == std::string::npos ? r.report().size() : to + 1;

                    // routine bookkeeping of new parts: the summary line above says it
                    static const char* routine[] = { "Processing symbol", "Updated ", "Changed REF**",
                                                     "Connected ", "Add net ", "Added " };

                    if( line.empty() || std::any_of( std::begin( routine ), std::end( routine ),
                                                     [&]( const char* r ) { return line.rfind( r, 0 ) == 0; } ) )
                        continue;

                    t += "\n  " + line;
                    shown++;
                }

                return TOOL_RESULT{ t, r.errors() > 0 };
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

                    for( const std::string& ref : r.refs() )
                        t += " " + ref;
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
