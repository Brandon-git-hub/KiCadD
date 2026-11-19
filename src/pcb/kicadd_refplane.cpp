/*
 * KiCadD: reference-plane checks for the board audit (see kicadd_refplane.h).
 *
 * How: the filled zones of each copper layer are indexed (their edges in a 1 mm grid). The tracks
 * of each checked net are joined into chains per layer (broken at vias, branches and ends); along a
 * chain the edges of the reference layers' fills cut it into runs, and each run's reference is the
 * set of (layer, net) fills under its middle. Runs with no fill under them are voids, splits (when
 * the planes on either side share nothing) or stretches past the plane's edge.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_refplane.h"

#include <board.h>
#include <footprint.h>
#include <lset.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_arc.h>
#include <geometry/shape_poly_set.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <unordered_map>

namespace kicadd
{

using namespace kicadd::board;

namespace
{

constexpr int CELL = 1000000;   // nm, the fill-edge grid

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

    return s;
}

std::string at( const VECTOR2I& p )
{
    return "(" + mm( p.x / 1e6 ) + ", " + mm( p.y / 1e6 ) + ")";
}

bool sameNet( const std::string& aNet, const std::string& aWanted )
{
    if( aNet == aWanted || aNet == "/" + aWanted )
        return true;

    return aNet.size() > aWanted.size()
           && aNet.compare( aNet.size() - aWanted.size() - 1, aWanted.size() + 1, "/" + aWanted ) == 0;
}

/// Named like a supply or ground: +5V, 3V3, GND, AGND, VCC_IO, VDD, VBUS, VBAT ...
bool supplyName( const std::string& aNet )
{
    static const std::regex re( R"(^([+-]?\d+(\.\d+)?V\d*|[+-]?\d+(\.\d+)?V\d*_.*|[PN]\d+V\d*(_.*)?|.*GND.*|VCC.*|VDD.*|VSS.*|VEE.*|VBUS.*|VBAT.*|VIN|VSYS.*|.*PWR.*)$)",
                                std::regex::icase );
    const size_t      slash = aNet.rfind( '/' );
    return std::regex_match( slash == std::string::npos ? aNet : aNet.substr( slash + 1 ), re );
}

using REF = std::pair<int, int>;   // (layer, net code)
using REFS = std::vector<REF>;     // sorted

bool disjoint( const REFS& a, const REFS& b )
{
    for( const REF& r : a )
    {
        if( std::binary_search( b.begin(), b.end(), r ) )
            return false;
    }

    return true;
}

/// No plane net in common (the same net on another layer is in common: the return current
/// changes layers through the stitching vias, not across a split).
bool noNetInCommon( const REFS& a, const REFS& b )
{
    for( const REF& x : a )
    {
        for( const REF& y : b )
        {
            if( x.second == y.second )
                return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------- the fills

struct FILL
{
    int                   net;
    const SHAPE_POLY_SET* poly;
    BOX2I                 box;
};

struct LAYER_FILLS
{
    std::vector<FILL>                               fills;
    std::vector<SEG>                                edges;
    std::unordered_map<int64_t, std::vector<int>>   grid;
    mutable std::vector<int>                        stamp;
    mutable int                                     query = 0;

    static int64_t key( int cx, int cy ) { return ( int64_t( cx ) << 32 ) ^ uint32_t( cy ); }

    /// aPoly: a fill, unfractured (so Contains rejects each hole by its box: a fractured plane is
    /// one outline with every antipad on it).
    void add( int aNet, const std::shared_ptr<SHAPE_POLY_SET>& aPoly )
    {
        aPoly->BuildBBoxCaches();
        fills.push_back( { aNet, aPoly.get(), aPoly->BBox() } );

        for( auto it = aPoly->CIterateSegmentsWithHoles(); it; it++ )
        {
            const SEG s = *it;
            const int i = (int) edges.size();
            edges.push_back( s );

            const int x0 = std::min( s.A.x, s.B.x ) / CELL, x1 = std::max( s.A.x, s.B.x ) / CELL;
            const int y0 = std::min( s.A.y, s.B.y ) / CELL, y1 = std::max( s.A.y, s.B.y ) / CELL;

            for( int cx = x0; cx <= x1; cx++ )
                for( int cy = y0; cy <= y1; cy++ )
                    grid[key( cx, cy )].push_back( i );
        }
    }

    /// Where along a→b (0..1) the fill edges cross it; a→b no longer than a few cells.
    void cuts( const VECTOR2I& a, const VECTOR2I& b, std::vector<double>& aT ) const
    {
        stamp.resize( edges.size(), 0 );
        query++;

        const SEG    me( a, b );
        const double len2 = (double) me.SquaredLength();
        const int    x0 = std::min( a.x, b.x ) / CELL, x1 = std::max( a.x, b.x ) / CELL;
        const int    y0 = std::min( a.y, b.y ) / CELL, y1 = std::max( a.y, b.y ) / CELL;

        for( int cx = x0; cx <= x1; cx++ )
        {
            for( int cy = y0; cy <= y1; cy++ )
            {
                auto cell = grid.find( key( cx, cy ) );

                if( cell == grid.end() )
                    continue;

                for( int i : cell->second )
                {
                    if( stamp[i] == query )
                        continue;

                    stamp[i] = query;

                    if( OPT_VECTOR2I x = me.Intersect( edges[i] ) )
                    {
                        VECTOR2D d = VECTOR2D( *x - a ), v = VECTOR2D( b - a );
                        aT.push_back( std::clamp( ( d.x * v.x + d.y * v.y ) / len2, 0.0, 1.0 ) );
                    }
                }
            }
        }
    }

    /// Inside the outer edge of a fill of aNet: a hole in it is a gap, not the plane's edge.
    bool inOuter( int aNet, const VECTOR2I& p ) const
    {
        for( const FILL& f : fills )
        {
            if( f.net != aNet || !f.box.Contains( p ) )
                continue;

            for( int i = 0; i < f.poly->OutlineCount(); i++ )
            {
                const SHAPE_LINE_CHAIN& o = f.poly->COutline( i );

                if( o.GetCachedBBox()->Contains( p ) && o.PointInside( p, 0, true ) )
                    return true;
            }
        }

        return false;
    }

    void refsAt( int aLayer, const VECTOR2I& p, REFS& aOut ) const
    {
        for( const FILL& f : fills )
        {
            if( f.box.Contains( p ) && f.poly->Contains( p, -1, 0, true ) )
                aOut.push_back( { aLayer, f.net } );
        }
    }
};

// ---------------------------------------------------------------------------- the chains

struct RUN
{
    double   from = 0, to = 0;   // nm along the chain
    VECTOR2I a, b;
    REFS     refs;
};

struct CHAIN
{
    int                   net = 0;
    PCB_LAYER_ID          layer = UNDEFINED_LAYER;
    std::vector<VECTOR2I> pts;
    std::vector<RUN>      runs;
    double                length = 0;
};

/// The point aDist nm along the chain.
VECTOR2I pointAt( const CHAIN& aChain, double aDist )
{
    for( size_t k = 0; k + 1 < aChain.pts.size(); k++ )
    {
        const VECTOR2I p = aChain.pts[k], q = aChain.pts[k + 1];
        const double   len = ( q - p ).EuclideanNorm();

        if( aDist <= len || k + 2 == aChain.pts.size() )
            return p + VECTOR2I( VECTOR2D( q - p ) * ( len > 0 ? std::min( 1.0, aDist / len ) : 0.0 ) );

        aDist -= len;
    }

    return aChain.pts.front();
}

using NODE = std::pair<int, int>;

NODE node( const VECTOR2I& p )
{
    return { p.x, p.y };
}

/// The tracks of one net on one layer as chains broken at vias, branches and ends.
void buildChains( const std::vector<std::vector<VECTOR2I>>& aPieces, const std::set<NODE>& aBreaks, int aNet,
                  PCB_LAYER_ID aLayer, std::vector<CHAIN>& aOut )
{
    std::map<NODE, std::vector<int>> nodes;

    for( int i = 0; i < (int) aPieces.size(); i++ )
    {
        nodes[node( aPieces[i].front() )].push_back( i );
        nodes[node( aPieces[i].back() )].push_back( i );
    }

    std::vector<bool> used( aPieces.size(), false );

    auto through = [&]( const NODE& n )
    {
        auto it = nodes.find( n );
        return it != nodes.end() && it->second.size() == 2 && !aBreaks.count( n );
    };

    auto walk = [&]( int aFirst, const NODE& aStart )
    {
        CHAIN c;
        c.net = aNet;
        c.layer = aLayer;
        NODE here = aStart;
        int  piece = aFirst;

        while( piece >= 0 && !used[piece] )
        {
            used[piece] = true;
            const std::vector<VECTOR2I>& p = aPieces[piece];
            const bool                   fwd = node( p.front() ) == here;

            for( size_t k = c.pts.empty() ? 0 : 1; k < p.size(); k++ )
                c.pts.push_back( fwd ? p[k] : p[p.size() - 1 - k] );

            here = node( c.pts.back() );
            piece = -1;

            if( through( here ) )
            {
                for( int next : nodes[here] )
                {
                    if( !used[next] )
                        piece = next;
                }
            }
        }

        if( c.pts.size() >= 2 )
            aOut.push_back( std::move( c ) );
    };

    for( const auto& [n, pieces] : nodes )
    {
        if( through( n ) )
            continue;

        for( int i : pieces )
        {
            if( !used[i] )
                walk( i, n );
        }
    }

    for( int i = 0; i < (int) aPieces.size(); i++ )   // loops
    {
        if( !used[i] )
            walk( i, node( aPieces[i].front() ) );
    }
}

/// Cut the chain into runs by what is under it on aPlanes.
void classify( CHAIN& aChain, const std::vector<std::pair<int, const LAYER_FILLS*>>& aPlanes )
{
    double base = 0;

    for( size_t k = 0; k + 1 < aChain.pts.size(); k++ )
    {
        const VECTOR2I p = aChain.pts[k], q = aChain.pts[k + 1];
        const double   len = ( q - p ).EuclideanNorm();

        if( len < 1 )
            continue;

        // pieces of at most 4 cells, so the grid query stays small
        const int pieces = std::max( 1, (int) std::ceil( len / ( 4.0 * CELL ) ) );

        for( int j = 0; j < pieces; j++ )
        {
            // in doubles: ( q - p ) * j overflows int on long tracks
            const VECTOR2I a = p + VECTOR2I( VECTOR2D( q - p ) * ( double( j ) / pieces ) );
            const VECTOR2I b = j + 1 == pieces ? q : p + VECTOR2I( VECTOR2D( q - p ) * ( double( j + 1 ) / pieces ) );
            const double   plen = ( b - a ).EuclideanNorm();
            std::vector<double> ts = { 0.0, 1.0 };

            for( const auto& [layer, fills] : aPlanes )
                fills->cuts( a, b, ts );

            std::sort( ts.begin(), ts.end() );

            for( size_t i = 0; i + 1 < ts.size(); i++ )
            {
                if( ( ts[i + 1] - ts[i] ) * plen < 1000 )   // under a micrometre
                    continue;

                const VECTOR2I s = a + VECTOR2I( VECTOR2D( b - a ) * ts[i] );
                const VECTOR2I e = a + VECTOR2I( VECTOR2D( b - a ) * ts[i + 1] );
                const VECTOR2I mid = a + VECTOR2I( VECTOR2D( b - a ) * ( ( ts[i] + ts[i + 1] ) / 2 ) );
                REFS           refs;

                for( const auto& [layer, fills] : aPlanes )
                    fills->refsAt( layer, mid, refs );

                std::sort( refs.begin(), refs.end() );
                refs.erase( std::unique( refs.begin(), refs.end() ), refs.end() );

                const double from = base + ts[i] * plen, to = base + ts[i + 1] * plen;

                if( !aChain.runs.empty() && aChain.runs.back().refs == refs )
                {
                    aChain.runs.back().to = to;
                    aChain.runs.back().b = e;
                }
                else
                {
                    aChain.runs.push_back( { from, to, s, e, refs } );
                }
            }

            base += plen;
        }
    }

    aChain.length = base;
}

/// The planes under the chain next to its end (front or back): from the first stretch over a plane
/// within aReach nm of the end, all planes in the 1.5 mm after it (one plane's antipad around the via
/// can be wider than another's).
REFS endRefs( const CHAIN& aChain, bool aFront, double aReach )
{
    REFS   out;
    double first = -1;

    for( size_t k = 0; k < aChain.runs.size(); k++ )
    {
        const RUN&   r = aChain.runs[aFront ? k : aChain.runs.size() - 1 - k];
        const double d = aFront ? r.from : aChain.length - r.to;

        if( d > ( first < 0 ? aReach : first + 1.5e6 ) )
            break;

        if( r.refs.empty() )
            continue;

        if( first < 0 )
            first = d;

        out.insert( out.end(), r.refs.begin(), r.refs.end() );
    }

    std::sort( out.begin(), out.end() );
    out.erase( std::unique( out.begin(), out.end() ), out.end() );
    return out;
}

// ---------------------------------------------------------------------------- findings

struct ISSUE
{
    ISSUE() = default;
    ISSUE( std::string aText, double aSize, VECTOR2I aWhere, std::vector<std::string> aRefs = {} ) :
            text( std::move( aText ) ), size( aSize ), where( aWhere ), refs( std::move( aRefs ) )
    {
    }

    std::string text;
    double      size = 0;   // sorts within a kind: longer / farther first
    VECTOR2I    where;
    std::vector<std::string> refs;
    std::string group;   // vias changing plane nets: the two plane nets
    int         net = 0;
    bool        fast = false;   // a diff pair member or named like a fast signal: listed first
};

void emitList( BoardAuditResponse& aOut, const char* aCat, const char* aSeverity, std::vector<ISSUE>& aList,
               const std::string& aMore, size_t aLimit )
{
    std::stable_sort( aList.begin(), aList.end(),
                      []( const ISSUE& a, const ISSUE& b ) { return a.fast != b.fast ? a.fast : a.size > b.size; } );

    for( size_t i = 0; i < aList.size() && i < aLimit; i++ )
    {
        BoardAuditFinding* f = aOut.add_findings();
        f->set_category( aCat );
        f->set_severity( aSeverity );
        f->set_message( aList[i].text + ( aList[i].fast ? " [fast]" : "" ) );
        f->mutable_at()->set_x( aList[i].where.x );
        f->mutable_at()->set_y( aList[i].where.y );

        for( const std::string& r : aList[i].refs )
            f->add_refs( r );
    }

    if( aList.size() > aLimit )
    {
        BoardAuditFinding* f = aOut.add_findings();
        f->set_category( aCat );
        f->set_severity( aSeverity );
        f->set_message( std::to_string( aList.size() - aLimit ) + " more " + aMore );
    }
}

/// Fast nets (and nets asked for by name) as warnings, up to 20; the rest as notes, up to 10.
void emit( BoardAuditResponse& aOut, const char* aCat, std::vector<ISSUE>& aList, const std::string& aMore,
           bool aAllWarnings )
{
    std::vector<ISSUE> warn, info;

    for( ISSUE& i : aList )
        ( aAllWarnings || i.fast ? warn : info ).push_back( std::move( i ) );

    emitList( aOut, aCat, "warning", warn, aMore, 20 );
    emitList( aOut, aCat, "info", info, aMore + ( warn.empty() ? "" : " on slower nets" ), 10 );
}

void note( BoardAuditResponse& aOut, const char* aCat, const std::string& aMsg )
{
    BoardAuditFinding* f = aOut.add_findings();
    f->set_category( aCat );
    f->set_severity( "info" );
    f->set_message( aMsg );
}

} // namespace


void CheckReferencePlanes( const BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    const double gap = ( aReq.plane_gap_mm() > 0 ? aReq.plane_gap_mm() : 1.0 ) * 1e6;
    const double reach = ( aReq.return_via_mm() > 0 ? aReq.return_via_mm() : 2.0 ) * 1e6;
    const LSEQ   stack = aBoard.GetEnabledLayers().CuStack();

    if( stack.size() < 2 )
        return;

    auto netName = [&]( int aCode )
    {
        NETINFO_ITEM* n = aBoard.FindNet( aCode );
        return n ? str( n->GetNetname() ) : std::string( "?" );
    };

    auto layerName = [&]( int aLayer ) { return str( aBoard.GetLayerName( PCB_LAYER_ID( aLayer ) ) ); };

    auto describe = [&]( const REFS& aRefs )
    {
        std::string s;

        for( const REF& r : aRefs )
            s += ( s.empty() ? "" : ", " ) + netName( r.second ) + " (" + layerName( r.first ) + ")";

        return s;
    };

    // ---- fast nets: diff pair members (X+ / X-, X_P / X_N, XP / XN with the partner on the board)
    // and names of fast interfaces and clocks
    std::set<std::string> names;

    for( NETINFO_ITEM* n : aBoard.GetNetInfo() )
        names.insert( str( n->GetNetname() ) );

    std::map<int, bool> fastCache;

    auto fastNet = [&]( int aCode )
    {
        auto it = fastCache.find( aCode );

        if( it != fastCache.end() )
            return it->second;

        static const std::regex fastName( R"((CLK(?!REQ)|XTAL|OSC|DQS?\d|_DQS?$))", std::regex::icase );
        const std::string name = netName( aCode );
        bool              fast = std::regex_search( name.substr( name.rfind( '/' ) == std::string::npos ? 0 : name.rfind( '/' ) + 1 ),
                                                    fastName );

        // P / N only after a separator or a D (USB_DP / USB_DN), so SRP / SRN are not a pair
        const bool pn = name.size() > 2 && std::strchr( "PpNn", name.back() )
                        && ( !std::isalpha( (unsigned char) name[name.size() - 2] ) || name[name.size() - 2] == 'D' );

        if( !fast && name.size() > 1 && ( pn || name.back() == '+' || name.back() == '-' ) )
        {
            std::string partner = name;
            char&       last = partner.back();
            last = last == '+' ? '-' : last == '-' ? '+' : last == 'P' ? 'N' : last == 'N' ? 'P' : last == 'p' ? 'n'
                                                                           : last == 'n' ? 'p' : 0;
            fast = last && names.count( partner );
        }

        return fastCache[aCode] = fast;
    };

    auto push = [&]( std::vector<ISSUE>& aList, int aNet, ISSUE aIssue )
    {
        aIssue.net = aNet;
        aIssue.fast = fastNet( aNet );
        aList.push_back( std::move( aIssue ) );
    };

    // ---- the fills, and nets that own a zone
    std::map<int, LAYER_FILLS>                   fills;
    std::vector<std::shared_ptr<SHAPE_POLY_SET>> keep;
    std::set<int>                                zoneNets;
    int                                          unfilled = 0;

    for( const ZONE* z : aBoard.Zones() )
    {
        if( z->GetIsRuleArea() || z->GetNetCode() <= 0 )
            continue;

        zoneNets.insert( z->GetNetCode() );

        for( PCB_LAYER_ID layer : z->GetLayerSet().CuStack() )
        {
            if( !aBoard.GetEnabledLayers().Contains( layer ) )
                continue;

            std::shared_ptr<SHAPE_POLY_SET> poly = z->GetFilledPolysList( layer );

            if( !poly || poly->OutlineCount() == 0 )
                continue;

            keep.push_back( std::make_shared<SHAPE_POLY_SET>( *poly ) );
            keep.back()->Unfracture();
            fills[layer].add( z->GetNetCode(), keep.back() );
        }

        unfilled += !z->IsFilled();
    }

    // ---- the nets to check
    std::set<int> nets;

    if( aReq.plane_nets_size() > 0 )
    {
        for( const std::string& wanted : aReq.plane_nets() )
        {
            bool found = false;

            for( NETINFO_ITEM* n : aBoard.GetNetInfo() )
            {
                if( n->GetNetCode() > 0 && sameNet( str( n->GetNetname() ), wanted ) )
                {
                    nets.insert( n->GetNetCode() );
                    found = true;
                }
            }

            if( !found )
            {
                BoardAuditFinding* f = aOut.add_findings();
                f->set_category( "plane" );
                f->set_severity( "warning" );
                f->set_message( "no net " + wanted + " on the board" );
            }
        }
    }
    else
    {
        for( const PCB_TRACK* t : aBoard.Tracks() )
        {
            const int code = t->GetNetCode();

            if( code > 0 && !nets.count( code ) && !zoneNets.count( code ) && !supplyName( netName( code ) ) )
                nets.insert( code );
        }
    }

    if( nets.empty() )
        return;

    if( fills.empty() )
    {
        note( aOut, "plane",
              unfilled ? "the zones are not filled: refill them (fill_zones) for the reference-plane check"
                       : "no copper zones, so no reference planes: " + std::to_string( nets.size() )
                                 + " signal nets not checked against a plane" );
        return;
    }

    if( unfilled )
        note( aOut, "plane", std::to_string( unfilled ) + " zone(s) not filled count as no copper: refill them "
                                                           "(fill_zones) first" );

    // ---- reference layers of each copper layer: its neighbours in the stack that have fills
    std::map<int, std::vector<std::pair<int, const LAYER_FILLS*>>> planesOf;
    std::map<int, std::string>                                    nextTo;   // layer -> its neighbours, named

    for( size_t i = 0; i < stack.size(); i++ )
    {
        for( size_t j : { i - 1, i + 1 } )
        {
            if( j >= stack.size() )   // i - 1 wraps
                continue;

            nextTo[stack[i]] += ( nextTo[stack[i]].empty() ? "" : " / " ) + layerName( stack[j] );

            auto f = fills.find( stack[j] );

            if( f != fills.end() )
                planesOf[stack[i]].push_back( { stack[j], &f->second } );
        }
    }

    // ---- the chains
    std::map<std::pair<int, int>, std::vector<std::vector<VECTOR2I>>> pieces;   // (net, layer)
    std::map<int, std::set<NODE>>                                     viaNodes;   // net -> via centres
    std::vector<const PCB_VIA*>                                       vias;

    for( const PCB_TRACK* t : aBoard.Tracks() )
    {
        if( !nets.count( t->GetNetCode() ) )
            continue;

        if( t->Type() == PCB_VIA_T )
        {
            vias.push_back( static_cast<const PCB_VIA*>( t ) );
            viaNodes[t->GetNetCode()].insert( node( t->GetPosition() ) );
            continue;
        }

        std::vector<VECTOR2I> pts;

        if( t->Type() == PCB_ARC_T )
        {
            const PCB_ARC*         arc = static_cast<const PCB_ARC*>( t );
            const SHAPE_LINE_CHAIN line = SHAPE_ARC( arc->GetStart(), arc->GetMid(), arc->GetEnd(), 0 ).ConvertToPolyline();

            for( int i = 0; i < line.PointCount(); i++ )
                pts.push_back( line.CPoint( i ) );

            if( pts.size() >= 2 )   // exact ends, so the chains join
            {
                pts.front() = arc->GetStart();
                pts.back() = arc->GetEnd();
            }
        }
        else
        {
            pts = { t->GetStart(), t->GetEnd() };
        }

        if( pts.size() >= 2 && pts.front() != pts.back() )
            pieces[{ t->GetNetCode(), t->GetLayer() }].push_back( std::move( pts ) );
    }

    std::vector<CHAIN>   chains;
    std::map<int, double> unplaned;   // layer -> mm of checked track on it with no plane next to it
    std::set<int>         checked;

    for( const auto& [key, list] : pieces )
    {
        const auto [net, layer] = key;
        const auto planes = planesOf.find( layer );

        if( planes == planesOf.end() )
        {
            for( const std::vector<VECTOR2I>& p : list )
                for( size_t k = 0; k + 1 < p.size(); k++ )
                    unplaned[layer] += ( p[k + 1] - p[k] ).EuclideanNorm() / 1e6;

            continue;
        }

        checked.insert( net );
        const size_t first = chains.size();
        buildChains( list, viaNodes[net], net, PCB_LAYER_ID( layer ), chains );

        for( size_t i = first; i < chains.size(); i++ )
            classify( chains[i], planes->second );
    }

    // ---- along the tracks
    std::vector<ISSUE> splits, voids, edges;
    double             total = 0, covered = 0;

    for( const CHAIN& c : chains )
    {
        total += c.length;
        const std::string who = netName( c.net ) + " on " + layerName( c.layer );

        for( size_t i = 0; i < c.runs.size(); i++ )
        {
            const RUN& r = c.runs[i];
            const RUN* prev = i > 0 ? &c.runs[i - 1] : nullptr;
            const RUN* next = i + 1 < c.runs.size() ? &c.runs[i + 1] : nullptr;
            const double len = r.to - r.from;

            if( !r.refs.empty() )
            {
                covered += len;

                if( prev && !prev->refs.empty() && noNetInCommon( prev->refs, r.refs ) )
                    push( splits, c.net, { who + " crosses from " + describe( prev->refs ) + " to " + describe( r.refs )
                                                + " at " + at( r.a ),
                                        1e9, r.a } );

                continue;
            }

            const VECTOR2I mid = pointAt( c, ( r.from + r.to ) / 2 );

            if( prev && next && !prev->refs.empty() && !next->refs.empty() )
            {
                if( noNetInCommon( prev->refs, next->refs ) )
                    push( splits, c.net, { who + " crosses a plane split at " + at( mid ) + ": " + describe( prev->refs )
                                                + " to " + describe( next->refs ) + " (" + mm( len / 1e6 )
                                                + " mm with no plane under it)",
                                        1e9 + len, mid } );
                else if( len >= gap )
                {
                    bool inside = false;   // a hole in the fill, not past its edge

                    for( const REF& ref : prev->refs )
                    {
                        bool all = fills.count( ref.first ) > 0;

                        for( double f : { 0.25, 0.5, 0.75 } )
                            all = all && fills.at( ref.first ).inOuter( ref.second, pointAt( c, r.from + f * len ) );

                        inside = inside || all;
                    }

                    if( inside )
                        push( voids, c.net, { who + " runs " + mm( len / 1e6 ) + " mm over a gap in " + describe( prev->refs )
                                                   + " at " + at( mid ),
                                           len, mid } );
                    else
                        push( edges, c.net, { who + " runs " + mm( len / 1e6 ) + " mm past the edge of " + describe( prev->refs )
                                                   + " at " + at( mid ),
                                           len, mid } );
                }
            }
            else if( !prev && !next )
            {
                if( len >= gap )
                    push( edges, c.net, { who + ": " + mm( len / 1e6 ) + " mm with no plane under it on "
                                               + nextTo[c.layer] + " (from " + at( c.pts.front() ) + ")",
                                       len, mid } );
            }
            else
            {
                // an end: the antipad of the via or pad it ends at is not a gap, so allow a millimetre more
                const RUN* side = prev ? prev : next;

                if( len >= gap + 1e6 )
                    push( edges, c.net, { who + " runs " + mm( len / 1e6 ) + " mm past the edge of "
                                               + describe( side->refs ) + " at " + at( mid ),
                                       len, mid } );
            }
        }
    }

    aOut.set_plane_nets( (int) checked.size() );
    aOut.set_plane_track_mm( total / 1e6 );
    aOut.set_plane_covered_mm( covered / 1e6 );

    for( const auto& [layer, len] : unplaned )
        note( aOut, "plane", layerName( layer ) + ": no zone on " + nextTo[layer] + ", so its " + mm( len, 0 )
                                     + " mm of signal track have no reference plane to check" );

    const bool named = aReq.plane_nets_size() > 0;
    emit( aOut, "plane", splits, "split crossing(s)", true );
    emit( aOut, "plane", voids, "gap(s) in a plane under a track", named );
    emit( aOut, "plane", edges, "stretch(es) off the plane", named );

    // ---- vias that change the reference
    struct CONDUCTOR
    {
        VECTOR2I pos;
        LSET     layers;
    };

    std::map<int, std::vector<CONDUCTOR>> joiners;   // plane net -> its vias and through-hole pads

    for( const PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->Type() == PCB_VIA_T && t->GetNetCode() > 0 )
            joiners[t->GetNetCode()].push_back( { t->GetPosition(), t->GetLayerSet() & LSET::AllCuMask() } );
    }

    struct CAP
    {
        VECTOR2I    pos;
        std::string ref;
        std::set<int> nets;
    };

    std::vector<CAP> caps;

    for( const FOOTPRINT* fp : aBoard.Footprints() )
    {
        const std::string ref = str( fp->GetReference() );
        CAP               cap{ fp->GetPosition(), ref, {} };

        for( const PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() <= 0 )
                continue;

            cap.nets.insert( pad->GetNetCode() );

            if( pad->GetAttribute() == PAD_ATTRIB::PTH )
                joiners[pad->GetNetCode()].push_back( { pad->GetPosition(), pad->GetLayerSet() & LSET::AllCuMask() } );
        }

        if( ref.size() > 1 && ref[0] == 'C' && std::isdigit( (unsigned char) ref[1] ) && cap.nets.size() == 2 )
            caps.push_back( cap );
    }

    // chain ends at each via: (net, centre) -> layer -> planes there
    std::map<std::pair<int, NODE>, std::map<int, REFS>> atVia;

    for( const CHAIN& c : chains )
    {
        for( bool front : { true, false } )
        {
            const NODE end = node( front ? c.pts.front() : c.pts.back() );

            if( !viaNodes[c.net].count( end ) )
                continue;

            REFS  refs = endRefs( c, front, 5e6 );
            REFS& have = atVia[{ c.net, end }][c.layer];
            have.insert( have.end(), refs.begin(), refs.end() );
            std::sort( have.begin(), have.end() );
            have.erase( std::unique( have.begin(), have.end() ), have.end() );
        }
    }

    std::vector<ISSUE> noReturn, netChange;
    int                changing = 0;

    for( const PCB_VIA* via : vias )
    {
        auto it = atVia.find( { via->GetNetCode(), node( via->GetPosition() ) } );

        if( it == atVia.end() )
            continue;

        std::vector<std::pair<int, REFS>> sides;

        for( const auto& [layer, refs] : it->second )
        {
            if( !refs.empty() )
                sides.push_back( { layer, refs } );
        }

        bool   counted = false;
        ISSUE  worst;
        bool   worstNoReturn = false;
        double worstDist = -1;

        for( size_t i = 0; i < sides.size(); i++ )
        {
            for( size_t j = i + 1; j < sides.size(); j++ )
            {
                const REFS& A = sides[i].second;
                const REFS& B = sides[j].second;

                if( !disjoint( A, B ) )
                    continue;   // a plane in common: the return current stays on it

                if( !counted )
                    changing++;

                counted = true;

                const std::string what = "via at " + at( via->GetPosition() ) + " takes " + netName( via->GetNetCode() )
                                         + " from " + layerName( sides[i].first ) + " (over " + describe( A ) + ") to "
                                         + layerName( sides[j].first ) + " (over " + describe( B ) + ")";

                // the same plane net on both sides: a via of it joining the two planes
                double best = -1;
                int    bestNet = 0;
                bool   common = false;

                for( const REF& a : A )
                {
                    for( const REF& b : B )
                    {
                        if( a.second != b.second )
                            continue;

                        common = true;

                        for( const CONDUCTOR& c : joiners[a.second] )
                        {
                            if( !c.layers.Contains( PCB_LAYER_ID( a.first ) ) || !c.layers.Contains( PCB_LAYER_ID( b.first ) ) )
                                continue;

                            const double d = ( c.pos - via->GetPosition() ).EuclideanNorm();

                            if( best < 0 || d < best )
                            {
                                best = d;
                                bestNet = a.second;
                            }
                        }
                    }
                }

                if( common )
                {
                    if( best >= 0 && best <= reach )
                        continue;

                    const double d = best < 0 ? 1e12 : best;

                    if( d > worstDist )
                    {
                        worstDist = d;
                        worstNoReturn = true;
                        worst = { what + ": "
                                          + ( best < 0 ? std::string( "no via of the plane net joins those layers" )
                                                       : "the nearest " + netName( bestNet ) + " via joining them is "
                                                                 + mm( best / 1e6 ) + " mm away (max "
                                                                 + mm( reach / 1e6 ) + ")" ),
                                  d, via->GetPosition() };
                    }

                    continue;
                }

                // different plane nets: the return current crosses through a capacitor between them
                double      capDist = -1;
                std::string capRef;

                for( const CAP& c : caps )
                {
                    bool hasA = false, hasB = false;

                    for( const REF& a : A )
                        hasA = hasA || c.nets.count( a.second );

                    for( const REF& b : B )
                        hasB = hasB || c.nets.count( b.second );

                    if( !hasA || !hasB )
                        continue;

                    const double d = ( c.pos - via->GetPosition() ).EuclideanNorm();

                    if( capDist < 0 || d < capDist )
                    {
                        capDist = d;
                        capRef = c.ref;
                    }
                }

                if( capDist >= 0 && capDist <= 2 * reach )
                    continue;

                const double d = capDist < 0 ? 1e12 : capDist;

                if( !worstNoReturn && d > worstDist )
                {
                    worstDist = d;
                    worst = { what + ( capDist < 0 ? std::string( ": no capacitor between them on the board" )
                                                   : ": the nearest capacitor between them, " + capRef + ", is "
                                                             + mm( capDist / 1e6 ) + " mm away" ),
                              d, via->GetPosition(), capRef.empty() ? std::vector<std::string>{}
                                                                    : std::vector<std::string>{ capRef } };

                    std::set<std::string> na, nb;

                    for( const REF& r : A )
                        na.insert( netName( r.second ) );

                    for( const REF& r : B )
                        nb.insert( netName( r.second ) );

                    std::string ga, gb;

                    for( const std::string& n : na )
                        ga += ( ga.empty() ? "" : " + " ) + n;

                    for( const std::string& n : nb )
                        gb += ( gb.empty() ? "" : " + " ) + n;

                    worst.group = std::min( ga, gb ) + " and " + std::max( ga, gb );
                }
            }
        }

        if( worstDist >= 0 )
            push( worstNoReturn ? noReturn : netChange, via->GetNetCode(), worst );
    }

    aOut.set_plane_vias( changing );
    emit( aOut, "return_via", noReturn, "signal via(s) without a return via", named );

    // vias between planes of different nets, by the pair of nets: how many, and the farthest from a capacitor
    std::map<std::string, std::vector<ISSUE*>> byPair;

    for( ISSUE& i : netChange )
        byPair[i.group].push_back( &i );

    std::vector<ISSUE> groups;

    for( auto& [pair, list] : byPair )
    {
        ISSUE* far = *std::max_element( list.begin(), list.end(), []( ISSUE* a, ISSUE* b ) { return a->size < b->size; } );
        ISSUE  g = *far;
        g.size = (double) list.size();
        g.text = std::to_string( list.size() ) + " signal via(s) change between planes of " + pair + " more than "
                 + mm( 2 * reach / 1e6 ) + " mm from a capacitor between them (the return current crosses "
                 "through one); the farthest: " + far->text;
        groups.push_back( g );
    }

    emitList( aOut, "return_via", "info", groups, "pair(s) of plane nets", 10 );
}

} // namespace kicadd
