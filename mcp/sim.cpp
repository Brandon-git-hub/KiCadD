/*
 * KiCadD MCP - regional circuit simulation with ngspice. See sim.h.
 *
 * Models, per part (the first that applies):
 *   - its own SPICE model (Sim.* fields): KiCad's lines, as the simulator in the editor has them
 *   - models {ref: {subckt, lib, pins}}: the agent's .subckt
 *   - from the parts index (LCSC field) or the reference / value / footprint:
 *       R; C as C_eff + ESR + ESL (MLCC: DC bias estimated from dielectric, size, C and the DC
 *       voltage across it; aluminium / tantalum / polymer: typical ESR / ESL); L with DCR (and
 *       SRF's parallel C when known); diodes and Zeners (Vf @ If, trr, Vz, Zzt); MOSFETs (level 1
 *       from Vgs(th) and Rds(on) @ Vgs, Ciss / Coss / Crss as fixed capacitances, a body diode);
 *       LDOs (Vout or a reference on FB, dropout, Iq, PSRR as one flat number, a 100 kHz loop
 *       and 10 mΩ out, assumed); op-amps (GBW as one pole after 100 dB, slew, Vos, Ib, rails)
 *   - else left out: its nets are cut there (said).
 * Every value not from the part's data is marked estimated or assumed in the answer.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sim.h"
#include "plot.h"
#include "ipc_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>

#include <kicadd/board.pb.h>
#include <kicadd/schematic.pb.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using json = nlohmann::json;
namespace ks = kicadd::schematic;
namespace kb = kicadd::board;
namespace fs = std::filesystem;

namespace
{

constexpr double PI = 3.14159265358979323846;

fs::path toPath( const std::string& aUtf8 )
{
    return fs::path( std::u8string( aUtf8.begin(), aUtf8.end() ) );
}

std::string fromPath( const fs::path& aPath )
{
    std::u8string s = aPath.u8string();
    return std::string( s.begin(), s.end() );
}

std::string genericOf( const fs::path& aPath )
{
    std::u8string s = aPath.generic_u8string();
    return std::string( s.begin(), s.end() );
}

} // namespace

namespace kicadd::mcp
{

namespace sim
{

bool ParseSI( const std::string& aText, double& aValue )
{
    size_t b = aText.find_first_not_of( " \t" );
    size_t e = aText.find_last_not_of( " \t" );

    if( b == std::string::npos )
        return false;

    std::string t = aText.substr( b, e - b + 1 );

    const char* s = t.c_str();
    char*       end = nullptr;
    double      v = std::strtod( s, &end );

    if( end == s )
        return false;

    std::string rest = end;

    while( !rest.empty() && rest[0] == ' ' )
        rest.erase( 0, 1 );

    double      mult = 1;
    size_t      used = 0;

    auto starts = [&]( const char* p ) { return rest.compare( 0, std::strlen( p ), p ) == 0; };

    if( starts( "Meg" ) || starts( "MEG" ) || starts( "meg" ) )
    {
        mult = 1e6;
        used = 3;
    }
    else if( starts( "µ" ) || starts( "μ" ) )
    {
        mult = 1e-6;
        used = std::string( "µ" ).size();
    }
    else if( !rest.empty() )
    {
        switch( rest[0] )
        {
        case 'f': mult = 1e-15; used = 1; break;
        case 'p': mult = 1e-12; used = 1; break;
        case 'n': mult = 1e-9; used = 1; break;
        case 'u': mult = 1e-6; used = 1; break;
        case 'm': mult = 1e-3; used = 1; break;
        case 'k': case 'K': mult = 1e3; used = 1; break;
        case 'M': mult = 1e6; used = 1; break;
        case 'G': mult = 1e9; used = 1; break;
        case 'R': case 'r': mult = 1; used = 1; break;
        default: break;
        }
    }

    rest = rest.substr( used );

    // "4k7": the digits after the prefix are the decimals
    if( used && !rest.empty() && std::isdigit( static_cast<unsigned char>( rest[0] ) ) && v == std::floor( v ) )
    {
        size_t i = 0;

        while( i < rest.size() && std::isdigit( static_cast<unsigned char>( rest[i] ) ) )
            ++i;

        v = std::stod( std::to_string( (long long) v ) + "." + rest.substr( 0, i ) );
        rest = rest.substr( i );
    }

    // "1k ohm": a space before the unit
    while( !rest.empty() && rest[0] == ' ' )
        rest.erase( 0, 1 );

    // a unit may follow ("F", "Ω", "ohm", "H", "V", "A", "Hz", "s"); anything else is not a value
    for( size_t i = 0; i < rest.size(); ++i )
    {
        unsigned char c = rest[i];

        if( !std::isalpha( c ) && c < 0x80 )
            return false;
    }

    aValue = v * mult;
    return std::isfinite( aValue );
}


std::string FormatSI( double aValue, const std::string& aUnit, int aDigits )
{
    static const std::pair<const char*, double> pre[] = {
        { "G", 1e9 }, { "M", 1e6 }, { "k", 1e3 }, { "", 1 }, { "m", 1e-3 }, { "µ", 1e-6 }, { "n", 1e-9 }, { "p", 1e-12 }, { "f", 1e-15 }
    };

    double      v = std::fabs( aValue ) < 1e-15 ? 0.0 : aValue;   // numerical zero
    const char* p = "";

    if( v != 0 && std::isfinite( v ) )
    {
        for( const auto& [name, f] : pre )
        {
            if( std::fabs( v ) >= f * 0.9995 || f == 1e-15 )
            {
                v /= f;
                p = name;
                break;
            }
        }
    }

    char buf[48];
    std::snprintf( buf, sizeof( buf ), "%.*g", std::max( aDigits, 3 ), v );
    return std::string( buf ) + " " + p + aUnit;
}


namespace
{

/// body size (mm) of the EIA chip packages, thickness the usual largest for the size
struct BODY
{
    const char* eia;
    const char* metric;
    double      l, w, t;
    double      esl;   // H, the part alone (mounting adds its own)
};

const BODY BODIES[] = {
    { "01005", "0402", 0.4, 0.2, 0.2, 0.25e-9 }, { "0201", "0603", 0.6, 0.3, 0.3, 0.3e-9 },
    { "0402", "1005", 1.0, 0.5, 0.5, 0.4e-9 },   { "0603", "1608", 1.6, 0.8, 0.8, 0.6e-9 },
    { "0805", "2012", 2.0, 1.25, 1.25, 0.8e-9 }, { "1206", "3216", 3.2, 1.6, 1.6, 1.0e-9 },
    { "1210", "3225", 3.2, 2.5, 2.5, 1.0e-9 },   { "1812", "4532", 4.5, 3.2, 2.0, 1.2e-9 },
    { "2220", "5750", 5.7, 5.0, 2.0, 1.2e-9 },
};

} // namespace


/// the body of a chip package named in a footprint / package text ("C_0402_1005Metric", "0603")
const BODY* bodyOf( const std::string& aText )
{
    // metric names first ("1005Metric" is an 0402, not a "1005")
    for( const BODY& b : BODIES )
    {
        if( aText.find( std::string( b.metric ) + "Metric" ) != std::string::npos )
            return &b;
    }

    for( const BODY& b : BODIES )
    {
        size_t at = aText.find( b.eia );

        while( at != std::string::npos )
        {
            bool before = at == 0 || !std::isdigit( static_cast<unsigned char>( aText[at - 1] ) );
            size_t e = at + std::strlen( b.eia );
            bool after = e >= aText.size() || !std::isdigit( static_cast<unsigned char>( aText[e] ) );

            if( before && after )
                return &b;

            at = aText.find( b.eia, at + 1 );
        }
    }

    return nullptr;
}


double MlccBias( const std::string& aDielectric, double aC, const std::string& aPackage, double aVdc, double* aLayer,
                 double* aField )
{
    std::string d = aDielectric;
    std::transform( d.begin(), d.end(), d.begin(), ::toupper );

    if( d.empty() || d == "C0G" || d == "NP0" || d == "COG" || d[0] == 'C' || d[0] == 'U' || d == "SL" )
        return 1.0;

    const BODY* b = bodyOf( aPackage );

    if( !b || aC <= 0 )
        return 1.0;

    // class 2 (BaTiO3): C = eps * V_active / d^2 for a stack of layers, so the layer thickness
    // follows from the capacitance per volume (half the body taken as active, eps_r 3000); the
    // capacitance left falls with the field V/d: 1 / (1 + (E / E0)^1.5), E0 2.5 V/µm (X5R / X7R
    // and the like), 1 V/µm (Y5V / Z5U). Checked against makers' curves: 0402 10 µF 6.3 V X5R
    // at 3.3 V ~23% (makers: 20-30%), 0402 100 nF X7R at 5 V ~83%, 0603 4.7 µF X5R at 5 V ~45%.
    const double eps = 8.854e-12 * 3000;
    const double vol = b->l * b->w * b->t * 1e-9 * 0.5;
    const double layer = std::sqrt( eps * vol / aC );
    const double e = std::fabs( aVdc ) / layer;
    const double e0 = ( d[0] == 'Y' || d[0] == 'Z' ) ? 1.0e6 : 2.5e6;

    if( aLayer )
        *aLayer = layer;

    if( aField )
        *aField = e;

    return 1.0 / ( 1.0 + std::pow( e / e0, 1.5 ) );
}

} // namespace sim


namespace
{

using sim::FormatSI;
using sim::ParseSI;

TOOL_RESULT fail( const std::string& aMsg ) { return { aMsg, true }; }

std::string upper( std::string s )
{
    std::transform( s.begin(), s.end(), s.begin(), ::toupper );
    return s;
}

std::string bare( const std::string& aNet )
{
    size_t slash = aNet.rfind( '/' );
    return slash == std::string::npos ? aNet : aNet.substr( slash + 1 );
}

std::string refPrefix( const std::string& aRef )
{
    size_t i = 0;

    while( i < aRef.size() && std::isalpha( static_cast<unsigned char>( aRef[i] ) ) )
        ++i;

    return upper( aRef.substr( 0, i ) );
}

std::string fmt( double aV, int aDigits = 4 )
{
    char buf[48];
    std::snprintf( buf, sizeof( buf ), "%.*g", aDigits, aV );
    return buf;
}

/// a SPICE-safe name piece ("U1A" -> "U1A", "C_3" stays)
std::string safe( const std::string& aText )
{
    std::string s;

    for( char c : aText )
        s += std::isalnum( static_cast<unsigned char>( c ) ) ? c : '_';

    return s;
}

/// a value from a JSON number or an SI string
bool jsonSI( const json& aV, double& aOut )
{
    if( aV.is_number() )
    {
        aOut = aV.get<double>();
        return true;
    }

    return aV.is_string() && ParseSI( aV.get<std::string>(), aOut );
}

/// a part's value field: its first word ("10uF 6.3V X5R" -> 1e-5)
bool valueOf( const std::string& aText, double& aOut )
{
    std::istringstream in( aText );
    std::string        tok;
    return ( in >> tok ) && ParseSI( tok, aOut );
}

/// "V(a,b)", or "V(a)" against ground
std::string vd( const std::string& a, const std::string& b )
{
    return b == "0" ? "V(" + a + ")" : "V(" + a + "," + b + ")";
}

/// a vector for wrdata: v(a) - v(b), ground as 0
std::string vx( const std::string& a, const std::string& b )
{
    if( a == "0" && b == "0" )
        return "0";

    // (never starting with "-" or "(": wrdata reads its list as one expression where it can, so
    // "i(x) -v(y)" would be one vector, i(x) - v(y))
    return a == "0" ? "v(" + b + ")*(-1)" : b == "0" ? "v(" + a + ")" : "v(" + a + ")-v(" + b + ")";
}

/// a source / load spec the agent wrote: a SPICE source value (no new statements)
bool specOk( const std::string& aSpec )
{
    if( aSpec.empty() || aSpec.size() > 400 )
        return false;

    for( char c : aSpec )
    {
        if( c == '\n' || c == '\r' || c == ';' || c == '$' || c == '{' || c == '}' || c == '\'' || c == '"' )
            return false;
    }

    return true;
}

/// the DC value of a source spec: "5", "dc 5 ac 1" -> 5, "pulse(0 3.3 ...)" -> 0 (its v1), "sin(5 ...)" -> 5
double specDc( const std::string& aSpec )
{
    std::string s = aSpec;
    std::transform( s.begin(), s.end(), s.begin(), ::tolower );
    double v = 0;

    if( ParseSI( s, v ) )
        return v;

    auto firstAfter = [&]( size_t aAt )
    {
        std::istringstream in( s.substr( aAt ) );
        std::string        tok;
        in >> tok;
        tok = tok.substr( 0, tok.find_first_of( ",)" ) );
        return ParseSI( tok, v );
    };

    size_t dc = s.find( "dc" );

    if( dc != std::string::npos && firstAfter( dc + 2 ) )
        return v;

    size_t paren = s.find( '(' );

    if( paren != std::string::npos && firstAfter( paren + 1 ) )
        return v;

    return 0;
}

/// a supply's voltage from its name: "+3V3" 3.3, "+5V" 5, "VCC_1V8" 1.8, "12V0" 12, "-12V" -12
std::optional<double> voltageFromName( const std::string& aNet )
{
    std::string n = upper( bare( aNet ) );
    std::smatch m;
    static const std::regex rxV( R"((^|[^0-9.])([+-]?)(\d+)V(\d*)($|[^0-9A-Z]|[A-Z]))" );
    static const std::regex rxP( R"((^|[^0-9])([+-]?)(\d+\.\d+)V?($|[^0-9]))" );

    if( std::regex_search( n, m, rxV ) )
    {
        double v = std::stod( m[3].str() + ( m[4].length() ? "." + m[4].str() : "" ) );
        return m[2] == "-" ? -v : v;
    }

    if( std::regex_search( n, m, rxP ) )
    {
        double v = std::stod( m[3].str() );
        return m[2] == "-" ? -v : v;
    }

    return std::nullopt;
}

// --- the parts ---------------------------------------------------------------------------

struct PART
{
    const ks::SchSimPart* sch = nullptr;
    const kb::LcscHit*    hit = nullptr;   // the parts index's row, when it has one
    json                  over;            // models[ref] from the agent

    std::string kind;    // R C L D Z M LDO OPAMP KICAD SUBCKT, "" no model
    std::string why;     // left out: why
    std::string notes;   // what the model is made of (for the answer)

    // values (SI), the tolerance (fraction), and what the agent overrode
    double r = 0, c = 0, l = 0, esr = 0, esl = 0, dcr = 0, srf = 0, tol = 0;
    std::string dielectric, package, capKind;   // capKind: MLCC, aluminium, tantalum, polymer
    double      vrated = 0;
    bool        biasOn = true;
    double      ceff = 0, vdc = 0, layer = 0, field = 0;   // after the DC bias pass
    double      vmin = 0;                                  // a polarised one: its lowest + to -
    bool        tolAssumed = false;

    mutable std::set<std::string> read;   // the models[ref] keys its model looked at

    double ilim = 0;   // a regulator's current limit as modelled (0: none)
};

double param( const PART& aP, const std::string& aId, double aDefault = 0 )
{
    aP.read.insert( aId );

    if( aP.over.contains( aId ) )
    {
        double v = 0;

        if( jsonSI( aP.over[aId], v ) )
            return v;
    }

    if( aP.hit )
    {
        auto it = aP.hit->values().find( aId );

        if( it != aP.hit->values().end() )
            return it->second;
    }

    return aDefault;
}

bool hasParam( const PART& aP, const std::string& aId )
{
    aP.read.insert( aId );
    return aP.over.contains( aId ) || ( aP.hit && aP.hit->values().count( aId ) );
}

std::string cond( const PART& aP, const std::string& aId )
{
    if( aP.hit )
    {
        auto it = aP.hit->conds().find( aId );

        if( it != aP.hit->conds().end() )
            return it->second;
    }

    return {};
}

/// the pins grouped by number (a multi-unit part lists shared pins in each unit)
std::vector<const ks::SchSimPin*> pinsByNumber( const ks::SchSimPart& aPart )
{
    std::vector<const ks::SchSimPin*> out;
    std::set<std::string>             seen;

    for( const ks::SchSimPin& p : aPart.pins() )
    {
        if( seen.insert( p.number() ).second )
            out.push_back( &p );
    }

    std::sort( out.begin(), out.end(), []( const ks::SchSimPin* a, const ks::SchSimPin* b )
               {
                   char* ea = nullptr;
                   char* eb = nullptr;
                   long  na = std::strtol( a->number().c_str(), &ea, 10 );
                   long  nb = std::strtol( b->number().c_str(), &eb, 10 );

                   if( *ea == 0 && *eb == 0 )
                       return na < nb;

                   return a->number() < b->number();
               } );

    return out;
}

std::string pinName( const ks::SchSimPin& aPin )
{
    std::string n = upper( aPin.name() );
    std::string out;

    for( char c : n )
    {
        if( c != '~' && c != '{' && c != '}' && c != ' ' )
            out += c;
    }

    return out;
}

/// what kind of part, from the index's subcategory or else the reference
std::string classify( const PART& aP )
{
    const std::string pre = refPrefix( aP.sch->ref() );
    std::string       sub = aP.hit ? upper( aP.hit->subcategory() ) : "";
    std::string       desc = upper( ( aP.hit ? aP.hit->description() + " " : "" ) + aP.sch->description() + " " + aP.sch->lib_id() );

    if( !sub.empty() )
    {
        if( sub.find( "MOSFET" ) != std::string::npos )
            return "M";

        if( sub.find( "LDO" ) != std::string::npos || sub.find( "LINEAR VOLTAGE REG" ) != std::string::npos || sub.find( "DROPOUT" ) != std::string::npos )
            return "LDO";

        if( sub.find( "AMPLIFIER" ) != std::string::npos || sub.find( "OPAMP" ) != std::string::npos || sub.find( "OP AMP" ) != std::string::npos )
            return "OPAMP";

        if( sub.find( "ZENER" ) != std::string::npos )
            return "Z";

        if( sub.find( "DIODE" ) != std::string::npos && sub.find( "LIGHT" ) == std::string::npos && sub.find( "TVS" ) == std::string::npos
            && sub.find( "ESD" ) == std::string::npos )
            return "D";

        if( sub.find( "FERRITE" ) != std::string::npos || sub.find( "BEAD" ) != std::string::npos )
            return "FB";

        if( sub.find( "INDUCTOR" ) != std::string::npos )
            return "L";

        if( sub.find( "CAPACITOR" ) != std::string::npos )
            return "C";

        if( sub.find( "RESISTOR" ) != std::string::npos && sub.find( "NETWORK" ) == std::string::npos && sub.find( "ARRAY" ) == std::string::npos )
            return "R";
    }

    const size_t pins = pinsByNumber( *aP.sch ).size();

    if( pins == 2 )
    {
        if( pre == "R" )
            return "R";

        if( pre == "C" )
            return "C";

        if( pre == "FB" || desc.find( "FERRITE" ) != std::string::npos )
            return "FB";

        if( pre == "L" )
            return "L";

        if( pre == "D" && desc.find( "LED" ) == std::string::npos )
            return desc.find( "ZENER" ) != std::string::npos ? "Z" : "D";
    }

    return "";
}

// --- what each built-in model leaves out (1d) ----------------------------------------------

/// one behaviour a built-in model does not have: its short name (the answer's line), what that
/// does to the result, and the models key that fills it in ("" none)
struct UNMODELLED
{
    const char* name;
    const char* effect;
    const char* key;
};

/// the built-in models' definitions by kind (the parts' kind codes); the models keys each one
/// reads are registered here too (2b)
struct MODEL_DEF
{
    const char*             kind;
    const char*             title;
    std::vector<UNMODELLED> unmodelled;
};

const std::vector<MODEL_DEF>& modelDefs()
{
    static const std::vector<MODEL_DEF> defs = {
        { "R", "resistor",
          { { "tempco", "the value is the same at any temperature", "" },
            { "parasitic L / C", "flat to any frequency", "" } } },
        { "C", "capacitor",
          { { "temperature", "C, ESR and the DC bias loss at 25 °C", "" },
            { "ageing", "class 2 MLCCs lose C over time (a few % per decade of hours)", "" },
            { "bias in tran / ac", "C_eff is set once from the op (or the window's average) and stays fixed while the voltage moves", "dc_bias" },
            { "ESR vs frequency", "one ESR at every frequency", "esr" },
            { "leakage, reverse bias", "a polarised capacitor is a capacitor either way round (reverse bias is said)", "" } } },
        { "L", "inductor",
          { { "saturation", "the inductance holds at any current (Isat is said with the peak)", "" },
            { "core loss", "only the DCR dissipates", "dcr" },
            { "temperature", "DCR at 25 °C", "" } } },
        { "FB", "ferrite bead",
          { { "impedance curve", "a resistor at its DCR: no filtering at any frequency", "kind l + l, dcr" },
            { "DC saturation", "", "" } } },
        { "D", "diode",
          { { "junction capacitance", "switches instantly apart from trr", "" },
            { "reverse recovery shape", "trr (when given) as a transit time only", "trr" },
            { "reverse leakage", "no Ir: reverse current is the model's IS only", "" },
            { "temperature", "Vf at 25 °C", "" } } },
        { "Z", "Zener",
          { { "knee shape", "breakdown is exponential at Izt, then Zzt per amp", "izt, zzt" },
            { "junction capacitance", "", "" },
            { "temperature", "Vz at 25 °C (its tempco not applied)", "" } } },
        { "M", "MOSFET",
          { { "level 1", "square law; no subthreshold conduction, Rds(on) set at one Vgs", "rds_on, vgs_th" },
            { "temperature", "Rds(on) at 25 °C (it rises about 1.5-2 x hot)", "" },
            { "avalanche", "no breakdown at Vds(max)", "" },
            { "capacitance vs voltage", "Ciss / Coss / Crss fixed", "" } } },
        { "LDO", "LDO",
          { { "thermal shutdown", "an overload stays at the current limit indefinitely", "" },
            { "output discharge", "off, the output decays through the load only", "" },
            { "reverse current", "output above input: leakage only (a part without reverse protection conducts through its body diode)", "" },
            { "dropout vs load", "one dropout at every current", "dropout" },
            { "noise", "", "" } } },
        { "OPAMP", "op-amp",
          { { "input common-mode range", "the inputs work from rail to rail", "" },
            { "output current limit", "25 Ω output resistance only", "" },
            { "phase reversal", "", "" },
            { "second pole", "one pole: no phase margin, no ringing from it", "gbw" },
            { "CMRR / PSRR, noise", "", "" } } },
    };

    return defs;
}

const MODEL_DEF* modelDef( const std::string& aKind )
{
    for( const MODEL_DEF& d : modelDefs() )
    {
        if( aKind == d.kind )
            return &d;
    }

    return nullptr;
}

/// the full list for tool_help: per model, each behaviour with its effect and key
std::string unmodelledHelp()
{
    std::string out = " Not modelled (each answer says it in short, per model): ";

    for( const MODEL_DEF& d : modelDefs() )
    {
        out += std::string( d.title ) + ": ";
        bool first = true;

        for( const UNMODELLED& u : d.unmodelled )
        {
            out += std::string( first ? "" : "; " ) + u.name + ( *u.effect ? std::string( " (" ) + u.effect + ")" : "" )
                   + ( *u.key ? std::string( " [" ) + u.key + "]" : "" );
            first = false;
        }

        out += ". ";
    }

    return out + "LDO soft start and current limit: said with the part when not known (soft_start, ilim).";
}

// --- the netlist -------------------------------------------------------------------------

struct NETLIST
{
    std::map<std::string, std::string> spiceOf;   // schematic net -> its node
    std::map<std::string, std::string> bySpice;   // KiCad's exporter name -> its node
    std::set<std::string>              grounds;
    std::vector<std::string>           lines;
    std::vector<std::string>           models;    // .model / .subckt
    std::set<std::string>              includes;
    std::set<std::string>              driven;    // nets a model drives (an LDO's output)
    std::vector<std::pair<std::string, std::string>> currents;   // (label, vector) to read
    int                                nc = 0;

    /// what the model checks after a run read (1c): the part, its kind, vectors by role, numbers
    struct CHECK
    {
        std::string                        ref, kind;
        std::map<std::string, std::string> vec;
        std::map<std::string, double>      num;
    };

    std::vector<CHECK> checks;

    std::string node( const ks::SchSimPin& aPin, const std::string& aRef )
    {
        if( aPin.net().empty() )
            return "nc_" + safe( aRef ) + "_" + safe( aPin.number() ) + "_" + std::to_string( nc++ );

        auto it = spiceOf.find( aPin.net() );
        return it == spiceOf.end() ? safe( aPin.net() ) : it->second;
    }
};

/// a SPICE node name ngspice reads (KiCad's spice names may hold '/', '+', '-': fine in ngspice,
/// but a leading digit or '+' / '-' confuses expressions, so nodes are written as they are and
/// read back through v(<node>) with the same text)
std::string nodeOf( const ks::SchSimNet& aNet )
{
    if( aNet.ground() )
        return "0";

    return "n" + safe( aNet.spice() );
}

/// R / C / L / FB / D / Z between the part's two pins
bool twoPinNodes( const PART& aP, NETLIST& aN, std::string& a, std::string& b )
{
    std::vector<const ks::SchSimPin*> pins = pinsByNumber( *aP.sch );

    if( pins.size() != 2 )
        return false;

    a = aN.node( *pins[0], aP.sch->ref() );
    b = aN.node( *pins[1], aP.sch->ref() );
    return true;
}

/// a diode's anode and cathode: by name, else KiCad's Device:D order (1 K, 2 A)
bool diodeNodes( const PART& aP, NETLIST& aN, std::string& aA, std::string& aK )
{
    std::vector<const ks::SchSimPin*> pins = pinsByNumber( *aP.sch );

    if( pins.size() != 2 )
        return false;

    const ks::SchSimPin* an = nullptr;
    const ks::SchSimPin* ca = nullptr;

    for( const ks::SchSimPin* p : pins )
    {
        std::string n = pinName( *p );

        if( n == "A" || n.rfind( "ANODE", 0 ) == 0 || n == "+" )
            an = p;
        else if( n == "K" || n == "C" || n.rfind( "CATHODE", 0 ) == 0 || n == "-" )
            ca = p;
    }

    if( !an || !ca || an == ca )
    {
        ca = pins[0];
        an = pins[1];
    }

    aA = aN.node( *an, aP.sch->ref() );
    aK = aN.node( *ca, aP.sch->ref() );
    return true;
}

/// the passive values of a part (once: the DC bias and the corners change c / r / l later)
void passiveValues( PART& aP, std::string& aNote )
{
    const ks::SchSimPart& s = *aP.sch;
    double                v = 0;
    const kb::LcscHit*    h = aP.hit;
    auto                  over = [&]( const char* k, double& aOut )
    {
        aP.read.insert( k );
        return aP.over.contains( k ) && jsonSI( aP.over[k], aOut );
    };

    aP.read.insert( "tol" );

    if( h && h->tolerance() > 0 )
        aP.tol = h->tolerance() / 100;

    if( aP.over.contains( "tol" ) && jsonSI( aP.over["tol"], v ) )
        aP.tol = v / 100;

    if( aP.kind == "R" )
    {
        if( !over( "r", aP.r ) )
        {
            if( h && h->unit() == "Ω" && h->value() > 0 )
                aP.r = h->value();
            else if( valueOf( s.value(), v ) )
                aP.r = v;
        }

        if( aP.tol == 0 )
        {
            aP.tol = 0.01;
            aP.tolAssumed = true;
        }
    }
    else if( aP.kind == "C" )
    {
        sim::CAP_MODEL m = sim::CapModel( s.value(), s.footprint() + " " + s.lib_id() + " " + s.value(), s.description(), h, aP.over );
        aP.c = m.c;
        aP.capKind = m.kind;
        aP.package = m.package;
        aP.dielectric = m.dielectric;
        aP.vrated = m.vrated;
        aP.esr = m.esr;
        aP.esl = m.esl;
        aP.biasOn = m.biasOn;

        if( !aP.over.contains( "tol" ) && !( h && h->tolerance() > 0 ) )
        {
            aP.tol = m.tol;
            aP.tolAssumed = true;
        }

        aNote = m.note;
    }
    else if( aP.kind == "L" || aP.kind == "FB" )
    {
        if( !over( "l", aP.l ) )
        {
            if( h && h->unit() == "H" && h->value() > 0 )
                aP.l = h->value();
            else if( hasParam( aP, "l" ) )
                aP.l = param( aP, "l" );
            else if( valueOf( s.value(), v ) && upper( s.value() ).find( "OHM" ) == std::string::npos )
                aP.l = v;
        }

        if( !over( "dcr", aP.dcr ) )
            aP.dcr = param( aP, "dcr" );

        aP.srf = param( aP, "srf" );

        if( aP.tol == 0 )
        {
            aP.tol = 0.2;
            aP.tolAssumed = true;
        }
    }
}

/// the model lines of one part; false: no model (aP.why says why)
bool emit( PART& aP, NETLIST& aN, double aScale )
{
    const ks::SchSimPart& s = *aP.sch;
    const std::string     id = safe( s.ref() );
    std::string           a, b;
    auto                  add = [&]( const std::string& l ) { aN.lines.push_back( l ); };

    if( aP.kind == "KICAD" )
    {
        // KiCad's line names nets as its exporter does (the nets' "spice" names): ours instead
        std::string        line, tok;
        std::istringstream in( s.spice_item() );
        std::string        rest = s.spice_item();
        size_t             i = 0;

        while( i < rest.size() )
        {
            size_t j = rest.find_first_of( " \t\n", i );

            if( j == std::string::npos )
                j = rest.size();

            tok = rest.substr( i, j - i );
            auto it = aN.bySpice.find( tok );
            line += it != aN.bySpice.end() ? it->second : tok;

            if( j < rest.size() )
                line += rest[j];

            i = j + 1;
        }

        add( line );
        return true;
    }

    if( aP.kind == "SUBCKT" )
    {
        // models {ref: {subckt, lib, pins}}: pins in the subckt's order, as pin numbers or names
        std::string line = "X" + id;

        for( const json& pin : aP.over["pins"] )
        {
            std::string want = pin.is_string() ? pin.get<std::string>() : pin.dump();
            const ks::SchSimPin* found = nullptr;

            for( const ks::SchSimPin& p : s.pins() )
            {
                if( p.number() == want )
                    found = &p;
            }

            for( const ks::SchSimPin& p : s.pins() )
            {
                if( !found && upper( p.name() ) == upper( want ) )
                    found = &p;
            }

            if( !found )
            {
                aP.why = "models." + s.ref() + ".pins: no pin " + want;
                return false;
            }

            line += " " + aN.node( *found, s.ref() );

            // a power output in the subckt drives its rail: no source from the rail's name there
            if( found->type().find( "power out" ) != std::string::npos || found->type() == "power_out" )
            {
                if( !found->net().empty() )
                    aN.driven.insert( found->net() );
            }
        }

        add( line + " " + aP.over["subckt"].get<std::string>() );
        return true;
    }

    if( aP.kind == "R" )
    {
        double v0 = -1;
        // a 0 Ω jumper ("0", "0R"): a short, as 1 mΩ (a node pair stays two nodes)
        const bool jumper = aP.r == 0 && !aP.over.contains( "r" ) && valueOf( s.value(), v0 ) && v0 == 0;

        if( ( aP.r <= 0 && !jumper ) || !twoPinNodes( aP, aN, a, b ) )
        {
            aP.why = "no resistance (value \"" + s.value() + "\")";
            return false;
        }

        if( jumper )
            aP.notes = "0 Ω jumper: 1 mΩ";

        add( "R" + id + " " + a + " " + b + " " + fmt( jumper ? 1e-3 : aP.r * aScale, 6 ) );
        return true;
    }

    if( aP.kind == "C" )
    {
        if( aP.c <= 0 || !twoPinNodes( aP, aN, a, b ) )
        {
            aP.why = "no capacitance (value \"" + s.value() + "\")";
            return false;
        }

        const double c = ( aP.ceff > 0 ? aP.ceff : aP.c ) * aScale;
        add( "C" + id + " " + a + " kd_" + id + "_1 " + fmt( c, 6 ) );
        add( "R" + id + "_esr kd_" + id + "_1 kd_" + id + "_2 " + fmt( aP.esr, 4 ) );
        add( "L" + id + "_esl kd_" + id + "_2 " + b + " " + fmt( aP.esl, 4 ) );
        return true;
    }

    if( aP.kind == "L" || aP.kind == "FB" )
    {
        if( !twoPinNodes( aP, aN, a, b ) )
            return false;

        if( aP.kind == "FB" || aP.l <= 0 )
        {
            // a ferrite bead: its DC resistance only (its impedance curve is not in the data)
            add( "R" + id + " " + a + " " + b + " " + fmt( std::max( aP.dcr, 0.01 ), 4 ) );
            aP.notes = "ferrite / no inductance: " + FormatSI( std::max( aP.dcr, 0.01 ), "Ω", 2 ) + " (its impedance curve is not modelled)";
            return true;
        }

        add( "V" + id + "_i " + a + " kd_" + id + "_1 0" );
        add( "L" + id + " kd_" + id + "_1 kd_" + id + "_2 " + fmt( aP.l * aScale, 6 ) );
        add( "R" + id + "_dcr kd_" + id + "_2 " + b + " " + fmt( std::max( aP.dcr, 1e-4 ), 4 ) );

        if( aP.srf > 0 )
            add( "C" + id + "_srf " + a + " " + b + " " + fmt( 1.0 / ( std::pow( 2 * PI * aP.srf, 2 ) * aP.l ), 4 ) );

        aN.currents.push_back( { s.ref(), "i(v" + id + "_i)" } );
        return true;
    }

    if( aP.kind == "D" || aP.kind == "Z" )
    {
        if( !diodeNodes( aP, aN, a, b ) )
        {
            aP.why = "not a two-pin diode";
            return false;
        }

        const bool schottky = upper( aP.hit ? aP.hit->subcategory() + aP.hit->description() : s.value() ).find( "SCHOTTKY" ) != std::string::npos;
        const double n = schottky ? 1.05 : 1.8;
        double       vf = param( aP, "vf", schottky ? 0.45 : 0.7 );
        double       iff = 0;

        if( !ParseSI( cond( aP, "vf" ), iff ) || iff <= 0 )
            iff = param( aP, "if", 0.01 );

        // the data's Vf is at its If, often the rated current (2.2 V @ 30 A): the junction's share at
        // most a typical drop there (silicon 0.85 V, Schottky 0.5 V, SiC 1.1 V), the rest in series
        // resistance; else at 4 mA a fast-recovery diode would drop 1.8 V (models.rs sets it)
        const std::string dsc = upper( aP.hit ? aP.hit->subcategory() + " " + aP.hit->description() : s.value() );
        const bool        sic = dsc.find( "SIC" ) != std::string::npos || dsc.find( "SILICON CARBIDE" ) != std::string::npos;
        const double      vjMax = schottky ? 0.5 : sic ? 1.1 : 0.85;
        double            rs = 0;
        double            vj = vf;

        if( hasParam( aP, "rs" ) )
        {
            rs = std::max( 0.0, param( aP, "rs" ) );
            vj = std::max( 0.05, vf - rs * iff );
        }
        else if( vf > vjMax )
        {
            vj = vjMax;
            rs = ( vf - vj ) / iff;
        }

        const double is = iff / std::exp( vj / ( n * 0.02585 ) );
        std::string  model = "D(IS=" + fmt( is ) + " N=" + fmt( n ) + " RS=" + fmt( rs );
        std::string  what;

        if( aP.kind == "Z" && hasParam( aP, "vz" ) )
        {
            // the breakdown as its own branch, cathode to anode: Zzt in series there only (the
            // forward drop is the diode's, without it), Vz at the test current Izt (the data's
            // condition of vz, else 5 mA), rising by Zzt per amp past it
            const double vz = param( aP, "vz" );
            const double zzt = hasParam( aP, "zzt" ) ? std::max( param( aP, "zzt" ), 0.1 ) : 1.0;
            double       izt = 0;

            const bool iztKnown = hasParam( aP, "izt" ) || ( ParseSI( cond( aP, "vz" ), izt ) && izt > 0 && izt < 1 );

            if( hasParam( aP, "izt" ) )
                izt = param( aP, "izt" );
            else if( !iztKnown )
                izt = 5e-3;

            const double isz = 1e-14;
            const double off = vz - 0.02585 * std::log( izt / isz ) - zzt * izt;
            add( "R" + id + "_zz " + b + " kd_" + id + "_z1 " + fmt( zzt ) );
            add( "V" + id + "_zv kd_" + id + "_z1 kd_" + id + "_z2 " + fmt( off, 6 ) );
            add( "D" + id + "_z kd_" + id + "_z2 " + a + " kd_dz_" + id );
            aN.models.push_back( ".model kd_dz_" + id + " D(IS=" + fmt( isz ) + " N=1)" );
            what = "Vz " + FormatSI( vz, "V" ) + " @" + FormatSI( izt, "A" ) + ( iztKnown ? "" : " assumed" ) + ", Zzt "
                   + FormatSI( zzt, "Ω" ) + ( hasParam( aP, "zzt" ) ? "" : " assumed" ) + " (reverse only)";
        }
        else if( aP.kind == "Z" )
        {
            what = "no Vz in the data (models." + s.ref() + ".vz): no breakdown modelled";
        }

        if( hasParam( aP, "trr" ) )
            model += " TT=" + fmt( param( aP, "trr" ) / 1.5 );

        // (its current through a 0 V source: the model checks read it; the breakdown branch is apart)
        add( "V" + id + "_di " + a + " kd_" + id + "_da 0" );
        add( "D" + id + " kd_" + id + "_da " + b + " kd_d_" + id );
        aN.models.push_back( ".model kd_d_" + id + " " + model + ")" );
        aN.checks.push_back( { s.ref(), "D", { { "i", "i(v" + id + "_di)" }, { "v", vx( a, b ) } },
                               { { "vz", aP.kind == "Z" && hasParam( aP, "vz" ) ? param( aP, "vz" ) : 0.0 } } } );
        aP.notes = ( hasParam( aP, "vf" ) ? "Vf " + FormatSI( vf, "V" ) + " @" + FormatSI( iff, "A" )
                                          : "Vf " + FormatSI( vf, "V" ) + " assumed (" + ( schottky ? "Schottky" : "silicon" ) + ")" )
                   + ( rs > 0 ? " (" + FormatSI( vj, "V" ) + " junction + " + FormatSI( rs, "Ω" ) + ( hasParam( aP, "rs" ) ? ")" : ", split assumed)" ) : "" )
                   + ( what.empty() ? "" : ", " + what ) + ( hasParam( aP, "trr" ) ? ", trr " + FormatSI( param( aP, "trr" ), "s" ) : "" )
                   + "; Cj not in the data (none)";
        return true;
    }

    std::vector<const ks::SchSimPin*> pins = pinsByNumber( s );

    // pins by role (names): the first net of each
    auto findPin = [&]( std::initializer_list<const char*> aNames, bool aPrefix = false ) -> const ks::SchSimPin*
    {
        for( const char* want : aNames )
        {
            for( const ks::SchSimPin* p : pins )
            {
                std::string n = pinName( *p );

                if( aPrefix ? n.rfind( want, 0 ) == 0 : n == want )
                    return p;
            }
        }

        return nullptr;
    };

    if( aP.kind == "M" )
    {
        const ks::SchSimPin* g = findPin( { "G", "GATE" } );
        const ks::SchSimPin* d = findPin( { "D", "DRAIN" } );
        const ks::SchSimPin* so = findPin( { "S", "SOURCE" } );

        if( !g || !d || !so )
        {
            aP.why = "MOSFET pins not named G / D / S (one MOSFET)";
            return false;
        }

        std::string desc = upper( ( aP.hit ? aP.hit->description() : "" ) + " " + s.value() + " " + s.description() );
        const bool  pch = desc.find( "P-CHANNEL" ) != std::string::npos || desc.find( "PMOS" ) != std::string::npos
                         || desc.find( "P CHANNEL" ) != std::string::npos || desc.find( "P沟道" ) != std::string::npos;
        const double vth = std::fabs( param( aP, "vgs_th", 2.0 ) );
        double       vgs = 10;
        double       cv = 0;

        if( ParseSI( cond( aP, "rds_on" ), cv ) && std::fabs( cv ) > vth )
            vgs = std::fabs( cv );

        const double rds = param( aP, "rds_on", 0.1 );
        const double kp = 1.0 / ( rds * std::max( 0.1, vgs - vth ) );
        const double ciss = param( aP, "ciss" ), coss = param( aP, "coss" ), crss = param( aP, "crss" );
        std::string  nd = aN.node( *d, s.ref() ), ng = aN.node( *g, s.ref() ), ns = aN.node( *so, s.ref() );

        // the channel's current through a 0 V source in the drain (the model checks; the body diode
        // and the capacitances stay on the pin)
        add( "V" + id + "_di " + nd + " kd_" + id + "_d 0" );
        add( "M" + id + " kd_" + id + "_d " + ng + " " + ns + " " + ns + " kd_m_" + id );
        aN.checks.push_back( { s.ref(), "M", { { "i", "i(v" + id + "_di)" }, { "vgs", vx( ng, ns ) }, { "vds", vx( nd, ns ) } },
                               { { "vth", vth }, { "pch", pch ? 1.0 : 0.0 } } } );
        aN.models.push_back( ".model kd_m_" + id + ( pch ? " PMOS" : " NMOS" ) + "(LEVEL=1 VTO=" + fmt( pch ? -vth : vth ) + " KP=" + fmt( kp )
                             + " LAMBDA=0.01)" );

        if( crss > 0 )
            add( "C" + id + "_gd " + ng + " " + nd + " " + fmt( crss ) );

        if( ciss > crss )
            add( "C" + id + "_gs " + ng + " " + ns + " " + fmt( ciss - crss ) );

        if( coss > crss )
            add( "C" + id + "_ds " + nd + " " + ns + " " + fmt( coss - crss ) );

        add( "D" + id + "_body " + ( pch ? nd + " " + ns : ns + " " + nd ) + " kd_db_" + id );
        aN.models.push_back( ".model kd_db_" + id + " D(IS=1e-12 N=1.2 RS=0.01)" );
        aP.notes = std::string( pch ? "P" : "N" ) + "-ch level 1: Vgs(th) " + FormatSI( vth, "V" ) + ( hasParam( aP, "vgs_th" ) ? "" : " assumed" )
                   + ", Rds(on) " + FormatSI( rds, "Ω" ) + " @" + fmt( vgs ) + " V" + ( hasParam( aP, "rds_on" ) ? "" : " assumed" )
                   + ( ciss > 0 ? ", Ciss " + FormatSI( ciss, "F" ) + " / Coss " + FormatSI( coss, "F" ) + " / Crss " + FormatSI( crss, "F" )
                                  + " fixed (no voltage dependence)"
                                : ", no capacitances in the data" );
        return true;
    }

    if( aP.kind == "LDO" )
    {
        const ks::SchSimPin* in = findPin( { "VIN", "IN", "VI", "INPUT", "VCC", "VDD" } );
        const ks::SchSimPin* out = findPin( { "VOUT", "OUT", "VO", "OUTPUT" } );
        const ks::SchSimPin* gnd = findPin( { "GND", "VSS", "ADJ/GND", "GND/ADJ" } );
        const ks::SchSimPin* fb = findPin( { "FB", "ADJ", "SENSE/ADJ", "ADJ/SENSE" } );
        const ks::SchSimPin* en = findPin( { "EN", "CE", "ON/OFF", "SHDN", "~SHDN", "INH" } );

        if( !in || !out )
        {
            aP.why = "regulator pins not named IN / OUT";
            return false;
        }

        if( !gnd )
        {
            // the AMS1117 kind: ADJ is the ground pin of the fixed versions
            gnd = fb;
            fb = nullptr;
        }

        std::string nin = aN.node( *in, s.ref() ), nout = aN.node( *out, s.ref() );
        std::string ng = gnd ? aN.node( *gnd, s.ref() ) : "0";
        const bool  adjustable = fb && !fb->net().empty() && fb->net() != out->net();
        double      vref = param( aP, "vref", 0 );
        double      vout = param( aP, "vout", 0 );

        if( adjustable && vref <= 0 )
            vref = vout > 0 && vout < 1.3 ? vout : 0;

        if( !adjustable )
            vref = vout;

        if( vref <= 0 )
        {
            aP.why = adjustable ? "adjustable regulator: its reference is not in the data (models." + s.ref() + ".vref)"
                                : "no output voltage in the data (models." + s.ref() + ".vout)";
            return false;
        }

        const double drop = param( aP, "dropout", 0.3 );
        const double iq = param( aP, "iq", 50e-6 );
        const double psrr = param( aP, "psrr", 60 );
        const double fbw = param( aP, "loop_bw", 100e3 );
        const double psrrF = param( aP, "psrr_f", fbw / 10 );
        const double psrrHf = std::min( psrr, param( aP, "psrr_hf", 20 ) );
        const double rout = param( aP, "rout", 0.01 );
        const double gain = 1e4;   // 0.01% off Vref at DC
        const std::string nfb = adjustable ? aN.node( *fb, s.ref() ) : "kd_" + id + "_o";
        const std::string k = "kd_" + id + "_";

        std::string on = "1";

        if( en && !en->net().empty() )
            on = "(" + vd( aN.node( *en, s.ref() ), ng ) + ">0.9?1:0)";

        // PSRR: the input's ripple (AC-coupled) on the output at the data's PSRR to psrr_f, then 20 dB / dec
        // worse to psrr_hf: a * (1 + s/w1) / (1 + s/w2) = a * v + a * (w2/w1 - 1) * (v high-passed at w2)
        const double a = std::pow( 10, -psrr / 20 );
        const double ratio = std::pow( 10, ( psrr - psrrHf ) / 20 );
        add( "C" + id + "_hp " + nin + " " + k + "h 1u" );
        add( "R" + id + "_hp " + k + "h " + ng + " 1Meg" );
        add( "C" + id + "_h2 " + k + "h " + k + "h2 1n" );
        add( "R" + id + "_h2 " + k + "h2 " + ng + " " + fmt( 1 / ( 2 * PI * psrrF * ratio * 1e-9 ) ) );
        // (only while the pass element conducts: off, or at the start of a soft start, it passes
        // no ripple; the scale is 0 .. 1 over its first 100 mV)
        add( "B" + id + "_r " + k + "r " + ng + " V=" + on + "*min(1,max(0," + vd( k + "p", ng ) + "*10))*(" + fmt( a ) + "*" + vd( k + "h", ng ) + "+"
             + fmt( a * ( ratio - 1 ) ) + "*" + vd( k + "h2", ng ) + ")" );

        // error amplifier (one pole: the loop crosses at loop_bw with the feedback at 1), then the
        // pass element: at most Vin - dropout, at least 0, plus the ripple; the amplifier does not
        // see the ripple (it is the PSRR the loop leaves); the input draws the output current plus Iq
        const std::string sense = adjustable ? "(" + vd( nfb, ng ) + "-" + vd( k + "r", ng ) + "*" + vd( nfb, ng ) + "/max(" + vd( k + "o", ng ) + ",1m))"
                                             : "(" + vd( nfb, ng ) + "-" + vd( k + "r", ng ) + ")";
        // soft start (models.soft_start, s: the datasheet's start-up time): the reference ramps 0 ->
        // Vref in that time from when EN is on and Vin is over uvlo (default 1 V, assumed), so the
        // output rises at Vout / soft_start and the inrush into its capacitors is C x that. A node
        // charged at 1/soft_start per second into 1 F, held at 0 while off; at the DC point it is
        // at 1 (on) or 0 (off)
        const double tss = param( aP, "soft_start", 0 );
        const double uvlo = tss > 0 ? param( aP, "uvlo", 1.0 ) : 0;
        std::string  ref = fmt( vref, 6 );

        if( tss > 0 )
        {
            const std::string ss = vd( k + "ss", ng );
            const std::string go = "(" + on + "*(" + vd( nin, ng ) + ">" + fmt( uvlo ) + "?1:0))";
            add( "C" + id + "_ss " + k + "ss " + ng + " 1" );
            add( "B" + id + "_ss " + ng + " " + k + "ss I=" + go + "*" + fmt( 1 / tss ) + "*min(1,max(0,(1-" + ss + ")*50))-(1-" + go + ")*" + ss + "*1e6" );
            add( "R" + id + "_ss " + k + "ss " + ng + " 1G" );
            ref = fmt( vref, 6 ) + "*min(1,max(0," + ss + "))";
        }

        add( "B" + id + "_e " + k + "e " + ng + " V=" + fmt( gain ) + "*(" + ref + "-" + sense + ")" );
        add( "R" + id + "_p " + k + "e " + k + "p 1Meg" );
        add( "C" + id + "_p " + k + "p " + ng + " " + fmt( gain / ( 2 * PI * 1e6 * fbw ) ) );

        // anti-windup: the amplifier's output stays within what the pass element can give, 0 ..
        // Vin - dropout (a stiff clamp, a few mV past it). Without it a tran that starts with Vin at
        // 0 V winds it up to gain * Vref and the output sits at Vin - dropout until that discharges;
        // with the bound at Vin instead, the output overshoots while it comes down the dropout
        // (and at 0 while EN is off, so the output starts from 0 when it turns on)
        // current limit: the pass element at most Rout * ilim above the output (ilim from the data,
        // models.ilim, else 1.5 x the rated Iout; none when neither is known)
        double ilim = param( aP, "ilim", 0 );

        if( ilim <= 0 && hasParam( aP, "iout" ) && param( aP, "iout" ) > 0 )
            ilim = 1.5 * param( aP, "iout" );

        aP.ilim = ilim;

        // (while it limits, the amplifier is held 20 mV over what the limit lets through: it does not
        // wind up to Vin - dropout and overshoot when the output catches up, as after EN turns on
        // into empty output capacitors)
        std::string top = "max(" + vd( nin, ng ) + "-" + fmt( drop ) + ",0)";

        if( ilim > 0 )
            top = "min(" + top + ",max(" + vd( nout, ng ) + "+" + fmt( ilim * rout + 0.02, 6 ) + ",0))";

        add( "B" + id + "_cl " + k + "p " + ng + " I=max(" + vd( k + "p", ng ) + "-" + on + "*" + top + ",0)+min(" + vd( k + "p", ng ) + ",0)" );

        // out of regulation (the amplifier at its top: in dropout, limiting, starting up or off) the
        // loop does not reject the input, so what the input did then is no ripple: the PSRR
        // high-pass is held at 0 and its feed-through starts where regulation does (else a start-up
        // from 0 V sits on the output as PSRR x Vin for the high-pass's second)
        add( "B" + id + "_hr " + k + "h " + ng + " I=(" + vd( k + "p", ng ) + ">" + on + "*" + top + "-0.005?1:0)*" + vd( k + "h", ng ) );

        // (on everything the pass element gives, the input's ripple too; ngspice's min() takes two)
        std::string pass = on + "*max(0,min(" + vd( k + "p", ng ) + "," + vd( nin, ng ) + "-" + fmt( drop ) + "))+" + vd( k + "r", ng );

        if( ilim > 0 )
            pass = "min(" + pass + "," + vd( nout, ng ) + "+" + fmt( ilim * rout, 6 ) + ")";

        add( "B" + id + "_o " + k + "x " + ng + " V=" + pass );
        add( "V" + id + "_i " + k + "x " + k + "o 0" );
        // Rout one way: an LDO's pass element does not sink current, nor pass it back to the input
        // (backwards 1 µS, leakage, so the operating point keeps a slope there; a pass element
        // without reverse protection would conduct through its body diode: not modelled, said)
        add( "B" + id + "_ro " + k + "o " + nout + " I=max(" + vd( k + "o", nout ) + "/" + fmt( rout ) + "," + vd( k + "o", nout ) + "*1u)" );
        add( "B" + id + "_in " + nin + " " + ng + " I=max(I(V" + id + "_i),0)+" + fmt( iq ) );
        add( "R" + id + "_gmin " + k + "p " + ng + " 1G" );

        aN.driven.insert( out->net() );
        aN.currents.push_back( { s.ref() + " out", "i(v" + id + "_i)" } );
        aN.checks.push_back( { s.ref(), "LDO", { { "in", vx( nin, ng ) }, { "out", vx( nout, ng ) }, { "i", "i(v" + id + "_i)" } },
                               { { "set", adjustable ? 0.0 : vref }, { "drop", drop }, { "ilim", ilim },
                                 { "iout", hasParam( aP, "iout" ) ? param( aP, "iout" ) : 0.0 } } } );

        auto src = [&]( const char* aId ) { return hasParam( aP, aId ) ? "" : " assumed"; };

        // the data says the part has a soft start but not its time: said, with where to give it
        const std::string desc = upper( ( aP.hit ? aP.hit->description() + " " : "" ) + s.description() );
        const bool        listsSoft = desc.find( "SOFT START" ) != std::string::npos || desc.find( "SOFT-START" ) != std::string::npos;
        std::string       softNote;

        if( tss > 0 )
            softNote = "; soft start " + FormatSI( tss, "s" ) + " (reference ramp from EN on and Vin over " + FormatSI( uvlo, "V" ) + src( "uvlo" ) + ")";
        else if( listsSoft )
            softNote = "; no soft start modelled although the data lists one (its time is not in the data): the output steps up to "
                       "Vin - dropout as fast as the loop allows, so the start-up inrush is overstated; models." + s.ref()
                       + ".soft_start (s, the datasheet's start-up time)";
        else
            softNote = "; no soft start (the output follows Vin - dropout up to its setpoint; models." + s.ref() + ".soft_start sets one)";

        aP.notes = std::string( adjustable ? "Vref " : "Vout " ) + FormatSI( vref, "V" ) + ( adjustable ? " on " + bare( fb->net() ) : "" )
                   + ", dropout " + FormatSI( drop, "V" ) + src( "dropout" ) + ", Iq " + FormatSI( iq, "A" ) + src( "iq" ) + ", PSRR "
                   + fmt( psrr, 3 ) + " dB" + ( hasParam( aP, "psrr" ) ? " (the data's one number)" : " assumed" ) + " to " + FormatSI( psrrF, "Hz" )
                   + src( "psrr_f" ) + ", then -20 dB/dec to " + fmt( psrrHf, 3 ) + " dB" + src( "psrr_hf" ) + "; loop "
                   + FormatSI( fbw, "Hz" ) + src( "loop_bw" ) + ", Rout " + FormatSI( rout, "Ω" ) + src( "rout" )
                   + ( ilim > 0 ? "; current limit " + FormatSI( ilim, "A" ) + ( hasParam( aP, "ilim" ) ? "" : " (1.5 x Iout, assumed)" )
                                : std::string( "; no current limit (no Iout in the data: models." ) + s.ref() + ".ilim)" )
                   + softNote;
        return true;
    }

    if( aP.kind == "OPAMP" )
    {
        // channels: the pins' names without IN / OUT / + / -, with the unit ("A" of a dual)
        struct CH
        {
            const ks::SchSimPin* p = nullptr;
            const ks::SchSimPin* n = nullptr;
            const ks::SchSimPin* o = nullptr;
        };

        // the pins' roles by name: +IN / IN+ / INA+ / NONINVERTING INPUT, -IN / INVERTING INPUT, OUT / OUTA
        struct ROLE
        {
            const ks::SchSimPin* pin;
            int                  role;   // 1 +, 2 -, 3 out
            std::string          key;    // the channel: the unit and what is left of the name
        };

        std::vector<ROLE>    roles;
        const ks::SchSimPin* vp = nullptr;
        const ks::SchSimPin* vn = nullptr;

        for( const ks::SchSimPin& pin : s.pins() )
        {
            std::string n;

            for( char c : pinName( pin ) )
            {
                if( c != '_' )
                    n += c;
            }

            if( n == "V+" || n == "VCC" || n == "VDD" || n == "VS+" || n == "+VS" || n == "VCC+" || n == "+V" || n == "V+/VDD" )
            {
                vp = vp ? vp : &pin;
                continue;
            }

            if( n == "V-" || n == "VEE" || n == "VSS" || n == "GND" || n == "VS-" || n == "-VS" || n == "VCC-" || n == "-V" || n == "V-/GND" )
            {
                vn = vn ? vn : &pin;
                continue;
            }

            int role = 0;

            if( n.find( "NONINV" ) != std::string::npos || n.find( "NON-INV" ) != std::string::npos )
                role = 1;
            else if( n.find( "INV" ) != std::string::npos )
                role = 2;
            else if( n.find( "OUT" ) != std::string::npos || n == "O" )
                role = 3;
            else if( n.find( '+' ) != std::string::npos )
                role = 1;
            else if( n.find( '-' ) != std::string::npos )
                role = 2;
            else if( n.empty() && pin.type().find( "output" ) != std::string::npos )
                role = 3;   // KiCad's Amplifier_Operational: "~" (no name) for the output

            if( !role )
                continue;

            std::string key = n;

            for( const char* tok : { "NON-INVERTING", "NONINVERTING", "INVERTING", "INPUT", "OUTPUT", "OUT", "IN", "+", "-" } )
            {
                size_t at;

                while( ( at = key.find( tok ) ) != std::string::npos )
                    key.erase( at, std::strlen( tok ) );
            }

            roles.push_back( { &pin, role, pin.unit() + "|" + key } );
        }

        // the channels: 1. the units (A / B); 2. what is left of the names (INA+ OUTA, 1IN- 1OUT);
        // 3. where one unit has the same names more than once, each output with the nearest inputs in
        // the symbol (who drew it put a channel's pins together); 4. without places, the nearest pin
        // numbers (a dual's 1 2 3 / 7 6 5). 3 and 4 are guesses: said as inferred.
        auto pinNo = []( const ks::SchSimPin* aPin ) { return std::strtol( aPin->number().c_str(), nullptr, 10 ); };
        std::map<std::string, std::vector<const ROLE*>> byKey;

        for( const ROLE& r : roles )
            byKey[r.key].push_back( &r );

        std::vector<CH> chans;
        std::string     inferred;

        for( auto& [key, list] : byKey )
        {
            std::vector<const ROLE*> outs;
            int                      nRole[4] = { 0, 0, 0, 0 };

            for( const ROLE* r : list )
            {
                nRole[r->role]++;

                if( r->role == 3 )
                    outs.push_back( r );
            }

            std::sort( outs.begin(), outs.end(), [&]( const ROLE* x, const ROLE* y ) { return pinNo( x->pin ) < pinNo( y->pin ); } );

            if( outs.size() == 1 && nRole[1] <= 1 && nRole[2] <= 1 )
            {
                CH ch;
                ch.o = outs[0]->pin;

                for( const ROLE* r : list )
                    ( r->role == 1 ? ch.p : r->role == 2 ? ch.n : ch.o ) = r->pin;

                chans.push_back( ch );
                continue;
            }

            const bool byPlace = std::all_of( list.begin(), list.end(), []( const ROLE* r ) { return r->pin->has_xy(); } );

            auto dist = [&]( const ROLE* x, const ROLE* y ) -> double
            {
                if( byPlace )
                    return std::hypot( x->pin->x() - y->pin->x(), x->pin->y() - y->pin->y() );

                return std::labs( pinNo( x->pin ) - pinNo( y->pin ) );
            };

            std::map<const ROLE*, CH> made;

            for( const ROLE* o : outs )
                made[o].o = o->pin;

            // the closest output / input pairs first, each pin once
            for( int want : { 1, 2 } )
            {
                std::vector<std::tuple<double, const ROLE*, const ROLE*>> pairs;

                for( const ROLE* o : outs )
                {
                    for( const ROLE* r : list )
                    {
                        if( r->role == want )
                            pairs.emplace_back( dist( o, r ), o, r );
                    }
                }

                std::stable_sort( pairs.begin(), pairs.end(), []( const auto& x, const auto& y ) { return std::get<0>( x ) < std::get<0>( y ); } );
                std::set<const ROLE*> taken;

                for( const auto& [d, o, r] : pairs )
                {
                    const ks::SchSimPin*& slot = want == 1 ? made[o].p : made[o].n;

                    if( slot || taken.count( r ) )
                        continue;

                    slot = r->pin;
                    taken.insert( r );
                }
            }

            std::string l;

            for( const ROLE* o : outs )
            {
                const CH& ch = made[o];
                chans.push_back( ch );
                l += ( l.empty() ? "" : "; " ) + std::string( "OUT " ) + o->pin->number() + " with +" + ( ch.p ? ch.p->number() : "?" ) + " -"
                     + ( ch.n ? ch.n->number() : "?" );
            }

            inferred += ( inferred.empty() ? "" : "; " ) + l + ( byPlace ? " (by place in the symbol)" : " (by pin number)" );
        }

        if( !vp || !vn )
        {
            aP.why = "op-amp supply pins not found (V+ / V-, VCC / GND...)";
            return false;
        }

        const double gbw = param( aP, "gbw", 1e6 );
        const double slew = param( aP, "slew", 0.5 ) * 1e6;   // V/us -> V/s
        const double vos = param( aP, "vos", 0 );
        const double ib = param( aP, "ib", 0 );
        const double iq = param( aP, "iq", 0 );
        const bool   rro = param( aP, "rr_out", 0 ) > 0.5;
        const double hr = rro ? 0.02 : 1.2;
        const double a0 = 1e5;
        const double r1 = 1e6;
        const double c1 = a0 / ( 2 * PI * r1 * gbw );
        const double imax = slew * c1;
        const std::string nvp = aN.node( *vp, s.ref() ), nvn = aN.node( *vn, s.ref() );
        int          made = 0;

        // the inputs' ESD diodes to the rails (a powered-down op-amp's input then conducts into V+):
        // on unless models.input_clamp 0, or the part says its inputs are protected when off
        const std::string desc = upper( ( aP.hit ? aP.hit->description() + " " : "" ) + s.description() );
        const bool offSafe = desc.find( "POWER-OFF" ) != std::string::npos || desc.find( "POWER OFF" ) != std::string::npos
                             || desc.find( "FAIL-SAFE" ) != std::string::npos || desc.find( "FAILSAFE" ) != std::string::npos;
        const bool clamp = param( aP, "input_clamp", offSafe ? 0 : 1 ) > 0.5;

        if( clamp )
            aN.models.push_back( ".model kd_dc_" + id + " D(IS=1e-15 N=1 RS=10)" );

        for( const CH& ch : chans )
        {
            if( !ch.p || !ch.n || !ch.o )
                continue;

            const std::string c = id + "_" + std::to_string( made++ );
            const std::string k = "kd_" + c + "_";
            const std::string np = aN.node( *ch.p, s.ref() ), nn = aN.node( *ch.n, s.ref() ), no = aN.node( *ch.o, s.ref() );

            add( "B" + c + "_g 0 " + k + "1 I=max(-" + fmt( imax ) + ",min(" + fmt( imax ) + "," + fmt( a0 / r1 ) + "*(V(" + np + ")-V(" + nn + ")+"
                 + fmt( vos ) + ")))" );
            add( "R" + c + "_1 " + k + "1 0 " + fmt( r1 ) );
            add( "C" + c + "_1 " + k + "1 0 " + fmt( c1 ) );
            // the output within its headroom of the rails; below 2 x the headroom (powered down) the
            // two limits meet at the rails' middle instead of crossing (no 1.2 V from a 0 V supply)
            const std::string mid = "(V(" + nvp + ")+V(" + nvn + "))/2";
            const std::string lo = "min(V(" + nvn + ")+" + fmt( hr ) + "," + mid + ")", hi = "max(V(" + nvp + ")-" + fmt( hr ) + "," + mid + ")";
            // keep the integrator within 20 mV of what the output can reach (no wind-up past it: a
            // follower whose supply ramps up would otherwise overshoot its input once the rail is
            // past it, by what the integrator stored while the output was at its headroom)
            add( "B" + c + "_cl " + k + "1 0 I=max(V(" + k + "1)-" + hi + "-0.02,0)+min(V(" + k + "1)-" + lo + "+0.02,0)" );
            add( "B" + c + "_o " + k + "o 0 V=max(" + lo + ",min(" + hi + ",V(" + k + "1)))" );
            add( "R" + c + "_o " + k + "o " + no + " 25" );
            aN.checks.push_back( { s.ref(), "OPAMP", { { "o", "v(" + k + "o)" }, { "vp", vx( nvp, "0" ) }, { "vn", vx( nvn, "0" ) },
                                                      { "io", no == "0" ? "v(" + k + "o)/25" : "v(" + k + "o)/25-v(" + no + ")/25" } },
                                   { { "iout", hasParam( aP, "iout" ) ? param( aP, "iout" ) : 0.0 } } } );

            if( clamp )
            {
                for( const auto& [tag, n] : { std::pair<const char*, std::string>{ "p", np }, { "n", nn } } )
                {
                    add( "D" + c + "_" + tag + "h " + n + " " + nvp + " kd_dc_" + id );
                    add( "D" + c + "_" + tag + "l " + nvn + " " + n + " kd_dc_" + id );
                }
            }

            if( ib != 0 )
            {
                add( "I" + c + "_bp " + np + " 0 " + fmt( ib ) );
                add( "I" + c + "_bn " + nn + " 0 " + fmt( ib ) );
            }
        }

        if( !made )
        {
            aP.why = "op-amp input / output pins not found (+IN / IN+ / NONINVERTING INPUT, -IN / INVERTING INPUT, OUT)";
            return false;
        }

        if( iq > 0 )
            add( "I" + id + "_q " + nvp + " " + nvn + " " + fmt( iq ) );

        auto src = [&]( const char* aId ) { return hasParam( aP, aId ) ? "" : " assumed"; };
        aP.notes = std::to_string( made ) + " channel(s): GBW " + FormatSI( gbw, "Hz" ) + src( "gbw" ) + " (one pole after 100 dB), slew "
                   + fmt( slew / 1e6, 3 ) + " V/µs" + src( "slew" ) + ", Vos " + FormatSI( vos, "V" ) + src( "vos" ) + ", output "
                   + ( rro ? "rail to rail" : "1.2 V from the rails" ) + ( hasParam( aP, "rr_out" ) ? "" : " assumed" )
                   + "; supply current Iq only; inputs "
                   + ( clamp ? std::string( "clamped to the rails by ESD diodes (models.input_clamp 0: off)" )
                           : hasParam( aP, "input_clamp" ) ? std::string( "not clamped (input_clamp 0)" )
                                                           : std::string( "not clamped: its description says power-off / fail-safe inputs (input_clamp 1: on)" ) )
                   + ( inferred.empty() ? "" : "; inferred pairing: " + inferred );
        return true;
    }

    aP.why = "no model";
    return false;
}

// --- running ngspice ---------------------------------------------------------------------

std::string exeDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    DWORD   n = GetModuleFileNameW( nullptr, buf, MAX_PATH * 2 );
    return n ? fromPath( fs::path( std::wstring( buf, n ) ).parent_path() ) : std::string();
#else
    char    buf[4096];
    ssize_t n = readlink( "/proc/self/exe", buf, sizeof( buf ) - 1 );
    return n > 0 ? fs::path( std::string( buf, n ) ).parent_path().string() : std::string();
#endif
}

std::string findNgspice()
{
    if( const char* e = std::getenv( "KICADD_NGSPICE" ); e && *e )
        return e;

    std::string dir = exeDir();
    std::vector<fs::path> c;

#ifdef _WIN32
    const char* names[] = { "ngspice_con.exe", "ngspice.exe" };
#else
    const char* names[] = { "ngspice" };
#endif

    if( !dir.empty() )
    {
        for( const char* n : names )
        {
            c.push_back( toPath( dir ) / n );
            c.push_back( toPath( dir ) / "ngspice" / "bin" / n );
            c.push_back( toPath( dir ) / ".." / "ngspice" / "bin" / n );
        }
    }

    if( const char* path = std::getenv( "PATH" ) )
    {
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::stringstream ss( path );
        std::string       d;

        while( std::getline( ss, d, sep ) )
        {
            for( const char* n : names )
            {
                if( !d.empty() )
                    c.push_back( toPath( d ) / n );
            }
        }
    }

    std::error_code ec;

    for( const fs::path& p : c )
    {
        if( fs::is_regular_file( p, ec ) )
            return fromPath( p );
    }

    return {};
}

/// run aExe with aArgs, at most aTimeoutS seconds; the exit code, -1 not started, -2 timed out
int runProgram( const std::string& aExe, const std::vector<std::string>& aArgs, int aTimeoutS )
{
#ifdef _WIN32
    auto wide = []( const std::string& s )
    {
        int          n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, nullptr, 0 );
        std::wstring w( n > 0 ? n - 1 : 0, L'\0' );

        if( n > 1 )
            MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, w.data(), n );

        return w;
    };

    std::wstring cmd = L"\"" + wide( aExe ) + L"\"";

    for( const std::string& a : aArgs )
        cmd += L" \"" + wide( a ) + L"\"";

    STARTUPINFOW        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof( si );

    if( !CreateProcessW( nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) )
        return -1;

    DWORD w = WaitForSingleObject( pi.hProcess, (DWORD) aTimeoutS * 1000 );
    DWORD code = 0;

    if( w == WAIT_TIMEOUT )
    {
        TerminateProcess( pi.hProcess, 1 );
        WaitForSingleObject( pi.hProcess, 2000 );
        code = (DWORD) -2;
    }
    else
    {
        GetExitCodeProcess( pi.hProcess, &code );
    }

    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return (int) code;
#else
    std::vector<char*> argv;
    argv.push_back( const_cast<char*>( aExe.c_str() ) );

    for( const std::string& a : aArgs )
        argv.push_back( const_cast<char*>( a.c_str() ) );

    argv.push_back( nullptr );

    // its output goes to the log file (-o); nothing on our stdout, which may be the MCP stdio
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init( &fa );
    posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
    posix_spawn_file_actions_addopen( &fa, 1, "/dev/null", O_WRONLY, 0 );
    posix_spawn_file_actions_addopen( &fa, 2, "/dev/null", O_WRONLY, 0 );

    pid_t pid = 0;
    int   rc = posix_spawn( &pid, aExe.c_str(), &fa, nullptr, argv.data(), environ );
    posix_spawn_file_actions_destroy( &fa );

    if( rc != 0 )
        return -1;

    auto until = std::chrono::steady_clock::now() + std::chrono::seconds( aTimeoutS );
    int  status = 0;

    while( true )
    {
        pid_t r = waitpid( pid, &status, WNOHANG );

        if( r == pid )
            return WIFEXITED( status ) ? WEXITSTATUS( status ) : 128 + ( WIFSIGNALED( status ) ? WTERMSIG( status ) : 0 );

        if( r < 0 )
            return -1;

        if( std::chrono::steady_clock::now() > until )
        {
            kill( pid, SIGKILL );
            waitpid( pid, &status, 0 );
            return -2;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
#endif
}

using sim::TABLE;

bool readTable( const fs::path& aFile, TABLE& aOut )
{
    std::ifstream in( aFile );
    std::string   line;

    if( !std::getline( in, line ) )
        return false;

    std::istringstream head( line );
    std::string        n;

    while( head >> n )
        aOut.names.push_back( n );

    aOut.cols.assign( aOut.names.size(), {} );

    while( std::getline( in, line ) )
    {
        std::istringstream row( line );
        double             v;
        size_t             i = 0;

        while( row >> v && i < aOut.cols.size() )
            aOut.cols[i++].push_back( v );
    }

    return !aOut.cols.empty() && !aOut.cols[0].empty();
}

using sim::RUN;

std::atomic<int> g_runs{ 0 };

/// write aCircuit and the control block, run ngspice, read back aVectors
RUN runSpice( const std::string& aExe, const std::string& aCircuit, const std::string& aAnalysis,
              const std::vector<std::string>& aVectors, int aTimeoutS, std::string* aKeepDir )
{
    RUN             r;
    std::error_code ec;
    fs::path        dir = fs::temp_directory_path( ec ) / ( "kicadd-sim-" + std::to_string(
#ifdef _WIN32
                                                                                       GetCurrentProcessId()
#else
                                                                                       getpid()
#endif
                                                                                               )
                                                         + "-" + std::to_string( g_runs++ ) );
    fs::create_directories( dir, ec );

    if( ec )
    {
        r.error = "cannot make a folder for the run: " + ec.message();
        return r;
    }

    fs::path cir = dir / "circuit.cir";
    fs::path out = dir / "out.txt";
    fs::path log = dir / "ngspice.log";

    {
        std::ofstream f( cir );
        std::string   outName = genericOf( out );

        f << aCircuit;
        f << ".control\nset wr_singlescale\nset wr_vecnames\noption numdgt=7\n" << aAnalysis << "\n";

        // wrdata writes at most a line's worth of vectors well; a few dozen is fine
        f << "wrdata " << outName;

        for( const std::string& v : aVectors )
            f << " " << v;

        f << "\n.endc\n.end\n";
    }

    auto t0 = std::chrono::steady_clock::now();
    int  code = runProgram( aExe, { "-b", "-o", fromPath( log ), fromPath( cir ) }, aTimeoutS );
    r.seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();

    auto logErrors = [&]()
    {
        std::ifstream in( log );
        std::string   line, errs;

        while( std::getline( in, line ) )
        {
            std::string l = upper( line );

            if( ( l.find( "ERROR" ) != std::string::npos || l.find( "FATAL" ) != std::string::npos
                  || l.find( "TIMESTEP TOO SMALL" ) != std::string::npos || l.find( "SINGULAR" ) != std::string::npos
                  || l.find( "NO SUCH VECTOR" ) != std::string::npos || l.find( "UNKNOWN" ) != std::string::npos )
                && errs.size() < 600 )
                errs += ( errs.empty() ? "" : " | " ) + line;
        }

        return errs;
    };

    if( code == -1 )
        r.error = "could not start ngspice (" + aExe + ")";
    else if( code == -2 )
        r.error = "ngspice took longer than " + std::to_string( aTimeoutS ) + " s (a shorter tran, or a larger step)";
    else if( !readTable( out, r.table ) )
        r.error = "ngspice gave no results: " + logErrors();
    else
        r.ok = true;

    if( aKeepDir )
        *aKeepDir = fromPath( dir );
    else
        fs::remove_all( dir, ec );

    return r;
}

// --- measuring ---------------------------------------------------------------------------

std::string sparkline( const std::vector<double>& aX, const std::vector<double>& aY, int aWidth = 32 )
{
    static const char* bars[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };

    if( aY.empty() )
        return {};

    double lo = *std::min_element( aY.begin(), aY.end() );
    double hi = *std::max_element( aY.begin(), aY.end() );
    double x0 = aX.front(), x1 = aX.back();
    std::string s;
    size_t      j = 0;

    for( int i = 0; i < aWidth; ++i )
    {
        double xa = x0 + ( x1 - x0 ) * i / aWidth, xb = x0 + ( x1 - x0 ) * ( i + 1 ) / aWidth;
        double sum = 0;
        int    n = 0;

        while( j < aX.size() && aX[j] < xb )
        {
            if( aX[j] >= xa )
            {
                sum += aY[j];
                n++;
            }

            ++j;
        }

        double v = n ? sum / n : aY[std::min( j, aY.size() - 1 )];
        int    k = hi > lo ? (int) std::lround( ( v - lo ) / ( hi - lo ) * 7 ) : 3;
        s += bars[std::clamp( k, 0, 7 )];
    }

    return s;
}

struct TRAN_M
{
    double avg = 0, pp = 0, min = 0, max = 0, tmin = 0, tmax = 0, first = 0, rms = 0;
    double overshoot = 0, settle = -1, rise = -1;
    bool   step = false;
};

TRAN_M measureTran( const std::vector<double>& t, const std::vector<double>& v, double aWindowFrom, double aSettlePct )
{
    TRAN_M m;
    double sum = 0, wsum = 0, sq = 0;
    double wlo = 1e300, whi = -1e300;

    m.min = 1e300;
    m.max = -1e300;
    m.first = v.front();

    for( size_t i = 0; i < t.size(); ++i )
    {
        if( v[i] < m.min )
        {
            m.min = v[i];
            m.tmin = t[i];
        }

        if( v[i] > m.max )
        {
            m.max = v[i];
            m.tmax = t[i];
        }

        if( t[i] >= aWindowFrom && i > 0 )
        {
            double dt = t[i] - t[i - 1];
            sum += v[i] * dt;
            sq += v[i] * v[i] * dt;
            wsum += dt;
            wlo = std::min( wlo, v[i] );
            whi = std::max( whi, v[i] );
        }
    }

    m.avg = wsum > 0 ? sum / wsum : v.back();
    m.rms = wsum > 0 ? std::sqrt( sq / wsum ) : std::fabs( v.back() );
    m.pp = whi >= wlo ? whi - wlo : 0;

    const double stepSize = m.avg - m.first;
    // a step: it moved, and it has settled (an AC signal moving about its average is not one)
    m.step = std::fabs( stepSize ) > std::max( 0.05 * std::max( std::fabs( m.avg ), std::fabs( m.first ) ), 1e-3 )
             && m.pp < 0.5 * std::fabs( stepSize );

    if( m.step )
    {
        m.overshoot = stepSize > 0 ? ( m.max - m.avg ) / stepSize * 100 : ( m.avg - m.min ) / -stepSize * 100;
        double band = std::fabs( stepSize ) * aSettlePct / 100;

        for( size_t i = t.size(); i-- > 0; )
        {
            if( std::fabs( v[i] - m.avg ) > band )
            {
                m.settle = i + 1 < t.size() ? t[i + 1] : -2;   // -2: still outside at the end
                break;
            }
        }

        double lo10 = m.first + 0.1 * stepSize, hi90 = m.first + 0.9 * stepSize;
        double t10 = -1;

        for( size_t i = 0; i < t.size(); ++i )
        {
            bool past10 = stepSize > 0 ? v[i] >= lo10 : v[i] <= lo10;
            bool past90 = stepSize > 0 ? v[i] >= hi90 : v[i] <= hi90;

            if( t10 < 0 && past10 )
                t10 = t[i];

            if( past90 )
            {
                m.rise = t10 >= 0 ? t[i] - t10 : -1;
                break;
            }
        }
    }

    return m;
}

std::string mv( double aV )
{
    return FormatSI( aV, "V", 4 );
}

} // namespace


std::string SimulateDescription()
{
    return "Simulate a region of the schematic with ngspice (headless): the parts named, a sheet, or around parts / nets "
           "through hops (a hop does not pass ground, and through a supply only to its two-pin parts: an LDO with "
           "around [\"U1\"] gets its input and output capacitors). Models come from the parts' data (LCSC field -> the "
           "parts index): R, C (MLCC with DC bias estimated from dielectric, size, C and the DC voltage across it, "
           "and typical ESR / ESL), L (DCR, SRF), diodes, Zeners, MOSFETs (level 1), LDOs and op-amps "
           "(behavioural), or the part's own Sim.* model; every estimated or assumed value is said. Parts without a "
           "model are left out and their nets cut there (said). sources {net: volts or a SPICE source: \"dc 5 ac 1\", "
           "\"pulse(0 5 0 1u 1u 5m 10m)\", \"sin(5 0.05 100k)\"} to ground, or null / \"none\": no source there; supplies not "
           "given and not made in the region (an LDO's output, a subckt's power output pin) get the voltage their name "
           "says (+3V3) and that is said. loads {net: amps, a SPICE current source "
           "(\"pulse(0.1 1 1m 1u 1u 1m 2m)\") or \"10 ohm\"}. analysis: op (default), tran {stop, step?, window?: "
           "the last part measured, default the last 20%}, ac {from, to, points per decade} (needs a source with "
           "ac 1) or dc {source: a net of sources (volts) or loads (amps), from, to, step}: that source swept "
           "(I-V curves: the source's current against its voltage). probes: nets to measure (default the loads', the regulators' outputs, else the region's nets). "
           "Measured: op the voltages; tran the average, ripple p-p and min / max over the window, a step's "
           "overshoot, 10-90% rise and settling (settle_pct, default 2), a sketch line; ac the gain at the "
           "lowest frequency, its peak and the -3 dB frequency; the sources' currents and power, inductors' peak "
           "and rms current (with Isat / Irated), regulators' output current. No verdicts: compare them with your "
           "own targets. corners true: two more runs, all low and all high: every R / C / L at the same end of its "
           "tolerance together (from the data, else assumed and said). Not a worst case or Monte Carlo: in a "
           "ratio (a divider, matched RCs) the ends cancel; set one part's value with models to try a mix. "
           "MLCC DC bias comes from an op run first (for tran its average over the window): right for linear "
           "circuits; a switching converter or an AC-coupled node is not at its DC point there. dc_bias false: MLCCs at their nominal C. models {ref: {c, esr, esl, r, "
           "l, dcr, tol (%), dc_bias, vout, vref, dropout, iq, psrr, psrr_f, psrr_hf, loop_bw, rout, gbw, slew, vos, rds_on, vgs_th, "
           "vf, rs (a diode's series resistance), vz, zzt, izt, ilim, soft_start (s: the reference ramps from EN on), uvlo, input_clamp ... (any parameter, SI; keys its model does not use are said), kind (a built-in model for a part "
           "with no LCSC number: ldo, opamp, r, c, l, d, zener, mosfet, bead), exclude: true, or subckt + lib (a .lib / .cir path, relative to the project) "
           "+ pins (the subckt's pin order as pin numbers or names)}} override what the data gives. netlist true "
           "adds the netlist made. plot true: a PNG chart too (tran: the probes over time; ac: gain and phase over log f; "
           "dc: the probes and the sources' currents against the swept value; corners as faint lines of the same colour). "
           "Model checks: after an op / tran / dc run, physics the result should obey is checked (a regulator's output over its input, "
           "stuck at Vin - dropout over its setpoint, current backwards through a pass element / an off MOSFET / a diode outside "
           "breakdown, an op-amp output past its rails or driving with its supply at 0 V, an LDO delivering with its input at 0 V, a source "
           "absorbing, a source feeding only regulators / op-amps / capacitors that delivers over 3 x the loads and their rated outputs): a breach adds a \"model check:\" line with the "
           "numbers and the fixed limit. It flags a result that may be the model's, not the design." + unmodelledHelp();
}


json SimulateSchema()
{
    json strArr = { { "type", "array" }, { "items", { { "type", "string" } } } };
    json anyMap = { { "type", "object" } };

    return { { "type", "object" },
             { "properties",
               { { "refs", strArr },
                 { "sheet", { { "type", "string" } } },
                 { "around", strArr },
                 { "hops", { { "type", "integer" } } },
                 { "sources", anyMap },
                 { "loads", anyMap },
                 { "analysis", { { "type", "string" }, { "enum", { "op", "tran", "ac", "dc" } } } },
                 { "tran", { { "type", "object" }, { "description", "{stop: \"2m\", step?: \"1u\", window?: \"0.4m\"}" } } },
                 { "ac", { { "type", "object" }, { "description", "{from: \"10\", to: \"10Meg\", points?: 20}" } } },
                 { "dc", { { "type", "object" }, { "description", "{source: \"VIN\", from: 0, to: 5, step: \"10m\"}" } } },
                 { "plot", { { "type", "boolean" } } },
                 { "probes", strArr },
                 { "settle_pct", { { "type", "number" } } },
                 { "corners", { { "type", "boolean" } } },
                 { "dc_bias", { { "type", "boolean" } } },
                 { "models", anyMap },
                 { "netlist", { { "type", "boolean" } } },
                 { "timeout_s", { { "type", "integer" } } } } } };
}


TOOL_RESULT Simulate( IPC_CLIENT& aIpc, const json& a )
{
    // --- the region
    ks::SchSimRegion req;

    for( const char* k : { "refs", "around" } )
    {
        if( a.contains( k ) && a[k].is_array() )
        {
            for( const json& v : a[k] )
            {
                if( v.is_string() )
                    ( std::string( k ) == "refs" ? req.add_refs( v.get<std::string>() ) : req.add_around( v.get<std::string>() ) );
            }
        }
    }

    if( a.contains( "sheet" ) && a["sheet"].is_string() )
        req.set_sheet( a["sheet"].get<std::string>() );

    if( a.contains( "hops" ) && a["hops"].is_number_integer() )
        req.set_hops( a["hops"].get<int>() );

    ks::SchSimRegionResponse region;
    std::string              err;

    if( !aIpc.Call( req, region, err ) )
        return fail( err );

    const std::string exe = findNgspice();

    if( exe.empty() )
        return fail( "ngspice not found: put it next to kicadd-mcp (ngspice/bin/), on the PATH, or set KICADD_NGSPICE to it" );

    // --- the parts' data
    kb::LcscSearch         lreq;
    kb::LcscSearchResponse lres;
    std::string            indexNote;

    for( const ks::SchSimPart& p : region.parts() )
    {
        if( !p.lcsc().empty() )
            lreq.add_lcsc( p.lcsc() );
    }

    if( lreq.lcsc_size() && !aIpc.Call( lreq, lres, err ) )
        indexNote = "parts index not read (" + err + "): models from the values only";

    std::map<std::string, const kb::LcscHit*> hits;

    for( const kb::LcscHit& h : lres.hits() )
        hits[upper( h.lcsc() )] = &h;

    const json models = a.contains( "models" ) && a["models"].is_object() ? a["models"] : json::object();
    const bool biasOn = !a.contains( "dc_bias" ) || !a["dc_bias"].is_boolean() || a["dc_bias"].get<bool>();

    for( const auto& [ref, m] : models.items() )
    {
        bool known = std::any_of( region.parts().begin(), region.parts().end(), [&]( const ks::SchSimPart& p ) { return p.ref() == ref; } );

        if( !known )
            return fail( "models: " + ref + " is not in the region" );

        if( !m.is_object() )
            return fail( "models." + ref + ": an object ({\"esr\": \"5m\"}, {\"exclude\": true}, ...)" );
    }

    std::vector<PART> parts;
    std::set<std::string> includes( region.includes().begin(), region.includes().end() );

    for( const ks::SchSimPart& p : region.parts() )
    {
        PART part;
        part.sch = &p;
        part.over = json::object();

        if( auto it = hits.find( upper( p.lcsc() ) ); it != hits.end() )
            part.hit = it->second;

        if( models.contains( p.ref() ) )
            part.over = models[p.ref()];

        if( part.over.value( "exclude", false ) )
        {
            part.why = "excluded (models)";
        }
        else if( part.over.contains( "subckt" ) )
        {
            if( !part.over["subckt"].is_string() || !part.over.contains( "pins" ) || !part.over["pins"].is_array() )
                return fail( "models." + p.ref() + ": subckt (its name) with pins (the subckt's pin order: numbers or names)" );

            part.kind = "SUBCKT";

            if( part.over.contains( "lib" ) && part.over["lib"].is_string() )
            {
                fs::path lib = toPath( part.over["lib"].get<std::string>() );

                if( lib.is_relative() )
                    lib = toPath( region.project_dir() ) / lib;

                std::error_code ec;

                if( !fs::is_regular_file( lib, ec ) )
                    return fail( "models." + p.ref() + ".lib: no file " + fromPath( lib ) );

                includes.insert( ".include \"" + genericOf( lib ) + "\"" );
            }
        }
        else if( part.over.contains( "kind" ) )
        {
            // the built-in model named, for a part the data does not classify (no LCSC number)
            static const std::map<std::string, std::string> kinds = { { "r", "R" }, { "c", "C" }, { "l", "L" }, { "d", "D" },
                                                                      { "zener", "Z" }, { "z", "Z" }, { "mosfet", "M" }, { "m", "M" },
                                                                      { "bead", "FB" }, { "fb", "FB" }, { "ldo", "LDO" },
                                                                      { "opamp", "OPAMP" } };
            std::string want = part.over["kind"].is_string() ? part.over["kind"].get<std::string>() : "";
            std::transform( want.begin(), want.end(), want.begin(), ::tolower );

            if( !kinds.count( want ) )
                return fail( "models." + p.ref() + ".kind: r, c, l, d, zener, mosfet, bead, ldo or opamp" );

            part.kind = kinds.at( want );
        }
        else if( !p.spice_item().empty() )
        {
            part.kind = "KICAD";
        }
        else if( !p.spice_error().empty() )
        {
            part.why = p.spice_error();
        }
        else
        {
            part.kind = classify( part );

            if( part.kind.empty() )
                part.why = p.lcsc().empty() ? "no model (no LCSC part to read its kind from)"
                                            : part.hit ? "no model for " + part.hit->subcategory() : "no model (" + p.lcsc() + " not in the parts index)";
        }

        parts.push_back( std::move( part ) );
    }

    // --- nets
    NETLIST base;
    std::map<std::string, const ks::SchSimNet*> netByName;

    for( const ks::SchSimNet& n : region.nets() )
    {
        std::string node = nodeOf( n );

        // two nets that read the same once made safe: keep them apart
        if( node != "0" )
        {
            std::string first = node;

            for( int k = 2; std::any_of( base.spiceOf.begin(), base.spiceOf.end(), [&]( auto& e ) { return e.second == node; } ); ++k )
                node = first + "_" + std::to_string( k );
        }

        base.spiceOf[n.name()] = node;
        base.bySpice[n.spice()] = node;
        netByName[n.name()] = &n;

        if( n.ground() )
            base.grounds.insert( n.name() );
    }

    auto findNet = [&]( const std::string& aName, std::string& aErr ) -> std::string
    {
        if( netByName.count( aName ) )
            return aName;

        std::string hit;

        for( const auto& [name, n] : netByName )
        {
            if( bare( name ) == aName || name == "/" + aName )
            {
                if( !hit.empty() )
                {
                    aErr = aName + ": which, " + hit + " or " + name + "?";
                    return {};
                }

                hit = name;
            }
        }

        if( hit.empty() )
            aErr = "no net " + aName + " in the region";

        return hit;
    };

    // --- passives' values, then the models (to know which nets they drive)
    std::map<std::string, std::string> passiveNote;

    for( PART& p : parts )
    {
        if( p.kind == "R" || p.kind == "C" || p.kind == "L" || p.kind == "FB" )
            passiveValues( p, passiveNote[p.sch->ref()] );
    }

    std::set<std::string> used;   // nets on parts with a model: the others do not matter

    {
        NETLIST probe = base;

        for( PART& p : parts )
        {
            if( !biasOn )
                p.biasOn = false;

            if( !p.kind.empty() && !emit( p, probe, 1.0 ) )
                p.kind.clear();

            if( !p.kind.empty() )
            {
                for( const ks::SchSimPin& pin : p.sch->pins() )
                {
                    if( !pin.net().empty() )
                        used.insert( pin.net() );
                }
            }
        }

        base.driven = probe.driven;
    }

    // models keys that did nothing: said, as other arguments are checked
    std::vector<std::string> modelNotes;

    for( const PART& p : parts )
    {
        std::string unused;

        for( const auto& [key, v] : p.over.items() )
        {
            static const std::set<std::string> structural = { "exclude", "subckt", "lib", "pins", "kind" };

            if( !structural.count( key ) && !p.read.count( key ) )
                unused += ( unused.empty() ? "" : ", " ) + key;
        }

        if( unused.empty() )
            continue;

        if( p.kind.empty() )
            modelNotes.push_back( "models." + p.sch->ref() + ": " + unused + " unused, " + p.sch->ref() + " is left out (" + p.why
                                  + "); models." + p.sch->ref() + ".kind names a built-in model (ldo, opamp, r, c, ...)" );
        else
            modelNotes.push_back( "models." + p.sch->ref() + ": " + unused + " not used by its model (" + p.kind + ")" );
    }

    // --- sources, loads
    struct SRC
    {
        std::string net, node, spec, why;
        bool        load = false, resistor = false;
    };

    std::vector<SRC> srcs;
    std::set<std::string> given;
    std::vector<std::string> noSource;   // sources {net: null / "none"}: no source, not even from its name

    for( const char* key : { "sources", "loads" } )
    {
        if( !a.contains( key ) )
            continue;

        if( !a[key].is_object() )
            return fail( std::string( key ) + ": {net: value}" );

        for( const auto& [name, v] : a[key].items() )
        {
            std::string e;
            std::string net = findNet( name, e );

            if( net.empty() )
                return fail( std::string( key ) + ": " + e );

            SRC s;
            s.net = net;
            s.node = base.spiceOf[net];
            s.load = std::string( key ) == "loads";

            if( !s.load && ( v.is_null() || ( v.is_string() && ( v.get<std::string>() == "none" || v.get<std::string>() == "off" ) ) ) )
            {
                if( netByName[net]->ground() )
                    return fail( "sources." + name + ": that is ground" );

                given.insert( net );
                noSource.push_back( bare( net ) );
                continue;
            }

            if( v.is_number() )
            {
                s.spec = "dc " + fmt( v.get<double>(), 6 );
            }
            else if( v.is_string() )
            {
                std::string t = v.get<std::string>();
                std::string lt = t;
                std::transform( lt.begin(), lt.end(), lt.begin(), ::tolower );
                double      ohm = 0;

                if( s.load && ( lt.find( "ohm" ) != std::string::npos || t.find( "Ω" ) != std::string::npos ) )
                {
                    if( !ParseSI( t, ohm ) || ohm <= 0 )
                        return fail( "loads." + name + ": a resistance (\"10 ohm\", \"4k7 ohm\"), got \"" + t + "\"" );

                    s.resistor = true;
                    s.spec = fmt( ohm, 6 );
                }
                else if( !specOk( t ) )
                {
                    return fail( std::string( key ) + "." + name + ": a number or a SPICE source value (\"dc 5 ac 1\", \"pulse(...)\")" );
                }
                else
                {
                    double num = 0;
                    s.spec = ParseSI( t, num ) ? "dc " + fmt( num, 6 ) : t;
                }
            }
            else
            {
                return fail( std::string( key ) + "." + name + ": a number or a string" );
            }

            if( !s.load && netByName[net]->ground() )
                return fail( "sources." + name + ": that is ground" );

            if( !s.load )
                given.insert( net );

            srcs.push_back( s );
        }
    }

    // supplies nobody drives: from their names
    std::vector<std::string> guessed, unknownSupply;

    for( const ks::SchSimNet& n : region.nets() )
    {
        if( n.ground() || !n.supply() || given.count( n.name() ) || base.driven.count( n.name() ) || !used.count( n.name() ) )
            continue;

        if( auto v = voltageFromName( n.name() ) )
        {
            SRC s;
            s.net = n.name();
            s.node = base.spiceOf[n.name()];
            s.spec = "dc " + fmt( *v, 6 );
            s.why = "from its name";
            srcs.push_back( s );
            guessed.push_back( bare( n.name() ) + " " + fmt( *v, 4 ) + " V" );
        }
        else
        {
            unknownSupply.push_back( bare( n.name() ) );
        }
    }

    if( !unknownSupply.empty() )
    {
        std::string l;

        for( const std::string& s : unknownSupply )
            l += ( l.empty() ? "" : ", " ) + s;

        // (often a regulator left out: its output then has nothing driving it; said with why)
        std::string lo;

        for( const PART& p : parts )
        {
            if( p.kind.empty() )
                lo += ( lo.empty() ? "" : "; " ) + p.sch->ref() + " " + p.sch->value() + " (" + p.why + ")";
        }

        return fail( "supplies with no source and no voltage in their name: " + l + " (sources {net: volts})"
                     + ( lo.empty() ? "" : "\nleft out (their pins open): " + lo ) );
    }

    // --- the analysis
    std::string analysis = a.contains( "tran" ) ? "tran" : a.contains( "ac" ) ? "ac" : a.contains( "dc" ) ? "dc" : "op";

    if( a.contains( "analysis" ) && a["analysis"].is_string() )
        analysis = a["analysis"].get<std::string>();

    double      tstop = 0, tstep = 0, window = 0, f0 = 10, f1 = 100e6, d0 = 0, d1 = 0, dstep = 0;
    int         ppd = 20;
    std::string sweptName, sweptNet;
    bool        sweptCurrent = false;

    if( analysis == "tran" )
    {
        const json t = a.value( "tran", json() );

        if( t.is_string() || t.is_number() )
            jsonSI( t, tstop );
        else if( t.is_object() && t.contains( "stop" ) )
            jsonSI( t["stop"], tstop );

        if( tstop <= 0 )
            return fail( "tran: {stop: \"2m\"} (seconds, SI)" );

        if( t.is_object() && t.contains( "step" ) )
            jsonSI( t["step"], tstep );

        if( t.is_object() && t.contains( "window" ) )
            jsonSI( t["window"], window );

        if( tstep <= 0 )
            tstep = tstop / 2000;

        if( tstop / tstep > 2e6 )
            return fail( "tran: over 2 million steps (a larger step, or a shorter stop)" );

        if( window <= 0 || window > tstop )
            window = tstop * 0.2;
    }
    else if( analysis == "ac" )
    {
        const json f = a.value( "ac", json::object() );

        if( f.is_object() )
        {
            if( f.contains( "from" ) )
                jsonSI( f["from"], f0 );

            if( f.contains( "to" ) )
                jsonSI( f["to"], f1 );

            if( f.contains( "points" ) && f["points"].is_number_integer() )
                ppd = std::clamp( f["points"].get<int>(), 1, 200 );
        }

        if( f0 <= 0 || f1 <= f0 )
            return fail( "ac: from < to, in Hz" );

        bool hasAc = std::any_of( srcs.begin(), srcs.end(), []( const SRC& s )
                                  {
                                      std::string l = s.spec;
                                      std::transform( l.begin(), l.end(), l.begin(), ::tolower );
                                      return l.find( "ac" ) != std::string::npos;
                                  } );

        if( !hasAc )
            return fail( "ac: give a source or load with ac in it, e.g. sources {VIN: \"dc 5 ac 1\"}" );
    }
    else if( analysis == "dc" )
    {
        const json d = a.value( "dc", json::object() );

        if( !d.is_object() || !d.contains( "source" ) || !d["source"].is_string() || !d.contains( "from" ) || !d.contains( "to" ) )
            return fail( "dc: {source: a net in sources or loads, from, to, step?}" );

        std::string e;
        std::string net = findNet( d["source"].get<std::string>(), e );

        if( net.empty() )
            return fail( "dc.source: " + e );

        for( size_t k = 0; k < srcs.size(); ++k )
        {
            if( srcs[k].net == net && !srcs[k].resistor )
            {
                sweptName = ( srcs[k].load ? "Ikd_load" : "Vkd_src" ) + std::to_string( k );
                sweptCurrent = srcs[k].load;
                sweptNet = net;
            }
        }

        if( sweptName.empty() )
            return fail( "dc.source: " + bare( net ) + " is not in sources (volts) or loads (amps, not ohms)" );

        if( !jsonSI( d["from"], d0 ) || !jsonSI( d["to"], d1 ) || d0 == d1 )
            return fail( "dc: from and to, two different values (SI)" );

        if( d.contains( "step" ) )
            jsonSI( d["step"], dstep );

        if( dstep <= 0 )
            dstep = std::fabs( d1 - d0 ) / 200;

        if( std::fabs( d1 - d0 ) / dstep > 1e5 )
            return fail( "dc: over 100000 points (a larger step)" );

        if( d1 < d0 )
            dstep = -dstep;
    }
    else if( analysis != "op" )
    {
        return fail( "analysis: op, tran, ac or dc" );
    }

    const bool plotOn = a.value( "plot", false );
    const bool chartOn = plotOn || plot::HasSink();   // the PNG, or the web view's Chart tab

    const double settlePct = a.contains( "settle_pct" ) && a["settle_pct"].is_number() ? a["settle_pct"].get<double>() : 2.0;
    const int    timeout = a.contains( "timeout_s" ) && a["timeout_s"].is_number_integer() ? std::clamp( a["timeout_s"].get<int>(), 1, 600 ) : 60;

    // --- probes
    std::vector<std::string> probes;
    std::set<std::string>    probeSet;

    auto addProbe = [&]( const std::string& n )
    {
        if( !netByName[n] || netByName[n]->ground() || !probeSet.insert( n ).second )
            return;

        probes.push_back( n );
    };

    if( a.contains( "probes" ) && a["probes"].is_array() )
    {
        for( const json& p : a["probes"] )
        {
            std::string e;
            std::string n = p.is_string() ? findNet( p.get<std::string>(), e ) : "";

            if( n.empty() )
                return fail( "probes: " + ( e.empty() ? std::string( "net names" ) : e ) );

            addProbe( n );
        }
    }
    else
    {
        for( const SRC& s : srcs )
        {
            if( s.load )
                addProbe( s.net );
        }

        for( const std::string& d : base.driven )
            addProbe( d );

        for( const std::string& x : req.around() )
        {
            std::string e;
            std::string n = netByName.count( x ) ? x : findNet( x, e );

            if( !n.empty() )
                addProbe( n );
        }

        if( probes.empty() )
        {
            for( const ks::SchSimNet& n : region.nets() )
            {
                if( probes.size() < 6 && !n.ground() && !given.count( n.name() ) && !n.supply() )
                    addProbe( n.name() );
            }
        }

        if( probes.empty() )
        {
            for( const ks::SchSimNet& n : region.nets() )
            {
                if( probes.size() < 6 )
                    addProbe( n.name() );
            }
        }
    }

    if( probes.size() > 24 )
        return fail( "probes: at most 24" );

    // a polarised capacitor (its DC voltage is measured: reverse bias is said)
    auto polar = []( const PART& p ) { return p.kind == "C" && ( p.capKind == "aluminium" || p.capKind == "tantalum" || p.capKind == "polymer" ); };

    // --- the netlist for one run: c / r / l scaled for the corners; the DC bias pass (an op, also
    // before an ac) asks for DC vectors: the MLCCs' voltages, not dB
    bool biasPass = false;
    std::vector<NETLIST::CHECK> checks;   // the nominal run's (1c)
    std::string                 checksSkipped;

    auto build = [&]( double aScale, std::vector<std::string>& aVectors, std::vector<std::string>& aLabels ) -> std::string
    {
        NETLIST n = base;
        n.driven.clear();
        n.checks.clear();

        for( PART& p : parts )
        {
            if( !p.kind.empty() )
                emit( p, n, p.kind == "R" || p.kind == "C" || p.kind == "L" ? ( aScale == 1 ? 1 : 1 + ( aScale - 1 ) * p.tol ) : 1.0 );
        }

        int k = 0;

        for( const SRC& s : srcs )
        {
            std::string name = std::to_string( k++ );

            if( s.resistor )
                n.lines.push_back( "Rkd_load" + name + " " + s.node + " 0 " + s.spec );
            else if( s.load )
                n.lines.push_back( "Ikd_load" + name + " " + s.node + " 0 " + s.spec );
            else
                n.lines.push_back( "Vkd_src" + name + " " + s.node + " 0 " + s.spec );
        }

        std::string text = "* KiCadD simulate: " + std::to_string( parts.size() ) + " part(s)\n.option rshunt=1e9\n";

        for( const std::string& inc : includes )
            text += inc + "\n";

        for( const ks::SchSimPart& p : region.parts() )
        {
            if( !p.spice_model().empty() )
                text += p.spice_model();
        }

        for( const std::string& m : n.models )
            text += m + "\n";

        for( const std::string& l : n.lines )
            text += l + "\n";

        aVectors.clear();
        aLabels.clear();

        for( const std::string& pr : probes )
        {
            const std::string& node = base.spiceOf[pr];
            aVectors.push_back( analysis == "ac" && !biasPass ? "vdb(" + node + ")" : "v(" + node + ")" );
            aLabels.push_back( "v:" + pr );

            if( analysis == "ac" && !biasPass && chartOn )
            {
                aVectors.push_back( "vp(" + node + ")" );
                aLabels.push_back( "ph:" + pr );
            }
        }

        if( analysis != "ac" || biasPass )
        {
            k = 0;

            for( const SRC& s : srcs )
            {
                std::string name = std::to_string( k++ );

                if( !s.load )
                {
                    aVectors.push_back( "i(vkd_src" + name + ")" );
                    aLabels.push_back( "src:" + std::to_string( k - 1 ) );
                }
            }

            for( const auto& [label, vec] : n.currents )
            {
                aVectors.push_back( vec );
                aLabels.push_back( "i:" + label );
            }

            // the model checks' vectors (the nominal run; not ac): each check's, the sources' voltages,
            // the resistive loads' nets. A large region would make the line too long for wrdata:
            // then no checks, said
            if( !biasPass && aScale == 1.0 && analysis != "ac" )
            {
                std::vector<std::string> cv, cl;

                for( size_t ci = 0; ci < n.checks.size(); ++ci )
                {
                    for( const auto& [role, expr] : n.checks[ci].vec )
                    {
                        if( expr != "0" )
                        {
                            cv.push_back( expr );
                            cl.push_back( "chk:" + std::to_string( ci ) + ":" + role );
                        }
                    }
                }

                for( size_t si = 0; si < srcs.size(); ++si )
                {
                    if( !srcs[si].load || srcs[si].resistor )
                    {
                        cv.push_back( vx( srcs[si].node, "0" ) );
                        cl.push_back( "chk:src:" + std::to_string( si ) );
                    }
                }

                checks = n.checks;

                if( aVectors.size() + cv.size() > 160 )
                {
                    checksSkipped = std::to_string( cv.size() ) + " more vectors than one run reads";
                    checks.clear();
                }
                else
                {
                    aVectors.insert( aVectors.end(), cv.begin(), cv.end() );
                    aLabels.insert( aLabels.end(), cl.begin(), cl.end() );
                }
            }

            // the MLCCs' DC voltage (the bias pass), and the polarised capacitors' (+ to -)
            for( PART& p : parts )
            {
                if( p.kind == "C" && ( p.capKind == "MLCC" || polar( p ) ) )
                {
                    std::vector<const ks::SchSimPin*> pins = pinsByNumber( *p.sch );

                    if( polar( p ) && pins.size() == 2 && ( pinName( *pins[1] ) == "+" || pinName( *pins[0] ) == "-" ) )
                        std::swap( pins[0], pins[1] );
                    NETLIST                           tmp = base;
                    std::string                       na = tmp.node( *pins[0], p.sch->ref() );
                    std::string                       nb = tmp.node( *pins[1], p.sch->ref() );

                    if( na.rfind( "nc_", 0 ) == 0 || nb.rfind( "nc_", 0 ) == 0 )
                        continue;

                    if( na == "0" && nb == "0" )
                        continue;

                    aVectors.push_back( vx( na, nb ) );
                    aLabels.push_back( "cap:" + p.sch->ref() );
                }
            }
        }

        return text;
    };

    std::string control;

    if( analysis == "op" )
        control = "op";
    else if( analysis == "tran" )
        control = "tran " + fmt( tstep, 6 ) + " " + fmt( tstop, 6 ) + " 0 " + fmt( tstep, 6 );
    else if( analysis == "dc" )
        control = "dc " + sweptName + " " + fmt( d0, 6 ) + " " + fmt( d1, 6 ) + " " + fmt( dstep, 6 );
    else
        control = "ac dec " + std::to_string( ppd ) + " " + fmt( f0, 6 ) + " " + fmt( f1, 6 );

    auto column = [&]( const RUN& aRun, const std::vector<std::string>& aLabels, const std::string& aLabel ) -> const std::vector<double>*
    {
        for( size_t i = 0; i < aLabels.size(); ++i )
        {
            if( aLabels[i] == aLabel && i + 1 < aRun.table.cols.size() )
                return &aRun.table.cols[i + 1];
        }

        return nullptr;
    };

    std::vector<std::string> vectors, labels;
    double                   spent = 0;
    std::string              keep;
    const bool               showNetlist = a.value( "netlist", false );

    // the parts with no model: said with an answer, and with a failed run (its cause is often there)
    auto leftOutText = [&]()
    {
        std::string l;

        for( const PART& p : parts )
        {
            if( p.kind.empty() )
                l += ( l.empty() ? "" : "; " ) + p.sch->ref() + " " + p.sch->value() + " (" + p.why + ")";
        }

        return l;
    };

    auto runFail = [&]( const std::string& aMsg )
    {
        std::string l = leftOutText();
        return fail( aMsg + ( l.empty() ? "" : "\nleft out (their pins open): " + l ) );
    };

    // a probe on a net no modelled part is on: no node there (one asked for: refused; a default
    // one: dropped)
    const bool askedProbes = a.contains( "probes" ) && a["probes"].is_array();

    for( size_t i = 0; i < probes.size(); )
    {
        const std::string pr = probes[i];
        bool on = std::any_of( parts.begin(), parts.end(), [&]( const PART& p )
                               { return !p.kind.empty() && std::any_of( p.sch->pins().begin(), p.sch->pins().end(),
                                                                         [&]( const ks::SchSimPin& pin ) { return pin.net() == pr; } ); } );

        bool src = std::any_of( srcs.begin(), srcs.end(), [&]( const SRC& x ) { return x.net == pr; } );

        if( !on && !src && askedProbes )
            return runFail( "probes: " + bare( pr ) + " has no modelled part on it" );

        if( !on && !src )
            probes.erase( probes.begin() + i );
        else
            ++i;
    }

    // --- the DC bias pass: the DC voltage across each MLCC (op; for tran its average over the window)
    bool anyBias = biasOn && std::any_of( parts.begin(), parts.end(), []( const PART& p )
                                          { return p.kind == "C" && p.biasOn && p.capKind == "MLCC" && p.dielectric != "C0G"; } )
                   || std::any_of( parts.begin(), parts.end(), polar );

    if( anyBias )
    {
        biasPass = true;
        std::string circuit = build( 1.0, vectors, labels );
        biasPass = false;
        RUN         r = runSpice( exe, circuit, analysis == "tran" ? control : "op", vectors, timeout, nullptr );
        spent += r.seconds;

        if( !r.ok )
            return runFail( "the DC bias pass: " + r.error + ( showNetlist ? "\n" + circuit : "" ) );

        for( PART& p : parts )
        {
            if( p.kind != "C" || !( ( p.biasOn && p.capKind == "MLCC" ) || polar( p ) ) )
                continue;

            const std::vector<double>* v = column( r, labels, "cap:" + p.sch->ref() );

            if( !v || v->empty() )
                continue;

            p.vdc = analysis == "tran" ? measureTran( r.table.cols[0], *v, tstop - window, 2 ).avg : v->back();

            if( polar( p ) )
            {
                // the most negative it gets (tran: over the whole run)
                p.vmin = *std::min_element( v->begin(), v->end() );
                continue;
            }

            const kb::LcscHit* h = p.hit;
            double share = sim::MlccBias( p.dielectric, p.c, ( h ? h->package() + " " : "" ) + p.sch->footprint(), p.vdc, &p.layer, &p.field );
            p.ceff = p.c * share;
        }
    }

    // --- the runs: nominal, and the corners
    const bool corners = a.value( "corners", false );
    std::vector<RUN> runs;
    std::vector<std::vector<std::string>> runLabels;
    std::string circuit;

    for( double scale : corners ? std::vector<double>{ 1.0, 0.0, 2.0 } : std::vector<double>{ 1.0 } )
    {
        // scale 0 / 2: each R / C / L at 1 -/+ its tolerance
        std::string c = build( scale, vectors, labels );

        if( scale == 1.0 )
            circuit = c;

        RUN r = runSpice( exe, c, control, vectors, timeout, nullptr );
        spent += r.seconds;

        if( !r.ok )
            return runFail( std::string( scale == 1.0 ? "" : "corner run: " ) + r.error + ( showNetlist ? "\n" + c : "" ) );

        runs.push_back( std::move( r ) );
        runLabels.push_back( labels );
    }

    // --- the answer
    int               modelled = std::count_if( parts.begin(), parts.end(), []( const PART& p ) { return !p.kind.empty(); } );
    const std::string leftOut = leftOutText();

    std::string out = "simulate " + analysis;

    if( analysis == "tran" )
        out += " " + FormatSI( tstop, "s" ) + " (step " + FormatSI( tstep, "s" ) + ")";
    else if( analysis == "ac" )
        out += " " + FormatSI( f0, "Hz" ) + " - " + FormatSI( f1, "Hz" );
    else if( analysis == "dc" )
        out += " " + bare( sweptNet ) + " " + FormatSI( d0, sweptCurrent ? "A" : "V" ) + " -> " + FormatSI( d1, sweptCurrent ? "A" : "V" )
               + " (step " + FormatSI( std::fabs( dstep ), sweptCurrent ? "A" : "V" ) + ")";

    out += ": " + std::to_string( modelled ) + " of " + std::to_string( parts.size() ) + " part(s) modelled, "
           + std::to_string( region.nets().size() ) + " net(s); ngspice " + fmt( spent, 2 ) + " s" + ( corners ? " (3 runs: nominal, all low, all high: every R / C / L moved together, not a worst case or Monte Carlo)" : "" );

    if( !indexNote.empty() )
        out += "\nnote: " + indexNote;

    for( const std::string& n : region.notes() )
        out += "\nnote: " + n;

    for( const std::string& n : modelNotes )
        out += "\nnote: " + n;

    // sources
    {
        std::string l;

        for( const SRC& s : srcs )
        {
            if( !s.load )
                l += ( l.empty() ? "" : "; " ) + bare( s.net ) + " " + s.spec + ( s.why.empty() ? "" : " (" + s.why + ")" );
        }

        for( const std::string& n : noSource )
            l += ( l.empty() ? "" : "; " ) + n + " none (sources: null)";

        std::string ld;

        for( const SRC& s : srcs )
        {
            if( s.load )
                ld += ( ld.empty() ? "" : "; " ) + bare( s.net ) + " " + ( s.resistor ? FormatSI( std::stod( s.spec ), "Ω" ) : s.spec );
        }

        out += "\nsources: " + ( l.empty() ? std::string( "none" ) : l ) + ( ld.empty() ? "" : "\nloads: " + ld );
    }

    if( base.grounds.size() > 1 )
    {
        std::string g;

        for( const std::string& n : base.grounds )
            g += ( g.empty() ? "" : ", " ) + bare( n );

        out += "\nnote: grounds joined as one: " + g;
    }

    // the boundary: nets with pins outside the region, or on parts left out
    {
        std::string cut;
        int         nCut = 0;

        for( const ks::SchSimNet& n : region.nets() )
        {
            if( n.ground() || n.outside() == 0 || given.count( n.name() ) || base.driven.count( n.name() ) || !used.count( n.name() ) )
                continue;

            bool isSrc = std::any_of( srcs.begin(), srcs.end(), [&]( const SRC& s ) { return s.net == n.name() && !s.load; } );

            if( isSrc )
                continue;

            if( nCut++ < 8 )
            {
                // which modelled parts end there (a resistor to an LED outside: its far end open)
                std::string here, refs;

                for( const PART& p : parts )
                {
                    if( p.kind.empty() )
                        continue;

                    bool on = std::any_of( p.sch->pins().begin(), p.sch->pins().end(), [&]( const ks::SchSimPin& pin ) { return pin.net() == n.name(); } );

                    if( on )
                        here += ( here.empty() ? "" : " " ) + p.sch->ref();
                }

                for( const std::string& r : n.outside_refs() )
                    refs += ( refs.empty() ? "" : " " ) + r;

                cut += ( cut.empty() ? "" : "; " ) + bare( n.name() ) + " (" + here + " here; " + refs
                       + ( n.outside() > n.outside_refs_size() ? " ..." : "" ) + " outside)";
            }
        }

        if( nCut )
            out += "\ncut at the region's edge (open there): " + cut + ( nCut > 8 ? "; " + std::to_string( nCut - 8 ) + " more" : "" )
                   + " -- more hops take them in, models {ref: {exclude: true}} leaves a dangling part out";
    }

    if( !leftOut.empty() )
        out += "\nleft out (their pins open): " + leftOut;

    // what the built-in models leave out: one line per model kind, its parts (tool_help has the effects)
    std::string unmod;

    for( const MODEL_DEF& d : modelDefs() )
    {
        std::string who;
        int         n = 0;

        for( const PART& p : parts )
        {
            if( p.kind == d.kind && n++ < 6 )
                who += ( who.empty() ? "" : " " ) + p.sch->ref();
        }

        if( !n )
            continue;

        std::string what;

        for( const UNMODELLED& u : d.unmodelled )
            what += ( what.empty() ? "" : ", " ) + std::string( u.name );

        unmod += "\n  " + std::string( d.title ) + " (" + who + ( n > 6 ? " +" + std::to_string( n - 6 ) : "" ) + "): " + what;
    }

    // models
    out += "\nmodels:";

    for( const PART& p : parts )
    {
        if( p.kind.empty() )
            continue;

        const ks::SchSimPart& s = *p.sch;
        std::string           line = "\n  " + s.ref() + " " + s.value() + ( s.lcsc().empty() ? "" : " (" + s.lcsc() + ")" ) + ": ";

        if( p.kind == "KICAD" )
            line += "its own SPICE model (Sim.* fields)";
        else if( p.kind == "SUBCKT" )
            line += "subckt " + p.over["subckt"].get<std::string>() + " (models)";
        else if( p.kind == "R" && !p.notes.empty() )
            line += p.notes;
        else if( p.kind == "R" )
            line += FormatSI( p.r, "Ω" ) + " ±" + fmt( p.tol * 100, 3 ) + "%" + ( p.tolAssumed ? " (assumed)" : "" );
        else if( p.kind == "C" )
        {
            line += FormatSI( p.c, "F" ) + " " + p.capKind + ( p.dielectric.empty() ? "" : " " + p.dielectric ) + ( p.package.empty() ? "" : " " + p.package )
                    + ( p.vrated > 0 ? " " + fmt( p.vrated, 3 ) + " V" : "" );

            if( p.ceff > 0 && std::fabs( p.ceff - p.c ) > 1e-3 * p.c )
                line += " @ " + fmt( std::fabs( p.vdc ), 3 ) + " V: C_eff ≈ " + FormatSI( p.ceff, "F" ) + " (est. DC bias: "
                        + fmt( p.ceff / p.c * 100, 2 ) + "%, layer ~" + fmt( p.layer * 1e6, 2 ) + " µm, " + fmt( p.field / 1e6, 2 ) + " V/µm)";
            else if( p.capKind == "MLCC" && p.dielectric != "C0G" && !p.biasOn )
                line += " (DC bias off: nominal C)";
            else if( p.capKind == "MLCC" && p.dielectric != "C0G" && biasOn )
                line += " @ " + fmt( std::fabs( p.vdc ), 3 ) + " V: no DC bias loss";

            if( p.vrated > 0 && std::fabs( p.vdc ) > p.vrated )
                line += " [over its rating]";

            if( polar( p ) && p.vmin < -0.05 )
                line += " [reverse biased: " + mv( p.vmin ) + " + to -" + ( analysis == "tran" ? " at its lowest" : "" )
                        + "; the model is a capacitor either way]";

            line += "; " + passiveNote[s.ref()] + "; ±" + fmt( p.tol * 100, 3 ) + "%" + ( p.tolAssumed ? " (assumed)" : "" );
        }
        else if( p.kind == "L" && p.l > 0 )
        {
            line += FormatSI( p.l, "H" ) + ", DCR " + ( p.dcr > 0 ? FormatSI( p.dcr, "Ω" ) : "not in the data (0.1 mΩ)" )
                    + ( p.srf > 0 ? ", SRF " + FormatSI( p.srf, "Hz" ) : "" ) + ( hasParam( p, "i_sat" ) ? ", Isat " + FormatSI( param( p, "i_sat" ), "A" ) : "" )
                    + ( hasParam( p, "i_rated" ) ? ", Irated " + FormatSI( param( p, "i_rated" ), "A" ) : "" ) + "; no saturation modelled";
        }
        else
        {
            line += ( p.kind == "LDO" ? "LDO, " : p.kind == "OPAMP" ? "op-amp, " : "" ) + p.notes;
        }

        out += line;
    }

    if( !unmod.empty() )
        out += "\nnot modelled (tool_help simulate: what each does):" + unmod;

    // results
    const RUN&                      nom = runs[0];
    const std::vector<std::string>& lab = runLabels[0];
    const std::vector<double>&      x = nom.table.cols[0];

    auto spread = [&]( const std::string& aLabel, auto aMeasure, const std::string& aUnit ) -> std::string
    {
        if( !corners )
            return {};

        double lo = 1e300, hi = -1e300;

        for( size_t i = 1; i < runs.size(); ++i )
        {
            const std::vector<double>* v = column( runs[i], runLabels[i], aLabel );

            if( !v )
                continue;

            double m = aMeasure( runs[i].table.cols[0], *v );
            lo = std::min( lo, m );
            hi = std::max( hi, m );
        }

        return lo <= hi ? " [corners " + FormatSI( lo, aUnit ) + " .. " + FormatSI( hi, aUnit ) + "]" : "";
    };

    out += "\nresults:";

    for( const std::string& pr : probes )
    {
        const std::string          label = "v:" + pr;
        const std::vector<double>* v = column( nom, lab, label );

        if( !v || v->empty() )
            continue;

        std::string line = "\n  " + bare( pr ) + ": ";

        if( analysis == "op" )
        {
            line += mv( v->back() ) + spread( label, []( auto&, auto& y ) { return y.back(); }, "V" );
        }
        else if( analysis == "dc" )
        {
            const auto [mn, mx] = std::minmax_element( v->begin(), v->end() );
            line += mv( v->front() ) + " at " + FormatSI( x.front(), sweptCurrent ? "A" : "V" ) + " -> " + mv( v->back() ) + " at "
                    + FormatSI( x.back(), sweptCurrent ? "A" : "V" ) + "; min " + mv( *mn ) + " at "
                    + FormatSI( x[mn - v->begin()], sweptCurrent ? "A" : "V" ) + ", max " + mv( *mx ) + " at "
                    + FormatSI( x[mx - v->begin()], sweptCurrent ? "A" : "V" ) + " " + sparkline( x, *v );
        }
        else if( analysis == "tran" )
        {
            TRAN_M m = measureTran( x, *v, tstop - window, settlePct );
            line += mv( m.avg ) + spread( label, [&]( auto& t, auto& y ) { return measureTran( t, y, tstop - window, settlePct ).avg; }, "V" )
                    + ", ripple " + mv( m.pp ) + " p-p"
                    + spread( label, [&]( auto& t, auto& y ) { return measureTran( t, y, tstop - window, settlePct ).pp; }, "V" ) + " over the last "
                    + FormatSI( window, "s" ) + "; min " + mv( m.min ) + " at " + FormatSI( m.tmin, "s" ) + ", max " + mv( m.max ) + " at "
                    + FormatSI( m.tmax, "s" );

            if( m.step )
            {
                line += "; step from " + mv( m.first ) + ": overshoot " + fmt( std::max( 0.0, m.overshoot ), 3 ) + "%"
                        + ( m.rise > 0 ? ", rise 10-90% " + FormatSI( m.rise, "s" ) : "" )
                        + ( m.settle >= 0   ? ", settled to " + fmt( settlePct, 2 ) + "% at " + FormatSI( m.settle, "s" )
                            : m.settle == -2 ? ", not within " + fmt( settlePct, 2 ) + "% of the step by the end"
                                             : ", settled from the start" );
            }

            line += " " + sparkline( x, *v );
        }
        else
        {
            // ac: vdb
            const double low = v->front();
            size_t       ipk = std::max_element( v->begin(), v->end() ) - v->begin();
            double       f3 = -1;

            for( size_t i = 1; i < v->size(); ++i )
            {
                if( ( *v )[i] < low - 3 && ( *v )[i - 1] >= low - 3 )
                {
                    double t = ( low - 3 - ( *v )[i - 1] ) / ( ( *v )[i] - ( *v )[i - 1] );
                    f3 = std::exp( std::log( x[i - 1] ) + t * ( std::log( x[i] ) - std::log( x[i - 1] ) ) );
                    break;
                }
            }

            line += fmt( low, 3 ) + " dB at " + FormatSI( x.front(), "Hz" );

            if( ( *v )[ipk] > low + 0.5 )
                line += ", peak " + fmt( ( *v )[ipk], 3 ) + " dB at " + FormatSI( x[ipk], "Hz" );

            line += f3 > 0 ? ", -3 dB at " + FormatSI( f3, "Hz" ) : ", no -3 dB point in range";
            line += spread( label, [&]( auto& f, auto& y )
                            {
                                for( size_t i = 1; i < y.size(); ++i )
                                {
                                    if( y[i] < y.front() - 3 && y[i - 1] >= y.front() - 3 )
                                        return f[i];
                                }

                                return -1.0;
                            },
                            "Hz" );
            // the sweep is by decades: the line too (linear, all but the last decade fell in one bar)
            std::vector<double> lx( x.size() );
            std::transform( x.begin(), x.end(), lx.begin(), []( double f ) { return std::log10( std::max( f, 1e-30 ) ); } );
            line += "; " + fmt( v->back(), 3 ) + " dB at " + FormatSI( x.back(), "Hz" ) + " " + sparkline( lx, *v ) + " (log f)";
        }

        out += line;
    }

    if( analysis != "ac" )
    {
        auto avgOf = [&]( const std::vector<double>& y )
        {
            return analysis == "tran" ? measureTran( x, y, tstop - window, settlePct ).avg : y.back();
        };

        int k = -1;

        for( const SRC& s : srcs )
        {
            k++;

            if( s.load )
                continue;

            const std::vector<double>* i = column( nom, lab, "src:" + std::to_string( k ) );

            if( !i || i->empty() )
                continue;

            // a source's current is into its + terminal: delivering is negative
            if( analysis == "dc" )
            {
                out += "\n  " + bare( s.net ) + " source: " + FormatSI( -i->front(), "A" ) + " at " + FormatSI( x.front(), sweptCurrent ? "A" : "V" )
                       + " -> " + FormatSI( -i->back(), "A" ) + " at " + FormatSI( x.back(), sweptCurrent ? "A" : "V" ) + " " + sparkline( x, *i );
                continue;
            }

            double ia = -avgOf( *i );
            double vdc = specDc( s.spec );
            out += "\n  " + bare( s.net ) + " source: " + FormatSI( ia, "A" ) + ( vdc != 0 ? " (" + FormatSI( ia * vdc, "W" ) + ")" : "" );

            if( analysis == "tran" )
            {
                double pk = 0;

                for( double v : *i )
                    pk = std::max( pk, -v );

                out += ", peak " + FormatSI( pk, "A" );
            }
        }

        for( size_t li = 0; li < lab.size(); ++li )
        {
            if( lab[li].rfind( "i:", 0 ) != 0 || li + 1 >= nom.table.cols.size() )
                continue;

            const std::vector<double>& y = nom.table.cols[li + 1];
            std::string                who = lab[li].substr( 2 );
            double                     pk = 0, rms = 0;

            for( double v : y )
                pk = std::max( pk, std::fabs( v ) );

            if( analysis == "tran" )
                rms = measureTran( x, y, tstop - window, settlePct ).rms;
            else
                rms = std::fabs( y.back() );

            out += "\n  " + who + ": " + FormatSI( rms, "A" ) + ( analysis == "tran" ? " rms, peak " + FormatSI( pk, "A" ) : "" );

            for( const PART& p : parts )
            {
                if( p.sch->ref() == who && p.kind == "L" )
                {
                    if( hasParam( p, "i_sat" ) )
                        out += " (Isat " + FormatSI( param( p, "i_sat" ), "A" ) + ")";

                    if( hasParam( p, "i_rated" ) )
                        out += " (Irated " + FormatSI( param( p, "i_rated" ), "A" ) + ")";
                }

                if( p.sch->ref() + " out" == who && hasParam( p, "iout" ) )
                    out += " (Iout " + FormatSI( param( p, "iout" ), "A" ) + ")";

                // the current limit reached: for how long (tran), said; the limit holds the
                // current there, so the peak is the limit's, not what the circuit would draw
                if( p.sch->ref() + " out" == who && p.ilim > 0 && pk >= 0.98 * p.ilim )
                {
                    double at = 0;

                    for( size_t i = 1; i < y.size() && i < x.size(); ++i )
                    {
                        if( std::fabs( y[i] ) >= 0.98 * p.ilim )
                            at += x[i] - x[i - 1];
                    }

                    out += "; at its current limit " + FormatSI( p.ilim, "A" )
                           + ( analysis == "tran" ? " for " + FormatSI( at, "s" ) : "" )
                           + " (the circuit would draw more: inrush into the output capacitors, or a load over the limit)";
                }
            }
        }
    }

    // --- the model checks (1c): physics the result should obey; a breach says the result may be
    // the model's, with its numbers and the fixed limit. Not a judgement of the design
    if( analysis != "ac" )
    {
        std::vector<std::string> mc;
        const size_t             nx = x.size();
        std::vector<double>      w( nx, 1.0 );

        if( analysis == "tran" )
        {
            for( size_t i = 0; i < nx; ++i )
                w[i] = i ? x[i] - x[i - 1] : 0;
        }

        auto col = [&]( const std::string& aLabel ) { return column( nom, lab, aLabel ); };
        auto val = [&]( const std::vector<double>* v, size_t i ) { return v && i < v->size() ? ( *v )[i] : 0.0; };

        // the share of the run (or of the window) where aPred holds, and the worst aMeasure there
        auto share = [&]( auto aPred, auto aMeasure, bool aWindow, double& aWorst )
        {
            double on = 0, all = 0;
            aWorst = 0;

            for( size_t i = 0; i < nx; ++i )
            {
                if( aWindow && analysis == "tran" && x[i] < tstop - window )
                    continue;

                all += w[i];

                if( aPred( i ) )
                {
                    on += w[i];
                    aWorst = std::max( aWorst, aMeasure( i ) );
                }
            }

            // op (one point) and a one-point window: the point counts whole
            return all > 0 ? on / all : ( nx && aPred( nx - 1 ) ? ( aWorst = aMeasure( nx - 1 ), 1.0 ) : 0.0 );
        };

        auto howLong = [&]( double aShare, bool aWindow )
        {
            if( analysis == "op" )
                return std::string( "at the operating point" );

            if( analysis == "dc" )
                return fmt( aShare * 100, 3 ) + "% of the sweep";

            return "for " + FormatSI( aShare * ( aWindow ? window : tstop ), "s" ) + " (" + fmt( aShare * 100, 3 ) + "% of the "
                   + ( aWindow ? "window" : "run" ) + ")";
        };

        // what the loads and the regulators may draw (for "a current nothing explains")
        double declared = 0;

        for( size_t si = 0; si < srcs.size(); ++si )
        {
            const SRC& sr = srcs[si];

            if( !sr.load )
                continue;

            if( sr.resistor )
            {
                double vmax = 0;

                if( const std::vector<double>* v = col( "chk:src:" + std::to_string( si ) ) )
                {
                    for( double y : *v )
                        vmax = std::max( vmax, std::fabs( y ) );
                }

                declared += vmax / std::stod( sr.spec );
                continue;
            }

            // a current: its largest level (pulse: the two levels; sin: offset + amplitude)
            std::string l = sr.spec;
            std::transform( l.begin(), l.end(), l.begin(), ::tolower );
            double      most = std::fabs( specDc( l ) );
            size_t      at = l.find( '(' );

            if( at != std::string::npos )
            {
                std::istringstream in( l.substr( at + 1 ) );
                std::string        tok;
                double             v1 = 0, v2 = 0;
                const bool         sine = l.find( "sin" ) != std::string::npos;

                if( in >> tok && ParseSI( tok.substr( 0, tok.find_first_of( ",)" ) ), v1 ) )
                {
                    most = std::max( most, std::fabs( v1 ) );

                    if( in >> tok && ParseSI( tok.substr( 0, tok.find_first_of( ",)" ) ), v2 ) )
                        most = std::max( most, sine ? std::fabs( v1 ) + std::fabs( v2 ) : std::fabs( v2 ) );
                }
            }

            declared += most;
        }

        for( const NETLIST::CHECK& c : checks )
        {
            if( c.kind == "LDO" )
                declared += c.num.at( "iout" ) > 0 ? c.num.at( "iout" ) : c.num.at( "ilim" );
            else if( c.kind == "OPAMP" )
                declared += c.num.at( "iout" );
        }

        for( size_t ci = 0; ci < checks.size(); ++ci )
        {
            const NETLIST::CHECK& c = checks[ci];
            const std::string     pre = "chk:" + std::to_string( ci ) + ":";
            auto                  v = [&]( const char* aRole ) { return col( pre + aRole ); };
            double                worst = 0, f = 0;

            if( c.kind == "LDO" )
            {
                const auto *vin = v( "in" ), *vo = v( "out" ), *io = v( "i" );
                const double set = c.num.at( "set" ), drop = c.num.at( "drop" );

                f = share( [&]( size_t i ) { return val( vo, i ) > val( vin, i ) + 0.05; },
                           [&]( size_t i ) { return val( vo, i ) - val( vin, i ); }, false, worst );

                if( f > 0.01 )
                    mc.push_back( c.ref + "'s output over its input by up to " + mv( worst ) + " " + howLong( f, false )
                                  + " (limit: 50 mV for 1%): its output is driven from elsewhere, or the model is wrong "
                                    "(current back through the pass element is not modelled)" );

                if( set > 0 )
                {
                    f = share( [&]( size_t i )
                               {
                                   const double a = val( vin, i ), o = val( vo, i );
                                   return a > set + drop + 0.1 && o > set * 1.02 && std::fabs( o - ( a - drop ) ) < 0.05;
                               },
                               [&]( size_t i ) { return val( vo, i ); }, true, worst );

                    if( f > 0.2 )
                        mc.push_back( c.ref + "'s output at Vin - dropout (up to " + mv( worst ) + "), over its setpoint " + mv( set ) + ", "
                                      + howLong( f, true ) + " (limit 20%): the error amplifier wound up (a model error)" );
                }

                f = share( [&]( size_t i ) { return val( io, i ) < -1e-4; }, [&]( size_t i ) { return -val( io, i ); }, false, worst );

                if( f > 0 )
                    mc.push_back( c.ref + "'s pass element conducts backwards, up to " + FormatSI( worst, "A" ) + " (limit 100 µA, leakage)" );

                // (into an output above ground: an ideal current load pulls a dead rail below 0 V, and
                // current from 0 V down to there is no power from nowhere)
                f = share( [&]( size_t i ) { return std::fabs( val( vin, i ) ) < 0.05 && val( io, i ) > 1e-3 && val( vo, i ) > 0.05; },
                           [&]( size_t i ) { return val( io, i ); }, false, worst );

                if( f > 0.01 )
                    mc.push_back( c.ref + " delivers up to " + FormatSI( worst, "A" ) + " with its input at 0 V " + howLong( f, false )
                                  + " (limit 1 mA for 1%): power from nowhere (a model error)" );
            }
            else if( c.kind == "OPAMP" )
            {
                const auto *o = v( "o" ), *vp = v( "vp" ), *vn = v( "vn" ), *io = v( "io" );

                f = share( [&]( size_t i ) { return val( o, i ) > val( vp, i ) + 0.05 || val( o, i ) < val( vn, i ) - 0.05; },
                           [&]( size_t i ) { return std::max( val( o, i ) - val( vp, i ), val( vn, i ) - val( o, i ) ); }, false, worst );

                if( f > 0.01 )
                    mc.push_back( c.ref + "'s output stage beyond its supply rails by up to " + mv( worst ) + " " + howLong( f, false )
                                  + " (limit: 50 mV for 1%; a model error)" );

                f = share( [&]( size_t i )
                           {
                               return std::fabs( val( vp, i ) - val( vn, i ) ) < 0.1 && std::fabs( val( io, i ) ) > 1e-3
                                      && std::fabs( val( o, i ) - val( vn, i ) ) > 0.05;
                           },
                           [&]( size_t i ) { return std::fabs( val( io, i ) ); }, false, worst );

                if( f > 0.01 )
                    mc.push_back( c.ref + "'s output drives up to " + FormatSI( worst, "A" ) + " with its supply at 0 V " + howLong( f, false )
                                  + " (limit 1 mA for 1%; a model error)" );
            }
            else if( c.kind == "M" )
            {
                const auto *io = v( "i" ), *vgs = v( "vgs" ), *vds = v( "vds" );
                const double vth = c.num.at( "vth" ), sign = c.num.at( "pch" ) > 0 ? -1 : 1;

                // (forward Vds only: reversed, the drain-bulk junction conducts as the body diode does)
                f = share( [&]( size_t i )
                           { return sign * val( vgs, i ) < vth - 0.2 && sign * val( vds, i ) > 0.1 && std::fabs( val( io, i ) ) > 1e-4; },
                           [&]( size_t i ) { return std::fabs( val( io, i ) ); }, false, worst );

                if( f > 0.01 )
                    mc.push_back( c.ref + "'s channel conducts up to " + FormatSI( worst, "A" ) + " forward with its gate off (|Vgs| under Vth "
                                  + mv( vth ) + " - 0.2 V) " + howLong( f, false ) + " (limit 100 µA for 1%; a model error)" );
            }
            else if( c.kind == "D" )
            {
                const auto *io = v( "i" ), *vak = v( "v" );
                const double vz = c.num.at( "vz" );

                f = share( [&]( size_t i )
                           { return val( vak, i ) < -0.1 && ( vz <= 0 || -val( vak, i ) < 0.8 * vz ) && val( io, i ) < -1e-4; },
                           [&]( size_t i ) { return -val( io, i ); }, false, worst );

                if( f > 0.01 )
                    mc.push_back( c.ref + " conducts up to " + FormatSI( worst, "A" ) + " backwards outside breakdown " + howLong( f, false )
                                  + " (limit 100 µA for 1%; a model error)" );
            }
        }

        // the sources: one at 0 V that delivers; one that absorbs; one delivering what nothing declared draws
        for( size_t si = 0; si < srcs.size(); ++si )
        {
            const SRC& sr = srcs[si];

            if( sr.load )
                continue;

            const std::vector<double>* iv = col( "src:" + std::to_string( si ) );
            const std::vector<double>* vv = col( "chk:src:" + std::to_string( si ) );

            if( !iv || !vv || checks.empty() )
                continue;

            // steady: tran the window's average, op the point, dc the worst of the sweep; delivering
            // as power goes (out of + at a positive voltage, into + at a negative one)
            std::vector<double> neg( iv->size() );

            for( size_t i = 0; i < neg.size(); ++i )
                neg[i] = -( *iv )[i] * ( val( vv, i ) < 0 ? -1 : 1 );
            double steadyOut = 0, steadyIn = 0;

            if( analysis == "tran" )
                steadyOut = steadyIn = measureTran( x, neg, tstop - window, settlePct ).avg;
            else if( analysis == "dc" )
            {
                steadyOut = *std::max_element( neg.begin(), neg.end() );
                steadyIn = *std::min_element( neg.begin(), neg.end() );
            }
            else
                steadyOut = steadyIn = neg.back();

            if( steadyIn < -0.01 )
                mc.push_back( bare( sr.net ) + " absorbs " + FormatSI( -steadyIn, "A" ) + ( analysis == "tran" ? " on average over the window" : "" )
                              + " (limit 10 mA): current is driven back into it (another source, a part's output, or a model)" );

            // what it feeds may draw only through the regulators / op-amps and the loads: no DC path
            // through a resistor, inductor, diode, MOSFET or a part's own model on its net
            const bool bounded = std::none_of( parts.begin(), parts.end(), [&]( const PART& p )
                                               {
                                                   static const std::set<std::string> dcPath = { "R", "L", "FB", "D", "Z", "M", "KICAD", "SUBCKT" };
                                                   return dcPath.count( p.kind ) && std::any_of( p.sch->pins().begin(), p.sch->pins().end(),
                                                                                                 [&]( const ks::SchSimPin& pin ) { return pin.net() == sr.net; } );
                                               } );

            if( bounded && steadyOut > std::max( 3 * declared, 0.1 ) )
                mc.push_back( bare( sr.net ) + " delivers " + FormatSI( steadyOut, "A" ) + ( analysis == "tran" ? " on average over the window" : "" )
                              + ", over 3 x what the loads and the regulators' rated outputs account for (" + FormatSI( declared, "A" )
                              + ") and over 100 mA: something draws what nothing declared does (a model, or a short)" );
        }

        for( size_t i = 0; i < mc.size() && i < 6; ++i )
            out += "\nmodel check: " + mc[i];

        if( mc.size() > 6 )
            out += "\nmodel check: " + std::to_string( mc.size() - 6 ) + " more";
    }

    if( !checksSkipped.empty() )
        out += "\nnote: model checks skipped (" + checksSkipped + ")";

    if( showNetlist )
        out += "\nnetlist:\n" + circuit + ".control\n" + control + "\n.endc";

    TOOL_RESULT result{ out + "\n", false };

    if( plotOn && analysis == "op" )
    {
        result.text += "plot: nothing to draw for op (one point)\n";
    }
    else if( chartOn && analysis != "op" )
    {
        plot::CHART chart;
        chart.title = "simulate " + analysis;
        chart.xUnit = analysis == "tran" ? "s" : analysis == "ac" ? "Hz" : sweptCurrent ? "A" : "V";
        chart.logX = analysis == "ac";

        // one panel: each probe (its corners faint, the same colour)
        auto panel = [&]( const std::string& aPrefix, const std::string& aUnit, double aScale, bool aOnlyCurrents ) -> plot::PANEL
        {
            plot::PANEL p;
            p.yUnit = aUnit;
            int colour = 0;

            for( size_t li = 0; li < lab.size(); ++li )
            {
                if( lab[li].rfind( aPrefix, 0 ) != 0 )
                    continue;

                std::string name = lab[li].substr( aPrefix.size() );

                if( aPrefix == "src:" )
                {
                    const SRC& s = srcs[std::stoi( name )];
                    name = bare( s.net ) + " source";
                }
                else if( !aOnlyCurrents )
                {
                    name = bare( name );
                }

                for( size_t ri = 0; ri < runs.size(); ++ri )
                {
                    const std::vector<double>* y = column( runs[ri], runLabels[ri], lab[li] );

                    if( !y )
                        continue;

                    plot::SERIES s;
                    s.label = ri == 0 ? name : "";
                    s.x = runs[ri].table.cols[0];
                    s.y = *y;
                    s.color = colour;
                    s.faint = ri > 0;

                    for( double& v : s.y )
                        v *= aScale;

                    p.series.push_back( std::move( s ) );
                }

                colour++;
            }

            return p;
        };

        if( analysis == "ac" )
        {
            chart.panels.push_back( panel( "v:", "dB", 1, false ) );
            chart.panels.push_back( panel( "ph:", "°", 57.29577951308232, false ) );
        }
        else
        {
            chart.panels.push_back( panel( "v:", "V", 1, false ) );
            plot::PANEL src = panel( "src:", "A", -1, true );
            plot::PANEL cur = panel( "i:", "A", 1, true );
            int         used = 0;

            for( const plot::SERIES& s : src.series )
                used = std::max( used, s.color + 1 );

            for( plot::SERIES& s : cur.series )
                s.color += used;

            src.series.insert( src.series.end(), cur.series.begin(), cur.series.end() );

            if( !src.series.empty() )
                chart.panels.push_back( std::move( src ) );
        }

        chart.panels.erase( std::remove_if( chart.panels.begin(), chart.panels.end(), []( const plot::PANEL& p ) { return p.series.empty(); } ),
                            chart.panels.end() );
        plot::Publish( chart, "simulate" );

        if( plotOn )
        {
            result.image = plot::Png( chart );

            if( result.image.empty() )
                result.text += "plot: nothing to draw\n";
        }
    }

    return result;
}


std::string sim::FindNgspice()
{
    return findNgspice();
}


sim::RUN sim::RunSpice( const std::string& aExe, const std::string& aCircuit, const std::string& aAnalysis,
                        const std::vector<std::string>& aVectors, int aTimeoutS )
{
    return runSpice( aExe, aCircuit, aAnalysis, aVectors, aTimeoutS, nullptr );
}


sim::CAP_MODEL sim::CapModel( const std::string& aValue, const std::string& aText, const std::string& aDescription, const kb::LcscHit* h,
                              const json& aOver )
{
    CAP_MODEL   m;
    double      v = 0;
    auto        over = [&]( const char* k, double& aOut ) { return aOver.is_object() && aOver.contains( k ) && jsonSI( aOver[k], aOut ); };

    if( !over( "c", m.c ) )
    {
        if( h && h->unit() == "F" && h->value() > 0 )
            m.c = h->value();
        else if( valueOf( aValue, v ) )
            m.c = v;
        else
        {
            // a value written as a part name: "CC0402_100NF_16V_10%_X7R", "C_4U7_0603", "10uF/6.3V",
            // "C_100n_0402" / "C_22u_25V_0805" (a bare p / n / u token; M would be ambiguous)
            static const std::regex rxF( R"((^|[^A-Z0-9.])(\d+(?:\.\d+)?)\s*([PNUM])F(?![A-Z]))" );
            static const std::regex rxMid( R"((^|[^A-Z0-9.])(\d+)([PNU])(\d+)(?![A-Z0-9]))" );
            static const std::regex rxBare( R"((^|[_\s/-])(\d+(?:\.\d+)?)([PNU])(?=$|[_\s/-]))" );
            const std::string       uv = upper( aValue );
            std::smatch             sm;
            const auto              scale = []( char c ) { return c == 'P' ? 1e-12 : c == 'N' ? 1e-9 : c == 'U' ? 1e-6 : 1e-3; };

            if( std::regex_search( uv, sm, rxF ) )
                m.c = std::stod( sm[2].str() ) * scale( sm[3].str()[0] );
            else if( std::regex_search( uv, sm, rxMid ) )
                m.c = std::stod( sm[2].str() + "." + sm[4].str() ) * scale( sm[3].str()[0] );
            else if( std::regex_search( uv, sm, rxBare ) )
                m.c = std::stod( sm[2].str() ) * scale( sm[3].str()[0] );
        }
    }

    // the kind: the index's subcategory, else the footprint / symbol / value words
    {
        std::string sub = h ? upper( h->subcategory() + " " + h->description() ) : "";
        std::string fp = upper( aText );

        if( sub.find( "TANTALUM" ) != std::string::npos || fp.find( "TANTALUM" ) != std::string::npos || fp.find( "CP_TANT" ) != std::string::npos )
            m.kind = "tantalum";
        else if( sub.find( "POLYMER" ) != std::string::npos || sub.find( "SOLID" ) != std::string::npos )
            m.kind = "polymer";
        else if( sub.find( "ALUMINUM" ) != std::string::npos || sub.find( "ALUMINIUM" ) != std::string::npos
                 || sub.find( "ELECTROLYTIC" ) != std::string::npos || fp.find( "CP_ELEC" ) != std::string::npos
                 || fp.find( "CP_RADIAL" ) != std::string::npos || fp.find( "ELEC" ) != std::string::npos )
            m.kind = "aluminium";
        else
            m.kind = "MLCC";
    }

    const BODY* b = bodyOf( ( h ? h->package() + " " : "" ) + aText );
    m.package = b ? b->eia : ( h ? h->package() : "" );
    m.dielectric = h ? h->dielectric() : "";
    m.vrated = h ? h->voltage() : 0;

    // "10uF 6.3V X5R" in the value
    std::string uv = upper( aValue + " " + aDescription );

    for( const char* d : { "X5R", "X7R", "X6S", "X7S", "X7T", "X8R", "Y5V", "Z5U", "C0G", "NP0" } )
    {
        if( m.dielectric.empty() && uv.find( d ) != std::string::npos )
            m.dielectric = std::string( d ) == "NP0" ? "C0G" : d;
    }

    std::smatch         sm;
    static const std::regex rxV( R"((\d+(?:\.\d+)?)\s*V\b)" );

    if( m.vrated == 0 && std::regex_search( uv, sm, rxV ) )
        m.vrated = std::stod( sm[1].str() );

    std::string assumed;

    if( m.kind == "MLCC" && m.dielectric.empty() )
    {
        // not said: C0G is what small values usually are, X5R the larger (the worse case)
        m.dielectric = m.c <= 1e-9 ? "C0G" : "X5R";
        assumed = "dielectric " + m.dielectric + " assumed";
    }

    if( aOver.is_object() && aOver.contains( "dc_bias" ) && aOver["dc_bias"].is_boolean() )
        m.biasOn = aOver["dc_bias"].get<bool>();

    if( m.kind != "MLCC" )
        m.biasOn = false;

    // ESL: the body's (mounting inductance, pads and vias, comes on top on the board)
    bool eslGiven = over( "esl", m.esl );
    bool esrGiven = over( "esr", m.esr );

    if( !eslGiven )
    {
        if( m.kind == "MLCC" )
            m.esl = b ? b->esl : 0.6e-9;
        else
            m.esl = m.kind == "aluminium" ? 3e-9 : 1.5e-9;
    }

    // ESR, typical: MLCC ~15 mΩ x (C / 1 µF)^-1/2 (100 nF ~50 mΩ, 10 µF ~5 mΩ), at least 2 mΩ;
    // aluminium ~0.3 Ω at 100 µF falling as the root of C; tantalum ~0.5 Ω, polymer ~20 mΩ
    if( !esrGiven )
    {
        const double cu = std::max( m.c, 1e-12 ) / 1e-6;

        if( m.kind == "MLCC" )
            m.esr = std::max( 0.002, 0.015 / std::sqrt( cu ) );
        else if( m.kind == "aluminium" )
            m.esr = std::clamp( 0.3 / std::sqrt( cu / 100 ), 0.02, 5.0 );
        else if( m.kind == "tantalum" )
            m.esr = std::clamp( 0.5 / std::sqrt( cu / 10 ), 0.05, 5.0 );
        else
            m.esr = 0.02;
    }

    m.tol = m.kind == "MLCC" ? ( m.dielectric == "C0G" ? 0.05 : 0.1 ) : 0.2;
    m.estimated = !( esrGiven && eslGiven );
    m.note = std::string( esrGiven ? "ESR " : "ESR ~" ) + FormatSI( m.esr, "Ω", 2 ) + ( eslGiven ? ", ESL " : ", ESL ~" )
             + FormatSI( m.esl, "H", 2 ) + ( esrGiven && eslGiven ? "" : " (est.: typical for " + m.kind + ( m.package.empty() ? "" : " " + m.package ) + ")" )
             + ( assumed.empty() ? "" : "; " + assumed );
    return m;
}


std::optional<double> sim::VoltageFromName( const std::string& aNet )
{
    return voltageFromName( aNet );
}

} // namespace kicadd::mcp
