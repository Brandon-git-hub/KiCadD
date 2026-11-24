/*
 * KiCadD MCP - resources and prompts (roadmap phase 5 item 2).
 *
 * Resources: what an agent reads on demand instead of carrying it from the start.
 *   kicadd://skill              the kicadd skill (skills/kicadd/SKILL.md, built in)
 *   kicadd://skill/{section}    one of its sections ("review-a-design", "routing", ...): read the
 *                               one the task needs
 *   kicadd://tools              every tool's full description (as skills/kicadd/TOOLS.md)
 *   kicadd://tool/{name}        one tool's (as tool_help)
 *   kicadd://board              board_summary of the open board
 *   kicadd://schematic          sch_summary's first lines (the parts list left out)
 * Prompts: review_design, new_board {spec}, prepare_fab: the start of those jobs, pointing at
 * the skill's sections.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "resources.h"

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace kicadd::mcp
{

extern const char* const KICADD_SKILL_MD;   // generated from skills/kicadd/SKILL.md

namespace
{

std::string slug( const std::string& aHeading )
{
    std::string s;

    for( char c : aHeading )
    {
        if( std::isalnum( (unsigned char) c ) )
            s += (char) std::tolower( (unsigned char) c );
        else if( !s.empty() && s.back() != '-' )
            s += '-';
    }

    while( !s.empty() && s.back() == '-' )
        s.pop_back();

    return s;
}


/// The skill without its front matter
std::string skillBody()
{
    std::string s = KICADD_SKILL_MD;

    if( s.rfind( "---", 0 ) == 0 )
    {
        size_t end = s.find( "\n---", 3 );

        if( end != std::string::npos )
            s = s.substr( s.find( '\n', end + 1 ) + 1 );
    }

    s.erase( 0, s.find_first_not_of( "\r\n" ) );
    return s;
}


std::string skillDescription()
{
    std::string s = KICADD_SKILL_MD;
    size_t      d = s.find( "\ndescription:" );

    if( d == std::string::npos )
        return {};

    size_t end = s.find( '\n', d + 1 );
    return s.substr( d + 14, end - d - 14 );
}

} // namespace


std::vector<SKILL_SECTION> SkillSections()
{
    std::vector<SKILL_SECTION> out;
    const std::string          body = skillBody();
    size_t                     pos = body.find( "\n## " );

    out.push_back( { "intro", "Working with KiCadD (the skill's start)", body.substr( 0, pos ) } );

    while( pos != std::string::npos )
    {
        size_t      next = body.find( "\n## ", pos + 4 );
        size_t      eol = body.find( '\n', pos + 4 );
        std::string heading = body.substr( pos + 4, eol - pos - 4 );
        out.push_back( { slug( heading ), heading, body.substr( pos + 1, ( next == std::string::npos ? body.size() : next ) - pos - 1 ) } );
        pos = next;
    }

    return out;
}


json ResourcesList()
{
    json list = json::array();

    list.push_back( { { "uri", "kicadd://skill" }, { "name", "kicadd skill" }, { "mimeType", "text/markdown" },
                      { "description", "How to use the kicadd tools together (all sections; about 40 kB). " + skillDescription() } } );

    for( const SKILL_SECTION& s : SkillSections() )
    {
        list.push_back( { { "uri", "kicadd://skill/" + s.slug }, { "name", "skill: " + s.title }, { "mimeType", "text/markdown" },
                          { "description", "One section of the kicadd skill (" + std::to_string( s.text.size() / 1000 + 1 ) + " kB)" } } );
    }

    list.push_back( { { "uri", "kicadd://tools" }, { "name", "kicadd tools (full)" }, { "mimeType", "text/markdown" },
                      { "description", "Every tool's full description and arguments (tools/list has the short ones)" } } );
    list.push_back( { { "uri", "kicadd://board" }, { "name", "open board" }, { "mimeType", "text/plain" },
                      { "description", "board_summary of the board open now" } } );
    list.push_back( { { "uri", "kicadd://schematic" }, { "name", "open schematic" }, { "mimeType", "text/plain" },
                      { "description", "sch_summary of the schematic open now (counts and sheets; the parts list left out)" } } );
    return { { "resources", list } };
}


json ResourceTemplatesList()
{
    return { { "resourceTemplates",
               json::array( { { { "uriTemplate", "kicadd://tool/{name}" }, { "name", "one tool's full description" },
                                { "mimeType", "text/markdown" } },
                              { { "uriTemplate", "kicadd://skill/{section}" }, { "name", "one section of the kicadd skill" },
                                { "mimeType", "text/markdown" } } } ) } };
}


std::optional<std::pair<std::string, std::string>> ResourceRead( const std::string& aUri, const std::vector<TOOL>& aTools,
                                                                const std::function<TOOL_RESULT( const std::string&, const json& )>& aCall )
{
    auto md = []( std::string aText ) { return std::make_pair( std::string( "text/markdown" ), std::move( aText ) ); };

    if( aUri == "kicadd://skill" )
        return md( skillBody() );

    if( aUri.rfind( "kicadd://skill/", 0 ) == 0 )
    {
        std::string want = aUri.substr( 15 );

        for( const SKILL_SECTION& s : SkillSections() )
        {
            if( s.slug == want )
                return md( s.text );
        }

        return std::nullopt;
    }

    if( aUri == "kicadd://tools" )
        return md( ToolDocsMarkdown( aTools ) );

    if( aUri.rfind( "kicadd://tool/", 0 ) == 0 )
    {
        TOOL_RESULT r = aCall( "tool_help", { { "name", aUri.substr( 14 ) } } );
        return r.isError ? std::nullopt : std::optional( md( r.text ) );
    }

    if( aUri == "kicadd://board" || aUri == "kicadd://schematic" )
    {
        TOOL_RESULT r = aCall( aUri == "kicadd://board" ? "board_summary" : "sch_summary", json::object() );
        std::string text = r.text;

        if( aUri == "kicadd://schematic" && !r.isError )
        {
            // the counts and the sheets; each part is a line of its own (sch_summary has them)
            std::string kept;
            size_t      start = 0, n = 0;

            while( start < text.size() )
            {
                size_t      eol = text.find( '\n', start );
                std::string line = text.substr( start, eol == std::string::npos ? std::string::npos : eol - start );

                if( n == 0 || line.rfind( "SHEET", 0 ) == 0 || line.rfind( "note:", 0 ) == 0 )
                    kept += line + "\n";

                n++;
                start = eol == std::string::npos ? text.size() : eol + 1;
            }

            text = kept + "(the parts: sch_summary)\n";
        }

        return std::make_pair( std::string( "text/plain" ), ( r.isError ? "not available: " : "" ) + text );
    }

    return std::nullopt;
}


// ---- prompts --------------------------------------------------------------------------------

json PromptsList()
{
    return { { "prompts",
               json::array( {
                       { { "name", "review_design" },
                         { "description", "Review the open design (ERC, DRC, the layout-aware audit) and report, without changing it" },
                         { "arguments", json::array( { { { "name", "focus" },
                                                         { "description", "what to look at most (e.g. USB, power, the 4-layer stackup)" },
                                                         { "required", false } } } ) } },
                       { { "name", "new_board" },
                         { "description", "Design a new board from a specification: project, schematic with LCSC parts, placement, routing, fab files" },
                         { "arguments", json::array( { { { "name", "spec" }, { "description", "what the board is to do, its size, layers, fab" }, { "required", true } },
                                                       { { "name", "path" }, { "description", "the project's folder (absolute, on the server)" }, { "required", false } } } ) } },
                       { { "name", "prepare_fab" },
                         { "description", "Get the open board ready to order at JLCPCB: checks, then Gerbers / BOM / CPL, the assembly drawing and 3D pictures" },
                         { "arguments", json::array() } },
               } ) } };
}


std::optional<json> PromptGet( const std::string& aName, const json& aArgs )
{
    auto arg = [&]( const char* k ) { return aArgs.is_object() && aArgs.contains( k ) && aArgs[k].is_string() ? aArgs[k].get<std::string>() : std::string(); };
    std::string text, description;

    if( aName == "review_design" )
    {
        description = "Review the open design";
        text = "Review the KiCad design that is open, with the kicadd tools; change nothing unless I ask.\n\n"
               "Read the resource kicadd://skill/review-a-design first (the kicadd skill's \"Review a design\"), and "
               "kicadd://skill/ground-rules. Start with board_summary and sch_summary, then run_erc, audit (its sections), "
               "run_drc, and the layout-aware checks that fit the board (reference planes, diff pairs and lengths, IR drop, "
               "crosstalk, impedance; give them limits only where the parts' datasheets or the interfaces call for them). "
               "Look at the board with board_render where coordinates are not enough.\n\n"
               "Report the findings worst first, each with the refs / nets / positions and what to do about it; say what "
               "you could not check.";

        if( std::string f = arg( "focus" ); !f.empty() )
            text += "\n\nFocus on: " + f;
    }
    else if( aName == "new_board" )
    {
        std::string spec = arg( "spec" );

        if( spec.empty() )
            return std::nullopt;

        description = "Design a new board";
        text = "Design a new board with the kicadd tools from this specification:\n\n" + spec + "\n\n";

        if( std::string p = arg( "path" ); !p.empty() )
            text += "Make the project in " + p + " (project {new}).\n\n";

        text += "Work through the kicadd skill's sections as you go (read each when you get to it): "
                "kicadd://skill/ground-rules, kicadd://skill/a-new-project-project-new, kicadd://skill/parts-lcsc-jlcpcb, "
                "kicadd://skill/edit-a-schematic, kicadd://skill/a-new-board-outline-layers-rules-pours, "
                "kicadd://skill/schematic-to-board-placement, kicadd://skill/routing-the-repetitive-part-not-an-autorouter, "
                "kicadd://skill/rules-and-zones, kicadd://skill/manufacturing-jlcpcb.\n"
                "Pick real LCSC parts (lcsc_search, prefer JLC basic parts), keep ERC clean, place with auto_place and check "
                "it, route what route / route_short can, and leave the rest (differential pairs, length matching) for a "
                "human to route, said in your summary. Save often (save_board, sch_save). End with export {fab, assembly, "
                "schematic_pdf} and a board_render view=3d of each side, and a summary of what is done and what is left.";
    }
    else if( aName == "prepare_fab" )
    {
        description = "Get the open board ready to order";
        text = "Get the open board ready to order at JLCPCB with the kicadd tools (kicadd://skill/manufacturing-jlcpcb):\n"
               "1. run_erc and audit; refill_zones and run_drc. Fix what is clearly a mistake and safe to fix; list the rest "
               "for me instead of guessing.\n"
               "2. Check that every part has an LCSC number (lcsc_match) and its footprint is placed on the board.\n"
               "3. export {fab: {strict: true}, assembly: {}}, and board_render view=3d of the top and the bottom.\n"
               "4. Tell me what was written, the DRC / ERC state, the parts without an LCSC number or a 3D model, and "
               "anything I should look at before ordering.";
    }
    else
    {
        return std::nullopt;
    }

    return json{ { "description", description },
                 { "messages", json::array( { { { "role", "user" }, { "content", { { "type", "text" }, { "text", text } } } } } ) } };
}

} // namespace kicadd::mcp
