/*
 * KiCadD MCP - pdn: a rail's impedance Z(f) at its load (roadmap phase 3, item 3).
 *
 * The board gives each decoupling capacitor's mounting (tracks, vias to the planes, the plane pair
 * to the load: PdnGeometry), the plane pair's capacitance and the load's own vias; the parts index
 * gives the capacitors' C, dielectric and package (ESR / ESL typical when not in the data, MLCC DC
 * bias at the rail's voltage: simulate's model, sim::CapModel / MlccBias). Each capacitor is one
 * branch R + jwL + 1/jwC with L its body's ESL plus its mounting; the branches, the planes and the
 * regulator (R + L, given or assumed) in parallel, the load's vias in series. Worked out here, no
 * ngspice: the network is linear and small.
 *
 * Measures and says where each number comes from; with a target (ohm) the bands above it.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "ipc_client.h"
#include "plot.h"
#include "sim.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>
#include <set>

#include <kicadd/board.pb.h>

namespace kb = kicadd::board;
using json = nlohmann::json;

namespace kicadd::mcp
{

namespace
{

using cplx = std::complex<double>;

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


struct CAP
{
    const kb::PdnCap* geo = nullptr;
    sim::CAP_MODEL    m;
    double            ceff = 0, mount = 0, l = 0, srf = 0, bias = 1;
    bool              mountGiven = false;
};


std::string pdnDescription()
{
    return "A rail's impedance Z(f) at its load, from the board: each capacitor between the rail and its ground as R + L + C "
           "(C from the parts data with the MLCC DC bias at the rail's voltage, ESR / ESL typical when not in the data, "
           "both marked est.), L its body's ESL plus its mounting as placed: tracks from its pads to their vias (Z0 x "
           "delay of the layer), the via loop down to the rail's and the ground's planes, and the plane pair from it to "
           "the load ((mu0 d / 2 pi) ln(D / r)); the plane pair's own capacitance; the regulator as R + L (vrm {r, l}; "
           "2 mΩ + 20 nH assumed and said); the load's own vias in series. Measures, no verdicts: the peaks "
           "(anti-resonances) with the capacitors on each side of them, the lowest |Z|, the capacitors by kind (count, "
           "C_eff, ESR, L body + mount, SRF, nearest / farthest from the load). target (ohm): the bands above it and "
           "the worst point. net (the rail), ground (default: where most of its capacitors go), at (the load, default "
           "the part with most pads on the rail), from / to (Hz, default 10k / 1G), volts (default from the name), "
           "dc_bias false, models {ref: {c, esr, esl, mount (H, replaces the board's), exclude}}, add [{c, esr?, esl?, "
           "mount?, count?, name?}] (capacitors not on the board yet: try a value), detail true (each capacitor), "
           "plot true (|Z| against f, with the body-only line and the target). Estimates: no cavity resonances, no "
           "field solver, the plane pair as one lumped spreading inductance per capacitor.";
}


std::vector<params::PARAM> pdnParams()
{
    using namespace params;
    const std::vector<PARAM> capKeys = { Si( "c", "", "F" ).Positive(), Si( "esr", "", "Ω" ).Range( 0 ), Si( "esl", "", "H" ).Range( 0 ),
                                         Si( "mount", "replaces the board's", "H" ).Range( 0 ), Boolean( "exclude" ), Boolean( "dc_bias" ) };
    std::vector<PARAM>       addKeys = capKeys;
    addKeys[0].Required();
    addKeys.pop_back();   // (no exclude / dc_bias for an added one: it is only data)
    addKeys.pop_back();
    addKeys.push_back( Integer( "count" ).Range( 1, 200 ).Default( 1 ) );
    addKeys.push_back( String( "name" ) );
    addKeys.push_back( String( "package", "\"0402\"" ) );
    addKeys.push_back( String( "dielectric", "\"X5R\"" ) );

    return { String( "net" ).Required(), String( "ground" ), String( "at", "the load's reference" ),
             Si( "from", "", "Hz" ).Positive().Default( "10k" ), Si( "to", "", "Hz" ).Positive().Default( "1G" ),
             Si( "target", "ohm", "Ω" ).Positive(), Number( "volts", "", "V" ),
             Object( "vrm", { Si( "r", "", "Ω" ).Range( 0 ).Default( "2m" ), Si( "l", "", "H" ).Range( 0 ).Default( "20n" ) },
                     "{r: \"2m\", l: \"20n\"}" ).Flat(),
             Boolean( "dc_bias" ), Map( "models", Object( "", capKeys ).Flat() ), Array( "add", Object( "", addKeys ).Flat() ),
             Boolean( "detail" ), Boolean( "plot" ) };
}


TOOL_RESULT pdn( IPC_CLIENT& aIpc, const json& a )
{
    auto fail = []( const std::string& m ) { return TOOL_RESULT{ m, true }; };

    kb::PdnGeometry req;
    req.set_net( a["net"].get<std::string>() );
    req.set_ground( a.value( "ground", std::string() ) );
    req.set_at( a.value( "at", std::string() ) );

    kb::PdnGeometryResponse geo;
    std::string             err;

    if( !aIpc.Call( req, geo, err ) )
        return fail( err );

    double f0 = 10e3, f1 = 1e9, target = 0;

    // (the parameter table checked the values: numbers, in range)
    f0 = a.value( "from", f0 );
    f1 = a.value( "to", f1 );
    target = a.value( "target", 0.0 );

    if( f1 <= f0 * 1.01 )
        return fail( "from < to, Hz" );

    // the rail's voltage (DC bias)
    double      volts = 0;
    std::string voltsWhy;

    if( a.contains( "volts" ) && a["volts"].is_number() )
    {
        volts = std::fabs( a["volts"].get<double>() );
    }
    else if( auto v = sim::VoltageFromName( geo.net() ) )
    {
        volts = std::fabs( *v );
        voltsWhy = " (from its name)";
    }
    else
    {
        voltsWhy = " (not in its name: no DC bias; give volts)";
    }

    const bool biasOn = !a.contains( "dc_bias" ) || !a["dc_bias"].is_boolean() || a["dc_bias"].get<bool>();
    const json models = a.contains( "models" ) && a["models"].is_object() ? a["models"] : json::object();

    for( const auto& [ref, m] : models.items() )
    {
        bool on = std::any_of( geo.caps().begin(), geo.caps().end(), [&]( const kb::PdnCap& c ) { return c.ref() == ref; } );

        if( !on )
            return fail( "models: " + ref + " is not one of " + geo.net() + "'s capacitors to " + geo.ground() );

    }

    // the VRM
    double vr = 2e-3, vl = 20e-9;
    bool   vrmGiven = false;

    if( a.contains( "vrm" ) && a["vrm"].is_object() )
    {
        const json& v = a["vrm"];
        vrmGiven = true;

        vr = v.value( "r", vr );
        vl = v.value( "l", vl );
    }

    // the parts' data
    kb::LcscSearch         lreq;
    kb::LcscSearchResponse lres;
    std::string            indexNote;

    for( const kb::PdnCap& c : geo.caps() )
    {
        if( !c.lcsc().empty() )
            lreq.add_lcsc( c.lcsc() );
    }

    if( lreq.lcsc_size() && !aIpc.Call( lreq, lres, err ) )
        indexNote = "parts index not read (" + err + "): capacitors from their values only";

    std::map<std::string, const kb::LcscHit*> hits;

    for( const kb::LcscHit& h : lres.hits() )
        hits[h.lcsc()] = &h;

    std::vector<CAP>         caps;
    std::vector<std::string> excluded;

    auto finish = [&]( CAP& c, const json& aOver )
    {
        c.ceff = c.m.c;

        if( biasOn && c.m.biasOn && c.m.kind == "MLCC" && c.m.dielectric != "C0G" && volts > 0 )
        {
            c.bias = sim::MlccBias( c.m.dielectric, c.m.c, c.m.package, volts );
            c.ceff = c.m.c * c.bias;
        }

        double mount = 0;

        if( aOver.is_object() && aOver.contains( "mount" ) && jsonSI( aOver["mount"], mount ) )
        {
            c.mount = mount;
            c.mountGiven = true;
        }

        c.l = c.m.esl + c.mount;
        c.srf = c.ceff > 0 && c.l > 0 ? 1 / ( 2 * M_PI * std::sqrt( c.l * c.ceff ) ) : 0;
    };

    for( const kb::PdnCap& gc : geo.caps() )
    {
        const json over = models.contains( gc.ref() ) ? models[gc.ref()] : json::object();

        if( over.value( "exclude", false ) )
        {
            excluded.push_back( gc.ref() );
            continue;
        }

        CAP c;
        c.geo = &gc;
        auto it = hits.find( gc.lcsc() );
        c.m = sim::CapModel( gc.value(), gc.footprint() + " " + gc.value(), "", it == hits.end() ? nullptr : it->second, over );

        if( c.m.c <= 0 )
        {
            excluded.push_back( gc.ref() + " (no capacitance in its value \"" + gc.value() + "\" or the data)" );
            continue;
        }

        c.mount = gc.trace_l() + gc.via_l() + gc.spread_l();
        finish( c, over );
        caps.push_back( std::move( c ) );
    }

    // capacitors to try, not on the board: the mounting given, else the median of the board's
    std::vector<kb::PdnCap> added;   // their names (kept alive for c.geo)
    added.reserve( 64 );

    if( a.contains( "add" ) && a["add"].is_array() )
    {
        std::vector<double> mounts;

        for( const CAP& c : caps )
            mounts.push_back( c.mount );

        std::sort( mounts.begin(), mounts.end() );
        const double median = mounts.empty() ? 1e-9 : mounts[mounts.size() / 2];
        int          k = 0;

        for( const json& x : a["add"] )
        {
            const double cval = x["c"].get<double>();
            const int    count = x.value( "count", 1 );
            const std::string name = x.value( "name", "added " + si( cval, "F" ) );
            std::string text = si( cval, "F" ) + " " + x.value( "package", std::string() ) + " " + x.value( "dielectric", std::string() );

            for( int i = 0; i < count && added.size() < 64; ++i )
            {
                kb::PdnCap& gc = added.emplace_back();
                gc.set_ref( name + ( count > 1 ? "#" + std::to_string( i + 1 ) : "" ) );
                gc.set_value( text );
                gc.set_how( "added" );

                CAP c;
                c.geo = &gc;
                json over = x;
                c.m = sim::CapModel( text, text, "", nullptr, over );
                c.m.c = cval;
                c.mount = median;
                finish( c, over );
                caps.push_back( std::move( c ) );
                k++;
            }
        }
    }

    if( caps.empty() )
    {
        std::string l;

        for( size_t i = 0; i < excluded.size() && i < 6; ++i )
            l += ( l.empty() ? "" : "; " ) + excluded[i];

        return fail( "no capacitor left to work with: " + l + ( excluded.size() > 6 ? " ..." : "" ) );
    }

    // --- Z(f)
    const int    ppd = 60;
    const int    n = std::max( 2, (int) std::ceil( std::log10( f1 / f0 ) * ppd ) + 1 );
    std::vector<double> f( n ), z( n ), zBody( n );
    const double planeC = geo.plane_c();
    const double loadL = geo.load_l();

    auto zAt = [&]( double aF, bool aMounting ) -> cplx
    {
        const double w = 2 * M_PI * aF;
        // the planes with their dielectric's loss (tan δ 0.02): without it the plane / mounting
        // anti-resonance has no damping but the capacitors' ESR
        cplx         y = cplx( w * planeC * 0.02, w * planeC ) + 1.0 / cplx( vr, w * vl );

        for( const CAP& c : caps )
        {
            const double l = aMounting ? c.l : c.m.esl;
            y += 1.0 / cplx( c.m.esr, w * l - 1 / ( w * c.ceff ) );
        }

        return 1.0 / y + cplx( 0, w * ( aMounting ? loadL : 0 ) );
    };

    for( int i = 0; i < n; ++i )
    {
        f[i] = f0 * std::pow( f1 / f0, double( i ) / ( n - 1 ) );
        z[i] = std::abs( zAt( f[i], true ) );
        zBody[i] = std::abs( zAt( f[i], false ) );
    }

    // --- the answer
    std::string out = "pdn " + geo.net() + " at " + geo.at() + " (" + g( geo.at_x(), 5 ) + ", " + g( geo.at_y(), 5 ) + ") to " + geo.ground() + ", "
                      + si( f0, "Hz" ) + " - " + si( f1, "Hz" ) + ": " + std::to_string( caps.size() ) + " capacitor(s)"
                      + ( excluded.empty() ? "" : ", " + std::to_string( excluded.size() ) + " left out" ) + "; "
                      + ( volts > 0 ? g( volts, 3 ) + " V" : std::string( "voltage unknown" ) ) + voltsWhy;

    if( planeC > 0 )
        out += "\nplanes: " + geo.plane_layers() + ", " + g( geo.plane_area(), 4 ) + " mm² at " + g( geo.plane_h(), 3 ) + " mm Er "
               + g( geo.plane_er(), 3 ) + ": " + si( planeC, "F" ) + " (est.: the rail's pour, ε A / d, tan δ 0.02)";

    out += "\nregulator: " + si( vr, "Ω" ) + " + " + si( vl, "H" ) + ( vrmGiven ? " (vrm)" : " (assumed: give vrm {r, l} for its output)" );
    out += "\nload " + geo.at() + ": its vias " + si( loadL, "H" ) + " in series (" + std::to_string( geo.load_vias() ) + " via(s) on the rail, est.)";

    if( !indexNote.empty() )
        out += "\nnote: " + indexNote;

    for( const std::string& note : geo.notes() )
        out += "\nnote: " + note;

    if( !excluded.empty() )
    {
        std::string l;

        for( const std::string& e : excluded )
            l += ( l.empty() ? "" : ", " ) + e;

        out += "\nleft out: " + l;
    }

    // peaks: local maxima standing 1.5x over the higher of the valleys beside them
    struct PEAK
    {
        int    i;
        double prom;
    };

    std::vector<int> maxima;

    for( int i = 1; i + 1 < n; ++i )
    {
        if( z[i] >= z[i - 1] && z[i] > z[i + 1] )
            maxima.push_back( i );
    }

    std::vector<PEAK> peaks;

    for( size_t k = 0; k < maxima.size(); ++k )
    {
        const int lo = k == 0 ? 0 : maxima[k - 1], hi = k + 1 < maxima.size() ? maxima[k + 1] : n - 1;
        const double left = *std::min_element( z.begin() + lo, z.begin() + maxima[k] + 1 );
        const double right = *std::min_element( z.begin() + maxima[k], z.begin() + hi + 1 );
        const double prom = z[maxima[k]] / std::max( left, right );

        if( prom >= 1.5 )
            peaks.push_back( { maxima[k], prom } );
    }

    std::sort( peaks.begin(), peaks.end(), [&]( const PEAK& x, const PEAK& y ) { return z[x.i] > z[y.i]; } );

    // groups: the same value, kind, package and dielectric
    struct GROUP
    {
        std::string         key;
        std::vector<CAP*>   members;
    };

    std::vector<GROUP> groups;

    for( CAP& c : caps )
    {
        std::string key = si( c.m.c, "F" ) + " " + c.m.kind + ( c.m.package.empty() ? "" : " " + c.m.package )
                          + ( c.m.kind == "MLCC" ? " " + c.m.dielectric : "" ) + ( c.geo->how() == "added" ? " (added)" : "" );
        auto it = std::find_if( groups.begin(), groups.end(), [&]( const GROUP& x ) { return x.key == key; } );

        if( it == groups.end() )
        {
            groups.push_back( { key, {} } );
            it = groups.end() - 1;
        }

        it->members.push_back( &c );
    }

    auto srfOf = []( const GROUP& x )
    {
        double s = 0;

        for( const CAP* c : x.members )
            s += c->srf;

        return s / x.members.size();
    };

    std::sort( groups.begin(), groups.end(), [&]( const GROUP& x, const GROUP& y ) { return srfOf( x ) < srfOf( y ); } );

    const int    iMin = (int) ( std::min_element( z.begin(), z.end() ) - z.begin() );
    out += "\n|Z|: " + si( z.front(), "Ω" ) + " at " + si( f.front(), "Hz" ) + ", lowest " + si( z[iMin], "Ω" ) + " at " + si( f[iMin], "Hz" )
           + ", " + si( z.back(), "Ω" ) + " at " + si( f.back(), "Hz" );

    if( peaks.empty() )
    {
        out += "\npeaks: none (no anti-resonance standing 1.5x over its valleys)";
    }
    else
    {
        out += "\npeaks (anti-resonances, highest first):";

        for( size_t k = 0; k < peaks.size() && k < 6; ++k )
        {
            const double fp = f[peaks[k].i];
            const GROUP* below = nullptr;
            const GROUP* above = nullptr;

            for( const GROUP& x : groups )
            {
                if( srfOf( x ) <= fp )
                    below = &x;
                else if( !above )
                    above = &x;
            }

            std::string between;

            if( below && above )
                between = "between " + below->key + " (SRF " + si( srfOf( *below ), "Hz" ) + ") and " + above->key + " (SRF "
                          + si( srfOf( *above ), "Hz" ) + ")";
            else if( below )
                between = "above every capacitor's SRF: " + below->key + " (" + si( srfOf( *below ), "Hz" ) + ") with "
                          + ( planeC > 0 ? "the planes" : "the load's vias" );
            else if( above )
                between = "below every SRF: the regulator's L with " + above->key;

            const double pr = peaks[k].prom;
            out += "\n  " + si( z[peaks[k].i], "Ω" ) + " at " + si( fp, "Hz" ) + " (" + ( pr >= 100 ? std::to_string( (long) std::lround( pr ) ) : g( pr, 2 ) )
                   + "x its valleys), " + between + ( !above && below && planeC > 0 ? " (lumped planes: its height is rough)" : "" );
        }

        if( peaks.size() > 6 )
            out += "\n  (" + std::to_string( peaks.size() - 6 ) + " more)";
    }

    if( z.back() > z[n - 2] * 1.001 )
        out += "\nat " + si( f.back(), "Hz" ) + " rising, inductive: " + si( z.back() / ( 2 * M_PI * f.back() ), "H" )
               + " (the mounting and the load's vias; with the capacitors' bodies alone " + si( zBody.back(), "Ω" ) + ")";

    if( target > 0 )
    {
        std::string bands;
        int         nb = 0;
        int         worst = -1;

        for( int i = 0; i < n; )
        {
            if( z[i] <= target )
            {
                i++;
                continue;
            }

            int j = i;

            while( j + 1 < n && z[j + 1] > target )
                j++;

            int top = (int) ( std::max_element( z.begin() + i, z.begin() + j + 1 ) - z.begin() );

            if( worst < 0 || z[top] > z[worst] )
                worst = top;

            if( nb++ < 6 )
                bands += "\n  " + si( f[i], "Hz" ) + " - " + si( f[j], "Hz" ) + ( j == n - 1 ? " (to the end)" : "" ) + ": up to "
                         + si( z[top], "Ω" ) + " at " + si( f[top], "Hz" ) + " (" + g( z[top] / target, 3 ) + "x)";

            i = j + 1;
        }

        out += "\ntarget " + si( target, "Ω" ) + ": " + ( nb == 0 ? "|Z| under it over the whole range" : std::to_string( nb ) + " band(s) above it" )
               + bands + ( nb > 6 ? "\n  (" + std::to_string( nb - 6 ) + " more)" : "" );
    }

    out += "\ncapacitors (by SRF; L = body + mount: tracks, vias, planes to the load; est. where not in the data):";

    for( const GROUP& x : groups )
    {
        double lo = 1e9, hi = 0, dmin = 1e9, dmax = 0, ceff = 0, esr = 0, esl = 0, tr = 0, vi = 0, sp = 0;
        std::string nearest;

        for( const CAP* c : x.members )
        {
            lo = std::min( lo, c->mount );
            hi = std::max( hi, c->mount );
            ceff += c->ceff;
            esr += c->m.esr;
            esl += c->m.esl;
            tr += c->geo->trace_l();
            vi += c->geo->via_l();
            sp += c->geo->spread_l();

            if( c->geo->how() != "added" && c->geo->distance() < dmin )
            {
                dmin = c->geo->distance();
                nearest = c->geo->ref();
            }

            if( c->geo->how() != "added" )
                dmax = std::max( dmax, c->geo->distance() );
        }

        const double k = (double) x.members.size();
        const CAP*   first = x.members.front();
        std::string  line = "\n  " + std::to_string( x.members.size() ) + "x " + x.key + ": C_eff " + si( ceff / k, "F" );

        if( first->bias < 0.999 )
            line += " (est. DC bias " + g( first->bias * 100, 2 ) + "% at " + g( volts, 3 ) + " V)";

        line += ", ESR " + std::string( first->m.estimated ? "~" : "" ) + si( esr / k, "Ω", 2 ) + ", L " + si( esl / k, "H", 2 ) + " + "
                + ( std::fabs( hi - lo ) < 0.05e-9 ? si( hi, "H", 2 ) : si( lo, "H", 2 ) + ".." + si( hi, "H", 2 ) )
                + ( first->mountGiven ? " (models)" : " (tracks " + si( tr / k, "H", 2 ) + ", vias " + si( vi / k, "H", 2 ) + ", planes " + si( sp / k, "H", 2 ) + ")" )
                + ", SRF " + si( srfOf( x ), "Hz" );

        if( !nearest.empty() )
            line += "; " + g( dmin, 3 ) + ( x.members.size() > 1 ? ".." + g( dmax, 3 ) : "" ) + " mm from " + geo.at() + " (" + nearest + " nearest)";

        out += line;

        if( a.value( "detail", false ) )
        {
            for( const CAP* c : x.members )
                out += "\n    " + c->geo->ref() + " " + c->geo->value() + ": mount " + si( c->mount, "H", 2 )
                       + ( c->geo->how() == "added" ? " (added: the board's median)" : ", " + g( c->geo->distance(), 3 ) + " mm, " + c->geo->how() );
        }
    }

    if( geo.others_size() )
    {
        std::string l;

        for( const std::string& o : geo.others() )
            l += ( l.empty() ? "" : "; " ) + o;

        out += "\nother two-pad parts on " + geo.net() + " (not modelled): " + l;
    }

    TOOL_RESULT r{ out + "\n", false };

    if( a.value( "plot", false ) || plot::HasSink() )
    {
        plot::CHART chart;
        chart.title = "|Z| of " + geo.net() + " at " + geo.at();
        chart.xUnit = "Hz";
        chart.logX = true;
        plot::PANEL p;
        p.yUnit = "Ω";
        p.logY = true;
        p.series.push_back( { "|Z| as mounted", f, z, 0 } );
        p.series.push_back( { "capacitors' body only", f, zBody, 1, true, true } );

        if( target > 0 )
        {
            p.limit = target;
            p.limitLabel = "target " + si( target, "Ω" );
        }

        chart.panels.push_back( std::move( p ) );
        plot::Publish( chart, "pdn" );

        if( a.value( "plot", false ) )
            r.image = plot::Png( chart );
    }

    return r;
}

} // namespace


TOOL PdnTool( IPC_CLIENT& aIpc )
{
    return { "pdn", pdnDescription(), json::object(), [&aIpc]( const json& a ) { return pdn( aIpc, a ); }, "", pdnParams() };
}

} // namespace kicadd::mcp
