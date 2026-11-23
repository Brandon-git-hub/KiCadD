/*
 * KiCadD MCP - resources and prompts (roadmap phase 5 item 2). See resources.cpp.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_RESOURCES_H
#define KICADD_MCP_RESOURCES_H

#include "tools.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace kicadd::mcp
{

struct SKILL_SECTION
{
    std::string slug;    ///< "review-a-design"
    std::string title;   ///< "Review a design"
    std::string text;    ///< from its "## " heading to the next
};

/// The built-in skill's sections, "intro" (before the first heading) first
std::vector<SKILL_SECTION> SkillSections();

nlohmann::json ResourcesList();
nlohmann::json ResourceTemplatesList();

/// (mime type, text) of aUri, nullopt when there is no such resource. aCall runs a tool.
std::optional<std::pair<std::string, std::string>>
ResourceRead( const std::string& aUri, const std::vector<TOOL>& aTools,
              const std::function<TOOL_RESULT( const std::string&, const nlohmann::json& )>& aCall );

nlohmann::json                PromptsList();
std::optional<nlohmann::json> PromptGet( const std::string& aName, const nlohmann::json& aArgs );

} // namespace kicadd::mcp

#endif // KICADD_MCP_RESOURCES_H
