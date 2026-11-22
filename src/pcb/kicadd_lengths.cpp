/*
 * KiCadD: diff pair and length checks for the board audit (see kicadd_lengths.h).
 *
 * How: lengths come from the board's LENGTH_DELAY_CALCULATION with the net inspector's options,
 * so they are the numbers a human sees in KiCad. Coupling: each member's tracks are sampled every
 * 0.1 mm; a sample's gap is the edge-to-edge distance to the partner's nearest track on the same
 * layer (a grid of the partner's segments). Uncoupled samples within 0.2 mm of each other are one
 * stretch.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_lengths.h"
#include "pcb/kicadd_netnames.h"

#include <board.h>
#include <connectivity/connectivity_data.h>
#include <footprint.h>
#include <length_delay_calculation/length_delay_calculation.h>
#include <netclass.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <ratsnest/ratsnest_data.h>
#include <geometry/seg.h>
#include <geometry/shape_arc.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_map>

namespace kicadd
{

using namespace kicadd::board;
using netnames::GLOBS;

namespace
{

constexpr double STEP = 100000;    // nm between coupling samples
constexpr int    CELL = 1000000;   // nm, the partner's segment grid
constexpr double JOIN = 200000;    // nm: samples this close are one stretch
constexpr double MIN_OFF = 500000; // nm: shorter off-target stretches are where the tracks part

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

std::string nm2mm( double aNm )
{
    return mm( aNm / 1e6 );
}

std::string at( const VECTOR2I& p )
{
    return "(" + mm( p.x / 1e6 ) + ", " + mm( p.y / 1e6 ) + ")";
}

void line( BoardAuditResponse& aOut, const char* aCat, const char* aSeverity, const std::string& aMsg,
           std::optional<VECTOR2I> aAt = std::nullopt )
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
}

struct PIECE
{
    SEG          seg;
    int          width;
    PCB_LAYER_ID layer;
};

struct NETLEN
{
    int                    code = 0;
    std::string            name;
    double                 total = 0, track = 0, via = 0, die = 0;   // nm
    int                    vias = 0, unrouted = 0;
    std::set<PCB_LAYER_ID> layers;
    std::vector<PIECE>     pieces;
    int                    wMin = INT_MAX, wMax = 0;
    std::vector<const BOARD_CONNECTED_ITEM*> ends;   // its pads and vias
};

struct SAMPLE
{
    VECTOR2I p;
    double   len;
    double   gap;   // nm, edge to edge; HUGE_VAL: no partner track beside on this layer
    bool     parallel;   // the partner's nearest track runs along it (not a fan-out to a via or pad)
    int      layer;
};

struct STRETCH
{
    double                     len = 0;
    VECTOR2I                   mid;
    std::vector<const SAMPLE*> samples;
};

/// Groups of samples within JOIN of each other, the longest first.
std::vector<STRETCH> stretches( const std::vector<const SAMPLE*>& aSamples )
{
    const size_t     n = aSamples.size();
    std::vector<int> parent( n );
    std::iota( parent.begin(), parent.end(), 0 );

    std::function<int( int )> find = [&]( int i ) { return parent[i] == i ? i : parent[i] = find( parent[i] ); };

    std::unordered_map<int64_t, std::vector<int>> grid;
    auto key = []( int cx, int cy ) { return ( int64_t( cx ) << 32 ) ^ uint32_t( cy ); };

    for( size_t i = 0; i < n; i++ )
    {
        const int cx = (int) std::floor( aSamples[i]->p.x / JOIN ), cy = (int) std::floor( aSamples[i]->p.y / JOIN );

        for( int dx = -1; dx <= 1; dx++ )
        {
            for( int dy = -1; dy <= 1; dy++ )
            {
                auto it = grid.find( key( cx + dx, cy + dy ) );

                if( it == grid.end() )
                    continue;

                for( int j : it->second )
                {
                    if( ( aSamples[j]->p - aSamples[i]->p ).EuclideanNorm() <= JOIN )
                        parent[find( (int) i )] = find( j );
                }
            }
        }

        grid[key( cx, cy )].push_back( (int) i );
    }

    std::map<int, std::vector<int>> groups;

    for( size_t i = 0; i < n; i++ )
        groups[find( (int) i )].push_back( (int) i );

    std::vector<STRETCH> out;

    for( const auto& [root, members] : groups )
    {
        STRETCH st;

        for( int i : members )
        {
            st.len += aSamples[i]->len;
            st.samples.push_back( aSamples[i] );
        }

        st.mid = aSamples[members[members.size() / 2]]->p;
        out.push_back( std::move( st ) );
    }

    std::sort( out.begin(), out.end(), []( const STRETCH& a, const STRETCH& b ) { return a.len > b.len; } );
    return out;
}

/// Sample aMine's tracks against aOther's on the same layer (gaps up to aReach, else HUGE_VAL).
std::vector<SAMPLE> sample( const NETLEN& aMine, const NETLEN& aOther, double aReach )
{
    std::unordered_map<int64_t, std::vector<int>> grid;   // (layer, cell) -> aOther's pieces
    auto key = []( int layer, int cx, int cy ) { return ( int64_t( layer ) << 48 ) ^ ( int64_t( cx & 0xFFFFFF ) << 24 ) ^ ( cy & 0xFFFFFF ); };

    for( size_t i = 0; i < aOther.pieces.size(); i++ )
    {
        const SEG& s = aOther.pieces[i].seg;
        const int  x0 = std::min( s.A.x, s.B.x ) / CELL, x1 = std::max( s.A.x, s.B.x ) / CELL;
        const int  y0 = std::min( s.A.y, s.B.y ) / CELL, y1 = std::max( s.A.y, s.B.y ) / CELL;

        for( int cx = x0; cx <= x1; cx++ )
            for( int cy = y0; cy <= y1; cy++ )
                grid[key( aOther.pieces[i].layer, cx, cy )].push_back( (int) i );
    }

    const int           ring = (int) std::ceil( ( aReach + aOther.wMax ) / CELL );
    std::vector<SAMPLE> out;
    std::vector<int>    stamp( aOther.pieces.size(), -1 );
    int                 query = 0;

    for( const PIECE& piece : aMine.pieces )
    {
        const double len = piece.seg.Length();
        const int    n = std::max( 1, (int) std::ceil( len / STEP ) );

        for( int k = 0; k < n; k++, query++ )
        {
            const VECTOR2I p = piece.seg.A + VECTOR2I( VECTOR2D( piece.seg.B - piece.seg.A ) * ( ( k + 0.5 ) / n ) );
            const int      cx = p.x / CELL, cy = p.y / CELL;
            double         best = HUGE_VAL;
            int            bestPiece = -1;

            for( int dx = -ring; dx <= ring; dx++ )
            {
                for( int dy = -ring; dy <= ring; dy++ )
                {
                    auto it = grid.find( key( piece.layer, cx + dx, cy + dy ) );

                    if( it == grid.end() )
                        continue;

                    for( int i : it->second )
                    {
                        if( stamp[i] == query )
                            continue;

                        stamp[i] = query;
                        const double gap = aOther.pieces[i].seg.Distance( p ) - ( piece.width + aOther.pieces[i].width ) / 2.0;
                        if( gap < best )
                        {
                            best = gap;
                            bestPiece = i;
                        }
                    }
                }
            }

            bool parallel = false;

            if( bestPiece >= 0 )   // within 10 degrees
            {
                const VECTOR2D a = VECTOR2D( piece.seg.B - piece.seg.A ), b = VECTOR2D( aOther.pieces[bestPiece].seg.B - aOther.pieces[bestPiece].seg.A );
                const double   n2 = a.EuclideanNorm() * b.EuclideanNorm();
                parallel = n2 > 0 && std::abs( a.x * b.y - a.y * b.x ) / n2 < 0.17;
            }

            out.push_back( { p, len / n, best <= aReach ? std::max( 0.0, best ) : HUGE_VAL, parallel, piece.layer } );
        }
    }

    return out;
}

struct PAIR
{
    /// The gap on one layer: its target (the class's, else the layer's median), what counts as
    /// coupled, the range of the middle 90 % of the coupled length.
    struct GAP
    {
        double target = -1, coupleMax = -1, lo = -1, hi = -1, len = 0;
    };

    const NETLEN*       p = nullptr;
    const NETLEN*       n = nullptr;
    std::string         name, className;
    double              skew = 0;
    bool                classTarget = false;
    std::map<int, GAP>  gaps;   // layer ->
    std::vector<SAMPLE> sp, sn;
    double              uncoupled = 0, longest = 0;
    VECTOR2I            longestAt;

    const GAP* gapOn( int aLayer ) const
    {
        auto it = gaps.find( aLayer );
        return it == gaps.end() || it->second.target < 0 ? nullptr : &it->second;
    }

    bool coupled( const SAMPLE& s ) const
    {
        const GAP* g = gapOn( s.layer );
        return g && s.gap <= g->coupleMax;
    }

    bool beside() const
    {
        for( const auto& [layer, g] : gaps )
        {
            if( g.target >= 0 )
                return true;
        }

        return false;
    }

    double coupleMax() const
    {
        double m = 0;

        for( const auto& [layer, g] : gaps )
            m = std::max( m, g.coupleMax );

        return m;
    }

    /// A stretch that runs into one of the pair's pads or vias: a breakout, where the gap follows
    /// the pins' or the vias' pitch.
    bool atEnd( const STRETCH& aStretch ) const
    {
        for( const SAMPLE* smp : aStretch.samples )
        {
            for( const NETLEN* m : { p, n } )
            {
                for( const BOARD_CONNECTED_ITEM* e : m->ends )
                {
                    if( e->HitTest( smp->p, (int) JOIN ) )
                        return true;
                }
            }
        }

        return false;
    }

    /// Off-target stretches (at least MIN_OFF, the tracks parallel) for a tolerance: (the worse member's total, the
    /// worst gap, where).
    std::tuple<double, const SAMPLE*> off( double aTol ) const
    {
        double        worstLen = 0, worstDev = -1;
        const SAMPLE* worst = nullptr;

        for( const std::vector<SAMPLE>* list : { &sp, &sn } )
        {
            std::vector<const SAMPLE*> bad;

            for( const SAMPLE& s : *list )
            {
                if( coupled( s ) && s.parallel && std::abs( s.gap - gapOn( s.layer )->target ) > aTol )
                    bad.push_back( &s );
            }

            double                     total = 0;
            std::vector<const SAMPLE*> counted;

            for( const STRETCH& st : stretches( bad ) )
            {
                if( st.len < MIN_OFF || atEnd( st ) )
                    continue;

                total += st.len;
                counted.insert( counted.end(), st.samples.begin(), st.samples.end() );
            }

            if( total > worstLen )
            {
                worstLen = total;

                for( const SAMPLE* s : counted )
                {
                    const double dev = std::abs( s->gap - gapOn( s->layer )->target );

                    if( dev > worstDev )
                    {
                        worstDev = dev;
                        worst = s;
                    }
                }
            }
        }

        return { worstLen, worst };
    }
};

} // namespace


void CheckLengths( BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    const size_t limit = aReq.length_limit() > 0 ? aReq.length_limit() : 10;
    const bool   rows = aReq.length_limit() > 0 || aReq.length_nets_size() > 0;

    GLOBS picked, skewLimit, uncoupledLimit, gapTol, matchTol, maxLen, minLen, anyLength;

    for( const std::string& g : aReq.length_nets() )
        picked.add( g, 0 );

    for( const auto& [g, v] : aReq.skew() )
        skewLimit.add( g, v );

    for( const auto& [g, v] : aReq.uncoupled() )
        uncoupledLimit.add( g, v );

    for( const auto& [g, v] : aReq.pair_gap() )
        gapTol.add( g, v );

    for( const auto& [g, v] : aReq.length_match() )
        matchTol.add( g, v );

    for( const auto& [g, v] : aReq.max_length() )
        maxLen.add( g, v );

    for( const auto& [g, v] : aReq.min_length() )
        minLen.add( g, v );

    for( const GLOBS* list : { &picked, &matchTol, &maxLen, &minLen } )
        for( const GLOBS::GLOB& g : list->list )
            anyLength.add( g.pattern, 0 );

    // ---- which nets: the pairs (both members on the board), and any net a length glob names
    std::map<std::string, int> codes;

    for( NETINFO_ITEM* n : aBoard.GetNetInfo() )
    {
        if( n->GetNetCode() > 0 )
            codes[str( n->GetNetname() )] = n->GetNetCode();
    }

    std::map<int, int> partner;   // P -> N, positive member first
    std::set<int>      wanted;

    for( const auto& [name, code] : codes )
    {
        const std::string other = netnames::PartnerName( name );
        auto              it = other.empty() ? codes.end() : codes.find( other );

        if( it != codes.end() && !netnames::NegativeMember( name ) )
        {
            if( !picked.empty() && !picked.match( name ) && !picked.match( other ) )
                continue;

            partner[code] = it->second;
            wanted.insert( code );
            wanted.insert( it->second );
        }
    }

    std::string             pairOrder;   // said after the diff pair header
    std::optional<VECTOR2I> pairOrderAt;

    // ---- pair order: between two parts the pair's P and N must not swap sides (the straight
    // lines P-P and N-N crossing means a crossing to route: a via pair, or the part turned,
    // moved or on the other side; a part on the bottom is mirrored). Routed or not.
    {
        int                      checked = 0;
        std::vector<std::string> crossed;
        std::optional<VECTOR2I>  firstAt;

        for( auto [pc, nc] : partner )
        {
            // per part: its P pad and its N pad (the first of each)
            std::map<const FOOTPRINT*, std::pair<const PAD*, const PAD*>> ends;

            for( FOOTPRINT* fp : aBoard.Footprints() )
            {
                for( PAD* pad : fp->Pads() )
                {
                    if( pad->GetNetCode() == pc && !ends[fp].first )
                        ends[fp].first = pad;
                    else if( pad->GetNetCode() == nc && !ends[fp].second )
                        ends[fp].second = pad;
                }
            }

            std::vector<std::pair<const FOOTPRINT*, std::pair<const PAD*, const PAD*>>> both;

            for( const auto& [fp, pn] : ends )
            {
                if( pn.first && pn.second )
                    both.push_back( { fp, pn } );
            }

            if( both.size() < 2 )
                continue;

            checked++;
            std::string which;

            for( size_t a = 0; a < both.size(); ++a )
            {
                for( size_t b = a + 1; b < both.size(); ++b )
                {
                    SEG ps( both[a].second.first->GetPosition(), both[b].second.first->GetPosition() );
                    SEG ns( both[a].second.second->GetPosition(), both[b].second.second->GetPosition() );

                    if( OPT_VECTOR2I x = ps.Intersect( ns ) )
                    {
                        auto name = [&]( const FOOTPRINT* f )
                        {
                            return str( f->GetReference() ) + ( f->IsFlipped() ? " (bottom)" : "" );
                        };

                        which += ( which.empty() ? "" : ", " ) + name( both[a].first ) + " - " + name( both[b].first );

                        if( !firstAt )
                            firstAt = *x;
                    }
                }
            }

            if( !which.empty() && crossed.size() < 12 )
            {
                crossed.push_back( str( aBoard.FindNet( pc )->GetNetname() ) + " / " + str( aBoard.FindNet( nc )->GetNetname() )
                                   + ": " + which );
            }
            else if( !which.empty() )
            {
                crossed.push_back( "" );
            }
        }

        if( checked > 0 )
        {
            if( crossed.empty() )
            {
                pairOrder = "pair order: P and N in the same order at both ends of all " + std::to_string( checked )
                            + " pair(s) between parts";
            }
            else
            {
                std::string list;
                size_t      shown = 0;

                for( const std::string& c : crossed )
                {
                    if( !c.empty() && shown++ < 6 )
                        list += ( list.empty() ? "" : "; " ) + c;
                }

                pairOrderAt = firstAt;
                pairOrder = ( "pair order: " + std::to_string( crossed.size() ) + " of " + std::to_string( checked )
                              + " pair(s) have P and N swapped between two of their parts (the straight lines P-P and "
                                "N-N cross: a crossing to route, a via pair; or turn, move or flip a part; a part on the "
                                "bottom is mirrored): " + list + ( crossed.size() > 6 ? "; ..." : "" ) );
            }
        }
    }

    std::set<std::string> usedGlobs;

    for( const auto& [name, code] : codes )
    {
        for( const GLOBS::GLOB& g : anyLength.list )
        {
            if( GLOBS::matches( g, name ) )
            {
                wanted.insert( code );
                usedGlobs.insert( g.pattern );
            }
        }
    }

    for( const GLOBS::GLOB& g : anyLength.list )
    {
        if( !usedGlobs.count( g.pattern ) )
            line( aOut, "length", "warning", "no net matches " + g.pattern );
    }

    if( wanted.empty() )
        return;

    // ---- lengths, as the net inspector measures them
    LENGTH_DELAY_CALCULATION*                                   calc = aBoard.GetLengthCalculation();
    std::map<int, std::vector<LENGTH_DELAY_CALCULATION_ITEM>>   items;
    std::map<int, NETLEN>                                       nets;

    for( int code : wanted )
    {
        nets[code].code = code;
        nets[code].name = str( aBoard.FindNet( code )->GetNetname() );
    }

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        auto it = nets.find( t->GetNetCode() );

        if( it == nets.end() )
            continue;

        items[t->GetNetCode()].push_back( calc->GetLengthCalculationItem( t ) );
        NETLEN& n = it->second;

        if( t->Type() == PCB_VIA_T )
        {
            n.vias++;
            n.ends.push_back( t );
            continue;
        }

        n.layers.insert( t->GetLayer() );
        n.wMin = std::min( n.wMin, t->GetWidth() );
        n.wMax = std::max( n.wMax, t->GetWidth() );

        if( t->Type() == PCB_ARC_T )
        {
            const PCB_ARC*         arc = static_cast<const PCB_ARC*>( t );
            const SHAPE_LINE_CHAIN chain = SHAPE_ARC( arc->GetStart(), arc->GetMid(), arc->GetEnd(), 0 ).ConvertToPolyline();

            for( int i = 0; i + 1 < chain.PointCount(); i++ )
                n.pieces.push_back( { SEG( chain.CPoint( i ), chain.CPoint( i + 1 ) ), t->GetWidth(), t->GetLayer() } );
        }
        else
        {
            n.pieces.push_back( { SEG( t->GetStart(), t->GetEnd() ), t->GetWidth(), t->GetLayer() } );
        }
    }

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( nets.count( pad->GetNetCode() ) )
            {
                items[pad->GetNetCode()].push_back( calc->GetLengthCalculationItem( pad ) );
                nets[pad->GetNetCode()].ends.push_back( pad );
            }
        }
    }

    constexpr PATH_OPTIMISATIONS opts = { .OptimiseVias = true, .MergeTracks = true, .OptimiseTracesInPads = true,
                                          .InferViaInPad = false };

    std::shared_ptr<CONNECTIVITY_DATA> conn = aBoard.GetConnectivity();

    for( auto& [code, n] : nets )
    {
        if( RN_NET* rn = conn->GetRatsnestForNet( code ) )
            n.unrouted = (int) rn->GetEdges().size();

        if( n.pieces.empty() && !n.vias )
            continue;

        LENGTH_DELAY_STATS s = calc->CalculateLengthDetails( items[code], opts );
        n.track = (double) s.TrackLength;
        n.via = s.ViaLength;
        n.die = s.PadToDieLength;
        n.total = (double) s.TotalLength();
    }

    auto routed = [&]( int aCode ) { return !nets[aCode].pieces.empty(); };

    auto unrouted = [&]( const NETLEN& n )
    {
        return n.unrouted ? " (" + std::to_string( n.unrouted ) + " connection(s) unrouted: length so far)" : std::string();
    };

    // ---- the pairs
    std::vector<PAIR> pairs;
    int               notRouted = 0;

    for( const auto& [pc, nc] : partner )
    {
        if( !routed( pc ) && !routed( nc ) )
        {
            notRouted++;
            continue;
        }

        PAIR pr;
        pr.p = &nets[pc];
        pr.n = &nets[nc];
        pr.name = pr.p->name + "/" + pr.n->name.back() + " (pair)";
        pr.skew = std::abs( pr.p->total - pr.n->total );

        const NETCLASS* nc_ = aBoard.FindNet( pc )->GetNetClass();
        pr.className = nc_ ? str( nc_->GetName() ) : "";
        const int width = std::max( pr.p->wMax, pr.n->wMax );

        // the class's gap is the target when a class of its own sets it (not inherited from Default,
        // whose gap is rarely meant for a pair); else each layer's median (the gap for an impedance
        // differs between outer and inner layers)
        const NETCLASS* setBy = nc_ && nc_->HasDiffPairGap() ? nc_->GetDiffPairGapParent() : nullptr;
        double          classGap = -1;

        if( setBy && str( setBy->GetName() ) != NETCLASS::Default && nc_->GetDiffPairGap() > 0 )
        {
            classGap = nc_->GetDiffPairGap();
            pr.classTarget = true;
            pr.className = str( setBy->GetName() );
        }

        // reach: a gap this far still counts as beside the partner (for the median and the target)
        const double reach = std::max( { 1e6, 4.0 * width, classGap > 0 ? 2 * classGap + 1e5 : 0.0 } );
        pr.sp = sample( *pr.p, *pr.n, reach );
        pr.sn = sample( *pr.n, *pr.p, reach );

        // (gap, length) by layer, sorted; the weighted quantile of a list
        auto quantile = []( const std::vector<std::pair<double, double>>& aList, double aQ )
        {
            double total = 0, run = 0;

            for( const auto& [gap, len] : aList )
                total += len;

            for( const auto& [gap, len] : aList )
            {
                run += len;

                if( run >= aQ * total )
                    return gap;
            }

            return aList.empty() ? -1.0 : aList.back().first;
        };

        std::map<int, std::vector<std::pair<double, double>>> nearby;

        for( const std::vector<SAMPLE>* list : { &pr.sp, &pr.sn } )
        {
            for( const SAMPLE& s : *list )
            {
                if( s.gap < HUGE_VAL )
                    nearby[s.layer].push_back( { s.gap, s.len } );
            }
        }

        for( auto& [layer, list] : nearby )
        {
            std::sort( list.begin(), list.end() );
            PAIR::GAP& g = pr.gaps[layer];
            g.target = classGap > 0 ? classGap : quantile( list, 0.5 );
            g.coupleMax = std::max( 2 * g.target, g.target + 1e5 );
        }

        // the gap where coupled: the range of the middle 90 % of the coupled length (where the
        // tracks part at their ends the gap runs up to coupleMax)
        std::map<int, std::vector<std::pair<double, double>>> gaps;

        for( const std::vector<SAMPLE>* list : { &pr.sp, &pr.sn } )
        {
            std::vector<const SAMPLE*> loose;
            double                     total = 0;

            for( const SAMPLE& s : *list )
            {
                if( pr.coupled( s ) )
                {
                    gaps[s.layer].push_back( { s.gap, s.len } );
                }
                else
                {
                    loose.push_back( &s );
                    total += s.len;
                }
            }

            pr.uncoupled = std::max( pr.uncoupled, total );

            std::vector<STRETCH> st = stretches( loose );

            if( !st.empty() && st[0].len > pr.longest )
            {
                pr.longest = st[0].len;
                pr.longestAt = st[0].mid;
            }
        }

        for( auto& [layer, list] : gaps )
        {
            std::sort( list.begin(), list.end() );
            PAIR::GAP& g = pr.gaps[layer];
            g.lo = quantile( list, 0.05 );
            g.hi = quantile( list, 0.95 );

            for( const auto& [gap, len] : list )
                g.len += len;
        }

        pairs.push_back( std::move( pr ) );
    }

    aOut.set_diff_pairs( (int) pairs.size() );

    auto layers = [&]( const NETLEN& n )
    {
        std::string s;

        for( PCB_LAYER_ID l : n.layers )
            s += ( s.empty() ? "" : ", " ) + str( aBoard.GetLayerName( l ) );

        return s;
    };

    auto pairRow = [&]( const PAIR& pr )
    {
        std::string t = pr.name + ": " + nm2mm( pr.p->total ) + " / " + nm2mm( pr.n->total ) + " mm, skew "
                        + nm2mm( pr.skew ) + " mm; ";

        if( !pr.beside() )
        {
            t += "never beside each other";
        }
        else
        {
            // per layer, the most coupled first: "0.12 mm In7.Cu, 0.18-0.2 F.Cu"
            std::vector<std::pair<double, int>> order;

            for( const auto& [layer, g] : pr.gaps )
            {
                if( g.len > 0 )
                    order.push_back( { -g.len, layer } );
            }

            std::sort( order.begin(), order.end() );
            std::string ranges, medians;

            for( const auto& [len, layer] : order )
            {
                const PAIR::GAP& g = pr.gaps.at( layer );
                const std::string ln = order.size() > 1 ? " " + str( aBoard.GetLayerName( PCB_LAYER_ID( layer ) ) ) : "";
                ranges += ( ranges.empty() ? "" : ", " ) + nm2mm( g.lo ) + ( g.hi > g.lo + 5000 ? "-" + nm2mm( g.hi ) : "" ) + ln;
                medians += ( medians.empty() ? "" : ", " ) + nm2mm( g.target ) + ln;
            }

            t += "gap " + ranges + " mm on 90% of the coupled length ("
                 + ( pr.classTarget ? "target " + nm2mm( pr.gaps.begin()->second.target ) + ", class " + pr.className
                                    : "median " + medians )
                 + ")";
        }

        t += ", width " + nm2mm( std::min( pr.p->wMin, pr.n->wMin ) )
             + ( std::max( pr.p->wMax, pr.n->wMax ) > std::min( pr.p->wMin, pr.n->wMin )
                         ? "-" + nm2mm( std::max( pr.p->wMax, pr.n->wMax ) )
                         : "" )
             + " mm; uncoupled " + nm2mm( pr.uncoupled ) + " mm";

        if( pr.longest > 0 && pr.longest < pr.uncoupled - 5000 )
            t += " (longest " + nm2mm( pr.longest ) + " at " + at( pr.longestAt ) + ")";
        else if( pr.longest > 0 )
            t += " (at " + at( pr.longestAt ) + ")";

        t += "; vias " + std::to_string( pr.p->vias ) + " / " + std::to_string( pr.n->vias ) + "; " + layers( *pr.p );

        if( pr.p->unrouted || pr.n->unrouted )
            t += "; " + std::to_string( pr.p->unrouted + pr.n->unrouted ) + " connection(s) unrouted (length so far)";

        return t;
    };

    // a pair's rule: the better of its members' globs
    auto pairRule = [&]( const GLOBS& aGlobs, const PAIR& pr ) -> const GLOBS::GLOB*
    {
        const GLOBS::GLOB* a = aGlobs.match( pr.p->name );
        const GLOBS::GLOB* b = aGlobs.match( pr.n->name );
        return !a ? b : !b ? a : a->rank >= b->rank ? a : b;
    };

    // ---- diff_pair
    if( !pairs.empty() || notRouted )
    {
        const bool judging = !skewLimit.empty() || !uncoupledLimit.empty() || !gapTol.empty();

        std::string head = std::to_string( pairs.size() ) + " routed diff pair(s)"
                           + ( notRouted ? " (" + std::to_string( notRouted ) + " not routed yet)" : "" );

        if( !pairs.empty() )
        {
            const PAIR& ws = *std::max_element( pairs.begin(), pairs.end(),
                                                []( const PAIR& a, const PAIR& b ) { return a.skew < b.skew; } );
            const PAIR& wu = *std::max_element( pairs.begin(), pairs.end(),
                                                []( const PAIR& a, const PAIR& b ) { return a.uncoupled < b.uncoupled; } );
            head += ": worst skew " + nm2mm( ws.skew ) + " mm (" + ws.name + "), most uncoupled " + nm2mm( wu.uncoupled )
                    + " mm (" + wu.name + "). Lengths as KiCad's net inspector";

            if( !judging )
            {
                head += ". No verdicts: judge them, or pass skew / uncoupled / pair_gap {glob: mm}";

                if( !rows )
                    head += ". The worst skew: " + pairRow( ws ) + ". Per pair: length_limit / length_nets";
            }
        }

        line( aOut, "diff_pair", "info", head );

        if( !pairOrder.empty() )
            line( aOut, "diff_pair", "info", pairOrder, pairOrderAt );

        if( !judging && rows )
        {
            std::vector<const PAIR*> list;

            for( const PAIR& pr : pairs )
                list.push_back( &pr );

            std::sort( list.begin(), list.end(), []( const PAIR* a, const PAIR* b ) { return a->skew > b->skew; } );

            for( size_t i = 0; i < list.size() && i < limit; i++ )
                line( aOut, "diff_pair", "info", pairRow( *list[i] ),
                      list[i]->p->pieces.empty() ? std::nullopt : std::optional<VECTOR2I>( list[i]->p->pieces[0].seg.A ) );

            if( list.size() > limit )
                line( aOut, "diff_pair", "info", std::to_string( list.size() - limit ) + " more (length_limit)" );
        }

        // the agent's limits: per kind, rule by rule (the tightest first), a count line each, then
        // the worst pairs; 3 x length_limit rows in all
        size_t budget = 3 * limit;

        struct OVER
        {
            const PAIR* pr;
            double      excess;
            std::string text;
            VECTOR2I    where;
        };

        auto judge = [&]( const char* aKind, const GLOBS& aGlobs, auto&& aMeasure )
        {
            std::map<const GLOBS::GLOB*, int>               judged;
            std::map<const GLOBS::GLOB*, std::vector<OVER>> over;

            for( const PAIR& pr : pairs )
            {
                if( const GLOBS::GLOB* g = pairRule( aGlobs, pr ) )
                {
                    judged[g]++;

                    if( std::optional<OVER> o = aMeasure( pr, *g ) )
                        over[g].push_back( *o );
                }
            }

            std::vector<const GLOBS::GLOB*> rules;

            for( const auto& [g, n] : judged )
                rules.push_back( g );

            std::sort( rules.begin(), rules.end(), []( const GLOBS::GLOB* a, const GLOBS::GLOB* b )
                       { return a->value != b->value ? a->value < b->value : a->pattern < b->pattern; } );

            for( const GLOBS::GLOB& g : aGlobs.list )
            {
                if( !judged.count( &g ) )
                    line( aOut, "diff_pair", "info", std::string( aKind ) + " " + g.pattern + ": no routed pair matches" );
            }

            for( const GLOBS::GLOB* g : rules )
            {
                std::vector<OVER>& bad = over[g];
                line( aOut, "diff_pair", bad.empty() ? "info" : "warning",
                      std::string( aKind ) + " " + g->pattern + " (" + ( std::string( aKind ) == "pair_gap" ? "±" : "max " )
                              + mm( g->value, 3 ) + " mm): " + std::to_string( judged[g] ) + " pair(s) judged, "
                              + ( bad.empty() ? std::string( "all within" ) : std::to_string( bad.size() ) + " over" ) );

                std::sort( bad.begin(), bad.end(), []( const OVER& a, const OVER& b ) { return a.excess > b.excess; } );
                size_t shown = 0;

                for( ; shown < bad.size() && shown < limit && budget > 0; shown++, budget-- )
                    line( aOut, "diff_pair", "warning", bad[shown].text, bad[shown].where );

                if( shown < bad.size() )
                    line( aOut, "diff_pair", "warning",
                          std::to_string( bad.size() - shown ) + " more pair(s) over for " + g->pattern + " (length_limit)" );
            }
        };

        auto firstAt = []( const PAIR& pr )
        { return pr.p->pieces.empty() ? VECTOR2I() : pr.p->pieces[pr.p->pieces.size() / 2].seg.A; };

        judge( "skew", skewLimit,
               [&]( const PAIR& pr, const GLOBS::GLOB& g ) -> std::optional<OVER>
               {
                   if( pr.skew <= g.value * 1e6 )
                       return std::nullopt;

                   const bool        pLong = pr.p->total > pr.n->total;
                   const NETLEN&     shorter = pLong ? *pr.n : *pr.p;
                   return OVER{ &pr, pr.skew - g.value * 1e6,
                                pr.name + ": skew " + nm2mm( pr.skew ) + " mm (" + nm2mm( pr.p->total ) + " / "
                                        + nm2mm( pr.n->total ) + "; " + shorter.name + " needs +"
                                        + nm2mm( pr.skew - g.value * 1e6 ) + " to " + nm2mm( pr.skew ) + " mm)"
                                        + ( pr.p->unrouted || pr.n->unrouted ? "; not fully routed" : "" ),
                                firstAt( pr ) };
               } );

        judge( "uncoupled", uncoupledLimit,
               [&]( const PAIR& pr, const GLOBS::GLOB& g ) -> std::optional<OVER>
               {
                   if( pr.uncoupled <= g.value * 1e6 )
                       return std::nullopt;

                   return OVER{ &pr, pr.uncoupled - g.value * 1e6,
                                pr.name + ": uncoupled " + nm2mm( pr.uncoupled ) + " mm"
                                        + ( !pr.beside() ? std::string( " (never beside each other)" )
                                                         : " (longest " + nm2mm( pr.longest ) + " at " + at( pr.longestAt )
                                                                   + "; coupled: gap up to " + nm2mm( pr.coupleMax() ) + ")" ),
                                pr.longestAt };
               } );

        judge( "pair_gap", gapTol,
               [&]( const PAIR& pr, const GLOBS::GLOB& g ) -> std::optional<OVER>
               {
                   if( !pr.beside() )
                       return std::nullopt;

                   auto [len, worst] = pr.off( g.value * 1e6 );

                   if( len <= 0 || !worst )
                       return std::nullopt;

                   const std::string layer = str( aBoard.GetLayerName( PCB_LAYER_ID( worst->layer ) ) );
                   return OVER{ &pr, len,
                                pr.name + ": gap off its target by more than " + mm( g.value, 3 ) + " mm for " + nm2mm( len )
                                        + " mm away from its pads and vias; worst " + nm2mm( worst->gap ) + " at "
                                        + at( worst->p ) + " on " + layer + " (target " + nm2mm( pr.gapOn( worst->layer )->target )
                                        + ( pr.classTarget ? ", class " + pr.className : ", the layer's median" ) + ")",
                                worst->p };
               } );
    }

    // ---- length
    if( !rows && matchTol.empty() && maxLen.empty() && minLen.empty() )
        return;

    // units: a net, or a pair as one (for the listing and the groups)
    struct UNIT
    {
        std::string   name;
        const NETLEN* a = nullptr;
        const NETLEN* b = nullptr;   // the pair's other member
        double        length() const { return b ? ( a->total + b->total ) / 2 : a->total; }
    };

    std::vector<UNIT> units;
    std::set<int>     inPair;

    for( const PAIR& pr : pairs )
    {
        units.push_back( { pr.name, pr.p, pr.n } );
        inPair.insert( pr.p->code );
        inPair.insert( pr.n->code );
    }

    for( const auto& [code, n] : nets )
    {
        if( !inPair.count( code ) && !n.pieces.empty() )
            units.push_back( { n.name, &n, nullptr } );
    }

    auto unitMatches = [&]( const GLOBS::GLOB& g, const UNIT& u )
    { return GLOBS::matches( g, u.a->name ) || ( u.b && GLOBS::matches( g, u.b->name ) ); };

    auto netRow = [&]( const NETLEN& n )
    {
        std::string t = n.name + ": " + nm2mm( n.total ) + " mm";

        if( n.vias || n.die > 0 )
            t += " (track " + nm2mm( n.track ) + ", " + std::to_string( n.vias ) + " via(s) " + nm2mm( n.via ) + " mm"
                 + ( n.die > 0 ? ", pad-to-die " + nm2mm( n.die ) : "" ) + ")";

        return t + "; " + layers( n ) + unrouted( n );
    };

    size_t lengthRows = 0;
    aOut.set_length_nets( (int) units.size() );

    if( !picked.empty() )
    {
        std::vector<const UNIT*> list;

        for( const UNIT& u : units )
        {
            bool match = false;

            for( const GLOBS::GLOB& g : picked.list )
                match = match || unitMatches( g, u );

            if( match )
                list.push_back( &u );
        }

        std::sort( list.begin(), list.end(), []( const UNIT* a, const UNIT* b ) { return a->length() > b->length(); } );

        line( aOut, "length", "info",
              std::to_string( list.size() ) + " net(s) / pair(s) of length_nets routed, longest first"
                      + ( list.empty() ? std::string() : ": " + nm2mm( list.front()->length() ) + " to " + nm2mm( list.back()->length() ) + " mm" ) );

        for( ; lengthRows < list.size() && lengthRows < limit; lengthRows++ )
        {
            const UNIT* u = list[lengthRows];
            line( aOut, "length", "info",
                  u->b ? u->name + ": " + nm2mm( u->a->total ) + " / " + nm2mm( u->b->total ) + " mm"
                                 + ( u->a->vias + u->b->vias ? ", vias " + std::to_string( u->a->vias ) + " / " + std::to_string( u->b->vias ) : "" )
                                 + "; " + layers( *u->a ) + unrouted( *u->a )
                       : netRow( *u->a ) );
        }

        if( list.size() > lengthRows )
            line( aOut, "length", "info", std::to_string( list.size() - lengthRows ) + " more (length_limit)" );
    }

    // length_match: each glob a group; what each short net lacks against the longest
    for( const GLOBS::GLOB& g : matchTol.list )
    {
        std::vector<const UNIT*> group;

        for( const UNIT& u : units )
        {
            if( unitMatches( g, u ) )
                group.push_back( &u );
        }

        if( group.empty() && !usedGlobs.count( g.pattern ) )
            continue;   // "no net matches" said above

        if( group.size() < 2 )
        {
            line( aOut, "length", "info",
                  "length_match " + g.pattern + ": " + std::to_string( group.size() ) + " routed net(s) / pair(s), nothing to match" );
            continue;
        }

        std::sort( group.begin(), group.end(), []( const UNIT* a, const UNIT* b ) { return a->length() > b->length(); } );
        const double longest = group.front()->length(), tol = g.value * 1e6;
        std::vector<const UNIT*> shortNets;

        for( const UNIT* u : group )
        {
            if( longest - u->length() > tol )
                shortNets.push_back( u );
        }

        bool partial = false;

        for( const UNIT* u : group )
            partial = partial || u->a->unrouted || ( u->b && u->b->unrouted );

        line( aOut, "length", shortNets.empty() ? "info" : "warning",
              "length_match " + g.pattern + " (within " + mm( g.value ) + " mm): " + std::to_string( group.size() )
                      + " net(s) / pair(s), " + nm2mm( group.back()->length() ) + " (" + group.back()->name + ") to "
                      + nm2mm( longest ) + " mm (" + group.front()->name + "), "
                      + ( shortNets.empty() ? std::string( "all within" )
                                            : std::to_string( shortNets.size() ) + " shorter than " + nm2mm( longest - tol )
                                                      + " mm (the longest less " + mm( g.value ) + ")" )
                      + ( partial ? "; some not fully routed" : "" ) );

        std::reverse( shortNets.begin(), shortNets.end() );   // the shortest first
        size_t shown = 0;

        for( ; shown < shortNets.size() && shown < limit; shown++ )
        {
            const UNIT* u = shortNets[shown];
            line( aOut, "length", "warning",
                  u->name + ": " + nm2mm( u->length() ) + " mm" + ( u->b ? " (mean)" : "" ) + ", needs +"
                          + nm2mm( longest - tol - u->length() ) + " to +" + nm2mm( longest - u->length() ) + " mm"
                          + unrouted( *u->a ) );
        }

        if( shown < shortNets.size() )
            line( aOut, "length", "warning", std::to_string( shortNets.size() - shown ) + " more short for " + g.pattern + " (length_limit)" );
    }

    // max_length / min_length: each net by its best glob
    for( bool isMax : { true, false } )
    {
        const GLOBS& globs = isMax ? maxLen : minLen;

        if( globs.empty() )
            continue;

        std::map<const GLOBS::GLOB*, int>                                       judged;
        std::map<const GLOBS::GLOB*, std::vector<std::pair<double, const NETLEN*>>> over;

        for( const auto& [code, n] : nets )
        {
            if( n.pieces.empty() )
                continue;

            if( const GLOBS::GLOB* g = globs.match( n.name ) )
            {
                judged[g]++;
                const double excess = isMax ? n.total - g->value * 1e6 : g->value * 1e6 - n.total;

                if( excess > 0 )
                    over[g].push_back( { excess, &n } );
            }
        }

        std::vector<const GLOBS::GLOB*> rules;

        for( const auto& [g, k] : judged )
            rules.push_back( g );

        std::sort( rules.begin(), rules.end(), []( const GLOBS::GLOB* a, const GLOBS::GLOB* b ) { return a->pattern < b->pattern; } );
        const std::string kind = isMax ? "max_length" : "min_length";

        for( const GLOBS::GLOB* g : rules )
        {
            auto& bad = over[g];
            line( aOut, "length", bad.empty() ? "info" : "warning",
                  kind + " " + g->pattern + " (" + mm( g->value ) + " mm): " + std::to_string( judged[g] ) + " net(s) judged, "
                          + ( bad.empty() ? std::string( "all within" )
                                          : std::to_string( bad.size() ) + ( isMax ? " longer" : " shorter" ) ) );

            std::sort( bad.begin(), bad.end(), []( const auto& a, const auto& b ) { return a.first > b.first; } );
            size_t shown = 0;

            for( ; shown < bad.size() && shown < limit; shown++ )
                line( aOut, "length", "warning",
                      bad[shown].second->name + ": " + nm2mm( bad[shown].second->total ) + " mm, "
                              + nm2mm( bad[shown].first ) + ( isMax ? " over" : " short" ) + unrouted( *bad[shown].second ) );

            if( shown < bad.size() )
                line( aOut, "length", "warning", std::to_string( bad.size() - shown ) + " more for " + g->pattern + " (length_limit)" );
        }
    }
}

} // namespace kicadd
