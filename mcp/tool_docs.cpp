/*
 * KiCadD MCP - what tools/list sends: one or two sentences per tool (what it does, when to use
 * it) and one-line parameter descriptions. Every tools/list costs the agent these tokens on each
 * turn, so the full descriptions (examples, formulas, edge cases) stay out of it: tool_help
 * returns a tool's full text, a tool's first error in a session points there, and
 * kicadd-mcp --tool-docs prints all of it (skills/kicadd/TOOLS.md is made from it).
 *
 * The full text is what each tool registers as its description (tools.cpp, sch_tools.cpp, ...);
 * ApplyBriefs moves it to TOOL::details and puts the brief here in its place.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"

#include <map>
#include <stdexcept>

using json = nlohmann::json;

namespace kicadd::mcp
{

namespace
{

const std::map<std::string, std::string> BRIEFS = {
    // the board
    { "board_summary", "Counts, copper layers, outline, via kinds and aspect ratio of the open PCB (mm, Y down), and the full unrouted count. Start here." },
    { "project", "Open a .kicad_pcb / .kicad_pro, or make a new project, on the server (headless only); refused with unsaved changes unless discard." },
    { "query_area", "Items near a part or a point, nearest first (pads with their size). Use instead of dumping the board; via_net finds free via spots." },
    { "query_net", "Pads, tracks, vias and zones of one net." },
    { "move_footprints", "Move parts by a delta or to absolute places (x, y, rotation, side), one undoable edit; returns a local DRC around them." },
    { "add_footprint", "Place a new footprint (library, LCSC number, or a copy of a part) with its pad nets, as one undoable edit." },
    { "delete_footprints", "Delete parts as one undoable edit (tracks stay)." },
    { "pcb_set_fields", "Change parts' fields, value, reference or attributes (DNP, BOM) on the board, as one undoable edit." },
    { "undo", "Undo (redo=true: redo) the agent's last board edit, or with target=\"schematic\" its last schematic edit." },
    { "board_diff", "What changed since the board was saved (or against another .kicad_pcb). Use before save_board." },
    { "board_render", "PNG of the board (layers, zoom to parts or nets, marks): to see silkscreen, crowding, routing; view=\"3d\": raytraced from any side." },
    { "launch_kicad", "Start KiCad (headless server, or the desktop PCB editor) on this kicadd-mcp's socket; refused while one answers." },
    { "close_kicad", "Stop the KiCad launch_kicad started; refused with unsaved board / schematic changes unless discard." },
    { "save_board", "Write the board to its .kicad_pcb (KiCad 10.99 format: KiCad 9 / 10 may not open it)." },
    // layout setup and rules
    { "board_outline", "Draw the board outline (rect / circle), cut-outs and mounting holes, as one undoable edit." },
    { "board_setup", "Show or set copper layers and stackup (or a JLCPCB impedance stackup)." },
    { "impedance", "Track width for a Z0 / pair for a Zdiff (or the impedance of a width) per layer, from the stackup." },
    { "set_rules", "No arguments: the net classes, patterns and minimums. Else change classes (width, clearance, via, diff pair, current, impedance), assignments or fab presets, undoable." },
    { "zones", "List, create or change copper pours." },
    { "refill_zones", "Refill all pours (before judging connectivity, DRC or fabrication)." },
    // placement and routing
    { "auto_place", "Place parts inside the outline near what they connect to, with optional rules (edges, regions, groups), undoable." },
    { "route", "Draw one connection like the interactive router (to a pad, a point, or the net's pour). Not an autorouter." },
    { "route_short", "Route the shortest unrouted connections of given parts / nets, one by one as route does, with vias where one layer won't do." },
    { "fanout", "Short track + via from SMD pads to a pour of the same net on another layer (GND / power fan-out)." },
    { "stitch_vias", "Stitching vias on a grid over a net's pours (default GND), plus one per unconnected island." },
    { "teardrops", "Add or remove KiCad teardrops on vias and pads." },
    { "delete_tracks", "Delete tracks / vias (all, by net, by part, or in an area), as one undoable edit." },
    { "silk_tidy", "Move silkscreen references off pads, other text and the board edge." },
    // checks and output
    { "run_drc", "Full-board DRC (all KiCad checks), counts by type and the violations; local=true: only around refs." },
    { "export", "Write Gerbers / drill / BOM / CPL for JLCPCB (after refill and DRC), the assembly drawing PDF and / or the schematic PDF." },
    { "audit", "Design review measurements (decoupling, power, ESD, currents, planes, diff pairs, lengths, IR drop, crosstalk, impedance); verdicts only against limits you give." },
    // parts
    { "lcsc_fetch", "Get an LCSC part's footprint, symbol and 3D model into the project libraries, with stock and price." },
    { "lcsc_search", "Find JLCPCB / LCSC parts by value, package, specs or electrical parameters." },
    { "lcsc_match", "Assign LCSC numbers to the schematic's passives in bulk (dry run, then apply)." },
    { "lib_create_symbol", "Make a box symbol from a pin list in the project library \"kicadd\"." },
    { "lib_create_footprint", "Make a footprint for a common package (chip, SOT-23, SOIC, QFN, QFP, header) in the project library." },
    { "lib_register", "Add a symbol or footprint library already on disk to the project." },
    // schematic
    { "sch_summary", "The schematic: sheets, parts, net count and unconnected pins." },
    { "sch_part", "One schematic part with its pins and their nets." },
    { "sch_netlist", "The schematic's nets with their pins, unconnected and no-connect pins." },
    { "sch_add_part", "Place a symbol (an LCSC number, or a library symbol) on the schematic." },
    { "sch_connect", "Connect pins to nets with a stub and a label (or power symbols, ports, no-connect); all or none." },
    { "sch_disconnect", "Take pins off their nets." },
    { "sch_add_sheet", "Add a hierarchical sheet (a new .kicad_sch)." },
    { "sch_delete", "Delete parts, sheets or wires from the schematic." },
    { "sch_move", "Move / rotate a schematic part; its connections go with it, and a move that would change a pin's net is refused." },
    { "sch_wire", "Draw a wire through points (for readable schematics; sch_connect needs none)." },
    { "sch_set_fields", "Change schematic parts' fields (value, footprint, LCSC, DNP ...) in one edit." },
    { "sch_annotate", "Number the parts (fill R? or renumber in position order)." },
    { "run_erc", "Electrical Rules Check of the schematic." },
    { "sch_render", "PNG of a schematic sheet, optionally zoomed to parts or nets." },
    { "sch_save", "Write the schematic files (KiCad 10.99 format: KiCad 9 / 10 may not open them)." },
    { "sync_pcb", "Update the PCB from the schematic (new footprints, nets, values)." },
    // simulation and integrity
    { "simulate", "Simulate a region of the schematic in ngspice (op, tran, ac, dc) with models from the parts' data; measurements, optional plot." },
    { "pdn", "A power rail's impedance Z(f) at its load from the layout's decoupling: peaks, the capacitors around them, bands over a target." },
    { "reflection", "Simulate a routed net's edge as transmission lines: overshoot and ringing per receiver, and a series termination to try." },
    // the rest
    { "batch", "Run several tools in one call (repeated or routine work)." },
    { "tool_help", "A tool's full description: examples, defaults, formulas, limits. Use when a short description is not enough." },
};


// one-line parameter descriptions where the full one is long ("tool.param" or "tool.obj.param";
// "*.detail" for every tool's)
const std::map<std::string, std::string> PARAM_BRIEFS = {
    { "*.detail", "list everything (default: long lists cut short)" },
    { "query_area.kinds", "default footprint, pad, track, arc, via" },
    { "auto_place.track_room", "mm pads keep from existing tracks beyond clearance" },
    { "sch_connect.partial", "connect what fits, list the pins at fault" },
    { "auto_place.escape_room", "mm kept free past fine-pitch IC pads for the escape (0 none)" },
    { "auto_place.keepout", "areas no part may go, [[x0, y0, x1, y1], ...]" },
    { "query_area.via_net", "free through-via spots of this net instead of items" },
    { "move_footprints.flip", "to the other side (mirrored)" },
    { "move_footprints.pivot", "each part around itself (default), or the group" },
    { "add_footprint.lcsc", "LCSC number, e.g. C8734" },
    { "add_footprint.nets", "{pad: net}" },
    { "add_footprint.ref", "e.g. C301, or C? for the next free" },
    { "route.from", "\"U1.5\" or [x, y]" },
    { "zones.create", "a new zone" },
    { "zones.create.layers", "default F.Cu, B.Cu; [\"all\"]: every copper layer" },
    { "impedance.layers", "default all" },
    { "run_drc.types", "only these violation types" },
    { "lcsc_search.category", "resistor, capacitor, inductor, LDO, MOSFET ..." },
    { "lcsc_search.where", "{param: \"<=20m\" | \">=30\" | \"1..2.5\"}" },
    { "board_render.layers", "front, back, copper, silk, fab, assembly, or layer names" },
    { "project.discard", "drop the open board's unsaved changes" },
    { "export.fab.strict", "write nothing on DRC or placement errors" },
    { "sch_add_part.sheet", "name or path (default the root)" },
    { "sch_connect.ports", "nets that are sheet ports: \"VOUT\" or \"VOUT:output\"" },
    { "sch_connect.power", "nets drawn as power symbols" },
    { "auto_place.back", "parts allowed on the bottom (patterns)" },
    { "auto_place.back_decaps", "decoupling caps may go under their IC" },
    { "auto_place.force", "apply even when worse than now" },
    { "auto_place.group_weight", "pull to the block's centre (default 3)" },
    { "auto_place.groups", "{name: [refs]}, \"sheets\", or false" },
    { "auto_place.measure_only", "report only, move nothing" },
    { "auto_place.rules", "edge / region / rotation / near (tool_help auto_place)" },
    { "auto_place.time_limit", "seconds of improving (default 10, -1 none)" },
};

constexpr size_t PARAM_MAX = 60;   // a longer parameter description needs a brief above


/// the properties of a schema, depth first, with their dotted paths
void eachProperty( json& aSchema, const std::string& aPrefix, const std::function<void( const std::string&, json& )>& aFn )
{
    if( !aSchema.is_object() || !aSchema.contains( "properties" ) || !aSchema["properties"].is_object() )
        return;

    for( auto& [name, prop] : aSchema["properties"].items() )
    {
        const std::string path = aPrefix.empty() ? name : aPrefix + "." + name;
        aFn( path, prop );
        eachProperty( prop, path, aFn );

        if( prop.is_object() && prop.contains( "items" ) )
            eachProperty( prop["items"], path, aFn );
    }
}

} // namespace


void ApplyBriefs( std::vector<TOOL>& aTools )
{
    for( TOOL& t : aTools )
    {
        auto brief = BRIEFS.find( t.name );

        if( brief == BRIEFS.end() )
            throw std::logic_error( "tool " + t.name + " has no brief in tool_docs.cpp" );
        else
        {
            t.details = t.description;
            t.description = brief->second;
        }

        // long parameter descriptions: the brief here, the full text to details
        std::string params;

        eachProperty( t.inputSchema, "",
                      [&]( const std::string& aPath, json& aProp )
                      {
                          if( !aProp.is_object() || !aProp.contains( "description" ) || !aProp["description"].is_string() )
                              return;

                          const std::string full = aProp["description"].get<std::string>();
                          auto              it = PARAM_BRIEFS.find( t.name + "." + aPath );

                          if( it == PARAM_BRIEFS.end() && aPath == "detail" )
                              it = PARAM_BRIEFS.find( "*.detail" );

                          if( it != PARAM_BRIEFS.end() && it->second.size() > PARAM_MAX )
                              throw std::logic_error( it->first + ": its brief is over " + std::to_string( PARAM_MAX ) + " characters" );

                          if( it != PARAM_BRIEFS.end() )
                          {
                              if( full != it->second && aPath != "detail" )
                                  params += "\n  " + aPath + ": " + full;

                              aProp["description"] = it->second;
                          }
                          else if( full.size() > PARAM_MAX )
                          {
                              throw std::logic_error( t.name + "." + aPath + ": a parameter description over " + std::to_string( PARAM_MAX )
                                                      + " characters needs a brief in tool_docs.cpp" );
                          }
                      } );

        if( !params.empty() )
            t.details += "\nParameters:" + params;
    }
}


TOOL ToolHelpTool( std::shared_ptr<std::vector<TOOL>> aTable )
{
    return { "tool_help", "", json::object(),
             [aTable]( const json& a ) -> TOOL_RESULT
             {
                 const std::string name = a.value( "name", std::string() );

                 for( const TOOL& t : *aTable )
                 {
                     if( t.name == name )
                     {
                         const std::string args = params::Docs( t.params );
                         return { name + ": " + ( t.details.empty() ? t.description : t.details ) + "\nArguments:"
                                  + ( args.empty() ? std::string( " none\n" ) : "\n" + args ) };
                     }
                 }

                 std::string names;

                 for( const TOOL& t : *aTable )
                     names += ( names.empty() ? "" : ", " ) + t.name;

                 return { "tool_help: no tool " + name + "; the tools: " + names, true };
             }, "", std::vector<params::PARAM>{
                   params::String( "name", "a tool's name" ).Required() }};
}


std::string ToolDocsMarkdown( const std::vector<TOOL>& aTools )
{
    std::string out = "# KiCadD tools: the full descriptions\n\n"
                      "Generated by `kicadd-mcp --tool-docs` (do not edit: the text lives with each tool in mcp/*.cpp). "
                      "The MCP tools/list sends only the first line of each; `tool_help {name}` returns the rest.\n";

    for( const TOOL& t : aTools )
    {
        out += "\n## " + t.name + "\n\n" + t.description + "\n";

        if( !t.details.empty() && t.details != t.description )
        {
            // the full parameter notes as a list
            std::string d = t.details;
            size_t      p = d.find( "\nParameters:\n  " );

            if( p != std::string::npos )
            {
                std::string notes = d.substr( p + 15 );
                d = d.substr( 0, p ) + "\n\nParameter notes:\n\n- ";

                for( size_t i = 0; i < notes.size(); ++i )
                    d += notes.compare( i, 3, "\n  " ) == 0 ? ( i += 2, std::string( "\n- " ) ) : std::string( 1, notes[i] );
            }

            out += "\n" + d + "\n";
        }

        if( !t.params.empty() )
            out += "\nArguments:\n\n" + params::Docs( t.params );
    }

    return out;
}

} // namespace kicadd::mcp
