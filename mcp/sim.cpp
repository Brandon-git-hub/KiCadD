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
    bool        tolAssumed = false;
};

double param( const PART& aP, const std::string& aId, double aDefault = 0 )
{
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

std::string capKindOf( const PART& aP )
{
    std::string sub = aP.hit ? upper( aP.hit->subcategory() + " " + aP.hit->description() ) : "";
    std::string fp = upper( aP.sch->footprint() + " " + aP.sch->lib_id() + " " + aP.sch->value() );

    if( sub.find( "TANTALUM" ) != std::string::npos || fp.find( "TANTALUM" ) != std::string::npos || fp.find( "CP_TANT" ) != std::string::npos )
        return "tantalum";

    if( sub.find( "POLYMER" ) != std::string::npos || sub.find( "SOLID" ) != std::string::npos )
        return "polymer";

    if( sub.find( "ALUMINUM" ) != std::string::npos || sub.find( "ALUMINIUM" ) != std::string::npos || sub.find( "ELECTROLYTIC" ) != std::string::npos
        || fp.find( "CP_ELEC" ) != std::string::npos || fp.find( "CP_RADIAL" ) != std::string::npos || fp.find( "ELEC" ) != std::string::npos )
        return "aluminium";

    return "MLCC";
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
    auto                  over = [&]( const char* k, double& aOut ) { return aP.over.contains( k ) && jsonSI( aP.over[k], aOut ); };

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
        if( !over( "c", aP.c ) )
        {
            if( h && h->unit() == "F" && h->value() > 0 )
                aP.c = h->value();
            else if( valueOf( s.value(), v ) )
                aP.c = v;
        }

        aP.capKind = capKindOf( aP );
        const sim::BODY* b = sim::bodyOf( ( h ? h->package() + " " : "" ) + s.footprint() );
        aP.package = b ? b->eia : ( h ? h->package() : "" );
        aP.dielectric = h ? h->dielectric() : "";
        aP.vrated = h ? h->voltage() : 0;

        // "10uF 6.3V X5R" in the value
        std::string uv = upper( s.value() + " " + s.description() );

        for( const char* d : { "X5R", "X7R", "X6S", "X7S", "X7T", "X8R", "Y5V", "Z5U", "C0G", "NP0" } )
        {
            if( aP.dielectric.empty() && uv.find( d ) != std::string::npos )
                aP.dielectric = std::string( d ) == "NP0" ? "C0G" : d;
        }

        std::smatch m;
        static const std::regex rxV( R"((\d+(?:\.\d+)?)\s*V\b)" );

        if( aP.vrated == 0 && std::regex_search( uv, m, rxV ) )
            aP.vrated = std::stod( m[1].str() );

        std::string assumed;

        if( aP.capKind == "MLCC" && aP.dielectric.empty() )
        {
            // not said: C0G is what small values usually are, X5R the larger (the worse case)
            aP.dielectric = aP.c <= 1e-9 ? "C0G" : "X5R";
            assumed = "dielectric " + aP.dielectric + " assumed";
        }

        if( aP.over.contains( "dc_bias" ) && aP.over["dc_bias"].is_boolean() )
            aP.biasOn = aP.over["dc_bias"].get<bool>();

        if( aP.capKind != "MLCC" )
            aP.biasOn = false;

        // ESL: the body's (mounting inductance, pads and vias, comes on top on the board)
        bool eslGiven = over( "esl", aP.esl );
        bool esrGiven = over( "esr", aP.esr );

        if( !eslGiven )
        {
            if( aP.capKind == "MLCC" )
                aP.esl = b ? b->esl : 0.6e-9;
            else
                aP.esl = aP.capKind == "aluminium" ? 3e-9 : 1.5e-9;
        }

        // ESR, typical: MLCC ~15 mΩ x (C / 1 µF)^-1/2 (100 nF ~50 mΩ, 10 µF ~5 mΩ), at least 2 mΩ;
        // aluminium ~0.3 Ω at 100 µF falling as the root of C; tantalum ~0.5 Ω, polymer ~20 mΩ
        if( !esrGiven )
        {
            const double cu = std::max( aP.c, 1e-12 ) / 1e-6;

            if( aP.capKind == "MLCC" )
                aP.esr = std::max( 0.002, 0.015 / std::sqrt( cu ) );
            else if( aP.capKind == "aluminium" )
                aP.esr = std::clamp( 0.3 / std::sqrt( cu / 100 ), 0.02, 5.0 );
            else if( aP.capKind == "tantalum" )
                aP.esr = std::clamp( 0.5 / std::sqrt( cu / 10 ), 0.05, 5.0 );
            else
                aP.esr = 0.02;
        }

        if( aP.tol == 0 )
        {
            aP.tol = aP.capKind == "MLCC" ? ( aP.dielectric == "C0G" ? 0.05 : 0.1 ) : 0.2;
            aP.tolAssumed = true;
        }

        aNote = std::string( esrGiven ? "ESR " : "ESR ~" ) + FormatSI( aP.esr, "Ω", 2 ) + ( eslGiven ? ", ESL " : ", ESL ~" )
                + FormatSI( aP.esl, "H", 2 ) + ( esrGiven && eslGiven ? "" : " (est.: typical for " + aP.capKind
                                                                                   + ( aP.package.empty() ? "" : " " + aP.package ) + ")" )
                + ( assumed.empty() ? "" : "; " + assumed );
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
        }

        add( line + " " + aP.over["subckt"].get<std::string>() );
        return true;
    }

    if( aP.kind == "R" )
    {
        if( aP.r <= 0 || !twoPinNodes( aP, aN, a, b ) )
        {
            aP.why = "no resistance (value \"" + s.value() + "\")";
            return false;
        }

        add( "R" + id + " " + a + " " + b + " " + fmt( aP.r * aScale, 6 ) );
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

        const double is = iff / std::exp( vf / ( n * 0.02585 ) );
        std::string  model = "D(IS=" + fmt( is ) + " N=" + fmt( n ) + " RS=0";
        std::string  what;

        if( aP.kind == "Z" && hasParam( aP, "vz" ) )
        {
            model += " BV=" + fmt( param( aP, "vz" ) ) + " IBV=1m";
            what = "Vz " + FormatSI( param( aP, "vz" ), "V" );

            if( hasParam( aP, "zzt" ) )
                model = "D(IS=" + fmt( is ) + " N=" + fmt( n ) + " RS=" + fmt( param( aP, "zzt" ) ) + " BV=" + fmt( param( aP, "vz" ) ) + " IBV=1m";
        }

        if( hasParam( aP, "trr" ) )
            model += " TT=" + fmt( param( aP, "trr" ) / 1.5 );

        add( "D" + id + " " + a + " " + b + " kd_d_" + id );
        aN.models.push_back( ".model kd_d_" + id + " " + model + ")" );
        aP.notes = ( hasParam( aP, "vf" ) ? "Vf " + FormatSI( vf, "V" ) + " @" + FormatSI( iff, "A" )
                                          : "Vf " + FormatSI( vf, "V" ) + " assumed (" + ( schottky ? "Schottky" : "silicon" ) + ")" )
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

        add( "M" + id + " " + nd + " " + ng + " " + ns + " " + ns + " kd_m_" + id );
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
        const double rout = param( aP, "rout", 0.01 );
        const double gain = 1e4;   // 0.01% off Vref at DC
        const std::string nfb = adjustable ? aN.node( *fb, s.ref() ) : "kd_" + id + "_o";
        const std::string k = "kd_" + id + "_";

        // error amplifier (one pole: the loop crosses at loop_bw with the feedback at 1), then the
        // pass element: at most Vin - dropout, at least 0; PSRR as a flat feed-through of the
        // input's ripple (AC-coupled); the input draws the output current plus Iq
        add( "B" + id + "_e " + k + "e " + ng + " V=" + fmt( gain ) + "*(" + fmt( vref, 6 ) + "-" + vd( nfb, ng ) + ")" );
        add( "R" + id + "_p " + k + "e " + k + "p 1k" );
        add( "C" + id + "_p " + k + "p " + ng + " " + fmt( gain / ( 2 * PI * 1e3 * fbw ) ) );
        add( "C" + id + "_hp " + nin + " " + k + "h 1u" );
        add( "R" + id + "_hp " + k + "h " + ng + " 1Meg" );

        std::string on = "1";

        if( en && !en->net().empty() )
            on = "(" + vd( aN.node( *en, s.ref() ), ng ) + ">0.9?1:0)";

        add( "B" + id + "_o " + k + "x " + ng + " V=" + on + "*max(0,min(" + vd( k + "p", ng ) + "," + vd( nin, ng ) + "-" + fmt( drop ) + "))+"
             + fmt( std::pow( 10, -psrr / 20 ) ) + "*" + vd( k + "h", ng ) );
        add( "V" + id + "_i " + k + "x " + k + "o 0" );
        add( "R" + id + "_o " + k + "o " + nout + " " + fmt( rout ) );
        add( "B" + id + "_in " + nin + " " + ng + " I=max(I(V" + id + "_i),0)+" + fmt( iq ) );
        add( "R" + id + "_gmin " + k + "p " + ng + " 1G" );

        aN.driven.insert( out->net() );
        aN.currents.push_back( { s.ref() + " out", "i(v" + id + "_i)" } );

        auto src = [&]( const char* aId ) { return hasParam( aP, aId ) ? "" : " assumed"; };
        aP.notes = std::string( adjustable ? "Vref " : "Vout " ) + FormatSI( vref, "V" ) + ( adjustable ? " on " + bare( fb->net() ) : "" )
                   + ", dropout " + FormatSI( drop, "V" ) + src( "dropout" ) + ", Iq " + FormatSI( iq, "A" ) + src( "iq" ) + ", PSRR "
                   + fmt( psrr, 3 ) + " dB" + ( hasParam( aP, "psrr" ) ? " (one number: flat over frequency)" : " assumed" ) + "; loop "
                   + FormatSI( fbw, "Hz" ) + src( "loop_bw" ) + ", Rout " + FormatSI( rout, "Ω" ) + src( "rout" )
                   + "; no current limit (Iout " + ( hasParam( aP, "iout" ) ? FormatSI( param( aP, "iout" ), "A" ) : "?" ) + ")";
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

        std::map<std::string, CH> chans;
        const ks::SchSimPin*      vp = nullptr;
        const ks::SchSimPin*      vn = nullptr;

        for( const ks::SchSimPin& pin : s.pins() )
        {
            std::string n = pinName( pin );

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

            std::string key = n;
            int         role = 0;

            if( n.find( "OUT" ) != std::string::npos || n == "~" || n == "O" )
                role = 3;
            else if( n.find( '+' ) != std::string::npos )
                role = 1;
            else if( n.find( '-' ) != std::string::npos )
                role = 2;

            if( !role )
                continue;

            for( const char* tok : { "OUT", "IN", "+", "-", "~" } )
            {
                size_t at;

                while( ( at = key.find( tok ) ) != std::string::npos )
                    key.erase( at, std::strlen( tok ) );
            }

            CH& ch = chans[pin.unit() + "|" + key];
            ( role == 1 ? ch.p : role == 2 ? ch.n : ch.o ) = &pin;
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

        for( const auto& [key, ch] : chans )
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
            // keep the integrator near the rails (no wind-up past them)
            add( "B" + c + "_cl " + k + "1 0 I=(V(" + k + "1)>V(" + nvp + ")+1?V(" + k + "1)-V(" + nvp + ")-1:0)+(V(" + k + "1)<V(" + nvn
                 + ")-1?V(" + k + "1)-V(" + nvn + ")+1:0)" );
            add( "B" + c + "_o " + k + "o 0 V=max(V(" + nvn + ")+" + fmt( hr ) + ",min(V(" + nvp + ")-" + fmt( hr ) + ",V(" + k + "1)))" );
            add( "R" + c + "_o " + k + "o " + no + " 25" );

            if( ib != 0 )
            {
                add( "I" + c + "_bp " + np + " 0 " + fmt( ib ) );
                add( "I" + c + "_bn " + nn + " 0 " + fmt( ib ) );
            }
        }

        if( !made )
        {
            aP.why = "op-amp input / output pins not found (+IN / -IN / OUT)";
            return false;
        }

        if( iq > 0 )
            add( "I" + id + "_q " + nvp + " " + nvn + " " + fmt( iq ) );

        auto src = [&]( const char* aId ) { return hasParam( aP, aId ) ? "" : " assumed"; };
        aP.notes = std::to_string( made ) + " channel(s): GBW " + FormatSI( gbw, "Hz" ) + src( "gbw" ) + " (one pole after 100 dB), slew "
                   + fmt( slew / 1e6, 3 ) + " V/µs" + src( "slew" ) + ", Vos " + FormatSI( vos, "V" ) + src( "vos" ) + ", output "
                   + ( rro ? "rail to rail" : "1.2 V from the rails" ) + ( hasParam( aP, "rr_out" ) ? "" : " assumed" )
                   + "; supply current Iq only";
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

struct TABLE
{
    std::vector<std::string>         names;
    std::vector<std::vector<double>> cols;   // cols[0] the scale (time / frequency), then each vector
};

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

struct RUN
{
    bool        ok = false;
    std::string error;
    TABLE       table;
    double      seconds = 0;
};

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
           "\"pulse(0 5 0 1u 1u 5m 10m)\", \"sin(5 0.05 100k)\"} to ground; supplies not given and not made in the region "
           "get the voltage their name says (+3V3) and that is said. loads {net: amps, a SPICE current source "
           "(\"pulse(0.1 1 1m 1u 1u 1m 2m)\") or \"10 ohm\"}. analysis: op (default), tran {stop, step?, window?: "
           "the last part measured, default the last 20%} or ac {from, to, points per decade} (needs a source with "
           "ac 1). probes: nets to measure (default the loads', the regulators' outputs, else the region's nets). "
           "Measured: op the voltages; tran the average, ripple p-p and min / max over the window, a step's "
           "overshoot, 10-90% rise and settling (settle_pct, default 2), a sketch line; ac the gain at the "
           "lowest frequency, its peak and the -3 dB frequency; the sources' currents and power, inductors' peak "
           "and rms current (with Isat / Irated), regulators' output current. No verdicts: compare them with your "
           "own targets. corners true: two more runs, all low and all high: every R / C / L at the same end of its "
           "tolerance together (from the data, else assumed and said). Not a worst case or Monte Carlo: in a "
           "ratio (a divider, matched RCs) the ends cancel; set one part's value with models to try a mix. "
           "MLCC DC bias comes from an op run first (for tran its average over the window): right for linear "
           "circuits; a switching converter or an AC-coupled node is not at its DC point there. dc_bias false: MLCCs at their nominal C. models {ref: {c, esr, esl, r, "
           "l, dcr, tol (%), dc_bias, vout, vref, dropout, iq, psrr, loop_bw, rout, gbw, slew, vos, rds_on, vgs_th, "
           "vf ... (any parameter, SI), exclude: true, or subckt + lib (a .lib / .cir path, relative to the project) "
           "+ pins (the subckt's pin order as pin numbers or names)}} override what the data gives. netlist true "
           "adds the netlist made.";
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
                 { "analysis", { { "type", "string" }, { "enum", { "op", "tran", "ac" } } } },
                 { "tran", { { "type", "object" }, { "description", "{stop: \"2m\", step?: \"1u\", window?: \"0.4m\"}" } } },
                 { "ac", { { "type", "object" }, { "description", "{from: \"10\", to: \"10Meg\", points?: 20}" } } },
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

    // --- sources, loads
    struct SRC
    {
        std::string net, node, spec, why;
        bool        load = false, resistor = false;
    };

    std::vector<SRC> srcs;
    std::set<std::string> given;

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

                if( s.load && ( lt.find( "ohm" ) != std::string::npos || t.find( "Ω" ) != std::string::npos ) && ParseSI( t, ohm ) && ohm > 0 )
                {
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

        return fail( "supplies with no source and no voltage in their name: " + l + " (sources {net: volts})" );
    }

    // --- the analysis
    std::string analysis = a.contains( "tran" ) ? "tran" : a.contains( "ac" ) ? "ac" : "op";

    if( a.contains( "analysis" ) && a["analysis"].is_string() )
        analysis = a["analysis"].get<std::string>();

    double tstop = 0, tstep = 0, window = 0, f0 = 10, f1 = 100e6;
    int    ppd = 20;

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
    else if( analysis != "op" )
    {
        return fail( "analysis: op, tran or ac" );
    }

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

    // --- the netlist for one run: c / r / l scaled for the corners
    auto build = [&]( double aScale, std::vector<std::string>& aVectors, std::vector<std::string>& aLabels ) -> std::string
    {
        NETLIST n = base;
        n.driven.clear();

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
            aVectors.push_back( analysis == "ac" ? "vdb(" + node + ")" : "v(" + node + ")" );
            aLabels.push_back( "v:" + pr );
        }

        if( analysis != "ac" )
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

            // the MLCCs' DC voltage (the bias pass)
            for( PART& p : parts )
            {
                if( p.kind == "C" && p.capKind == "MLCC" )
                {
                    std::vector<const ks::SchSimPin*> pins = pinsByNumber( *p.sch );
                    NETLIST                           tmp = base;
                    std::string                       na = tmp.node( *pins[0], p.sch->ref() );
                    std::string                       nb = tmp.node( *pins[1], p.sch->ref() );

                    if( na.rfind( "nc_", 0 ) == 0 || nb.rfind( "nc_", 0 ) == 0 )
                        continue;

                    if( na == "0" && nb == "0" )
                        continue;

                    aVectors.push_back( na == "0" ? "-v(" + nb + ")" : nb == "0" ? "v(" + na + ")" : "v(" + na + ")-v(" + nb + ")" );
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

    // --- the DC bias pass: the DC voltage across each MLCC (op; for tran its average over the window)
    bool anyBias = biasOn && std::any_of( parts.begin(), parts.end(), []( const PART& p )
                                          { return p.kind == "C" && p.biasOn && p.capKind == "MLCC" && p.dielectric != "C0G"; } );

    if( anyBias )
    {
        std::string circuit = build( 1.0, vectors, labels );
        RUN         r = runSpice( exe, circuit, analysis == "tran" ? control : "op", vectors, timeout, nullptr );
        spent += r.seconds;

        if( !r.ok )
            return fail( "the DC bias pass: " + r.error + ( showNetlist ? "\n" + circuit : "" ) );

        for( PART& p : parts )
        {
            if( p.kind != "C" || !p.biasOn || p.capKind != "MLCC" )
                continue;

            const std::vector<double>* v = column( r, labels, "cap:" + p.sch->ref() );

            if( !v || v->empty() )
                continue;

            p.vdc = analysis == "tran" ? measureTran( r.table.cols[0], *v, tstop - window, 2 ).avg : v->back();
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
            return fail( std::string( scale == 1.0 ? "" : "corner run: " ) + r.error + ( showNetlist ? "\n" + c : "" ) );

        runs.push_back( std::move( r ) );
        runLabels.push_back( labels );
    }

    // --- the answer
    int modelled = 0;
    std::string leftOut;

    for( const PART& p : parts )
    {
        if( !p.kind.empty() )
            modelled++;
        else
            leftOut += ( leftOut.empty() ? "" : "; " ) + p.sch->ref() + " " + p.sch->value() + " (" + p.why + ")";
    }

    std::string out = "simulate " + analysis;

    if( analysis == "tran" )
        out += " " + FormatSI( tstop, "s" ) + " (step " + FormatSI( tstep, "s" ) + ")";
    else if( analysis == "ac" )
        out += " " + FormatSI( f0, "Hz" ) + " - " + FormatSI( f1, "Hz" );

    out += ": " + std::to_string( modelled ) + " of " + std::to_string( parts.size() ) + " part(s) modelled, "
           + std::to_string( region.nets().size() ) + " net(s); ngspice " + fmt( spent, 2 ) + " s" + ( corners ? " (3 runs: nominal, all low, all high: every R / C / L moved together, not a worst case or Monte Carlo)" : "" );

    if( !indexNote.empty() )
        out += "\nnote: " + indexNote;

    for( const std::string& n : region.notes() )
        out += "\nnote: " + n;

    // sources
    {
        std::string l;

        for( const SRC& s : srcs )
        {
            if( !s.load )
                l += ( l.empty() ? "" : "; " ) + bare( s.net ) + " " + s.spec + ( s.why.empty() ? "" : " (" + s.why + ")" );
        }

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
            line += "; " + fmt( v->back(), 3 ) + " dB at " + FormatSI( x.back(), "Hz" ) + " " + sparkline( x, *v );
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
            }
        }
    }

    if( showNetlist )
        out += "\nnetlist:\n" + circuit + ".control\n" + control + "\n.endc";

    return TOOL_RESULT{ out + "\n", false };
}

} // namespace kicadd::mcp
