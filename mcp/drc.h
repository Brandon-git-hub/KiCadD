/*
 * KiCadD MCP - full-board DRC via the upstream DRC task, with readable item handles.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_DRC_H
#define KICADD_MCP_DRC_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kicadd::mcp
{

class IPC_CLIENT;

struct DRC_ENTRY
{
    std::string type;        ///< e.g. "clearance", "shorting_items", "unconnected_items"
    bool        warning = false;
    std::string a, b;        ///< item handles ("U5.3", "C12", track id, "zone:GND", "edge")
    std::string netA, netB;
    std::string layer;
    std::string description;
    int64_t     x = 0, y = 0;   ///< nm
};

struct DRC_REPORT
{
    std::vector<DRC_ENTRY>     entries;     ///< errors first, then warnings; each by type
    std::map<std::string, int> errors;      ///< count by type
    std::map<std::string, int> warnings;
    int                        excluded = 0;
    int                        ignored = 0;
    double                     seconds = 0;
};

/// Run upstream DRC on the open board and translate its markers. Blocks until done.
bool RunFullDrc( IPC_CLIENT& aIpc, DRC_REPORT& aReport, std::string& aError );

} // namespace kicadd::mcp

#endif // KICADD_MCP_DRC_H
