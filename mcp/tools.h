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
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace kicadd::mcp
{

class IPC_CLIENT;

struct TOOL_RESULT
{
    std::string text;
    bool        isError = false;
};

struct TOOL
{
    std::string                                       name;
    std::string                                       description;
    nlohmann::json                                    inputSchema;
    std::function<TOOL_RESULT( const nlohmann::json& )> run;
};

/// Build the tool set bound to aIpc (which must outlive the tools).
std::vector<TOOL> MakeTools( IPC_CLIENT& aIpc );

} // namespace kicadd::mcp

#endif // KICADD_MCP_TOOLS_H
