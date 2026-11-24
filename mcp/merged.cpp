/*
 * KiCadD MCP - the merged tools (beta.13 phase 2): thin tools of one domain become one, so
 * tools/list (sent on every turn) is shorter.
 *
 *   undo / redo / sch_undo / sch_redo          -> undo {target, redo}
 *   get_rules                                  -> set_rules with no arguments
 *   check_drc                                  -> run_drc {refs, local: true}
 *   flush_ratsnest                             -> board_summary (the full unrouted count)
 *   open_board / new_project                   -> project {open} / project {new}
 *   export_fab / export_assembly / sch_export_pdf -> export {fab, assembly, schematic_pdf}
 *   board_render_3d                            -> board_render {view: "3d"}
 *
 * The old tools go: the merged ones run their implementations (no aliases while in beta).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"

#include <algorithm>
#include <stdexcept>

using json = nlohmann::json;

namespace kicadd::mcp
{

namespace
{

using RUN = std::function<TOOL_RESULT( const json& )>;


TOOL_RESULT fail( const std::string& aText )
{
    return { aText, true };
}


/// the arguments of aArgs not in aKeys
std::vector<std::string> outside( const json& aArgs, std::initializer_list<const char*> aKeys )
{
    std::vector<std::string> out;

    for( auto& [k, v] : aArgs.items() )
    {
        if( std::none_of( aKeys.begin(), aKeys.end(), [&]( const char* aKey ) { return k == aKey; } ) )
            out.push_back( k );
    }

    return out;
}


std::string joined( const std::vector<std::string>& aList )
{
    std::string s;

    for( const std::string& v : aList )
        s += ( s.empty() ? "" : ", " ) + v;

    return s;
}


class MERGER
{
public:
    explicit MERGER( std::vector<TOOL>& aTools ) : m_tools( aTools ) {}

    TOOL& get( const std::string& aName )
    {
        auto it = std::find_if( m_tools.begin(), m_tools.end(), [&]( const TOOL& t ) { return t.name == aName; } );

        if( it == m_tools.end() )
            throw std::logic_error( "merged.cpp: no tool " + aName );

        return *it;
    }

    /// aOld goes once the merged tools are made (at the end: references into the list stay good)
    void drop( const std::string& aOld )
    {
        get( aOld );
        m_drop.push_back( aOld );
    }

    void finish()
    {
        std::erase_if( m_tools, [&]( const TOOL& t )
                       { return std::find( m_drop.begin(), m_drop.end(), t.name ) != m_drop.end(); } );
    }

    /// a tool's properties (each one's schema) for a nested object
    json props( const std::string& aName ) { return get( aName ).inputSchema.value( "properties", json::object() ); }

    /// aTool replaces the one of its name, or goes in before aBefore
    void put( TOOL aTool, const std::string& aBefore )
    {
        auto it = std::find_if( m_tools.begin(), m_tools.end(), [&]( const TOOL& t ) { return t.name == aTool.name; } );

        if( it != m_tools.end() )
        {
            *it = std::move( aTool );
            return;
        }

        it = std::find_if( m_tools.begin(), m_tools.end(), [&]( const TOOL& t ) { return t.name == aBefore; } );
        m_tools.insert( it, std::move( aTool ) );
    }

private:
    std::vector<TOOL>&       m_tools;
    std::vector<std::string> m_drop;
};


json schemaOf( json aProps, std::vector<std::string> aRequired = {} )
{
    json s = { { "type", "object" }, { "properties", std::move( aProps ) } };

    if( !aRequired.empty() )
        s["required"] = aRequired;

    return s;
}

} // namespace


void MergeTools( std::vector<TOOL>& aTools )
{
    MERGER m( aTools );

    // --- undo {target, redo}
    {
        RUN boardUndo = m.get( "undo" ).run, boardRedo = m.get( "redo" ).run;
        RUN schUndo = m.get( "sch_undo" ).run, schRedo = m.get( "sch_redo" ).run;
        json detail = m.get( "undo" ).inputSchema["properties"]["detail"];

        TOOL undo = {
            "undo",
            "Undo (or with redo=true redo) the agent's last edit, restoring the exact previous state. target \"board\" "
            "(default): move / rotate / flip / add / delete / auto_place / route / delete_tracks / zones / rules ...; "
            "tracks, zones or the outline put back are refilled; returns what was undone, new positions and a local "
            "DRC around moved parts. target \"schematic\": sch_add_part, sch_connect, sch_wire, sch_move, sch_delete, "
            "...; the agent keeps its last 50. On either side the user's own edits are not undone: parts the user "
            "changed since are left alone and counted, and an edit the user already undid in the editor (Ctrl+Z) is "
            "said so.",
            schemaOf( { { "target", { { "type", "string" }, { "enum", { "board", "schematic" } } } },
                        { "redo", { { "type", "boolean" } } },
                        { "detail", detail } } ),
            [=]( const json& a ) -> TOOL_RESULT
            {
                const std::string target = a.value( "target", std::string( "board" ) );
                const bool        redo = a.contains( "redo" ) && a["redo"].is_boolean() && a["redo"].get<bool>();
                json              rest = a;
                rest.erase( "target" );
                rest.erase( "redo" );

                if( target == "schematic" )
                    return ( redo ? schRedo : schUndo )( json::object() );

                if( target != "board" )
                    return fail( "target: \"board\" (default) or \"schematic\"" );

                return ( redo ? boardRedo : boardUndo )( rest );
            } };

        // the old undo's name is the new tool's: the other three go
        m.drop( "redo" );
        m.drop( "sch_undo" );
        m.drop( "sch_redo" );
        m.put( undo, "" );
    }

    // --- set_rules with no arguments: get_rules
    {
        TOOL& set = m.get( "set_rules" );
        RUN   get = m.get( "get_rules" ).run;
        set.description = "With no arguments: the design rules (net classes with clearance, track width, via, diff "
                          "pair and how many nets each has, unset values from Default; the net class assignments by "
                          "name pattern; the board's minimum constraints; mm). With arguments: "
                          + set.description + " The answer ends with the rules as they are now.";
        set.run = [get, run = set.run]( const json& a ) { return a.empty() ? get( a ) : run( a ); };
        m.drop( "get_rules" );
    }

    // --- run_drc {refs, local: true}: check_drc
    {
        TOOL& drc = m.get( "run_drc" );
        RUN   local = m.get( "check_drc" ).run;
        drc.description += " local=true with refs: only those parts against their surroundings, at once (the "
                           "local DRC move_footprints returns, without moving them).";
        drc.inputSchema["properties"]["local"] = { { "type", "boolean" } };
        drc.inputSchema["properties"]["detail"] = m.get( "check_drc" ).inputSchema["properties"]["detail"];
        drc.run = [local, run = drc.run]( const json& a ) -> TOOL_RESULT
        {
            if( !( a.contains( "local" ) && a["local"].is_boolean() && a["local"].get<bool>() ) )
            {
                json rest = a;
                rest.erase( "local" );
                rest.erase( "detail" );
                return run( rest );
            }

            if( std::vector<std::string> extra = outside( a, { "local", "refs", "detail" } ); !extra.empty() )
                return fail( "local=true takes refs and detail only, not " + joined( extra ) );

            if( !a.contains( "refs" ) || !a["refs"].is_array() || a["refs"].empty() )
                return fail( "local=true needs refs: the parts to check, e.g. [\"U1\", \"C3\"]" );

            json rest = a;
            rest.erase( "local" );
            return local( rest );
        };
        m.drop( "check_drc" );
    }

    // --- board_summary: with the full unrouted count (flush_ratsnest)
    {
        // board_summary itself ends with the ratsnest line (tools.cpp, boardSummary: it must not
        // wait out a busy editor, so it calls the server directly)
        TOOL& sum = m.get( "board_summary" );
        sum.description += " Then the complete unrouted-connection count: the ratsnest of large nets deferred "
                           "during moves is rebuilt first (not while the editor is busy or auto_place runs: said so).";
        m.drop( "flush_ratsnest" );
    }

    // --- project {open} / {new}
    {
        RUN  open = m.get( "open_board" ).run, make = m.get( "new_project" ).run;
        json newProps = m.props( "new_project" );
        newProps.erase( "discard" );

        TOOL project = {
            "project",
            "Open or make the project on the server (kicad-cli api-server), replacing the open one: the board, its "
            "schematic when there is one, the project's rules; the agent history starts over. open: a .kicad_pcb or "
            ".kicad_pro (absolute); the same file again reloads it from disk. new: {path (the folder, absolute; made "
            "when missing, or its .kicad_pro), name (default the folder's name), copper_layers (2), thickness (mm, "
            "1.6) or stackup (a JLCPCB one, as board_setup), outline [width, height] mm (a rectangle at (100, 100), "
            "corner_radius)}: writes <name>.kicad_pro, an empty .kicad_pcb and .kicad_sch (refused when any is there: "
            "never written over), opens and saves them. Both are refused while the open board has unsaved changes, "
            "unless discard=true (the refusal lists what board_diff finds changed). Not with the desktop editor: "
            "there the user opens and makes projects.",
            schemaOf( { { "open", { { "type", "string" }, { "description", "a .kicad_pcb / .kicad_pro, absolute" } } },
                        { "new", { { "type", "object" }, { "properties", newProps }, { "required", { "path" } } } },
                        { "discard", { { "type", "boolean" }, { "description", "Drop the open board's unsaved changes (default false)" } } } } ),
            [=]( const json& a ) -> TOOL_RESULT
            {
                const bool isOpen = a.contains( "open" ), isNew = a.contains( "new" );

                if( isOpen == isNew )
                    return fail( "give one of open: \"/path/board.kicad_pcb\" or new: {\"path\": \"/path/folder\", ...}" );

                json args = isOpen ? json{ { "path", a["open"] } } : a["new"];

                if( !args.is_object() )
                    return fail( "new: an object, e.g. {\"path\": \"/path/folder\", \"copper_layers\": 4}" );

                if( a.contains( "discard" ) )
                    args["discard"] = a["discard"];

                return ( isOpen ? open : make )( args );
            } };

        m.drop( "open_board" );
        m.drop( "new_project" );
        m.put( project, "open_board" );
    }

    // --- export {fab, assembly, schematic_pdf}
    {
        RUN fab = m.get( "export_fab" ).run, assembly = m.get( "export_assembly" ).run, pdf = m.get( "sch_export_pdf" ).run;

        auto part = [&]( const std::string& aName ) {
            return json{ { "type", "object" }, { "properties", m.props( aName ) } };
        };

        TOOL exp = {
            "export",
            "Write the outputs from the board / schematic as they are now (unsaved edits included); give one or more, "
            "{} for the defaults, written in this order. fab: the fabrication files for JLCPCB: Gerbers (Protel "
            "extensions, X2, job file) and Excellon drill (mm) zipped for the upload, BOM (Comment, Designator, "
            "Footprint, LCSC Part #) and CPL (Designator, Mid X, Mid Y, Layer, Rotation) as CSV; refills the zones "
            "first (refill=false skips it), then runs the full DRC and puts its error count first (drc=false skips "
            "it); strict=true writes nothing when DRC has errors, a reference is on two footprints or a part is "
            "outside the outline; dir defaults to <project>/fab. assembly: the assembly drawing (PDF), a page for the "
            "top and, when parts are on the bottom, one seen from below, each with the title block and scaled to the "
            "page: KiCad's plot of the Fab layer and the outline; parts with no Fab drawing (LCSC / EasyEDA "
            "footprints) get their outline, pin 1 and reference from the courtyard; DNP parts crossed out; "
            "values=true adds the values; dir defaults to <project>/fab (<board>-assembly.pdf), path names the "
            "file. schematic_pdf: the whole schematic as one PDF, every sheet in page order with its frame and "
            "title block, sheets linked; path default <project>/<schematic name>.pdf (relative: the project "
            "folder); black_and_white for printing. One failing does not stop the others.",
            schemaOf( { { "fab", part( "export_fab" ) },
                        { "assembly", part( "export_assembly" ) },
                        { "schematic_pdf", part( "sch_export_pdf" ) } } ),
            [=]( const json& a ) -> TOOL_RESULT
            {
                std::string out;
                bool        failed = false;
                int         n = 0;
                const bool  several = a.contains( "fab" ) + a.contains( "assembly" ) + a.contains( "schematic_pdf" ) > 1;

                for( auto [key, run] : { std::pair<const char*, RUN>{ "fab", fab }, { "assembly", assembly },
                                         { "schematic_pdf", pdf } } )
                {
                    if( !a.contains( key ) )
                        continue;

                    const json& args = a[key];

                    if( !args.is_object() )
                        return fail( std::string( key ) + ": an object ({} for the defaults)" );

                    TOOL_RESULT r = run( args );
                    n++;
                    failed |= r.isError;

                    while( !r.text.empty() && r.text.back() == '\n' )
                        r.text.pop_back();

                    out += std::string( r.isError && several ? "[" + std::string( key ) + " FAILED] " : "" ) + r.text + "\n";
                }

                if( n == 0 )
                    return fail( "give fab, assembly and / or schematic_pdf, e.g. {\"fab\": {\"strict\": true}, \"assembly\": {}}" );

                return { out, failed };
            } };

        m.drop( "export_fab" );
        m.drop( "export_assembly" );
        m.drop( "sch_export_pdf" );
        m.put( exp, "export_fab" );
    }

    // --- board_render {view: "3d"}: board_render_3d
    {
        TOOL& render = m.get( "board_render" );
        RUN   flat = render.run, solid = m.get( "board_render_3d" ).run;
        json& p = render.inputSchema["properties"];

        render.description += " view=\"3d\": KiCad's raytracer instead (as kicad-cli pcb render; unsaved edits "
                              "included): the assembled board, connectors at the edge, tall parts. side top (default), "
                              "bottom (or back=true), front, back, left, right; rot_x / rot_y / rot_z degrees after it "
                              "(side top, rot_x -45: tilted); zoom a factor (default 1); size the width (default 1200, "
                              "4:3); quality basic or high (reflections, shadows: slower); floor; perspective. Only "
                              "VRML models load in this build: the parts drawn without a body are listed. In the "
                              "desktop editor the window waits while it renders (seconds).";
        p["view"] = { { "type", "string" }, { "enum", { "2d", "3d" } } };
        p["zoom"] = { { "description", "2d: true / false; 3d: a factor (1)" } };
        p["side"] = { { "type", "string" }, { "enum", { "top", "bottom", "front", "back", "left", "right" } } };
        p["rot_x"] = { { "type", "number" } };
        p["rot_y"] = { { "type", "number" } };
        p["rot_z"] = { { "type", "number" } };
        p["quality"] = { { "type", "string" }, { "enum", { "basic", "high" } } };
        p["floor"] = { { "type", "boolean" } };
        p["perspective"] = { { "type", "boolean" } };

        render.run = [flat, solid]( const json& a ) -> TOOL_RESULT
        {
            const std::string view = a.value( "view", std::string( "2d" ) );
            json              args = a;
            args.erase( "view" );

            if( view == "2d" )
            {
                if( std::vector<std::string> extra = outside( args, { "layers", "refs", "nets", "area", "marks", "zoom", "size",
                                                                      "margin", "ratsnest", "labels", "back", "path" } );
                    !extra.empty() )
                    return fail( joined( extra ) + ": for view=\"3d\" only" );

                if( args.contains( "zoom" ) && !args["zoom"].is_boolean() )
                    return fail( "zoom: true / false in the 2d view (a factor only with view=\"3d\")" );

                return flat( args );
            }

            if( view != "3d" )
                return fail( "view: \"2d\" (default) or \"3d\"" );

            if( std::vector<std::string> extra = outside( args, { "side", "rot_x", "rot_y", "rot_z", "zoom", "size", "quality",
                                                                  "floor", "perspective", "back", "path" } );
                !extra.empty() )
                return fail( joined( extra ) + ": not in the 3d view (it has side, rot_x / rot_y / rot_z, zoom, size, "
                                               "quality, floor, perspective, path)" );

            if( args.contains( "zoom" ) && args["zoom"].is_boolean() )
                args.erase( "zoom" );

            if( args.contains( "back" ) )
            {
                if( args["back"].is_boolean() && args["back"].get<bool>() && !args.contains( "side" ) )
                    args["side"] = "bottom";

                args.erase( "back" );
            }

            if( args.contains( "size" ) && args["size"].is_number() )
            {
                const int w = args["size"].get<int>();
                args["width"] = w;
                args["height"] = w * 3 / 4;
            }

            args.erase( "size" );
            return solid( args );
        };

        m.drop( "board_render_3d" );
    }

    m.finish();
}

} // namespace kicadd::mcp
