/*
 * KiCadD MCP - reflection: a net's edge as routed, in ngspice with each track piece a lossless
 * transmission line (roadmap phase 3, item 4: reflections and series termination).
 *
 * The board gives the net's copper as a graph (NetTopology: track pieces with Z0 and delay of their
 * layer and width, vias and pads as nodes); the driver is the net's output pin (or the agent's),
 * through a series resistor on the net when there is one (its other net fetched too); the
 * receivers are the other parts' pins, each a capacitance; resistors to a supply or ground are
 * terminations, capacitors to ground loads. The driver: a step of vdd with rise time and output
 * resistance (given, or assumed and said). Runs as routed and with a series resistor of the line's
 * Z0 minus the driver's resistance (E24), and any the agent asks for; measures at each receiver
 * (overshoot, ringback, settling, flight time, a non-monotonic threshold crossing). No verdicts.
 *
 * Not modelled: losses, via barrels (joints), coupling to other nets, the package.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "ipc_client.h"
#include "plot.h"
#include "sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <set>
#include <sstream>

#include <kicadd/board.pb.h>

namespace kb = kicadd::board;
using json = nlohmann::json;

namespace kicadd::mcp
{

namespace
{

std::string g( double v, int aDigits = 3 )
{
    char buf[40];
    std::snprintf( buf, sizeof buf, "%.*g", aDigits, v );
    return buf;
}


std::string si( double v, const std::string& aUnit, int aDigits = 3 )
{
    return sim::FormatSI( v, aUnit, aDigits );
}


bool jsonSI( const json& aV, double& aOut )
{
    if( aV.is_number() )
    {
        aOut = aV.get<double>();
        return true;
    }

    return aV.is_string() && sim::ParseSI( aV.get<std::string>(), aOut );
}


std::string prefix( const std::string& aRef )
{
    size_t i = 0;

    while( i < aRef.size() && std::isalpha( (unsigned char) aRef[i] ) )
        i++;

    return aRef.substr( 0, i );
}


bool groundName( const std::string& aNet )
{
    std::string u = aNet;
    std::transform( u.begin(), u.end(), u.begin(), ::toupper );
    return u.find( "GND" ) != std::string::npos || u.find( "VSS" ) != std::string::npos || u == "0V";
}


/// a resistor's or capacitor's value from its value field, also when written as a part name:
/// "22", "4.7k", "4K7", "R_0R_0402", "R0402_10K_1%", "100nF", "CC0402_100NF_16V"
bool partValue( const std::string& aPrefix, const std::string& aValue, double& aOut )
{
    if( aPrefix == "C" )
    {
        sim::CAP_MODEL m = sim::CapModel( aValue, aValue, "", nullptr, json::object() );
        aOut = m.c;
        return m.c > 0;
    }

    double v = 0;

    if( sim::ParseSI( aValue, v ) && aValue.find_first_not_of( "0123456789.,kKmMRrΩ " ) == std::string::npos )
    {
        aOut = v;
        return true;
    }

    static const std::set<std::string> PACKAGES = { "0201", "0402", "0603", "0805", "1206", "1210", "2010", "2512", "1005", "1608", "2012", "3216" };
    std::string                        u = aValue;
    std::transform( u.begin(), u.end(), u.begin(), ::toupper );

    for( char& c : u )
    {
        if( c == '_' || c == '/' || c == '-' || c == ',' )
            c = ' ';
    }

    std::istringstream in( u );
    std::string        tok;

    while( in >> tok )
    {
        if( PACKAGES.count( tok ) || tok.find( '%' ) != std::string::npos )
            continue;

        // 4K7, 0R, 1R5, 10K, 2M2
        size_t i = 0;

        while( i < tok.size() && std::isdigit( (unsigned char) tok[i] ) )
            i++;

        if( i > 0 && i < tok.size() && ( tok[i] == 'R' || tok[i] == 'K' || tok[i] == 'M' ) )
        {
            std::string rest = tok.substr( i + 1 );

            if( rest == "OHM" || rest == "Ω" )
                rest.clear();

            if( rest.find_first_not_of( "0123456789" ) == std::string::npos )
            {
                const double mult = tok[i] == 'R' ? 1 : tok[i] == 'K' ? 1e3 : 1e6;
                aOut = std::stod( tok.substr( 0, i ) + ( rest.empty() ? "" : "." + rest ) ) * mult;
                return true;
            }
        }

        // 100OHM, 33
        if( i > 0 && ( tok.substr( i ) == "OHM" || tok.substr( i ) == "Ω" || ( i == tok.size() && tok.size() < 4 ) ) )
        {
            aOut = std::stod( tok.substr( 0, i ) );
            return true;
        }
    }

    return false;
}


/// the E24 value nearest to aR (0 stays 0)
double e24( double aR )
{
    static const double E[] = { 1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1, 10.0 };

    if( aR <= 0.5 )
        return 0;

    const double d = std::pow( 10.0, std::floor( std::log10( aR ) ) );
    double       best = d, err = 1e9;

    for( double e : E )
    {
        double v = e * d;

        if( std::fabs( std::log( v / aR ) ) < err )
        {
            err = std::fabs( std::log( v / aR ) );
            best = v;
        }
    }

    return best;
}


/// one net's graph in SPICE terms: node names, the lines, where each pad is
struct NETG
{
    const kb::NetTopologyResponse* topo = nullptr;
    std::string                    tag;              // node prefix: "a" / "b"
    std::vector<int>               merged;           // node -> its representative (short pieces joined)
    std::vector<std::string>       lines;            // T elements
    double                         copper = 0;       // mm
    std::map<std::string, double>  layerLen;         // mm per layer
    std::map<std::string, double>  layerZ;           // length-weighted Z0 per layer (sum z * mm)
    double                         zMin = 1e9, zMax = 0, tdMin = 1e9;
    int                            noZ = 0, lumped = 0;
    std::vector<std::pair<int, double>> caps;        // lines far shorter than the edge: their C at a node

    int rep( int n )
    {
        while( merged[n] != n )
            n = merged[n] = merged[merged[n]];

        return n;
    }

    std::string node( int n ) { return tag + std::to_string( rep( n ) ); }
};


void buildNet( NETG& aN, const kb::NetTopologyResponse& aTopo, const std::string& aTag, int& aT, double aShort )
{
    aN.topo = &aTopo;
    aN.tag = aTag;
    aN.merged.resize( std::max( 1, aTopo.nodes() ) );

    for( int i = 0; i < (int) aN.merged.size(); ++i )
        aN.merged[i] = i;

    // pieces with no Z0 are joints
    for( const kb::TopoEdge& e : aTopo.edges() )
    {
        if( e.z0() <= 0 )
        {
            aN.noZ++;
            aN.merged[aN.rep( e.a() )] = aN.rep( e.b() );
        }
    }

    struct PIECE
    {
        int    a, b;
        double z, td;
    };

    std::vector<PIECE>                                pieces;
    std::map<int, std::vector<int>>                   at;   // node -> pieces
    std::set<int>                                     keep;

    for( const kb::TopoEdge& e : aTopo.edges() )
    {
        aN.copper += e.length();
        aN.layerLen[e.layer()] += e.length();

        if( e.z0() <= 0 )
            continue;

        aN.layerZ[e.layer()] += e.z0() * e.length();
        aN.zMin = std::min( aN.zMin, e.z0() );
        aN.zMax = std::max( aN.zMax, e.z0() );

        const int a = aN.rep( e.a() ), b = aN.rep( e.b() );

        if( a == b )
            continue;

        pieces.push_back( { a, b, e.z0(), e.length() / 10 * e.delay() * 1e-12 } );
        at[a].push_back( (int) pieces.size() - 1 );
        at[b].push_back( (int) pieces.size() - 1 );
    }

    // nodes that stay: pads, branches, ends; a chain of pieces between them is one line, cut where
    // its Z0 moves more than 10% (a layer change to another width / stackup)
    for( const kb::TopoPad& p : aTopo.pads() )
        keep.insert( aN.rep( p.node() ) );

    for( auto& [n, list] : at )
    {
        if( list.size() != 2 )
            keep.insert( n );
    }

    std::vector<bool> used( pieces.size(), false );

    auto emit = [&]( int a, int b, double ztd, double td )
    {
        if( a == b || td <= 0 )
            return;

        // a line far shorter than the edge: its capacitance (td / Z0) at a joint (ngspice's step
        // would have to follow its delay down, and can hang on a step longer than a delay)
        if( td < aShort )
        {
            aN.merged[aN.rep( a )] = aN.rep( b );
            aN.caps.push_back( { b, td / ( ztd / td ) } );
            aN.lumped++;
            return;
        }

        aN.tdMin = std::min( aN.tdMin, td );

        aN.lines.push_back( "T" + std::to_string( aT++ ) + " " + aN.tag + std::to_string( a ) + " 0 " + aN.tag + std::to_string( b ) + " 0 Z0="
                            + g( ztd / td, 5 ) + " TD=" + g( td, 5 ) );
    };

    auto walk = [&]( int aStart, int aPiece )
    {
        int    from = aStart, cur = aStart, pi = aPiece;
        double ztd = 0, td = 0;

        while( true )
        {
            used[pi] = true;
            const PIECE& p = pieces[pi];
            const int    next = p.a == cur ? p.b : p.a;

            if( td > 0 && std::fabs( p.z - ztd / td ) > 0.1 * ztd / td )
            {
                emit( from, cur, ztd, td );
                from = cur;
                ztd = td = 0;
            }

            ztd += p.z * p.td;
            td += p.td;
            cur = next;

            if( keep.count( cur ) )
                break;

            int nextPiece = -1;

            for( int q : at[cur] )
            {
                if( !used[q] )
                    nextPiece = q;
            }

            if( nextPiece < 0 )
                break;

            pi = nextPiece;
        }

        emit( from, cur, ztd, td );
    };

    for( int n : keep )
    {
        for( int pi : at[n] )
        {
            if( !used[pi] )
                walk( n, pi );
        }
    }

    // loops with no kept node on them
    for( size_t pi = 0; pi < pieces.size(); ++pi )
    {
        if( !used[pi] )
        {
            keep.insert( pieces[pi].a );
            walk( pieces[pi].a, (int) pi );
        }
    }

    // a joint made after a line was written: its node names follow the representative
    for( std::string& l : aN.lines )
    {
        std::istringstream in( l );
        std::string        name, na, g0, nb, rest;
        in >> name >> na >> g0 >> nb;
        std::getline( in, rest );
        auto fix = [&]( const std::string& x ) { return aN.tag + std::to_string( aN.rep( std::stoi( x.substr( aN.tag.size() ) ) ) ); };
        l = name + " " + fix( na ) + " 0 " + fix( nb ) + rest;
    }
}


struct MEAS
{
    double vf = 0, max = 0, min = 0, overshoot = 0, ringback = 0, settle = -1, flight = -1;
    int    crossings = 0;
};


/// at a receiver: the final value, overshoot over it, the lowest after the first peak (ringback),
/// the time from the driver's 50% to its own, settling to 5% of the final value, crossings of 50%
MEAS measure( const std::vector<double>& t, const std::vector<double>& v, double aT50drv, double aVdd )
{
    MEAS m;

    if( v.empty() )
        return m;

    m.vf = v.back();
    const double half = aVdd / 2;
    size_t       ipk = 0;

    for( size_t i = 0; i < v.size(); ++i )
    {
        if( v[i] > v[ipk] )
            ipk = i;

        if( i > 0 && ( v[i - 1] < half ) != ( v[i] < half ) )
        {
            if( m.crossings++ == 0 )
            {
                double f = ( half - v[i - 1] ) / ( v[i] - v[i - 1] );
                m.flight = t[i - 1] + f * ( t[i] - t[i - 1] ) - aT50drv;
            }
        }
    }

    m.max = v[ipk];
    m.min = *std::min_element( v.begin() + ipk, v.end() );
    m.overshoot = m.vf > 0 ? ( m.max - m.vf ) / m.vf * 100 : 0;
    m.ringback = m.min;

    const double band = std::max( 0.05 * std::fabs( m.vf ), 1e-3 );

    for( size_t i = v.size(); i-- > 0; )
    {
        if( std::fabs( v[i] - m.vf ) > band )
        {
            m.settle = ( i + 1 < t.size() ? t[i + 1] : t[i] ) - aT50drv;
            break;
        }
    }

    return m;
}


std::string reflectionDescription()
{
    return "A net's edge as routed, simulated (ngspice): every track piece a lossless transmission line with the Z0 and "
           "delay of its layer and width (the impedance tool's cross-section), vias and pads as joints, the driver a "
           "step through its output resistance, each receiver pin a capacitance; a series resistor on the net (driver -> R "
           "-> net) is taken in with the driver's net, resistors to a supply / ground and capacitors to ground on the net "
           "too. Runs as routed and with a series resistor at the driver of the line's Z0 minus rout (E24), plus any rs "
           "[ohm, ...] asked for (it replaces the net's own series resistor). Measures at each receiver, no verdicts: "
           "overshoot over the final value, the ringback after it, settling to 5%, flight time from the driver's 50%, "
           "and how many times it crosses 50% (more than once: not monotonic). net; driver (\"U3\" or \"U3.12\"; default "
           "the net's output pin); rise (10-90%, default 1 ns), rout (ohm, default 30), vdd (default 3.3), cin "
           "(receiver F, default 3p): each said when assumed. plot true: the receivers' waveforms (as routed, and with "
           "the suggested resistor dashed). Not modelled: losses, via barrels, coupling, the packages. Single-ended "
           "nets; a pair's members one at a time.";
}


json reflectionSchema()
{
    json num = { { "type", { "number", "string" } } };
    return { { "type", "object" },
             { "properties",
               { { "net", { { "type", "string" } } },
                 { "driver", { { "type", "string" } } },
                 { "rise", num },
                 { "rout", num },
                 { "vdd", num },
                 { "cin", num },
                 { "rs", { { "type", "array" }, { "items", num } } },
                 { "plot", { { "type", "boolean" } } },
                 { "netlist", { { "type", "boolean" } } } } },
             { "required", { "net" } } };
}


TOOL_RESULT reflection( IPC_CLIENT& aIpc, const json& a )
{
    auto fail = []( const std::string& m ) { return TOOL_RESULT{ m, true }; };

    if( !a.contains( "net" ) || !a["net"].is_string() )
        return fail( "net: the signal (\"CLK\")" );

    const std::string exe = sim::FindNgspice();

    if( exe.empty() )
        return fail( "ngspice not found: put it next to kicadd-mcp (ngspice/bin/), on the PATH, or set KICADD_NGSPICE to it" );

    // the driver's parameters
    std::vector<std::string> assumed;

    auto param = [&]( const char* k, double aDefault, const char* aUnit, double& aOut ) -> bool
    {
        if( !a.contains( k ) )
        {
            aOut = aDefault;
            assumed.push_back( std::string( k ) + " " + si( aDefault, aUnit ) );
            return true;
        }

        return jsonSI( a[k], aOut ) && aOut >= 0;
    };

    double rise = 0, rout = 0, vdd = 0, cin = 0;

    if( !param( "rise", 1e-9, "s", rise ) || rise <= 0 || !param( "rout", 30, "Ω", rout ) || !param( "vdd", 3.3, "V", vdd ) || vdd <= 0
        || !param( "cin", 3e-12, "F", cin ) )
        return fail( "rise (s), rout (ohm), vdd (V), cin (F): numbers or SI strings, above 0" );

    std::vector<double> tryRs;

    if( a.contains( "rs" ) )
    {
        if( !a["rs"].is_array() )
            return fail( "rs: [ohm, ...]" );

        for( const json& v : a["rs"] )
        {
            double r = 0;

            if( !jsonSI( v, r ) || r < 0 )
                return fail( "rs: [ohm, ...]" );

            if( tryRs.size() < 4 )
                tryRs.push_back( r );
        }
    }

    // the net
    kb::NetTopology         req;
    kb::NetTopologyResponse b;
    std::string             err;
    req.set_net( a["net"].get<std::string>() );

    if( !aIpc.Call( req, b, err ) )
        return fail( err );

    if( b.edges_size() == 0 )
        return fail( b.net() + " has no tracks: route it first (reflections are about the routed copper)" );

    // the driver: given, or the output pin; else through a series resistor on the net
    std::string drvRef, drvPin;

    if( a.contains( "driver" ) && a["driver"].is_string() )
    {
        std::string d = a["driver"].get<std::string>();
        size_t      dot = d.find( '.' );
        drvRef = d.substr( 0, dot );
        drvPin = dot == std::string::npos ? "" : d.substr( dot + 1 );
    }

    kb::NetTopologyResponse aside;   // the driver's net behind a series resistor
    bool                    chained = false;
    std::string             seriesRef;
    double                  seriesR = 0;

    auto findDriver = [&]( const kb::NetTopologyResponse& aT, std::vector<const kb::TopoPad*>& aOut )
    {
        aOut.clear();

        for( const kb::TopoPad& p : aT.pads() )
        {
            if( !drvRef.empty() )
            {
                if( p.ref() == drvRef && ( drvPin.empty() || p.pin() == drvPin ) )
                    aOut.push_back( &p );
            }
            else if( p.pads() > 2 && ( p.pin_type() == "output" || p.pin_type() == "tri_state" ) )
            {
                aOut.push_back( &p );
            }
        }
    };

    std::vector<const kb::TopoPad*> drv;
    findDriver( b, drv );

    if( drv.empty() )
    {
        // a series resistor whose other net has the driver
        for( const kb::TopoPad& p : b.pads() )
        {
            if( p.pads() != 2 || prefix( p.ref() ) != "R" || p.other_net().empty() || sim::VoltageFromName( p.other_net() )
                || groundName( p.other_net() ) )
                continue;

            kb::NetTopology         r2;
            kb::NetTopologyResponse t2;
            std::string             e2;
            r2.set_net( p.other_net() );

            if( !aIpc.Call( r2, t2, e2 ) )
                continue;

            std::vector<const kb::TopoPad*> d2;
            findDriver( t2, d2 );

            if( d2.size() == 1 )
            {
                aside = t2;
                chained = true;
                seriesRef = p.ref();

                if( !partValue( "R", p.value(), seriesR ) )
                    return fail( "series resistor " + p.ref() + " \"" + p.value() + "\": no resistance in its value" );

                break;
            }
        }

        if( chained )
            findDriver( aside, drv );
    }

    if( drv.size() != 1 )
    {
        std::string l;
        int         k = 0;

        for( const kb::TopoPad& p : b.pads() )
        {
            if( p.pads() > 2 && k++ < 10 )
                l += ( l.empty() ? "" : ", " ) + p.ref() + "." + p.pin() + ( p.pin_type().empty() ? "" : " " + p.pin_type() );
        }

        if( drv.empty() )
            return fail( ( drvRef.empty() ? "no output pin on " + b.net() + " (nor behind a series resistor)" : "driver " + drvRef + ( drvPin.empty() ? "" : "." + drvPin ) + " is not on " + b.net() )
                         + ": give driver (\"U3.12\"); its pins: " + l );

        std::string c;

        for( const kb::TopoPad* p : drv )
            c += ( c.empty() ? "" : ", " ) + p->ref() + "." + p->pin() + " " + p->pin_type();

        return fail( "more than one driver on " + b.net() + " (" + c + "): give driver (\"U3.12\")" );
    }

    const kb::TopoPad& dp = *drv[0];

    // --- the circuit: the nets' lines, the driver, receivers, terminations
    int  tcount = 0;
    NETG nb, na;
    const double shortLine = std::max( 1e-12, rise / 40 );
    buildNet( nb, b, "b", tcount, shortLine );

    if( chained )
        buildNet( na, aside, "a", tcount, shortLine );

    NETG& drvNet = chained ? na : nb;
    const std::string drvNode = drvNet.node( dp.node() );

    // where the series resistor sits: its pad's node on each net
    std::string seriesA, seriesB;

    if( chained )
    {
        for( const kb::TopoPad& p : b.pads() )
        {
            if( p.ref() == seriesRef )
                seriesB = nb.node( p.node() );
        }

        for( const kb::TopoPad& p : aside.pads() )
        {
            if( p.ref() == seriesRef )
                seriesA = na.node( p.node() );
        }
    }

    struct RX
    {
        std::string name, node, type;
    };

    std::vector<RX>          rx;
    std::vector<std::string> extra, said;
    int                      k = 0;

    for( const kb::TopoPad& p : b.pads() )
    {
        if( !chained && &p == &dp )
            continue;

        if( p.ref() == seriesRef )
            continue;

        const std::string node = nb.node( p.node() );

        if( p.pads() == 2 && !p.other_net().empty() )
        {
            const std::string pre = prefix( p.ref() );
            double            val = 0;
            bool              ok = ( pre == "R" || pre == "C" ) && partValue( pre, p.value(), val );
            const bool        gnd = groundName( p.other_net() );
            const auto        supply = sim::VoltageFromName( p.other_net() );

            if( ( pre == "R" || pre == "C" ) && ok && val > 0 && ( gnd || supply ) )
            {
                std::string farNode = "0";

                if( !gnd && *supply != 0 )
                {
                    farNode = "sup" + std::to_string( k );
                    extra.push_back( "Vsup" + std::to_string( k ) + " " + farNode + " 0 dc " + g( *supply, 5 ) );
                }

                extra.push_back( pre + "kd" + std::to_string( k++ ) + " " + node + " " + farNode + " " + g( val, 6 ) );
                said.push_back( p.ref() + " " + si( val, pre == "R" ? "Ω" : "F" ) + " to " + p.other_net() );
                continue;
            }

            said.push_back( p.ref() + " " + p.value() + " to " + p.other_net() + " left out" );
            continue;
        }

        rx.push_back( { p.ref() + "." + p.pin(), node, p.pin_type() } );
    }

    // the driver net's other pins (behind the series resistor): receivers too
    if( chained )
    {
        for( const kb::TopoPad& p : aside.pads() )
        {
            if( &p == &dp || p.ref() == seriesRef || p.pads() <= 2 )
                continue;

            rx.push_back( { p.ref() + "." + p.pin(), na.node( p.node() ), p.pin_type() } );
        }
    }

    if( rx.empty() )
        return fail( b.net() + ": no receiver (no other part's pin on it)" );

    // the delay from the driver to each receiver: Dijkstra on the lines' TD
    std::map<std::string, std::vector<std::pair<std::string, double>>> adj;

    for( NETG* n : { &nb, &na } )
    {
        if( !n->topo )
            continue;

        for( const kb::TopoEdge& e : n->topo->edges() )
        {
            const double td = e.length() / 10 * e.delay() * 1e-12;
            adj[n->node( e.a() )].push_back( { n->node( e.b() ), td } );
            adj[n->node( e.b() )].push_back( { n->node( e.a() ), td } );
        }
    }

    if( chained )
    {
        adj[seriesA].push_back( { seriesB, 0 } );
        adj[seriesB].push_back( { seriesA, 0 } );
    }

    std::map<std::string, double> dist;
    {
        using QE = std::pair<double, std::string>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        dist[drvNode] = 0;
        q.push( { 0, drvNode } );

        while( !q.empty() )
        {
            auto [d, u] = q.top();
            q.pop();

            if( d > dist[u] )
                continue;

            for( auto [v, w] : adj[u] )
            {
                if( !dist.count( v ) || d + w < dist[v] )
                {
                    dist[v] = d + w;
                    q.push( { d + w, v } );
                }
            }
        }
    }

    std::vector<std::string> offCopper;
    double                   tdMax = 0;

    for( const RX& r : rx )
    {
        if( !dist.count( r.node ) )
            offCopper.push_back( r.name );
        else
            tdMax = std::max( tdMax, dist[r.node] );
    }

    // the line's Z0 seen from the driver: the length-weighted average of the driver's own net
    double zsum = 0, zlen = 0;

    for( auto [layer, len] : nb.layerLen )
    {
        zsum += nb.layerZ[layer];
        zlen += nb.layerZ[layer] > 0 ? len : 0;
    }

    const double z0 = zlen > 0 ? zsum / zlen : 50;
    const double rsSuggested = e24( std::max( 0.0, z0 - rout ) );

    // one run: the driver through rout and aRs (aRs < 0: as routed)
    const double t0 = 0.2e-9;
    const double tstop = std::max( 5e-9, t0 + 2 * rise + 14 * tdMax + 3 * rise );

    auto circuit = [&]( double aRs ) -> std::string
    {
        std::string c = "* KiCadD reflection: " + b.net() + "\n";
        c += "Vdrv kd_src 0 pulse(0 " + g( vdd, 5 ) + " " + g( t0, 5 ) + " " + g( rise / 0.8, 5 ) + " " + g( rise / 0.8, 5 ) + " 1 2)\n";
        // a 10-90% rise is 80% of the ramp
        // chained: the net's own series resistor, or the one tried in its place; else a new one at the driver
        const double rTot = rout + ( !chained && aRs > 0 ? aRs : 0 );
        c += "Rdrv kd_src " + drvNode + " " + g( std::max( rTot, 1e-3 ), 6 ) + "\n";

        if( chained )
            c += "Rseries " + seriesA + " " + seriesB + " " + g( std::max( aRs < 0 ? seriesR : aRs, 1e-3 ), 6 ) + "\n";

        int ci = 0;

        for( NETG* n : { &nb, &na } )
        {
            for( const std::string& l : n->lines )
                c += l + "\n";

            for( auto [node, cap] : n->caps )
                c += "Ckd_line" + std::to_string( ci++ ) + " " + n->node( node ) + " 0 " + g( cap, 5 ) + "\n";
        }

        for( const std::string& l : extra )
            c += l + "\n";

        int i = 0;

        for( const RX& r : rx )
            c += "Ckd_rx" + std::to_string( i++ ) + " " + r.node + " 0 " + g( cin, 5 ) + "\n";

        // every node has a DC path (a node with only lines and a C floats otherwise)
        std::set<std::string> nodes;

        for( auto& [n, list] : adj )
            nodes.insert( n );

        for( const RX& r : rx )
            nodes.insert( r.node );

        for( const std::string& n : nodes )
            c += "Rkd_dc_" + n + " " + n + " 0 1e9\n";

        return c;
    };

    std::vector<std::string> vectors = { "v(" + drvNode + ")" };

    for( const RX& r : rx )
        vectors.push_back( "v(" + r.node + ")" );

    const double step = std::max( 0.2e-12, std::min( { rise / 20, nb.tdMin / 2, na.tdMin / 2 } ) );
    const std::string control = "tran " + g( step, 4 ) + " " + g( tstop, 5 ) + " 0 " + g( step, 4 );

    struct CASE
    {
        std::string label;
        double      rs;   // -1: as routed
        sim::RUN    run;
    };

    std::vector<CASE> cases;
    cases.push_back( { chained ? "as routed (" + seriesRef + " " + si( seriesR, "Ω" ) + ")" : "as routed (no series resistor)", -1, {} } );

    if( !( chained && std::fabs( seriesR - rsSuggested ) < 0.5 ) && !( !chained && rsSuggested == 0 ) )
        cases.push_back( { "with " + si( rsSuggested, "Ω" ) + ( chained ? " for " + seriesRef : " in series at " + dp.ref() + "." + dp.pin() ) + " (Z0 "
                                   + g( z0, 3 ) + " Ω - rout " + g( rout, 3 ) + " Ω, E24)",
                           rsSuggested, {} } );

    for( double r : tryRs )
        cases.push_back( { "with " + si( r, "Ω" ) + ( chained ? " for " + seriesRef : " in series" ) + " (rs)", r, {} } );

    double spent = 0;

    for( CASE& c : cases )
    {
        c.run = sim::RunSpice( exe, circuit( c.rs ), control, vectors, 60 );
        spent += c.run.seconds;

        if( !c.run.ok )
            return fail( "ngspice: " + c.run.error + ( a.value( "netlist", false ) ? "\n" + circuit( c.rs ) : "" ) );
    }

    // --- the answer
    std::string out = "reflection " + b.net() + ": driver " + dp.ref() + "." + dp.pin() + ( dp.pin_type().empty() ? "" : " (" + dp.pin_type() + ")" )
                      + ( chained ? " on " + aside.net() + " through " + seriesRef + " " + si( seriesR, "Ω" ) : "" ) + ", rout " + si( rout, "Ω" )
                      + ", rise " + si( rise, "s" ) + ", " + si( vdd, "V" ) + "; " + std::to_string( rx.size() ) + " receiver(s) at " + si( cin, "F" )
                      + "; ngspice " + g( spent, 2 ) + " s";

    if( !assumed.empty() )
    {
        std::string l;

        for( const std::string& s : assumed )
            l += ( l.empty() ? "" : ", " ) + s;

        out += "\nassumed: " + l + " (give them from the datasheet / IBIS)";
    }

    // the copper
    {
        std::string                   layers;
        std::map<std::string, double> len, zl;

        for( NETG* n : { &na, &nb } )
        {
            for( auto [layer, l] : n->layerLen )
            {
                len[layer] += l;
                zl[layer] += n->layerZ[layer];
            }
        }

        for( auto [layer, l] : len )
            layers += ( layers.empty() ? "" : ", " ) + layer + " " + g( l, 3 ) + " mm" + ( zl[layer] > 0 ? " " + g( zl[layer] / l, 3 ) + " Ω" : "" );

        out += "\ncopper: " + g( nb.copper + na.copper, 4 ) + " mm (" + layers + "), " + std::to_string( b.vias() + ( chained ? aside.vias() : 0 ) )
               + " via(s) as joints" + ( nb.lumped + na.lumped ? ", " + std::to_string( nb.lumped + na.lumped ) + " short stretch(es) (under rise/40) as their capacitance" : "" ) + "; Z0 " + ( nb.zMax > 0 ? g( std::min( nb.zMin, chained ? na.zMin : 1e9 ), 3 ) + ".." + g( std::max( nb.zMax, na.zMax ), 3 ) + " Ω" : std::string( "unknown" ) )
               + "; the farthest receiver " + si( tdMax, "s" ) + " one way: 2·TD " + si( 2 * tdMax, "s" ) + " against the rise " + si( rise, "s" )
               + " (" + g( 2 * tdMax / rise, 2 ) + "x)";
    }

    if( b.unrouted() || ( chained && aside.unrouted() ) )
        out += "\nnote: not fully routed (" + std::to_string( b.unrouted() + ( chained ? aside.unrouted() : 0 ) ) + " connection(s) in the ratsnest): the copper as it is";

    if( !offCopper.empty() )
    {
        std::string l;

        for( const std::string& r : offCopper )
            l += ( l.empty() ? "" : ", " ) + r;

        out += "\nnote: not reached through the copper from the driver (left as they are, a C alone): " + l;
    }

    if( nb.noZ || na.noZ )
        out += "\nnote: " + std::to_string( nb.noZ + na.noZ ) + " piece(s) with no Z0 (no reference plane?) taken as joints";

    for( const std::string& n : b.notes() )
        out += "\nnote: " + n;

    if( !said.empty() )
    {
        std::string l;

        for( const std::string& s : said )
            l += ( l.empty() ? "" : "; " ) + s;

        out += "\non the net: " + l;
    }

    for( const CASE& c : cases )
    {
        out += "\n" + c.label + ":";
        const std::vector<double>& x = c.run.table.cols[0];
        // the source's own 50% (not the driver pin's, which the reflections move)
        const double ref50 = t0 + rise / 0.8 / 2;

        int shown = 0;

        for( size_t i = 0; i < rx.size(); ++i )
        {
            if( i + 2 >= c.run.table.cols.size() || shown >= 12 )
                break;

            const std::vector<double>& v = c.run.table.cols[i + 2];
            MEAS                       m = measure( x, v, ref50, vdd );
            shown++;

            out += "\n  " + rx[i].name + ( rx[i].type.empty() ? "" : " (" + rx[i].type + ")" ) + ": final " + si( m.vf, "V" ) + ", peak "
                   + si( m.max, "V" ) + " (overshoot " + g( std::max( 0.0, m.overshoot ), 2 ) + "%), then down to " + si( m.ringback, "V" )
                   + " (" + g( m.vf > 0 ? m.ringback / m.vf * 100 : 0, 3 ) + "%)"
                   + ( m.flight >= 0 ? ", flight " + si( m.flight, "s" ) : ", never at 50%" )
                   + ( m.settle >= 0 ? ", within 5% after " + si( m.settle, "s" ) : ", within 5% throughout" )
                   + ( m.crossings > 1 ? ", crosses 50% " + std::to_string( m.crossings ) + " times (not monotonic)" : "" );
        }

        if( rx.size() > 12 )
            out += "\n  (" + std::to_string( rx.size() - 12 ) + " more receivers)";
    }

    if( a.value( "netlist", false ) )
        out += "\nnetlist:\n" + circuit( -1 ) + ".control\n" + control + "\n.endc";

    TOOL_RESULT r{ out + "\n", false };

    if( a.value( "plot", false ) )
    {
        plot::CHART chart;
        chart.title = "reflection " + b.net() + " (" + dp.ref() + "." + dp.pin() + ")";
        chart.xUnit = "s";
        plot::PANEL p;
        p.yUnit = "V";

        for( size_t ci = 0; ci < cases.size() && ci < 2; ++ci )
        {
            const CASE& c = cases[ci];

            for( size_t i = 0; i < rx.size() && i < 6 && i + 2 < c.run.table.cols.size(); ++i )
            {
                plot::SERIES s;
                s.label = ci == 0 ? rx[i].name : ( i == 0 ? "with " + si( c.rs, "Ω" ) + " (dashed)" : "" );
                s.x = c.run.table.cols[0];
                s.y = c.run.table.cols[i + 2];
                s.color = (int) i;
                s.dashed = ci > 0;
                p.series.push_back( std::move( s ) );
            }
        }

        p.limit = vdd;
        p.limitLabel = si( vdd, "V" );
        chart.panels.push_back( std::move( p ) );
        r.image = plot::Png( chart );
    }

    return r;
}

} // namespace


TOOL ReflectionTool( IPC_CLIENT& aIpc )
{
    return { "reflection", reflectionDescription(), reflectionSchema(), [&aIpc]( const json& a ) { return reflection( aIpc, a ); } };
}

} // namespace kicadd::mcp
