/*
 * KiCadD: impedance along the line (see kicadd_zprofile.h).
 *
 * Each track segment is cut where the edge of a fill on another copper layer crosses it (the
 * reference-plane check's fill index), and into 1 mm pieces (0.25 mm for a diff pair, for its
 * gap); each piece's middle gives its state: the copper above and below (the nearest layer each
 * way with a fill there, or an unpoured plane taken as solid), and for a pair the gap to the
 * partner. A state's Z is calculated once.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_zprofile.h"
#include "pcb/kicadd_fills.h"
#include "pcb/kicadd_impedance.h"
#include "pcb/kicadd_netnames.h"

#include <board.h>
#include <footprint.h>
#include <pad.h>
#include <netinfo.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_arc.h>
#include <geometry/shape.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace kicadd
{

using namespace kicadd::board;
using netnames::GLOBS;

namespace
{

constexpr double PIECE = 1e6;        // nm: a single track's pieces at most
constexpr double PAIR_PIECE = 0.25e6; // nm: a diff pair's (its gap)
constexpr double BREAKOUT = 0.5e6;    // nm around a net's own pads and vias: left out

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

std::string mm( double aMm, int aDigits = 2 )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.*f", aDigits, aMm );
    std::string s( buf );

    if( s.find( '.' ) != std::string::npos )
    {
        while( s.back() == '0' )
            s.pop_back();

        if( s.back() == '.' )
            s.pop_back();
    }

    return s == "-0" ? "0" : s;
}

std::string at( const VECTOR2D& p )
{
    return "(" + mm( p.x / 1e6 ) + ", " + mm( p.y / 1e6 ) + ")";
}

std::string lname( PCB_LAYER_ID l )
{
    return str( LSET::Name( l ) );
}

void line( BoardAuditResponse& aOut, const char* aSeverity, const std::string& aMsg,
           std::optional<VECTOR2D> aAt = std::nullopt )
{
    BoardAuditFinding* f = aOut.add_findings();
    f->set_category( "impedance" );
    f->set_severity( aSeverity );
    f->set_message( aMsg );

    if( aAt )
    {
        f->mutable_at()->set_x( (int64_t) aAt->x );
        f->mutable_at()->set_y( (int64_t) aAt->y );
    }
}

/// What a piece of track sees.
struct STATE
{
    PCB_LAYER_ID layer = UNDEFINED_LAYER;
    PCB_LAYER_ID above = UNDEFINED_LAYER;   // the copper towards F.Cu (UNDEFINED: none)
    PCB_LAYER_ID below = UNDEFINED_LAYER;
    int          widthUm = 0;
    int          gapUm = -1;                // a pair's gap to its partner (-1: uncoupled, or no pair)

    auto key() const { return std::make_tuple( (int) layer, (int) above, (int) below, widthUm, gapUm ); }
    bool operator<( const STATE& o ) const { return key() < o.key(); }
};

/// The length in one state, and where it first is.
struct SPAN
{
    double   len = 0;   // nm
    VECTOR2D where;
};

/// A net, or a diff pair as one.
struct GROUP
{
    std::string              name;
    std::vector<std::string> members;
    std::vector<int>         codes;
    bool                     pair = false;
    std::map<STATE, SPAN>    spans;
    double                   len = 0;      // nm (a pair: its members' mean)
    double                   offPlane = 0; // nm
};

/// A group's Z over its length, against a reference Z (a target, or its own main one).
struct PROFILE
{
    const STATE* main = nullptr;
    double       mainZ = 0;
    double       off = 0;          // nm off the reference by more than the tolerance
    const STATE* worst = nullptr;  // the furthest off (of the states 0.1 mm or longer)
    double       worstZ = 0;
    const STATE* most = nullptr;   // the longest off
    double       mostZ = 0;
};

struct SEGP
{
    VECTOR2D a, b;
    double   w;
};

double segDist( const VECTOR2D& p, const SEGP& s )
{
    const VECTOR2D v = s.b - s.a;
    const double   l2 = v.x * v.x + v.y * v.y;
    double         t = l2 > 0 ? ( ( p - s.a ).x * v.x + ( p - s.a ).y * v.y ) / l2 : 0;
    t = std::clamp( t, 0.0, 1.0 );
    return ( p - ( s.a + v * t ) ).EuclideanNorm();
}

} // namespace


void CheckImpedance( BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    const double tol = ( aReq.z_tolerance() > 0 ? aReq.z_tolerance() : 10.0 ) / 100.0;
    const double offMm = aReq.z_off_mm() > 0 ? aReq.z_off_mm() : 1.0;

    // ---- the signal nets with tracks, diff pairs grouped (as the crosstalk check)
    std::set<int> zoneNets;

    for( ZONE* z : aBoard.Zones() )
    {
        if( !z->GetIsRuleArea() && z->GetNetCode() > 0 )
            zoneNets.insert( z->GetNetCode() );
    }

    std::map<int, std::string> signal;

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        const int code = t->GetNetCode();

        if( code <= 0 || t->Type() == PCB_VIA_T || signal.count( code ) || zoneNets.count( code ) )
            continue;

        const std::string name = str( t->GetNet()->GetNetname() );

        if( !netnames::SupplyName( name ) )
            signal[code] = name;
    }

    if( signal.empty() )
        return;

    std::map<std::string, int> byName;

    for( const auto& [code, name] : signal )
        byName[name] = code;

    std::vector<GROUP>           groups;
    std::unordered_map<int, int> groupOf;

    for( const auto& [code, name] : signal )
    {
        if( groupOf.count( code ) )
            continue;

        const std::string partner = netnames::PartnerName( name );
        auto              p = partner.empty() ? byName.end() : byName.find( partner );
        GROUP             g;

        if( p != byName.end() && !groupOf.count( p->second ) )
        {
            const bool neg = netnames::NegativeMember( name );
            const std::string& pos = neg ? partner : name;
            const std::string& negName = neg ? name : partner;
            g.name = pos + "/" + negName.back() + " (pair)";
            g.members = { pos, negName };
            g.codes = { byName[pos], byName[negName] };
            g.pair = true;
        }
        else
        {
            g.name = name;
            g.members = { name };
            g.codes = { code };
        }

        groups.push_back( g );

        for( int c : groups.back().codes )
            groupOf[c] = (int) groups.size() - 1;
    }

    // ---- the stack, each layer's nominal planes, the fills
    const LSEQ                       stack = aBoard.GetEnabledLayers().CuStack();
    std::map<PCB_LAYER_ID, int>      idx;
    const std::set<PCB_LAYER_ID>     planes = impedance::Planes( aBoard );

    for( size_t i = 0; i < stack.size(); ++i )
        idx[stack[i]] = (int) i;

    std::map<PCB_LAYER_ID, std::pair<PCB_LAYER_ID, PCB_LAYER_ID>> nominal;   // layer -> (above, below)
    std::map<PCB_LAYER_ID, double>                                hNm;

    for( PCB_LAYER_ID l : stack )
    {
        std::string err;

        if( std::optional<impedance::LAYER_LINE> ln = impedance::Line( aBoard, l, err ) )
        {
            std::pair<PCB_LAYER_ID, PCB_LAYER_ID> n( UNDEFINED_LAYER, UNDEFINED_LAYER );

            for( PCB_LAYER_ID p : ln->planes )
                ( idx[p] < idx[l] ? n.first : n.second ) = p;

            nominal[l] = n;
            hNm[l] = ( ln->line.microstrip ? ln->line.h : std::min( ln->line.h, ln->line.hBelow ) ) * 1e9;
        }
    }

    std::map<PCB_LAYER_ID, fills::LAYER_FILLS>    fill;
    std::vector<std::shared_ptr<SHAPE_POLY_SET>> keep;
    int                                           unfilled = 0;

    for( const ZONE* z : aBoard.Zones() )
    {
        if( z->GetIsRuleArea() || z->GetNetCode() <= 0 )
            continue;

        unfilled += !z->IsFilled();

        for( PCB_LAYER_ID l : z->GetLayerSet().CuStack() )
        {
            if( !idx.count( l ) || !z->HasFilledPolysForLayer( l ) )
                continue;

            std::shared_ptr<SHAPE_POLY_SET> poly = z->GetFilledPolysList( l );

            if( !poly || poly->OutlineCount() == 0 )
                continue;

            keep.push_back( std::make_shared<SHAPE_POLY_SET>( *poly ) );
            keep.back()->Unfracture();
            fill[l].add( z->GetNetCode(), keep.back() );
        }
    }

    // a plane with no fill yet (not poured, or the zones not filled): taken as solid; with no
    // planes found at all, each layer's nominal ones
    std::set<PCB_LAYER_ID> solid;

    for( PCB_LAYER_ID l : stack )
    {
        if( fill.count( l ) )
            continue;

        if( planes.count( l ) )
            solid.insert( l );

        for( const auto& [layer, n] : nominal )
            if( planes.empty() && ( n.first == l || n.second == l ) )
                solid.insert( l );
    }

    // ---- the pieces of each profiled track
    std::map<int, std::map<PCB_LAYER_ID, std::vector<SEGP>>> segsOf;   // net -> layer -> segments

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->Type() == PCB_VIA_T || !groupOf.count( t->GetNetCode() ) || !nominal.count( t->GetLayer() ) )
            continue;

        std::vector<SEGP>& list = segsOf[t->GetNetCode()][t->GetLayer()];

        if( t->Type() == PCB_ARC_T )
        {
            const PCB_ARC*         arc = static_cast<const PCB_ARC*>( t );
            const SHAPE_LINE_CHAIN chain =
                    SHAPE_ARC( arc->GetStart(), arc->GetMid(), arc->GetEnd(), 0 ).ConvertToPolyline( 5000 );

            for( int i = 0; i + 1 < chain.PointCount(); ++i )
                list.push_back( { VECTOR2D( chain.CPoint( i ) ), VECTOR2D( chain.CPoint( i + 1 ) ), (double) t->GetWidth() } );
        }
        else
        {
            list.push_back( { VECTOR2D( t->GetStart() ), VECTOR2D( t->GetEnd() ), (double) t->GetWidth() } );
        }
    }

    auto copperAt = [&]( PCB_LAYER_ID aLayer, const VECTOR2I& p, int aStep ) -> PCB_LAYER_ID
    {
        for( int k = idx[aLayer] + aStep; k >= 0 && k < (int) stack.size(); k += aStep )
        {
            const PCB_LAYER_ID l = stack[k];

            if( solid.count( l ) )
                return l;

            auto f = fill.find( l );

            if( f != fill.end() && f->second.any( p ) )
                return l;
        }

        return UNDEFINED_LAYER;
    };

    // each net's own pads and vias, with 0.5 mm around their copper: the breakout, left out (the
    // pad or via is the discontinuity there, and the antipad around it no gap in the plane). A
    // pad by its shape (a long SMA or backplane pad: 0.5 mm past its narrow side too, not its
    // long half), a via by its round pad.
    struct BREAK
    {
        BOX2I                  box;     // the copper, 0.5 mm out: a quick first test
        std::shared_ptr<SHAPE> shape;   // a pad's copper; null for a via (a circle)
        VECTOR2D               centre;
        double                 radius = 0;
    };

    std::unordered_map<int, std::vector<BREAK>> breakout;   // net -> its pads and vias

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( groupOf.count( pad->GetNetCode() ) )
            {
                BREAK b;
                b.shape = pad->GetEffectiveShape( pad->GetPrincipalLayer() );
                b.box = b.shape->BBox();
                b.box.Inflate( KiROUND( BREAKOUT ) );
                breakout[pad->GetNetCode()].push_back( std::move( b ) );
            }
        }
    }

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->Type() == PCB_VIA_T && groupOf.count( t->GetNetCode() ) )
        {
            BREAK b;
            b.centre = VECTOR2D( t->GetPosition() );
            b.radius = static_cast<PCB_VIA*>( t )->GetWidth( F_Cu ) / 2.0 + BREAKOUT;
            b.box = BOX2I( t->GetPosition(), VECTOR2I( 0, 0 ) );
            b.box.Inflate( KiROUND( b.radius ) );
            breakout[t->GetNetCode()].push_back( std::move( b ) );
        }
    }

    auto inBreakout = [&]( int aNet, const VECTOR2D& p )
    {
        auto list = breakout.find( aNet );

        if( list == breakout.end() )
            return false;

        const VECTOR2I q( KiROUND( p.x ), KiROUND( p.y ) );

        for( const BREAK& b : list->second )
        {
            if( !b.box.Contains( q ) )
                continue;

            if( b.shape ? b.shape->Collide( q, KiROUND( BREAKOUT ) )
                        : ( p - b.centre ).SquaredEuclideanNorm() < b.radius * b.radius )
                return true;
        }

        return false;
    };

    std::vector<std::pair<double, bool>> ts;   // along the segment (0..1), and whether a fill edge

    for( GROUP& g : groups )
    {
        for( size_t m = 0; m < g.codes.size(); ++m )
        {
            auto mine = segsOf.find( g.codes[m] );

            if( mine == segsOf.end() )
                continue;

            for( const auto& [layer, list] : mine->second )
            {
                const std::vector<SEGP>* partner = nullptr;

                if( g.pair )
                {
                    auto o = segsOf.find( g.codes[1 - m] );

                    if( o != segsOf.end() && o->second.count( layer ) )
                        partner = &o->second.at( layer );
                }

                const double uncoupled = std::max( 4.0 * hNm[layer], 0.5e6 );   // nm: past it, twice Z0

                for( const SEGP& s : list )
                {
                    const double len = ( s.b - s.a ).EuclideanNorm();

                    if( len <= 0 )
                        continue;

                    // where fill edges on other layers cross it, and the pieces between
                    ts.assign( { { 0.0, true }, { 1.0, false } } );
                    const double piece = g.pair ? PAIR_PIECE : PIECE;
                    const int    n = std::max( 1, (int) std::ceil( len / std::min( piece, 4.0 * fills::CELL ) ) );

                    for( int k = 1; k < n; ++k )
                        ts.push_back( { (double) k / n, false } );

                    for( const auto& [l, f] : fill )
                    {
                        if( l == layer )
                            continue;

                        for( int k = 0; k < n; ++k )
                        {
                            const VECTOR2D a = s.a + ( s.b - s.a ) * ( (double) k / n );
                            const VECTOR2D b = s.a + ( s.b - s.a ) * ( (double) ( k + 1 ) / n );
                            std::vector<double> sub;
                            f.cuts( VECTOR2I( a ), VECTOR2I( b ), sub );

                            for( double t : sub )
                                ts.push_back( { ( k + t ) / n, true } );
                        }
                    }

                    std::sort( ts.begin(), ts.end() );

                    // the copper over / under changes only at a fill edge
                    PCB_LAYER_ID above = UNDEFINED_LAYER, below = UNDEFINED_LAYER;
                    bool         fresh = true;

                    for( size_t k = 0; k + 1 < ts.size(); ++k )
                    {
                        fresh = fresh || ts[k].second;
                        const double dl = ( ts[k + 1].first - ts[k].first ) * len;

                        if( dl < 1000 )   // under a micrometre
                            continue;

                        const VECTOR2D mid = s.a + ( s.b - s.a ) * ( 0.5 * ( ts[k].first + ts[k + 1].first ) );

                        if( inBreakout( g.codes[m], mid ) )
                        {
                            fresh = true;
                            continue;
                        }

                        if( fresh )
                        {
                            above = copperAt( layer, VECTOR2I( mid ), -1 );
                            below = copperAt( layer, VECTOR2I( mid ), +1 );
                            fresh = false;
                        }

                        STATE st;
                        st.layer = layer;
                        st.above = above;
                        st.below = below;
                        st.widthUm = (int) std::lround( s.w / 1000 );

                        if( partner )
                        {
                            double d = HUGE_VAL;

                            for( const SEGP& o : *partner )
                                d = std::min( d, segDist( mid, o ) - o.w / 2 );

                            const double gap = d - s.w / 2;

                            if( gap > 0 && gap < uncoupled )
                                st.gapUm = 5 * (int) std::lround( gap / 5000 );
                        }

                        SPAN& sp = g.spans[st];

                        if( sp.len == 0 )
                            sp.where = mid;

                        sp.len += dl;
                        g.len += dl;

                        if( st.above == UNDEFINED_LAYER && st.below == UNDEFINED_LAYER )
                            g.offPlane += dl;
                    }
                }
            }
        }

        if( g.pair )
        {
            g.len /= 2;
            g.offPlane /= 2;
        }
    }

    // ---- Z of a state (cached): Z0, or a pair's Zdiff (uncoupled: twice Z0); nullopt off plane
    std::map<std::tuple<int, int, int, int, int, bool>, std::optional<double>> zCache;

    auto zOf = [&]( const STATE& st, bool aDiff ) -> std::optional<double>
    {
        if( st.above == UNDEFINED_LAYER && st.below == UNDEFINED_LAYER )
            return std::nullopt;

        auto key = std::make_tuple( (int) st.layer, (int) st.above, (int) st.below, st.widthUm,
                                    aDiff ? st.gapUm : -2, aDiff );
        auto c = zCache.find( key );

        if( c != zCache.end() )
            return c->second;

        std::optional<double>                 z;
        std::string                           err;
        std::optional<impedance::LAYER_LINE> ln = impedance::LineOver( aBoard, st.layer, st.above, st.below, err );

        if( ln )
        {
            const double w = st.widthUm * 1e-6;

            if( aDiff && st.gapUm > 0 )
            {
                if( zcalc::RESULT r = zcalc::Pair( ln->line, w, st.gapUm * 1e-6 ); r.ok )
                    z = r.z;
            }
            else if( zcalc::RESULT r = zcalc::Single( ln->line, w ); r.ok )
            {
                z = aDiff ? 2 * r.z : r.z;
            }
        }

        zCache[key] = z;
        return z;
    };

    auto profile = [&]( const GROUP& g, bool aDiff, std::optional<double> aTarget ) -> PROFILE
    {
        PROFILE p;
        double  best = 0;

        for( const auto& [st, sp] : g.spans )
        {
            if( sp.len > best && zOf( st, aDiff ) )
            {
                best = sp.len;
                p.main = &st;
                p.mainZ = *zOf( st, aDiff );
            }
        }

        if( !p.main )
            return p;

        const double ref = aTarget ? *aTarget : p.mainZ;
        double       worstDev = 0, mostLen = 0;

        for( const auto& [st, sp] : g.spans )
        {
            std::optional<double> z = zOf( st, aDiff );

            if( !z )
                continue;

            const double dev = std::fabs( *z - ref ) / ref;

            if( dev <= tol )
                continue;

            const double l = g.pair ? sp.len / 2 : sp.len;
            p.off += l;

            if( dev > worstDev && sp.len >= ( g.pair ? 0.5e6 : 0.25e6 ) )
            {
                worstDev = dev;
                p.worst = &st;
                p.worstZ = *z;
            }

            if( l > mostLen )
            {
                mostLen = l;
                p.most = &st;
                p.mostZ = *z;
            }
        }

        // the furthest only when it says more than the longest
        if( p.worst && p.most && std::fabs( p.worstZ - p.mostZ ) < 0.05 * ref )
            p.worst = nullptr;

        return p;
    };

    // a state in words: the layer and width (and gap), and its copper where it is not the layer's planes
    auto describeState = [&]( const STATE& st ) -> std::string
    {
        std::string s = lname( st.layer ) + " " + mm( st.widthUm / 1000.0, 3 ) + " mm";

        if( st.gapUm > 0 )
            s += " gap " + mm( st.gapUm / 1000.0, 3 );

        const auto& n = nominal[st.layer];

        for( int side = 0; side < 2; ++side )
        {
            const PCB_LAYER_ID local = side ? st.below : st.above, nom = side ? n.second : n.first;

            if( local == nom )
                continue;

            if( local == UNDEFINED_LAYER )
                s += std::string( ", no copper " ) + ( side ? "below" : "above" ) + " (a gap in " + lname( nom ) + ")";
            else if( nom != UNDEFINED_LAYER && std::abs( idx[local] - idx[st.layer] ) < std::abs( idx[nom] - idx[st.layer] ) )
                s += ", over " + lname( local ) + " copper (not the " + lname( nom ) + " plane)";
            else
                s += ", over " + lname( local ) + ( nom != UNDEFINED_LAYER ? " (a gap in " + lname( nom ) + ")" : "" );
        }

        return s;
    };

    auto ohm = []( double z ) { return mm( z, 1 ) + " ohm"; };

    auto spanOf = [&]( const GROUP& g, const STATE* st ) { return g.spans.at( *st ); };

    // "4.2 mm at 41.3 ohm (In2.Cu 0.1 mm, over In3.Cu copper ...) at (x, y)"
    auto offPart = [&]( const GROUP& g, const STATE* st, double z ) -> std::string
    {
        const SPAN&  sp = spanOf( g, st );
        const double l = g.pair ? sp.len / 2 : sp.len;
        return mm( l / 1e6, 1 ) + " mm at " + ohm( z ) + " (" + describeState( *st ) + ") at " + at( sp.where );
    };

    // ---- the agent's globs
    GLOBS picked, zTarget, zDiffTarget;

    for( const std::string& g : aReq.z_nets() )
        picked.add( g, 0 );

    for( const auto& [g, v] : aReq.z_target() )
        zTarget.add( g, v );

    for( const auto& [g, v] : aReq.z_diff_target() )
        zDiffTarget.add( g, v );

    auto groupMatches = [&]( const GROUP& g, const GLOBS::GLOB& gl )
    {
        for( const std::string& m : g.members )
            if( GLOBS::matches( gl, m ) )
                return true;

        return false;
    };

    for( const GLOBS::GLOB& gl : picked.list )
    {
        bool any = false;

        for( const GROUP& g : groups )
            any = any || groupMatches( g, gl );

        if( !any )
            line( aOut, "warning", "no signal net with tracks matches " + gl.pattern );
    }

    // ---- the overview: each group against its own main Z
    struct ROW
    {
        const GROUP* g;
        PROFILE      p;
    };

    std::vector<ROW> rows;
    double           offPlane = 0;
    int              offPlaneNets = 0, nets = 0, pairs = 0;

    for( const GROUP& g : groups )
    {
        if( g.len <= 0 )
            continue;

        ( g.pair ? pairs : nets )++;

        if( g.offPlane > 0.1e6 )
        {
            offPlane += g.offPlane;
            offPlaneNets++;
        }

        rows.push_back( { &g, profile( g, g.pair, std::nullopt ) } );
    }

    aOut.set_z_nets( (int) rows.size() );

    // diff pairs first (the controlled ones), each kind the most off first
    std::sort( rows.begin(), rows.end(),
               []( const ROW& a, const ROW& b ) { return a.g->pair != b.g->pair ? a.g->pair : a.p.off > b.p.off; } );

    const double pct = tol * 100;

    auto describeRow = [&]( const ROW& r ) -> std::string
    {
        std::string s = r.g->name + ": " + mm( r.g->len / 1e6, 1 ) + " mm, main " + ( r.g->pair ? "Zdiff " : "Z0 " )
                        + ( r.p.main ? ohm( r.p.mainZ ) + " (" + describeState( *r.p.main ) + ")" : "none (off plane)" );

        if( r.p.most )
        {
            s += "; " + mm( r.p.off / 1e6, 1 ) + " mm " + mm( pct, 0 ) + "% or more off it, the longest " + offPart( *r.g, r.p.most, r.p.mostZ );

            if( r.p.worst )
                s += ", the furthest " + offPart( *r.g, r.p.worst, r.p.worstZ );
        }

        if( r.g->offPlane > 0.1e6 )
            s += "; " + mm( r.g->offPlane / 1e6, 1 ) + " mm off plane";

        return s;
    };

    const bool   judging = !zTarget.list.empty() || !zDiffTarget.list.empty();
    const bool   listing = aReq.z_limit() > 0 || aReq.z_nets_size() > 0;
    const size_t limit = aReq.z_limit() > 0 ? aReq.z_limit() : 10;

    // per kind (diff pairs first, they are the controlled ones): the range of main Z, how many vary
    std::string head = "Z along " + std::to_string( nets ) + " net(s) and " + std::to_string( pairs )
                       + " diff pair(s), from each piece's width, layer and the copper over / under it (0.5 mm "
                         "around a net's own pads and vias left out)";
    const ROW* first = nullptr;

    for( int kind = 1; kind >= 0; --kind )
    {
        double     lo = HUGE_VAL, hi = 0;
        int        varying = 0, count = 0;
        const ROW* most = nullptr;

        for( const ROW& r : rows )
        {
            if( r.g->pair != (bool) kind || !r.p.main )
                continue;

            count++;
            lo = std::min( lo, r.p.mainZ );
            hi = std::max( hi, r.p.mainZ );

            if( r.p.off >= offMm * 1e6 )
            {
                varying++;

                if( !most )
                    most = &r;   // rows: the most off first
            }
        }

        if( !count )
            continue;

        head += std::string( kind ? "; diff pairs: main Zdiff " : "; nets: main Z0 " ) + mm( lo, 0 ) + "-" + mm( hi, 0 )
                + " ohm, " + std::to_string( varying ) + " vary " + mm( pct, 0 ) + "% or more from their own over "
                + mm( offMm, 1 ) + " mm or more";

        if( most && !first )
        {
            first = most;
            head += ", the most: " + describeRow( *most );
        }
    }

    if( offPlaneNets )
        head += ". " + mm( offPlane / 1e6, 1 ) + " mm on " + std::to_string( offPlaneNets )
                + " net(s) with no copper over or under (off plane, no Z: see the plane check)";

    if( !solid.empty() )
    {
        std::string s;

        for( PCB_LAYER_ID l : solid )
            s += ( s.empty() ? "" : ", " ) + lname( l );

        head += ". " + s + " not poured" + ( unfilled ? " or not filled" : "" ) + ": taken as solid planes";
    }

    if( !judging && first )
        head += ". No verdicts: judge them, or pass z_target / z_diff_target {glob: ohm}"
                + std::string( listing ? "" : "; rows: z_limit / z_nets" );

    line( aOut, "info", head,
          first && first->p.most ? std::optional<VECTOR2D>( spanOf( *first->g, first->p.most ).where ) : std::nullopt );

    if( listing && !judging )
    {
        size_t shown = 0, more = 0;

        for( const ROW& r : rows )
        {
            if( !picked.list.empty() )
            {
                bool any = false;

                for( const GLOBS::GLOB& gl : picked.list )
                    any = any || groupMatches( *r.g, gl );

                if( !any )
                    continue;
            }

            if( shown < limit )
            {
                line( aOut, "info", describeRow( r ),
                      r.p.most ? std::optional<VECTOR2D>( spanOf( *r.g, r.p.most ).where ) : std::nullopt );
                shown++;
            }
            else
            {
                more++;
            }
        }

        if( more )
            line( aOut, "info", std::to_string( more ) + " more net(s) (z_limit)" );
    }

    if( !judging )
        return;

    // ---- the targets, rule by rule: z_target on single nets (Z0), z_diff_target on pairs (Zdiff)
    for( int diff = 0; diff < 2; ++diff )
    {
        const GLOBS& rules = diff ? zDiffTarget : zTarget;

        for( const GLOBS::GLOB& rule : rules.list )
        {
            struct OFF
            {
                std::string name;
                PROFILE     p;
                const GROUP* g;
            };

            std::vector<OFF> off;
            int              judged = 0;

            for( const GROUP& g : groups )
            {
                if( g.len <= 0 || ( diff && !g.pair ) )
                    continue;

                if( diff )
                {
                    const GLOBS::GLOB* best = nullptr;

                    for( const std::string& m : g.members )
                        if( const GLOBS::GLOB* gl = rules.match( m ) )
                            best = gl;

                    if( best != &rule )
                        continue;

                    judged++;
                    PROFILE p = profile( g, true, rule.value );

                    if( p.off >= offMm * 1e6 )
                        off.push_back( { g.name, p, &g } );

                    continue;
                }

                if( !g.pair )
                {
                    if( rules.match( g.members[0] ) != &rule )
                        continue;

                    judged++;
                    PROFILE p = profile( g, false, rule.value );

                    if( p.off >= offMm * 1e6 )
                        off.push_back( { g.name, p, &g } );
                }
            }

            if( !judged )
            {
                line( aOut, "warning", std::string( diff ? "z_diff_target " : "z_target " ) + rule.pattern + ": no "
                                               + ( diff ? "diff pair" : "net (not a diff pair member)" ) + " with tracks matches" );
                continue;
            }

            std::sort( off.begin(), off.end(), []( const OFF& a, const OFF& b ) { return a.p.off > b.p.off; } );

            line( aOut, off.empty() ? "info" : "warning",
                  std::string( diff ? "z_diff_target " : "z_target " ) + rule.pattern + " (" + ohm( rule.value ) + " +/-"
                          + mm( pct, 0 ) + "%, " + mm( offMm, 1 ) + " mm off allowed): " + std::to_string( judged )
                          + ( diff ? " pair(s)" : " net(s)" ) + " judged, "
                          + ( off.empty() ? "all within" : std::to_string( off.size() ) + " off" ) );

            for( size_t k = 0; k < off.size() && k < limit; ++k )
            {
                const OFF&  o = off[k];
                // len: the stretches measured (breakouts near the net's own pads and vias left out);
                // off a plane there is no Z, said apart, so the numbers add up to what is seen
                std::string s = o.name + ": " + mm( o.p.off / 1e6, 1 ) + " mm off " + ohm( rule.value ) + " of the "
                                + mm( o.g->len / 1e6, 1 ) + " mm measured (breakouts left out"
                                + ( o.g->offPlane > 0 ? ", " + mm( o.g->offPlane / 1e6, 1 ) + " mm of it off plane, no Z" : std::string() )
                                + ")";

                if( o.p.most )
                    s += ", the longest " + offPart( *o.g, o.p.most, o.p.mostZ );

                if( o.p.worst )
                    s += ", the furthest " + offPart( *o.g, o.p.worst, o.p.worstZ );

                if( o.p.main && o.p.main != o.p.most )
                    s += "; main " + ohm( o.p.mainZ ) + " (" + describeState( *o.p.main ) + ")";

                line( aOut, "warning", s, o.p.most ? std::optional<VECTOR2D>( spanOf( *o.g, o.p.most ).where ) : std::nullopt );
            }

            if( off.size() > limit )
                line( aOut, "warning", std::to_string( off.size() - limit ) + " more (z_limit)" );
        }
    }
}

} // namespace kicadd
