/*
 * KiCadD MCP - regional circuit simulation with ngspice (ROADMAP phase 3, item 2).
 *
 * The schematic gives the region (SchSimRegion); the parts index gives each part's parameters
 * (LcscSearch by number); this makes a model per part, the netlist with the agent's sources and
 * loads, runs ngspice (an external program: KICADD_NGSPICE, next to kicadd-mcp, or on the PATH)
 * and measures. Estimated values (MLCC DC bias, ESR / ESL, a regulator's loop) are always said
 * to be estimated, with what they come from; the agent can override any of them per part.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_SIM_H
#define KICADD_MCP_SIM_H

#include "tools.h"

#include <string>

namespace kicadd::mcp
{

/// the simulate tool's description and input schema, and its run
std::string    SimulateDescription();
nlohmann::json SimulateSchema();
TOOL_RESULT    Simulate( IPC_CLIENT& aIpc, const nlohmann::json& aArgs );

namespace sim
{

/// "10u" -> 1e-5, "4k7" -> 4700, "100nF" -> 1e-7, "2.2Meg" -> 2.2e6 (unit letters after it ignored)
bool ParseSI( const std::string& aText, double& aValue );

/// 1e-5 "F" -> "10 µF"
std::string FormatSI( double aValue, const std::string& aUnit, int aDigits = 3 );

/// the estimated share of an MLCC's capacitance left at aVdc (1 for class 1), and the dielectric
/// layer's estimated thickness (m) and field (V/m) it comes from
double MlccBias( const std::string& aDielectric, double aC, const std::string& aPackage, double aVdc,
                 double* aLayer = nullptr, double* aField = nullptr );

} // namespace sim

} // namespace kicadd::mcp

#endif
