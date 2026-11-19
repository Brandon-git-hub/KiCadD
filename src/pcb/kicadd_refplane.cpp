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

/// Named like a supply or ground: +5V, 3V3, GND, AGND, VCC_IO, VDD, VBUS, VBAT ...
bool supplyName( const std::string& aNet )
{
    static const std::regex re( R"(^([+-][A-Za-z].*|[+-]?\d+(\.\d+)?V\d*|[+-]?\d+(\.\d+)?V\d*_.*|[PN]\d+V\d*(_.*)?|.*GND.*|VCC.*|VDD.*|VSS.*|VEE.*|VBUS.*|VBAT.*|VIN|VSYS.*|.*PWR.*)$)",
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

// ---------------------------------------------------------------------------- the agent's limits

/// Net name globs with a number each ({"*PCIE*TX*": 1.0, "*": 3}). For a net, an exact name wins,
/// then the glob with the most literal characters. A glob matches the whole name or the part after
/// a sheet path ("USB_D+" matches "/USB/USB_D+"), case blind.
struct GLOBS
{
    struct GLOB
    {
        std::string pattern;
        std::regex  re;
        int         rank;
        double      value;
    };

    std::vector<GLOB> list;

    void add( const std::string& aPattern, double aValue )
    {
        std::string re;
        int         literal = 0;

        for( char ch : aPattern )
        {
            if( ch == '*' )
                re += ".*";
            else if( ch == '?' )
                re += ".";
            else
            {
                literal++;
                re += std::strchr( "\\^$.|+()[]{}", ch ) ? std::string( "\\" ) + ch : std::string( 1, ch );
            }
        }

        const bool exact = aPattern.find_first_of( "*?" ) == std::string::npos;
        list.push_back( { aPattern, std::regex( "^(.*/)?" + re + "$", std::regex::icase ), literal + ( exact ? 100000 : 0 ),
                          aValue } );
    }

    bool empty() const { return list.empty(); }

    const GLOB* match( const std::string& aName ) const
    {
        const GLOB* best = nullptr;

        for( const GLOB& g : list )
        {
            if( ( !best || g.rank > best->rank ) && std::regex_match( aName, g.re ) )
                best = &g;
        }

        return best;
    }
};

// ---------------------------------------------------------------------------- output

void line( BoardAuditResponse& aOut, const char* aCat, const char* aSeverity, const std::string& aMsg,
           std::optional<VECTOR2I> aAt = std::nullopt, const std::vector<std::string>& aRefs = {} )
{
    BoardAuditFinding* f = aOut.add_findings();
    f->set_category( aCat );
    f->set_severity( aSeverity );
    f->set_message( aMsg );

    if( aAt )
    {
        f->mutable_at()->set_x( aAt->x );
        f->mutable_at()->set_y( aAt->y );
    }

    for( const std::string& r : aRefs )
        f->add_refs( r );
}

void note( BoardAuditResponse& aOut, const char* aCat, const std::string& aMsg )
{
    line( aOut, aCat, "info", aMsg );
}

} // namespace


void CheckReferencePlanes( const BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    const LSEQ   stack = aBoard.GetEnabledLayers().CuStack();
    const size_t limit = aReq.plane_limit() > 0 ? aReq.plane_limit() : 10;

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

    // ---- diff pairs: X+ / X-, X_P / X_N, XDP / XDN with the partner on the board (a fact for the
    // agent: "(pair)" in the overview)
    std::map<std::string, int> codes;

    for( NETINFO_ITEM* n : aBoard.GetNetInfo() )
        codes[str( n->GetNetname() )] = n->GetNetCode();

    auto partnerOf = [&]( int aCode ) -> int
    {
        const std::string name = netName( aCode );

        if( name.size() < 2 )
            return 0;

        // P / N only after a separator or a D (USB_DP / USB_DN), so SRP / SRN are not a pair
        const bool pn = name.size() > 2 && std::strchr( "PpNn", name.back() )
                        && ( !std::isalpha( (unsigned char) name[name.size() - 2] ) || name[name.size() - 2] == 'D' );

        if( !pn && name.back() != '+' && name.back() != '-' )
            return 0;

        std::string partner = name;
        char&       last = partner.back();
        last = last == '+' ? '-' : last == '-' ? '+' : last == 'P' ? 'N' : last == 'N' ? 'P' : last == 'p' ? 'n' : 'p';
        auto it = codes.find( partner );
        return it == codes.end() ? 0 : it->second;
    };

    auto tagged = [&]( int aCode ) { return netName( aCode ) + ( partnerOf( aCode ) ? " (pair)" : "" ); };

    GLOBS gapLimit, viaLimit, twoSided, picked;

    for( const auto& [g, mm] : aReq.plane_gap() )
        gapLimit.add( g, mm );

    for( const auto& [g, mm] : aReq.return_via() )
        viaLimit.add( g, mm );

    for( const std::string& g : aReq.two_sided() )
        twoSided.add( g, 0 );

    for( const std::string& g : aReq.plane_nets() )
        picked.add( g, 0 );

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

    // ---- the nets: plane_nets globs, else every net with tracks, no zone and no supply name
    std::set<int> nets;

    if( !picked.empty() )
    {
        std::set<std::string> used;

        for( NETINFO_ITEM* n : aBoard.GetNetInfo() )
        {
            if( n->GetNetCode() <= 0 )
                continue;

            if( const GLOBS::GLOB* g = picked.match( str( n->GetNetname() ) ) )
            {
                nets.insert( n->GetNetCode() );
                used.insert( g->pattern );
            }
        }

        for( const GLOBS::GLOB& g : picked.list )
        {
            if( !used.count( g.pattern ) )
                line( aOut, "plane", "warning", "plane_nets: no net matches " + g.pattern );
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

    // ---- along the tracks: every split, gap and stretch off the plane, measured
    struct EVENT
    {
        int         net;
        int         kind;   // 0 split, 1 gap in a plane, 2 off the plane
        double      len;    // nm with no plane under the track
        VECTOR2I    at;
        std::string text;
    };

    struct NETSTAT
    {
        double                len = 0, covered = 0;
        std::set<std::string> layers;
        const EVENT*          split = nullptr;
        int                   splits = 0;
        const EVENT*          gap = nullptr;
        const EVENT*          off = nullptr;
    };

    std::vector<EVENT> events;
    double             total = 0, covered = 0;

    for( const CHAIN& c : chains )
    {
        total += c.length;
        const std::string who = netName( c.net ) + " on " + layerName( c.layer );

        for( size_t i = 0; i < c.runs.size(); i++ )
        {
            const RUN&   r = c.runs[i];
            const RUN*   prev = i > 0 ? &c.runs[i - 1] : nullptr;
            const RUN*   next = i + 1 < c.runs.size() ? &c.runs[i + 1] : nullptr;
            const double len = r.to - r.from;

            if( !r.refs.empty() )
            {
                covered += len;

                if( prev && !prev->refs.empty() && noNetInCommon( prev->refs, r.refs ) )
                    events.push_back( { c.net, 0, 0, r.a,
                                        who + " crosses from " + describe( prev->refs ) + " to " + describe( r.refs )
                                                + " at " + at( r.a ) } );

                continue;
            }

            const VECTOR2I mid = pointAt( c, ( r.from + r.to ) / 2 );

            if( prev && next && !prev->refs.empty() && !next->refs.empty() )
            {
                if( noNetInCommon( prev->refs, next->refs ) )
                {
                    events.push_back( { c.net, 0, len, mid,
                                        who + " crosses a plane split at " + at( mid ) + ": " + describe( prev->refs )
                                                + " to " + describe( next->refs ) + " (" + mm( len / 1e6 )
                                                + " mm with no plane under it)" } );
                    continue;
                }

                bool inside = false;   // a hole in the fill, not past its edge

                for( const REF& ref : prev->refs )
                {
                    bool all = fills.count( ref.first ) > 0;

                    for( double f : { 0.25, 0.5, 0.75 } )
                        all = all && fills.at( ref.first ).inOuter( ref.second, pointAt( c, r.from + f * len ) );

                    inside = inside || all;
                }

                events.push_back( { c.net, inside ? 1 : 2, len, mid,
                                    who + " runs " + mm( len / 1e6 ) + " mm "
                                            + ( inside ? "over a gap in " : "past the edge of " ) + describe( prev->refs )
                                            + " at " + at( mid ) } );
            }
            else if( !prev && !next )
            {
                events.push_back( { c.net, 2, len, mid,
                                    who + ": " + mm( len / 1e6 ) + " mm with no plane under it on " + nextTo[c.layer]
                                            + " (from " + at( c.pts.front() ) + ")" } );
            }
            else if( len > 1e6 )   // at an end, the antipad of its via or pad is not counted
            {
                const RUN* side = prev ? prev : next;
                events.push_back( { c.net, 2, len, mid,
                                    who + " runs " + mm( len / 1e6 ) + " mm past the edge of " + describe( side->refs )
                                            + " at " + at( mid ) } );
            }
        }
    }

    std::map<int, NETSTAT> stats;

    for( const CHAIN& c : chains )
    {
        NETSTAT& st = stats[c.net];
        st.len += c.length;
        st.layers.insert( layerName( c.layer ) );

        for( const RUN& r : c.runs )
            st.covered += r.refs.empty() ? 0 : r.to - r.from;
    }

    for( const EVENT& e : events )
    {
        NETSTAT& st = stats[e.net];

        if( e.kind == 0 )
        {
            st.splits++;
            st.split = st.split ? st.split : &e;
        }
        else
        {
            const EVENT*& worst = e.kind == 1 ? st.gap : st.off;
            worst = !worst || e.len > worst->len ? &e : worst;
        }
    }

    aOut.set_plane_nets( (int) stats.size() );
    aOut.set_plane_track_mm( total / 1e6 );
    aOut.set_plane_covered_mm( covered / 1e6 );

    for( const auto& [layer, len] : unplaned )
        note( aOut, "plane", layerName( layer ) + ": no zone on " + nextTo[layer] + ", so its " + mm( len, 0 )
                                     + " mm of signal track have no reference plane to check" );

    if( gapLimit.empty() )
    {
        // the overview: per net, worst first (splits, then the longest stretch with no plane)
        std::vector<std::pair<int, const NETSTAT*>> rows;
        int                                         withSplit = 0, withGap = 0;

        for( const auto& [net, st] : stats )
        {
            const double worst = std::max( st.gap ? st.gap->len : 0.0, st.off ? st.off->len : 0.0 );
            withSplit += st.splits > 0;
            withGap += worst > 1e6;

            // stretches under 0.5 mm (antipads, mostly) are left out of the overview
            if( st.splits || worst >= 0.5e6 )
                rows.push_back( { net, &st } );
        }

        auto worstLen = []( const NETSTAT* s ) { return std::max( s->gap ? s->gap->len : 0.0, s->off ? s->off->len : 0.0 ); };

        std::sort( rows.begin(), rows.end(),
                   [&]( const auto& a, const auto& b )
                   {
                       return a.second->splits != b.second->splits ? a.second->splits > b.second->splits
                                                                   : worstLen( a.second ) > worstLen( b.second );
                   } );

        note( aOut, "plane", std::to_string( stats.size() ) + " signal net(s) measured: " + std::to_string( withSplit )
                                     + " cross a plane split, " + std::to_string( withGap )
                                     + " have a stretch over 1 mm with no plane under them (nets under 0.5 mm not listed). Worst first; "
                                       "no verdicts: "
                                       "judge them, or pass plane_gap {glob: mm}" );

        for( size_t i = 0; i < rows.size() && i < limit; i++ )
        {
            const auto& [net, st] = rows[i];
            std::string layers;

            for( const std::string& l : st->layers )
                layers += ( layers.empty() ? "" : "/" ) + l;

            std::string t = tagged( net ) + ": " + mm( st->len / 1e6, 1 ) + " mm on " + layers + ", "
                            + mm( st->len > 0 ? 100 * st->covered / st->len : 0, 0 ) + "% over a plane";

            if( st->splits )
                t += "; " + std::to_string( st->splits ) + " split crossing(s), the first: " + st->split->text;

            if( st->gap )
                t += "; longest gap " + mm( st->gap->len / 1e6 ) + " mm at " + at( st->gap->at );

            if( st->off )
                t += "; longest off the plane " + mm( st->off->len / 1e6 ) + " mm at " + at( st->off->at );

            line( aOut, "plane", "info", t, st->split ? st->split->at : st->gap ? st->gap->at : st->off->at );
        }

        if( rows.size() > limit )
            note( aOut, "plane", std::to_string( rows.size() - limit ) + " more net(s) (plane_limit)" );
    }
    else
    {
        // the agent's limits: splits always, gaps and stretches off the plane longer than its mm
        std::vector<std::pair<const EVENT*, const GLOBS::GLOB*>> bad;

        for( const EVENT& e : events )
        {
            const GLOBS::GLOB* g = gapLimit.match( netName( e.net ) );

            if( g && ( e.kind == 0 || e.len > g->value * 1e6 ) )
                bad.push_back( { &e, g } );
        }

        std::stable_sort( bad.begin(), bad.end(),
                          []( const auto& a, const auto& b )
                          { return ( a.first->kind == 0 ) != ( b.first->kind == 0 ) ? a.first->kind == 0 : a.first->len > b.first->len; } );

        for( size_t i = 0; i < bad.size() && i < 2 * limit; i++ )
            line( aOut, "plane", "warning",
                  bad[i].first->text + ( bad[i].first->kind ? " (max " + mm( bad[i].second->value ) + " for "
                                                                  + bad[i].second->pattern + ")"
                                                            : std::string() ),
                  bad[i].first->at );

        if( bad.size() > 2 * limit )
            line( aOut, "plane", "warning", std::to_string( bad.size() - 2 * limit ) + " more (plane_limit)" );
    }

    // ---- vias that change the reference: the return path measured
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
        VECTOR2I      pos;
        std::string   ref;
        std::set<int> nets;
    };

    struct BGA
    {
        std::string ref;
        BOX2I       box;
        int         pads;
    };

    std::vector<CAP> caps;
    std::vector<BGA> bgas;

    for( const FOOTPRINT* fp : aBoard.Footprints() )
    {
        const std::string ref = str( fp->GetReference() );
        CAP               cap{ fp->GetPosition(), ref, {} };
        BOX2I             padBox;
        bool              first = true;

        for( const PAD* pad : fp->Pads() )
        {
            if( first )
                padBox = BOX2I( pad->GetPosition(), VECTOR2I( 0, 0 ) );
            else
                padBox.Merge( pad->GetPosition() );

            first = false;

            if( pad->GetNetCode() <= 0 )
                continue;

            cap.nets.insert( pad->GetNetCode() );

            if( pad->GetAttribute() == PAD_ATTRIB::PTH )
                joiners[pad->GetNetCode()].push_back( { pad->GetPosition(), pad->GetLayerSet() & LSET::AllCuMask() } );
        }

        if( ref.size() > 1 && ref[0] == 'C' && std::isdigit( (unsigned char) ref[1] ) && cap.nets.size() == 2 )
            caps.push_back( cap );

        if( fp->GetFPID().GetLibItemName().wx_str().Upper().Contains( wxS( "BGA" ) ) )
            bgas.push_back( { ref, padBox, (int) fp->Pads().size() } );
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

    std::map<int, std::vector<const PCB_VIA*>> viasOf;

    for( const PCB_VIA* v : vias )
        viasOf[v->GetNetCode()].push_back( v );

    struct VIAM
    {
        const PCB_VIA* via = nullptr;
        std::string    what;            // "F.Cu (over GND (In1.Cu)) to B.Cu (over GND (In4.Cu))"
        int            planeNet = 0;    // the same plane net on both sides; 0: different plane nets
        double         nearest = -1;    // the nearest return via (nm); -1: none joins the two planes
        double         otherSide = -1;  // the nearest one on the other side of the via (or pair)
        std::string    bga;             // "in U1's BGA field (40 GND vias, 600 pads)"
        double         capDist = -1;
        std::string    capRef, group;
    };

    std::vector<VIAM> measured;

    for( const PCB_VIA* via : vias )
    {
        auto it = atVia.find( { via->GetNetCode(), node( via->GetPosition() ) } );

        if( it == atVia.end() )
            continue;

        std::vector<std::pair<int, REFS>> sides;

        for( const auto& [layer, chainRefs] : it->second )
        {
            REFS refs = chainRefs;

            // a short track (a BGA's dogbone) inside the antipads: the planes round the via, 0.75 to
            // 1.5 mm out
            if( refs.empty() && planesOf.count( layer ) )
            {
                for( double r : { 0.75e6, 1.0e6, 1.5e6 } )
                {
                    for( int k = 0; k < 8; k++ )
                    {
                        const VECTOR2I p = via->GetPosition()
                                           + VECTOR2I( VECTOR2D( std::cos( k * M_PI / 4 ), std::sin( k * M_PI / 4 ) ) * r );

                        for( const auto& [plane, f] : planesOf.at( layer ) )
                            f->refsAt( plane, p, refs );
                    }

                    if( !refs.empty() )
                        break;
                }

                std::sort( refs.begin(), refs.end() );
                refs.erase( std::unique( refs.begin(), refs.end() ), refs.end() );
            }

            if( !refs.empty() )
                sides.push_back( { layer, refs } );
        }

        // the first pair of sides with no plane in common (a plane in common keeps the return current)
        int si = -1, sj = -1;

        for( size_t i = 0; i < sides.size() && si < 0; i++ )
        {
            for( size_t j = i + 1; j < sides.size() && si < 0; j++ )
            {
                if( disjoint( sides[i].second, sides[j].second ) )
                {
                    si = (int) i;
                    sj = (int) j;
                }
            }
        }

        if( si < 0 )
            continue;

        const REFS& A = sides[si].second;
        const REFS& B = sides[sj].second;
        const VECTOR2I pos = via->GetPosition();
        VIAM m;
        m.via = via;
        m.what = layerName( sides[si].first ) + " (over " + describe( A ) + ") to " + layerName( sides[sj].first ) + " (over "
                 + describe( B ) + ")";

        // the same plane net on both sides: its vias joining the two planes
        std::vector<VECTOR2I> found;

        for( const REF& a : A )
        {
            for( const REF& b : B )
            {
                if( a.second != b.second )
                    continue;

                m.planeNet = a.second;

                for( const CONDUCTOR& c : joiners[a.second] )
                {
                    if( c.layers.Contains( PCB_LAYER_ID( a.first ) ) && c.layers.Contains( PCB_LAYER_ID( b.first ) ) )
                        found.push_back( c.pos );
                }
            }
        }

        if( m.planeNet )
        {
            // the centre: between the pair's vias when the partner's is close by
            VECTOR2I centre = pos, partnerPos = pos;

            if( int partner = partnerOf( via->GetNetCode() ) )
            {
                double best = 2.5e6;

                for( const PCB_VIA* p : viasOf[partner] )
                {
                    const double d = ( p->GetPosition() - pos ).EuclideanNorm();

                    if( d < best )
                    {
                        best = d;
                        centre = ( pos + p->GetPosition() ) / 2;
                        partnerPos = p->GetPosition();
                    }
                }
            }

            const VECTOR2I* near = nullptr;

            for( const VECTOR2I& f : found )
            {
                const double d = ( f - pos ).EuclideanNorm();

                if( m.nearest < 0 || d < m.nearest )
                {
                    m.nearest = d;
                    near = &f;
                }
            }

            if( near )   // the other side: at least 120 degrees round the centre from the nearest
            {
                const VECTOR2D a = VECTOR2D( *near - centre );

                for( const VECTOR2I& f : found )
                {
                    const VECTOR2D b = VECTOR2D( f - centre );
                    const double   n = a.EuclideanNorm() * b.EuclideanNorm();

                    if( n <= 0 || ( a.x * b.x + a.y * b.y ) / n > -0.5 )
                        continue;

                    // from the nearer of the pair's vias: beyond the partner is this side's far end
                    const double d = std::min( ( f - pos ).EuclideanNorm(), ( f - partnerPos ).EuclideanNorm() );

                    if( m.otherSide < 0 || d < m.otherSide )
                        m.otherSide = d;
                }
            }

            for( const BGA& g : bgas )
            {
                if( !g.box.Contains( pos ) )
                    continue;

                int n = 0;

                for( const CONDUCTOR& c : joiners[m.planeNet] )
                    n += g.box.Contains( c.pos );

                m.bga = "in " + g.ref + "'s BGA field (" + std::to_string( n ) + " " + netName( m.planeNet ) + " vias, "
                        + std::to_string( g.pads ) + " pads)";
            }
        }
        else
        {
            // different plane nets: the return current crosses through a capacitor between them
            for( const CAP& c : caps )
            {
                bool hasA = false, hasB = false;

                for( const REF& a : A )
                    hasA = hasA || c.nets.count( a.second );

                for( const REF& b : B )
                    hasB = hasB || c.nets.count( b.second );

                const double d = ( c.pos - pos ).EuclideanNorm();

                if( hasA && hasB && ( m.capDist < 0 || d < m.capDist ) )
                {
                    m.capDist = d;
                    m.capRef = c.ref;
                }
            }

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

            m.group = std::min( ga, gb ) + " / " + std::max( ga, gb );
        }

        measured.push_back( m );
    }

    aOut.set_plane_vias( (int) measured.size() );

    auto far = []( double d ) { return d < 0 ? 1e15 : d; };
    auto dist = [&]( double d ) { return d < 0 ? std::string( "none" ) : mm( d / 1e6 ) + " mm"; };

    // vias between planes of different nets, by the pair of plane nets (measured in both modes)
    std::map<std::string, std::vector<const VIAM*>> byPair;
    int                                             sameNet = 0;

    for( const VIAM& m : measured )
    {
        if( m.planeNet )
            sameNet++;
        else
            byPair[m.group].push_back( &m );
    }

    if( measured.empty() )
    {
        // nothing to say: no signal via changes planes
    }
    else if( viaLimit.empty() )
    {
        // the overview: per signal net, the farthest return via first
        std::map<int, std::vector<const VIAM*>> byNet;

        for( const VIAM& m : measured )
        {
            if( m.planeNet )
                byNet[m.via->GetNetCode()].push_back( &m );
        }

        std::vector<std::pair<int, const VIAM*>> rows;   // net, its worst via

        for( const auto& [net, list] : byNet )
            rows.push_back( { net, *std::max_element( list.begin(), list.end(), [&]( const VIAM* a, const VIAM* b )
                                                      { return far( a->nearest ) < far( b->nearest ); } ) } );

        std::sort( rows.begin(), rows.end(),
                   [&]( const auto& a, const auto& b ) { return far( a.second->nearest ) > far( b.second->nearest ); } );

        note( aOut, "return_via",
              std::to_string( measured.size() ) + " signal via(s) change reference planes: " + std::to_string( sameNet )
                      + " between planes of the same net (the return via measured, per net below, the farthest first), "
                      + std::to_string( measured.size() - sameNet )
                      + " between different plane nets (a capacitor measured). No verdicts: judge them, or pass "
                        "return_via {glob: mm}" );

        for( size_t i = 0; i < rows.size() && i < limit; i++ )
        {
            const auto& [net, w] = rows[i];
            const auto& list = byNet[net];
            int         oneSide = 0, inBga = 0;
            double      worstOther = 0;
            std::string bga;

            for( const VIAM* m : list )
            {
                oneSide += m->otherSide < 0;
                worstOther = std::max( worstOther, far( m->otherSide ) );
                inBga += !m->bga.empty();
                bga = m->bga.empty() ? bga : m->bga;
            }

            std::string t = tagged( net ) + ": " + std::to_string( list.size() ) + " via(s); farthest return via "
                            + dist( w->nearest ) + " (" + netName( w->planeNet ) + ") at " + at( w->via->GetPosition() )
                            + ", " + w->what + "; other side " + ( worstOther >= 1e15 ? std::string( "none" )
                                                                                      : "up to " + mm( worstOther / 1e6 ) + " mm" );

            if( oneSide )
                t += " (" + std::to_string( oneSide ) + " via(s) with return vias on one side only)";

            if( inBga )
                t += "; " + std::to_string( inBga ) + " " + bga;

            line( aOut, "return_via", "info", t, w->via->GetPosition() );
        }

        if( rows.size() > limit )
            note( aOut, "return_via", std::to_string( rows.size() - limit ) + " more net(s) (plane_limit)" );
    }
    else
    {
        // the agent's limits: the nearest return via, and with two_sided one on the other side too
        std::vector<std::pair<double, std::string>> bad;   // how far over, text
        std::vector<VECTOR2I>                       where;

        for( const VIAM& m : measured )
        {
            if( !m.planeNet )
                continue;

            const GLOBS::GLOB* g = viaLimit.match( netName( m.via->GetNetCode() ) );

            if( !g )
                continue;

            double      max = g->value * 1e6;
            std::string rule = "max " + mm( g->value ) + " for " + g->pattern;

            if( !m.bga.empty() && aReq.bga_return_via_mm() > 0 )
            {
                max = aReq.bga_return_via_mm() * 1e6;
                rule = "max " + mm( aReq.bga_return_via_mm() ) + " in a BGA field";
            }

            const std::string head = "via at " + at( m.via->GetPosition() ) + " takes " + tagged( m.via->GetNetCode() )
                                     + " from " + m.what + ": ";

            if( m.nearest < 0 || m.nearest > max )
                bad.push_back( { far( m.nearest ) - max,
                                 head + "nearest " + netName( m.planeNet ) + " return via " + dist( m.nearest ) + " ("
                                         + rule + ( m.bga.empty() ? "" : "; " + m.bga ) + ")" } );
            else if( twoSided.match( netName( m.via->GetNetCode() ) ) && ( m.otherSide < 0 || m.otherSide > max ) )
                bad.push_back( { far( m.otherSide ) - max,
                                 head + "return via on one side only: nearest " + dist( m.nearest ) + ", on the other side "
                                         + dist( m.otherSide ) + " (two_sided, " + rule + ")" } );
            else
                continue;

            where.push_back( m.via->GetPosition() );
        }

        std::vector<size_t> order( bad.size() );

        for( size_t i = 0; i < order.size(); i++ )
            order[i] = i;

        std::sort( order.begin(), order.end(), [&]( size_t a, size_t b ) { return bad[a].first > bad[b].first; } );

        for( size_t i = 0; i < order.size() && i < 2 * limit; i++ )
            line( aOut, "return_via", "warning", bad[order[i]].second, where[order[i]] );

        if( bad.size() > 2 * limit )
            line( aOut, "return_via", "warning", std::to_string( bad.size() - 2 * limit ) + " more (plane_limit)" );
    }

    for( const auto& [pair, list] : byPair )
    {
        const VIAM* w = *std::max_element( list.begin(), list.end(),
                                           [&]( const VIAM* a, const VIAM* b ) { return far( a->capDist ) < far( b->capDist ); } );
        line( aOut, "return_via", "info",
              std::to_string( list.size() ) + " via(s) go between planes of " + pair
                      + " (the return current crosses through a capacitor between them); farthest from one: "
                      + tagged( w->via->GetNetCode() ) + " at " + at( w->via->GetPosition() ) + ", "
                      + ( w->capDist < 0 ? std::string( "no such capacitor on the board" )
                                         : w->capRef + " " + mm( w->capDist / 1e6 ) + " mm away" ),
              w->via->GetPosition(), w->capRef.empty() ? std::vector<std::string>{} : std::vector<std::string>{ w->capRef } );
    }
}

} // namespace kicadd
