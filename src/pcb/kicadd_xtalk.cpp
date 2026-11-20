/*
 * KiCadD: parallel-track crosstalk pre-screen for the board audit (see kicadd_xtalk.h).
 *
 * How: every signal track (arcs as chords) on a grid per layer; for two segments of different
 * nets within 10 degrees of each other, the stretch of the first where the other runs beside it
 * (projected on it) with an edge-to-edge gap under the limit (spacing x H of that layer). The
 * gap varies linearly along a straight segment, so the stretch is exact. Summed per net pair;
 * a diff pair is one name, and against another net it counts as its member that couples most
 * (no double counting of the two members).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_xtalk.h"
#include "pcb/kicadd_impedance.h"
#include "pcb/kicadd_netnames.h"

#include <board.h>
#include <netinfo.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_arc.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace kicadd
{

using namespace kicadd::board;
using netnames::GLOBS;

namespace
{

constexpr double COS_PARALLEL = 0.98480775;   // 10 degrees
constexpr double MIN_LIST = 2e6;              // nm: shorter couplings are not listed (breakouts, crossings near)
constexpr double MIN_CELL = 1e6;              // nm

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

void line( BoardAuditResponse& aOut, const char* aSeverity, const std::string& aMsg,
           std::optional<VECTOR2D> aAt = std::nullopt )
{
    BoardAuditFinding* f = aOut.add_findings();
    f->set_category( "crosstalk" );
    f->set_severity( aSeverity );
    f->set_message( aMsg );

    if( aAt )
    {
        f->mutable_at()->set_x( (int64_t) aAt->x );
        f->mutable_at()->set_y( (int64_t) aAt->y );
    }
}

struct SEGP
{
    VECTOR2D     a, b;
    double       w;
    int          net;
    PCB_LAYER_ID layer;
};

/// Two nets side by side.
struct COUPLE
{
    double                         len = 0;          // nm
    std::map<PCB_LAYER_ID, double> perLayer;         // nm
    double                         gap = HUGE_VAL;   // nm, the closest edge to edge
    double                         gapH = HUGE_VAL;  // the closest in H of its layer
    PCB_LAYER_ID                   gapLayer = UNDEFINED_LAYER;
    VECTOR2D                       gapAt;
};

/// A net, or a diff pair as one.
struct GROUP
{
    std::string              name;
    std::vector<std::string> members;
};

} // namespace


void CheckCrosstalk( BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    const double spacing = aReq.xtalk_spacing() > 0 ? aReq.xtalk_spacing() : 3.0;
    const std::string sp = mm( spacing, 1 ) + "H";

    // ---- the signal nets with tracks, diff pairs grouped
    std::set<int> zoneNets;

    for( ZONE* z : aBoard.Zones() )
    {
        if( !z->GetIsRuleArea() && z->GetNetCode() > 0 )
            zoneNets.insert( z->GetNetCode() );
    }

    std::map<int, std::string> signal;   // code -> name

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        const int code = t->GetNetCode();

        if( code <= 0 || t->Type() == PCB_VIA_T || signal.count( code ) || zoneNets.count( code ) )
            continue;

        const std::string name = str( t->GetNet()->GetNetname() );

        if( !netnames::SupplyName( name ) )
            signal[code] = name;
    }

    std::map<std::string, int> byName;

    for( const auto& [code, name] : signal )
        byName[name] = code;

    std::vector<GROUP>      groups;
    std::unordered_map<int, int> groupOf;   // net code -> group

    for( const auto& [code, name] : signal )
    {
        if( groupOf.count( code ) )
            continue;

        const std::string partner = netnames::PartnerName( name );
        auto              p = partner.empty() ? byName.end() : byName.find( partner );

        if( p != byName.end() && !groupOf.count( p->second ) )
        {
            const bool         neg = netnames::NegativeMember( name );
            const std::string& pos = neg ? partner : name;
            const std::string& negName = neg ? name : partner;
            groups.push_back( { pos + "/" + negName.back() + " (pair)", { pos, negName } } );
            groupOf[code] = groupOf[p->second] = (int) groups.size() - 1;
        }
        else
        {
            groups.push_back( { name, { name } } );
            groupOf[code] = (int) groups.size() - 1;
        }
    }

    if( signal.empty() )
        return;

    // ---- H per layer (the dielectric to the nearest copper layer) and the limit
    std::map<PCB_LAYER_ID, double> hNm, limitNm;
    std::vector<std::string>       noH;

    for( PCB_LAYER_ID l : aBoard.GetEnabledLayers().CuStack() )
    {
        std::string err;

        if( std::optional<impedance::LAYER_LINE> ln = impedance::Line( aBoard, l, err ) )
        {
            const double h = ln->line.microstrip ? ln->line.h : std::min( ln->line.h, ln->line.hBelow );

            if( h > 0 )
            {
                hNm[l] = h * 1e9;
                limitNm[l] = spacing * h * 1e9;
                continue;
            }
        }

        noH.push_back( str( LSET::Name( l ) ) );
    }

    // ---- the segments, per layer
    std::map<PCB_LAYER_ID, std::vector<SEGP>> segs;

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->Type() == PCB_VIA_T || !signal.count( t->GetNetCode() ) || !limitNm.count( t->GetLayer() ) )
            continue;

        std::vector<SEGP>& list = segs[t->GetLayer()];

        if( t->Type() == PCB_ARC_T )
        {
            const PCB_ARC*         arc = static_cast<const PCB_ARC*>( t );
            const SHAPE_LINE_CHAIN chain = SHAPE_ARC( arc->GetStart(), arc->GetMid(), arc->GetEnd(), 0 ).ConvertToPolyline( 5000 );

            for( int k = 0; k + 1 < chain.PointCount(); k++ )
                list.push_back( { VECTOR2D( chain.CPoint( k ) ), VECTOR2D( chain.CPoint( k + 1 ) ), (double) t->GetWidth(),
                                  t->GetNetCode(), t->GetLayer() } );
        }
        else if( t->GetStart() != t->GetEnd() )
        {
            list.push_back( { VECTOR2D( t->GetStart() ), VECTOR2D( t->GetEnd() ), (double) t->GetWidth(), t->GetNetCode(),
                              t->GetLayer() } );
        }
    }

    // ---- side by side, per net pair (lower code first)
    std::map<std::pair<int, int>, COUPLE> nets;

    for( auto& [layer, list] : segs )
    {
        const double limit = limitNm[layer];
        double       wMax = 0;

        for( const SEGP& s : list )
            wMax = std::max( wMax, s.w );

        const double reach = limit + wMax;
        const double cell = std::max( MIN_CELL, reach );
        std::unordered_map<int64_t, std::vector<int>> grid;

        auto key = []( int64_t x, int64_t y ) { return ( x << 32 ) ^ ( y & 0xffffffff ); };

        for( int i = 0; i < (int) list.size(); i++ )
        {
            const SEGP& s = list[i];
            const int64_t x0 = (int64_t) std::floor( std::min( s.a.x, s.b.x ) / cell );
            const int64_t x1 = (int64_t) std::floor( std::max( s.a.x, s.b.x ) / cell );
            const int64_t y0 = (int64_t) std::floor( std::min( s.a.y, s.b.y ) / cell );
            const int64_t y1 = (int64_t) std::floor( std::max( s.a.y, s.b.y ) / cell );

            for( int64_t y = y0; y <= y1; y++ )
            {
                for( int64_t x = x0; x <= x1; x++ )
                    grid[key( x, y )].push_back( i );
            }
        }

        std::vector<int> seen( list.size(), -1 );

        for( int i = 0; i < (int) list.size(); i++ )
        {
            const SEGP&    A = list[i];
            const VECTOR2D da = A.b - A.a;
            const double   la = da.EuclideanNorm();
            const VECTOR2D u = da / la;
            const int64_t  x0 = (int64_t) std::floor( ( std::min( A.a.x, A.b.x ) - reach ) / cell );
            const int64_t  x1 = (int64_t) std::floor( ( std::max( A.a.x, A.b.x ) + reach ) / cell );
            const int64_t  y0 = (int64_t) std::floor( ( std::min( A.a.y, A.b.y ) - reach ) / cell );
            const int64_t  y1 = (int64_t) std::floor( ( std::max( A.a.y, A.b.y ) + reach ) / cell );

            for( int64_t y = y0; y <= y1; y++ )
            {
                for( int64_t x = x0; x <= x1; x++ )
                {
                    auto g = grid.find( key( x, y ) );

                    if( g == grid.end() )
                        continue;

                    for( int j : g->second )
                    {
                        if( j <= i || seen[j] == i )
                            continue;

                        seen[j] = i;
                        const SEGP& B = list[j];

                        if( B.net == A.net || groupOf[B.net] == groupOf[A.net] )
                            continue;

                        const VECTOR2D db = B.b - B.a;
                        const double   lb = db.EuclideanNorm();

                        if( lb <= 0 || std::fabs( u.Dot( db / lb ) ) < COS_PARALLEL )
                            continue;

                        // B in A's frame: t along A, d across
                        const double t0 = ( B.a - A.a ).Dot( u ), t1 = ( B.b - A.a ).Dot( u );
                        const double d0 = u.Cross( B.a - A.a ), d1 = u.Cross( B.b - A.a );
                        const double lo = std::max( 0.0, std::min( t0, t1 ) ), hi = std::min( la, std::max( t0, t1 ) );

                        if( hi - lo < 1000 || std::fabs( t1 - t0 ) < 1 )
                            continue;

                        auto across = [&]( double t ) { return d0 + ( t - t0 ) * ( d1 - d0 ) / ( t1 - t0 ); };
                        const double dl = across( lo ), dh = across( hi );

                        if( ( dl > 0 ) != ( dh > 0 ) )
                            continue;   // crossing A's line: not side by side

                        const double half = ( A.w + B.w ) / 2;
                        const double gl = std::max( 0.0, std::fabs( dl ) - half ), gh = std::max( 0.0, std::fabs( dh ) - half );

                        if( gl >= limit && gh >= limit )
                            continue;

                        // the part with a gap under the limit (the gap is linear along it)
                        double a = lo, b = hi;

                        if( gl >= limit )
                            a = lo + ( hi - lo ) * ( gl - limit ) / ( gl - gh );
                        else if( gh >= limit )
                            b = lo + ( hi - lo ) * ( limit - gl ) / ( gh - gl );

                        if( b - a <= 0 )
                            continue;

                        const std::pair<int, int> k = std::minmax( A.net, B.net );
                        COUPLE&                   c = nets[k];
                        c.len += b - a;
                        c.perLayer[layer] += b - a;
                        const double gMin = std::min( gl, gh );

                        if( gMin < c.gap )
                        {
                            c.gap = gMin;
                            c.gapH = gMin / hNm[layer];
                            c.gapLayer = layer;
                            c.gapAt = A.a + u * ( gl <= gh ? std::max( a, lo ) : std::min( b, hi ) );
                        }
                    }
                }
            }
        }
    }

    // ---- per group pair: the member pair that couples most
    std::map<std::pair<int, int>, const COUPLE*> pairs;

    for( const auto& [k, c] : nets )
    {
        const std::pair<int, int> g = std::minmax( groupOf[k.first], groupOf[k.second] );
        const COUPLE*&            best = pairs[g];

        if( !best || c.len > best->len )
            best = &c;
    }

    struct ROW
    {
        const GROUP*  a;
        const GROUP*  b;
        const COUPLE* c;
    };

    std::vector<ROW> all, rows;   // all: every coupled pair; rows: the ones listed (MIN_LIST)

    for( const auto& [g, c] : pairs )
        all.push_back( { &groups[g.first], &groups[g.second], c } );

    std::sort( all.begin(), all.end(), []( const ROW& x, const ROW& y ) { return x.c->len > y.c->len; } );

    for( const ROW& r : all )
    {
        if( r.c->len >= MIN_LIST )
            rows.push_back( r );
    }
    aOut.set_xtalk_pairs( (int) rows.size() );

    auto describe = [&]( const ROW& r )
    {
        std::string s = r.a->name + " / " + r.b->name + ": " + mm( r.c->len / 1e6, 1 ) + " mm within " + sp;

        if( r.c->perLayer.size() > 1 )
        {
            std::vector<std::pair<double, PCB_LAYER_ID>> by;

            for( const auto& [l, v] : r.c->perLayer )
                by.emplace_back( v, l );

            std::sort( by.rbegin(), by.rend() );
            s += " (";

            for( size_t k = 0; k < by.size() && k < 3; k++ )
                s += ( k ? ", " : "" ) + str( LSET::Name( by[k].second ) ) + " " + mm( by[k].first / 1e6, 1 );

            s += ")";
        }
        else
        {
            s += " on " + str( LSET::Name( r.c->perLayer.begin()->first ) );
        }

        s += ", closest " + mm( r.c->gap / 1e6, 3 ) + " mm = " + mm( r.c->gapH, 1 ) + "H (" + str( LSET::Name( r.c->gapLayer ) )
             + ", H " + mm( hNm[r.c->gapLayer] / 1e6, 3 ) + ") at " + at( r.c->gapAt );
        return s;
    };

    // ---- the agent's globs
    GLOBS picked, lenLimit;

    for( const std::string& g : aReq.xtalk_nets() )
        picked.add( g, 0 );

    for( const auto& [g, v] : aReq.xtalk_length() )
        lenLimit.add( g, v );

    auto pickedRow = [&]( const ROW& r )
    {
        for( const GROUP* g : { r.a, r.b } )
        {
            for( const std::string& m : g->members )
            {
                for( const GLOBS::GLOB& gl : picked.list )
                {
                    if( GLOBS::matches( gl, m ) )
                        return true;
                }
            }
        }

        return false;
    };

    for( const GLOBS::GLOB& gl : picked.list )
    {
        bool any = false;

        for( const auto& [code, name] : signal )
            any = any || GLOBS::matches( gl, name );

        if( !any )
            line( aOut, "warning", "no signal net with tracks matches " + gl.pattern );
    }

    const bool   judging = !lenLimit.empty();
    const bool   listing = aReq.xtalk_limit() > 0 || aReq.xtalk_nets_size() > 0;
    const size_t limit = aReq.xtalk_limit() > 0 ? aReq.xtalk_limit() : 10;

    // ---- the header
    std::string head = std::to_string( rows.size() ) + " net pair(s) side by side within " + sp + " for "
                       + mm( MIN_LIST / 1e6, 0 ) + " mm or more (same layer, H: the dielectric to the nearest copper layer; "
                       "diff pair partners together and supplies left out)";

    if( !rows.empty() )
        head += "; the longest: " + describe( rows[0] );

    // a layer far from any copper (a 2-layer board: H is the board): 3H takes in most neighbours
    std::vector<std::string> far;
    double                   farH = 0;

    for( const auto& [l, list] : segs )
    {
        if( !list.empty() && hNm[l] > 600000 )
        {
            far.push_back( str( LSET::Name( l ) ) );
            farH = std::max( farH, hNm[l] );
        }
    }

    if( !far.empty() )
    {
        head += ". On " + far[0] + ( far.size() > 1 ? ", " + far[1] : "" ) + ( far.size() > 2 ? " +more" : "" ) + " H is "
                + mm( farH / 1e6 ) + " mm (no copper layer closer: a 2-layer board), so " + sp + " = "
                + mm( spacing * farH / 1e6, 1 ) + " mm takes in most neighbours: read the closest gaps, or xtalk_spacing 1";
    }

    if( !noH.empty() )
    {
        head += ". No H on " + noH[0] + ( noH.size() > 1 ? " +" + std::to_string( noH.size() - 1 ) + " more" : "" )
                + " (no stackup dielectric): not checked";
    }

    if( !judging && !rows.empty() )
        head += ". No verdicts: judge them, or pass xtalk_length {glob: mm}" + std::string( listing ? "" : "; rows: xtalk_limit / xtalk_nets" );

    line( aOut, "info", head, rows.empty() ? std::nullopt : std::optional<VECTOR2D>( rows[0].c->gapAt ) );

    if( listing && !judging )
    {
        size_t shown = 0, more = 0;

        for( const ROW& r : rows )
        {
            if( !picked.empty() && !pickedRow( r ) )
                continue;

            if( shown < limit )
            {
                line( aOut, "info", describe( r ), r.c->gapAt );
                shown++;
            }
            else
            {
                more++;
            }
        }

        if( more )
            line( aOut, "info", std::to_string( more ) + " more pair(s) (xtalk_limit)" );
    }

    if( !judging )
        return;

    // ---- xtalk_length, rule by rule (the tightest first); a pair takes the tightest glob of its nets
    auto ruleOf = [&]( const ROW& r ) -> const GLOBS::GLOB*
    {
        const GLOBS::GLOB* best = nullptr;

        for( const GROUP* g : { r.a, r.b } )
        {
            for( const std::string& m : g->members )
            {
                const GLOBS::GLOB* gl = lenLimit.match( m );

                if( gl && ( !best || gl->value < best->value ) )
                    best = gl;
            }
        }

        return best;
    };

    std::vector<const GLOBS::GLOB*> order;

    for( const GLOBS::GLOB& g : lenLimit.list )
        order.push_back( &g );

    std::sort( order.begin(), order.end(), []( auto* x, auto* y ) { return x->value < y->value; } );

    for( const GLOBS::GLOB* g : order )
    {
        std::vector<const ROW*> over;
        int                     judged = 0;

        for( const ROW& r : all )
        {
            if( ruleOf( r ) != g )
                continue;

            judged++;

            if( r.c->len > g->value * 1e6 )
                over.push_back( &r );
        }

        if( judged == 0 )
            continue;

        line( aOut, over.empty() ? "info" : "warning",
              "xtalk_length " + g->pattern + " (max " + mm( g->value ) + " mm within " + sp + "): " + std::to_string( judged )
                      + " coupled pair(s) judged, " + ( over.empty() ? "all within" : std::to_string( over.size() ) + " over" ) );

        for( size_t k = 0; k < over.size() && k < limit; k++ )
        {
            line( aOut, "warning",
                  describe( *over[k] ) + "; " + mm( over[k]->c->len / 1e6 - g->value, 1 ) + " mm over: space them to "
                          + sp + " or more, or put a ground between",
                  over[k]->c->gapAt );
        }
    }
}

} // namespace kicadd
