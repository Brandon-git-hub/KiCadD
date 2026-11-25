/*
 * KiCadD MCP - tool definitions.
 *
 * Tools take millimetres and degrees and return compact text, not JSON: the point of this
 * layer is to keep an agent's context small (see README "Direction 2").
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_TOOLS_H
#define KICADD_MCP_TOOLS_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "params.h"

namespace kicadd::mcp
{

class IPC_CLIENT;

struct TOOL_RESULT
{
    std::string text;
    bool        isError = false;
    std::string image;        ///< PNG bytes, sent after the text as an image content block
};

struct TOOL
{
    std::string                                       name;
    std::string                                       description;
    nlohmann::json                                    inputSchema;
    std::function<TOOL_RESULT( const nlohmann::json& )> run;
    std::string                                       details;   ///< the full description (tool_help); description is the brief
    std::vector<params::PARAM>                        params;    ///< declared once: makes inputSchema and the check before run (2b)
};

/// a tool's parameters made its inputSchema, and its run checking the arguments first (so tools/call
/// and batch check alike)
void ApplyParams( TOOL& aTool );

/// Build the tool set bound to aIpc (which must outlive the tools). aAllowLaunch: launch_kicad /
/// close_kicad may start and stop programs (stdio; HTTP only with --allow-launch).
std::vector<TOOL> MakeTools( IPC_CLIENT& aIpc, bool aAllowLaunch = true );

/// M5a schematic tools (sch_tools.cpp)
void AddSchematicTools( std::vector<TOOL>& aTools, IPC_CLIENT& aIpc );

/// phase 3 items 3 / 4: pdn (pdn.cpp), reflection (reflect.cpp)
TOOL PdnTool( IPC_CLIENT& aIpc );
TOOL ReflectionTool( IPC_CLIENT& aIpc );

/// beta.13 phase 2 (merged.cpp): the merged tools replace the thin ones of their domain
void MergeTools( std::vector<TOOL>& aTools );

/// tools/list sends briefs (tool_docs.cpp): each tool's registered description moves to details
void ApplyBriefs( std::vector<TOOL>& aTools );

/// tool_help: a tool's full description from aTable
TOOL ToolHelpTool( std::shared_ptr<std::vector<TOOL>> aTable );

/// every tool's brief, full description and arguments as Markdown (kicadd-mcp --tool-docs)
std::string ToolDocsMarkdown( const std::vector<TOOL>& aTools );

/// phase 4 item 3: new_project (project.cpp); it runs open_board, board_setup, board_outline,
/// save_board and sch_save from aTable
TOOL NewProjectTool( std::shared_ptr<std::vector<TOOL>> aTable );

/// "; board_diff: N change(s) since saved: ..." for a refusal over unsaved changes (open_board, close_kicad)
std::string UnsavedBoardChanges( IPC_CLIENT& aIpc );

/// "path: the folder X does not exist" when an absolute output path's folder is missing (a
/// relative one is the server's to resolve), else empty
std::string MissingFolder( const std::string& aPath );

/// phase 4 item 3: launch_kicad / close_kicad (launch.cpp): start the server or the desktop
/// editor on aIpc's socket, stop the one started here. aAllowed false (HTTP without
/// --allow-launch): both refuse.
void AddLaunchTools( std::vector<TOOL>& aTools, IPC_CLIENT& aIpc, bool aAllowed );

} // namespace kicadd::mcp

#endif // KICADD_MCP_TOOLS_H
