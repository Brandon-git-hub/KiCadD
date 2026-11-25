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

#include <optional>
#include <string>
#include <vector>

namespace kicadd::board
{
class LcscHit;
}

namespace kicadd::mcp
{

/// the simulate tool's description and input schema, and its run
std::string    SimulateDescription();
std::vector<params::PARAM> SimulateParams();   ///< its arguments (params.h)
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

/// A capacitor's model from its value, footprint / symbol text and the parts index's row (when
/// there is one); aOver: the agent's overrides (c, esr, esl, dc_bias). Estimated values say so in
/// note.
struct CAP_MODEL
{
    double      c = 0, esr = 0, esl = 0, vrated = 0, tol = 0;
    std::string kind, dielectric, package, note;
    bool        biasOn = true, estimated = false;
};

CAP_MODEL CapModel( const std::string& aValue, const std::string& aText, const std::string& aDescription,
                    const kicadd::board::LcscHit* aHit, const nlohmann::json& aOver );

/// a supply's voltage from its name: "+3V3" 3.3, "VCC_1V8" 1.8, "-12V" -12
std::optional<double> VoltageFromName( const std::string& aNet );

/// ngspice's output: cols[0] the scale (time / frequency / swept value), then each vector
struct TABLE
{
    std::vector<std::string>         names;
    std::vector<std::vector<double>> cols;
};

struct RUN
{
    bool        ok = false;
    std::string error;
    TABLE       table;
    double      seconds = 0;
};

/// ngspice: KICADD_NGSPICE, next to kicadd-mcp (ngspice/bin/), or on the PATH ("" none)
std::string FindNgspice();

/// run aCircuit (no .control / .end) with the analysis line aAnalysis ("tran 1p 20n"), reading
/// back aVectors
RUN RunSpice( const std::string& aExe, const std::string& aCircuit, const std::string& aAnalysis,
              const std::vector<std::string>& aVectors, int aTimeoutS );

} // namespace sim

} // namespace kicadd::mcp

#endif
