/*
 * KiCadD M5c: automatic placement. See kicadd_autoplace.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_autoplace.h"

#include <trigo.h>
#include <geometry/eda_angle.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

namespace kicadd::autoplace
{

namespace
{

VECTOR2I rotated( const VECTOR2I& aOffset, int aRot )
{
    VECTOR2I p = aOffset;
    RotatePoint( p, EDA_ANGLE( 90.0 * ( aRot & 3 ), DEGREES_T ) );
    return p;
}


/// Bounding box of points turned by aRot quarter turns; aFallback when there are none.
BOX2I boxOf( const std::vector<VECTOR2I>& aPoints, int aRot, const BOX2I& aFallback )
{
    BOX2I box;
    bool  first = true;

    for( const VECTOR2I& pt : aPoints )
    {
        VECTOR2I q = rotated( pt, aRot );

        if( first )
            box = BOX2I( q, VECTOR2I( 0, 0 ) );
        else
            box.Merge( q );

        first = false;
    }

    return first ? aFallback : box;
}


/// Index of a part's shapes on its current side in rotation aRot.
inline int at( const PART& aPart, int aRot )
{
    return ( aPart.back ? 4 : 0 ) + ( aRot & 3 );
}


/// Everything the placer needs per part, precomputed for both sides and the four rotations.
struct WORK
{
    // per side (front 0..3, back 4..7) and rotation: see at()
    BOX2I                 box[8];        ///< courtyard (+ silk) box relative to the anchor
    BOX2I                 cu[8];         ///< pads' box relative to the anchor
    std::vector<VECTOR2I> pad[8];        ///< pad offsets
    std::vector<BOX2I>    padBox[8];     ///< each pad's copper
    std::vector<BOX2I>    holeBox[8];    ///< the copper of the pads with a hole
    bool                  startBack = false;
    bool                  placed = false;
    int                   affPad = -1;   ///< small passive: its pad pulled to ...
    int                   affPart = -1;  ///< ... this part's ...
    int                   affOther = -1; ///< ... pad
};


class PLACER
{
public:
    PLACER( std::vector<PART>& aParts, int aNetCount, const SHAPE_POLY_SET& aOutline, const OPTIONS& aOpt,
            int aVariant = 0 ) :
            m_parts( aParts ),
            m_netCount( aNetCount ),
            m_outline( aOutline ),
            m_opt( aOpt ),
            m_variant( aVariant )
    {
        m_area = m_outline.BBox();
        m_work.resize( m_parts.size() );
        m_newcomer.assign( m_parts.size(), false );
        m_netPads.resize( m_netCount );

        const BOX2I unit( VECTOR2I( -500000, -500000 ), VECTOR2I( 1000000, 1000000 ) );

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            PART& p = m_parts[i];

            // its own side from the shape, the other one from the flipped shape (when it has one)
            m_work[i].startBack = p.back;
            const bool flip = p.canFlip && p.flipPadOffset.size() == p.pads.size()
                              && p.flipPadBox.size() == p.pads.size();
            p.canFlip = flip;

            for( int side = 0; side < 2; ++side )
            {
                const bool other = ( side == 1 ) != p.back;
                const bool use = other && flip;

                for( int r = 0; r < 4; ++r )
                {
                    const int k = side * 4 + r;
                    m_work[i].box[k] = boxOf( use ? p.flipOutline : p.outline, r, unit );
                    m_work[i].cu[k] = boxOf( use ? p.flipCopper : p.copper, r, m_work[i].box[k] );

                    for( size_t n = 0; n < p.pads.size(); ++n )
                    {
                        const PAD& pad = p.pads[n];
                        m_work[i].pad[k].push_back( rotated( use ? p.flipPadOffset[n] : pad.offset, r ) );

                        const BOX2I& rel = use ? p.flipPadBox[n] : pad.box;
                        BOX2I        pb( rotated( rel.GetOrigin(), r ), VECTOR2I( 0, 0 ) );
                        pb.Merge( rotated( rel.GetEnd(), r ) );
                        m_work[i].padBox[k].push_back( pb );

                        if( pad.hole )
                            m_work[i].holeBox[k].push_back( pb );
                    }
                }
            }

            for( size_t k = 0; k < p.pads.size(); ++k )
            {
                if( p.pads[k].net >= 0 && p.pads[k].net < m_netCount )
                    m_netPads[p.pads[k].net].push_back( { (int) i, (int) k } );
            }

            m_work[i].placed = !p.movable;
        }

        // Nets that touch a large share of the parts (GND, the main supply on a small board)
        // pull everything to one point: leave them out of the attraction.
        const size_t big = std::max<size_t>( 6, (size_t) std::ceil( 0.3 * m_parts.size() ) );
        m_weight.assign( m_netCount, 0.0 );
        int largest = -1;

        for( int n = 0; n < m_netCount; ++n )
        {
            size_t k = m_netPads[n].size();

            if( largest < 0 || k > m_netPads[largest].size() )
                largest = n;

            if( k >= 2 && k <= big )
                m_weight[n] = 1.0 / ( k - 1 );
        }

        // Small passives: pulled to the nearest pin of the biggest part sharing a net with
        // them, the largest net (ground) aside.
        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            const PART& p = m_parts[i];

            if( !p.movable || p.pads.size() > 3 )
                continue;

            int bestPart = -1, bestPad = -1, myPad = -1;

            for( size_t k = 0; k < p.pads.size(); ++k )
            {
                int n = p.pads[k].net;

                if( n < 0 || n == largest )
                    continue;

                for( auto [q, b] : m_netPads[n] )
                {
                    if( q == (int) i || m_parts[q].pads.size() < 4 )
                        continue;

                    if( bestPart < 0 || m_parts[q].pads.size() > m_parts[bestPart].pads.size() )
                    {
                        bestPart = q;
                        bestPad = b;
                        myPad = (int) k;
                    }
                }
            }

            m_work[i].affPart = bestPart;
            m_work[i].affPad = myPad;
            m_work[i].affOther = bestPad;
        }

        m_satellites.resize( m_parts.size() );

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( m_work[i].affPart >= 0 )
                m_satellites[m_work[i].affPart].push_back( (int) i );
        }

        m_nearOf.resize( m_parts.size() );

        for( size_t r = 0; r < m_opt.nearRules.size(); ++r )
        {
            for( int k : m_opt.nearRules[r].parts )
            {
                if( k >= 0 && k < (int) m_parts.size() )
                    m_nearOf[k].push_back( (int) r );
            }
        }

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            PART& p = m_parts[i];

            if( p.movable && p.fixedRot >= 0 )
                p.rot = p.fixedRot & 3;

            // a decoupling cap: two pads, one on ground (the largest net), tied to an IC pin
            bool decap = m_work[i].affPart >= 0 && p.pads.size() == 2
                         && ( p.pads[0].net == largest || p.pads[1].net == largest );

            if( p.flipIfSatellite && ( !m_opt.backSatellites || !decap ) )
                p.canFlip = false;

            m_maxHoleGap = std::max( m_maxHoleGap, p.holeGap );
        }

        bucketRebuild();
    }

    RESULT Run( bool aRefine = true )
    {
        RESULT res;
        res.hpwlBefore = hpwl();

        for( size_t i = 0; i < m_parts.size(); ++i )
            m_newcomer[i] = m_parts[i].movable && !insideBoard( boxAt( (int) i, m_parts[i].pos, m_parts[i].rot ) );

        m_startPos.resize( m_parts.size() );
        m_startRot.resize( m_parts.size() );

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            m_startPos[i] = m_parts[i].pos;
            m_startRot[i] = m_parts[i].rot;
        }

        res.mixedBefore = mixed();
        res.satBefore = satelliteDistance( true, &res.satellites );

        if( m_opt.measureOnly )
        {
            res.hpwlAfter = res.hpwlBefore;
            res.mixed = res.mixedBefore;
            res.satAfter = res.satBefore;
            return res;
        }

        blocks();
        global();
        legalize( res );

        if( cancelled() )
        {
            res.cancelled = true;
            return res;
        }

        // Another start that got here with the same layout ends with the same result: only one
        // of them goes on (parts already on the board: the starts differ little).
        if( m_claim && !m_claim( fingerprint() ) )
        {
            m_duplicate = true;
            return res;
        }

        // Improve until a pass gains less than 0.1 % or the time is up. Every part keeps a
        // legal spot throughout, so stopping anywhere leaves a valid placement.
        m_limited = m_hasDeadline;
        double cost = totalCost();

        if( m_opt.progress )
            m_opt.progress->improving = true;

        for( int pass = 0; aRefine && pass < m_opt.refinePasses; ++pass )
        {
            if( expired() )
                break;

            bool improved = refine();
            res.refinePasses++;

            if( m_opt.progress )
                raise( m_opt.progress->passes, res.refinePasses );

            if( !improved || expired() )
                break;

            double now = totalCost();

            if( cost - now < 1e-3 * std::abs( cost ) )
                break;

            cost = now;
        }

        res.timedOut = m_late && !cancelled();
        m_limited = false;

        if( cancelled() )
        {
            res.cancelled = true;
            return res;
        }

        centre();
        res.mixed = mixed();

        for( const NEAR_RULE& rule : m_opt.nearRules )
        {
            for( size_t a = 0; a < rule.parts.size(); ++a )
            {
                for( size_t b = a + 1; b < rule.parts.size(); ++b )
                {
                    int i = rule.parts[a], j = rule.parts[b];

                    if( !m_work[i].placed || !m_work[j].placed )
                        continue;

                    double d = ( nearCentre( i ) - nearCentre( j ) ).EuclideanNorm();

                    if( d > rule.maxDist + 1000 )
                    {
                        char buf[160];
                        snprintf( buf, sizeof( buf ), "%s %s: %.1f mm apart (max %.1f)", m_parts[i].ref.c_str(),
                                  m_parts[j].ref.c_str(), d / 1e6, rule.maxDist / 1e6 );
                        res.violations.push_back( buf );
                    }
                }
            }
        }
        res.satAfter = satelliteDistance( false, &res.satellites );

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( m_parts[i].movable && m_work[i].placed && m_parts[i].back )
                res.onBack++;

            if( m_parts[i].movable && m_work[i].placed && m_parts[i].back != m_work[i].startBack )
                res.flipped++;
        }

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( m_parts[i].movable && m_work[i].placed )
                res.moved++;
        }

        res.hpwlAfter = hpwl();
        return res;
    }

    /// Stop improving at aDeadline; aClaim(layout) is false when another start has that layout
    void Limit( std::chrono::steady_clock::time_point aDeadline, bool aHasDeadline,
                std::function<bool( uint64_t )> aClaim )
    {
        m_deadline = aDeadline;
        m_hasDeadline = aHasDeadline;
        m_claim = std::move( aClaim );
    }

    bool Duplicate() const { return m_duplicate; }

    double hpwl() const
    {
        double total = 0;

        for( int n = 0; n < m_netCount; ++n )
            total += netHpwl( n );

        return total / 1e6;
    }

private:
    /// Out of time (only while improving: placing every part is never cut short)
    bool expired() const
    {
        if( cancelled() )
            return true;

        if( !m_limited )
            return false;

        if( !m_late && std::chrono::steady_clock::now() >= m_deadline )
            m_late = true;

        return m_late;
    }

    bool cancelled() const { return m_opt.progress && m_opt.progress->cancel.load( std::memory_order_relaxed ); }

    /// aCounter = max( aCounter, aValue ): the starts run side by side, report the furthest
    static void raise( std::atomic<int>& aCounter, int aValue )
    {
        int now = aCounter.load();

        while( now < aValue && !aCounter.compare_exchange_weak( now, aValue ) )
        {
        }
    }

    /// What refine() lowers, summed over the placed movable parts
    double totalCost()
    {
        refreshBlocks();
        double total = 0;

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            const PART& p = m_parts[i];

            if( p.movable && m_work[i].placed )
                total += localCost( (int) i ) + ( p.back && p.canFlip ? m_opt.backPenalty : 0 );
        }

        return total;
    }

    /// The layout of the movable parts (FNV-1a)
    uint64_t fingerprint() const
    {
        uint64_t h = 1469598103934665603ULL;

        auto mix = [&]( int64_t v )
        {
            for( int k = 0; k < 8; ++k, v >>= 8 )
            {
                h ^= (uint64_t) ( v & 0xff );
                h *= 1099511628211ULL;
            }
        };

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            const PART& p = m_parts[i];

            if( !p.movable )
                continue;

            mix( m_work[i].placed ? 1 : 0 );
            mix( p.pos.x );
            mix( p.pos.y );
            mix( p.rot + ( p.back ? 4 : 0 ) );
        }

        return h;
    }

    VECTOR2I padPos( int aPart, int aPad ) const
    {
        const PART& p = m_parts[aPart];
        return p.pos + m_work[aPart].pad[at( p, p.rot )][aPad];
    }

    BOX2I boxAt( int aPart, const VECTOR2I& aPos, int aRot ) const
    {
        BOX2I b = m_work[aPart].box[at( m_parts[aPart], aRot )];
        b.Move( aPos );
        return b;
    }

    BOX2I cuAt( int aPart, const VECTOR2I& aPos, int aRot ) const
    {
        BOX2I b = m_work[aPart].cu[at( m_parts[aPart], aRot )];
        b.Move( aPos );
        return b;
    }

    double netHpwl( int aNet ) const
    {
        const auto& pads = m_netPads[aNet];

        if( pads.size() < 2 )
            return 0;

        BOX2I box( padPos( pads[0].first, pads[0].second ), VECTOR2I( 0, 0 ) );

        for( auto [q, b] : pads )
            box.Merge( padPos( q, b ) );

        return (double) box.GetWidth() + box.GetHeight();
    }

    /// What moving part i cares about: weighted half-perimeters of its nets + its affinity.
    double localCost( int i ) const
    {
        double           cost = 0;
        std::vector<int> nets;

        for( const PAD& pad : m_parts[i].pads )
        {
            if( pad.net >= 0 && std::find( nets.begin(), nets.end(), pad.net ) == nets.end() )
                nets.push_back( pad.net );
        }

        for( int n : nets )
            cost += ( m_weight[n] > 0 ? 1.0 : 0.05 ) * netHpwl( n );

        const WORK& w = m_work[i];

        if( w.affPart >= 0 )
            cost += 2.0 * ( padPos( i, w.affPad ) - padPos( w.affPart, w.affOther ) ).EuclideanNorm();

        // what pulls other passives to this part
        for( int j : m_satellites[i] )
            cost += 2.0 * ( padPos( j, m_work[j].affPad ) - padPos( i, m_work[j].affOther ) ).EuclideanNorm();

        // near rules: every mm beyond the limit costs like 10 mm of wire
        for( int r : m_nearOf[i] )
        {
            const NEAR_RULE& rule = m_opt.nearRules[r];
            VECTOR2D         me = nearCentre( i );

            for( int j : rule.parts )
            {
                if( j == i || !m_work[j].placed )
                    continue;

                double d = ( nearCentre( j ) - me ).EuclideanNorm();

                if( d > rule.maxDist )
                    cost += 10.0 * ( d - rule.maxDist );
            }
        }

        VECTOR2D bc;

        if( blockCentre( i, bc ) )
            cost += m_softWeight * ( VECTOR2D( boxAt( i, m_parts[i].pos, m_parts[i].rot ).Centre() ) - bc ).EuclideanNorm();

        return cost;
    }

    /// Weighted centre of what part i connects to, as an anchor position for rotation aRot.
    bool pullPoint( int i, int aRot, VECTOR2I& aOut ) const
    {
        double      sx = 0, sy = 0, sw = 0;
        const PART& p = m_parts[i];

        for( size_t k = 0; k < p.pads.size(); ++k )
        {
            int n = p.pads[k].net;

            if( n < 0 || m_weight[n] <= 0 )
                continue;

            for( auto [q, b] : m_netPads[n] )
            {
                if( q == i || !m_work[q].placed )
                    continue;

                VECTOR2I want = padPos( q, b ) - m_work[i].pad[at( p, aRot )][k];
                sx += m_weight[n] * want.x;
                sy += m_weight[n] * want.y;
                sw += m_weight[n];
            }
        }

        const WORK& w = m_work[i];

        if( w.affPart >= 0 && m_work[w.affPart].placed )
        {
            VECTOR2I want = padPos( w.affPart, w.affOther ) - w.pad[at( p, aRot )][w.affPad];
            sx += 2.0 * want.x;
            sy += 2.0 * want.y;
            sw += 2.0;
        }

        // near rules: towards the other parts of the rule
        for( int r : m_nearOf[i] )
        {
            for( int j : m_opt.nearRules[r].parts )
            {
                if( j == i || !m_work[j].placed )
                    continue;

                VECTOR2D want = nearCentre( j ) - VECTOR2D( m_work[i].cu[at( p, aRot )].Centre() );
                sx += 3.0 * want.x;
                sy += 3.0 * want.y;
                sw += 3.0;
            }
        }

        VECTOR2D bc;

        if( blockCentre( i, bc ) )
        {
            VECTOR2D want = bc - VECTOR2D( m_work[i].box[at( p, aRot )].Centre() );
            sx += m_softWeight * want.x;
            sy += m_softWeight * want.y;
            sw += m_softWeight;
        }

        if( sw <= 0 )
            return false;

        aOut = VECTOR2I( KiROUND( sx / sw ), KiROUND( sy / sw ) );
        return true;
    }

    /// Mean distance, mm, from the movable small passives' pad to the IC pin they belong to
    /// (aOnBoard: only those already on the board, for the "before").
    double satelliteDistance( bool aOnBoard, int* aCount ) const
    {
        double sum = 0;
        int    n = 0;

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            const WORK& w = m_work[i];

            if( !m_parts[i].movable || w.affPart < 0 )
                continue;

            auto on = [&]( int k )
            {
                return m_outline.Contains( boxAt( k, m_parts[k].pos, m_parts[k].rot ).Centre() );
            };

            if( aOnBoard ? !( on( (int) i ) && on( w.affPart ) ) : !( w.placed && m_work[w.affPart].placed ) )
                continue;

            sum += ( VECTOR2D( padPos( (int) i, w.affPad ) ) - VECTOR2D( padPos( w.affPart, w.affOther ) ) ).EuclideanNorm();
            n++;
        }

        if( aCount )
            *aCount = n;

        return n ? sum / n / 1e6 : 0;
    }

    /// Placed parts in a block whose nearest neighbour (courtyard gap) is in another block.
    int mixed() const
    {
        std::vector<int>   on;
        std::vector<BOX2I> box( m_parts.size() );

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            box[i] = boxAt( (int) i, m_parts[i].pos, m_parts[i].rot );

            // on the board: its centre (silk of big parts may stick out of the outline)
            if( m_parts[i].group >= 0 && m_outline.Contains( box[i].Centre() ) )
                on.push_back( (int) i );
        }

        auto gap = [&]( const BOX2I& a, const BOX2I& b )
        {
            double dx = std::max<double>( 0, std::max( a.GetLeft() - b.GetRight(), b.GetLeft() - a.GetRight() ) );
            double dy = std::max<double>( 0, std::max( a.GetTop() - b.GetBottom(), b.GetTop() - a.GetBottom() ) );
            return std::hypot( dx, dy );
        };

        int count = 0;

        for( int i : on )
        {
            int    best = -1;
            double bd = std::numeric_limits<double>::max();

            for( int j : on )
            {
                if( j == i || m_parts[j].back != m_parts[i].back )
                    continue;

                double d = gap( box[i], box[j] );

                if( d < bd )
                {
                    bd = d;
                    best = j;
                }
            }

            if( best >= 0 && m_parts[best].group != m_parts[i].group )
                count++;
        }

        return count;
    }

    /// Soft blocks: the members' centres, taken once per pass (a pass moves parts one at a
    /// time; the block centre hardly moves meanwhile).
    void refreshBlocks()
    {
        if( !m_soft )
            return;

        m_blockSum.assign( m_members.size(), VECTOR2D( 0, 0 ) );
        m_blockN.assign( m_members.size(), 0 );
        m_cachedCentre.resize( m_parts.size() );

        for( size_t g = 0; g < m_members.size(); ++g )
        {
            for( int j : m_members[g] )
            {
                m_cachedCentre[j] = VECTOR2D( boxAt( j, m_parts[j].pos, m_parts[j].rot ).Centre() );

                if( m_work[j].placed )
                {
                    m_blockSum[g] += m_cachedCentre[j];
                    m_blockN[g]++;
                }
            }
        }
    }

    /// Centre of the other parts of part i's block (soft blocks), false when there are none.
    bool blockCentre( int i, VECTOR2D& aOut ) const
    {
        int g = m_parts[i].group;

        if( !m_soft || g < 0 || g >= (int) m_blockSum.size() )
            return false;

        VECTOR2D sum = m_blockSum[g];
        int      n = m_blockN[g];

        if( m_work[i].placed && n > 0 )
        {
            sum -= m_cachedCentre[i];
            n--;
        }

        if( n == 0 )
            return false;

        aOut = sum / n;
        return true;
    }

    /// Blocks: the members of each (soft pull to their centre), when there are two or more.
    void blocks()
    {
        const int G = m_opt.groups;

        if( G < 2 || m_opt.groupWeight <= 0 )
            return;

        m_members.assign( G, {} );

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( m_parts[i].group >= 0 && m_parts[i].group < G )
                m_members[m_parts[i].group].push_back( (int) i );
        }

        m_soft = true;
    }

    bool insideBoard( const BOX2I& aBox ) const
    {
        BOX2I b = aBox;
        b.Inflate( m_opt.edge );

        const VECTOR2I corners[4] = { b.GetOrigin(), { b.GetRight(), b.GetTop() }, b.GetEnd(),
                                      { b.GetLeft(), b.GetBottom() } };

        for( const VECTOR2I& c : corners )
        {
            if( !m_outline.Contains( c ) )
                return false;
        }

        // a notch of the outline reaching into the box
        for( auto it = m_outline.CIterate(); it; ++it )
        {
            const VECTOR2I& v = *it;

            if( v.x > b.GetLeft() && v.x < b.GetRight() && v.y > b.GetTop() && v.y < b.GetBottom() )
                return false;
        }

        return true;
    }

    /// Part i at aPos / aRot: courtyard clear of the placed parts (+ spacing), pads clear of
    /// the obstacles (silk may go over board text).
    // A bucket grid over the board of the placed parts' boxes (courtyard and pads), so a
    // legality test looks at the neighbours only. A part's entry covers where it was when it
    // was entered: update it (bucketUpdate) when a part moves for good.
    static constexpr int BUCKET = 4000000;   // 4 mm

    void bucketRange( const BOX2I& aBox, int& x0, int& y0, int& x1, int& y1 ) const
    {
        auto cx = [&]( int v ) { return std::clamp( (int) ( ( (int64_t) v - m_bucketOrigin.x ) / BUCKET ), 0, m_bucketW - 1 ); };
        auto cy = [&]( int v ) { return std::clamp( (int) ( ( (int64_t) v - m_bucketOrigin.y ) / BUCKET ), 0, m_bucketH - 1 ); };
        x0 = cx( aBox.GetLeft() );
        x1 = cx( aBox.GetRight() );
        y0 = cy( aBox.GetTop() );
        y1 = cy( aBox.GetBottom() );
    }

    BOX2I footprintOf( int j ) const
    {
        BOX2I b = boxAt( j, m_parts[j].pos, m_parts[j].rot );
        b.Merge( cuAt( j, m_parts[j].pos, m_parts[j].rot ) );
        return b;
    }

    void bucketRemove( int j )
    {
        if( m_bucketAt[j].GetWidth() < 0 )
            return;

        int x0, y0, x1, y1;
        bucketRange( m_bucketAt[j], x0, y0, x1, y1 );

        for( int y = y0; y <= y1; ++y )
            for( int x = x0; x <= x1; ++x )
            {
                auto& v = m_buckets[y * m_bucketW + x];
                v.erase( std::remove( v.begin(), v.end(), j ), v.end() );
            }

        m_bucketAt[j] = BOX2I( VECTOR2I( 0, 0 ), VECTOR2I( -1, -1 ) );
    }

    void bucketUpdate( int j )
    {
        bucketRemove( j );

        if( !m_work[j].placed )
            return;

        BOX2I b = footprintOf( j );
        int   x0, y0, x1, y1;
        bucketRange( b, x0, y0, x1, y1 );

        for( int y = y0; y <= y1; ++y )
            for( int x = x0; x <= x1; ++x )
                m_buckets[y * m_bucketW + x].push_back( j );

        m_bucketAt[j] = b;
    }

    void bucketRebuild()
    {
        BOX2I all = m_area;
        all.Inflate( 100000000 );     // 100 mm round the board: parts sticking out
        m_bucketOrigin = all.GetOrigin();
        m_bucketW = std::max( 1, (int) ( all.GetWidth() / BUCKET ) + 1 );
        m_bucketH = std::max( 1, (int) ( all.GetHeight() / BUCKET ) + 1 );
        m_buckets.assign( (size_t) m_bucketW * m_bucketH, {} );
        m_bucketAt.assign( m_parts.size(), BOX2I( VECTOR2I( 0, 0 ), VECTOR2I( -1, -1 ) ) );
        m_stamp.assign( m_parts.size(), 0 );

        for( size_t j = 0; j < m_parts.size(); ++j )
            bucketUpdate( (int) j );
    }

    /// aFn( j ) for each part entered near aBox, once; stops when aFn returns false.
    template <typename F>
    void forNear( const BOX2I& aBox, F aFn ) const
    {
        int x0, y0, x1, y1;
        bucketRange( aBox, x0, y0, x1, y1 );
        ++m_stampNow;

        for( int y = y0; y <= y1; ++y )
            for( int x = x0; x <= x1; ++x )
                for( int j : m_buckets[y * m_bucketW + x] )
                {
                    if( m_stamp[j] == m_stampNow )
                        continue;

                    m_stamp[j] = m_stampNow;

                    if( !aFn( j ) )
                        return;
                }
    }

    bool free( int i, const VECTOR2I& aPos, int aRot ) const
    {
        if( !m_opt.obstacles.empty() )
        {
            BOX2I cu = cuAt( i, aPos, aRot );

            const PART& me = m_parts[i];
            const int   mine = me.tht ? ( OBST_FRONT | OBST_BACK | OBST_INNER ) : me.back ? OBST_BACK : OBST_FRONT;
            const bool  layered = m_opt.obstacleLayers.size() == m_opt.obstacles.size();

            for( size_t k = 0; k < m_opt.obstacles.size(); ++k )
            {
                if( layered && !( m_opt.obstacleLayers[k] & mine ) )
                    continue;

                if( !cu.Intersects( m_opt.obstacles[k] ) )
                    continue;

                // copper text and graphics: not under the part at all
                if( !layered || !( m_opt.obstacleLayers[k] & OBST_PADS ) )
                    return false;

                // a track: the box around all pads meets it, does a pad? (a track between the
                // rows of an IC's pins doesn't keep the IC out)
                for( BOX2I pb : m_work[i].padBox[at( me, aRot )] )
                {
                    pb.Move( aPos );

                    if( pb.Intersects( m_opt.obstacles[k] ) )
                        return false;
                }
            }
        }

        BOX2I b = boxAt( i, aPos, aRot );
        b.Inflate( m_opt.spacing );

        const PART& p = m_parts[i];
        const int   holeGap = std::max( m_opt.spacing, m_opt.holeClearance );

        BOX2I reach = b;
        reach.Inflate( std::max( holeGap, m_maxHoleGap ) );     // neighbours' holes

        bool clear = true;

        forNear( reach, [&]( int j )
        {
            if( j == i || !m_work[j].placed )
                return true;

            const PART& q = m_parts[j];

            const BOX2I qBox = boxAt( j, q.pos, q.rot );

            if( q.back == p.back && b.Intersects( qBox ) )
            {
                clear = false;
                return false;
            }

            if( !q.tht && !p.tht )
                return true;

            // Through-hole pins stick out on the other side (no part body over them) and the
            // holes want their clearance from every pad on both sides.
            const int gap = std::max( { holeGap, p.holeGap, q.holeGap } );

            auto holesHit = [&]( int aHoles, const VECTOR2I& aHPos, int aHRot, const BOX2I& aBody, int aPads,
                                 const VECTOR2I& aPPos, int aPRot, bool aOtherSide )
            {
                BOX2I padsCu = cuAt( aPads, aPPos, aPRot );
                padsCu.Inflate( gap );

                for( BOX2I h : m_work[aHoles].holeBox[at( m_parts[aHoles], aHRot )] )
                {
                    h.Move( aHPos );

                    if( aOtherSide && h.Intersects( aBody ) )
                        return true;

                    BOX2I hg = h;
                    hg.Inflate( gap );

                    if( !hg.Intersects( padsCu ) )
                        continue;

                    for( BOX2I pb : m_work[aPads].padBox[at( m_parts[aPads], aPRot )] )
                    {
                        pb.Move( aPPos );

                        if( hg.Intersects( pb ) )
                            return true;
                    }
                }

                return false;
            };

            const bool otherSide = q.back != p.back;

            if( q.tht && holesHit( j, q.pos, q.rot, b, i, aPos, aRot, otherSide ) )
                clear = false;
            else if( p.tht && holesHit( i, aPos, aRot, qBox, j, q.pos, q.rot, otherSide ) )
                clear = false;

            return clear;
        } );

        return clear;
    }

    /// Near rules measure between the parts' pad-box centres.
    VECTOR2D nearCentre( int i ) const
    {
        return VECTOR2D( cuAt( i, m_parts[i].pos, m_parts[i].rot ).Centre() );
    }

    /// Anchor for part i in rotation aRot that puts its centre on the centre of its near-rule
    /// partners already placed; false when there are none.
    bool nearTarget( int i, int aRot, VECTOR2I& aOut ) const
    {
        VECTOR2D sum( 0, 0 );
        int      n = 0;

        for( int r : m_nearOf[i] )
        {
            for( int j : m_opt.nearRules[r].parts )
            {
                if( j != i && m_work[j].placed )
                {
                    sum += nearCentre( j );
                    n++;
                }
            }
        }

        if( !n )
            return false;

        VECTOR2D want = sum / n - VECTOR2D( m_work[i].cu[at( m_parts[i], aRot )].Centre() );
        aOut = VECTOR2I( KiROUND( want.x ), KiROUND( want.y ) );
        return true;
    }

    /// The agent's hard rules for part i at aPos / aRot: board edge, region.
    bool ruled( int i, const VECTOR2I& aPos, int aRot ) const
    {
        const PART& p = m_parts[i];

        if( p.edgeSide < 0 && !p.hasRegion )
            return true;

        BOX2I b = boxAt( i, aPos, aRot );

        if( p.hasRegion && !( p.region.Contains( b.GetOrigin() ) && p.region.Contains( b.GetEnd() ) ) )
            return false;

        if( p.edgeSide >= 0 )
        {
            const int tol = m_opt.edge + 2 * m_opt.grid;
            const int d[4] = { b.GetLeft() - m_area.GetLeft(), m_area.GetRight() - b.GetRight(),
                               b.GetTop() - m_area.GetTop(), m_area.GetBottom() - b.GetBottom() };

            if( p.edgeSide == EDGE_ANY )
                return *std::min_element( d, d + 4 ) <= tol;

            return d[p.edgeSide] <= tol;
        }

        return true;
    }

    /// Where the rules want part i near aTarget: onto its edge (the nearest one for "any"),
    /// into its region.
    VECTOR2I ruleTarget( int i, const VECTOR2I& aTarget, int aRot ) const
    {
        const PART& p = m_parts[i];
        VECTOR2I    t = aTarget;
        const BOX2I rel = m_work[i].box[at( p, aRot )];

        if( p.edgeSide >= 0 )
        {
            const int x[2] = { m_area.GetLeft() + m_opt.edge - rel.GetLeft(), m_area.GetRight() - m_opt.edge - rel.GetRight() };
            const int y[2] = { m_area.GetTop() + m_opt.edge - rel.GetTop(), m_area.GetBottom() - m_opt.edge - rel.GetBottom() };
            int       side = p.edgeSide;

            if( side == EDGE_ANY )
            {
                const int d[4] = { std::abs( t.x - x[0] ), std::abs( t.x - x[1] ), std::abs( t.y - y[0] ),
                                   std::abs( t.y - y[1] ) };
                side = (int) ( std::min_element( d, d + 4 ) - d );
            }

            if( side == EDGE_LEFT || side == EDGE_RIGHT )
                t.x = x[side == EDGE_RIGHT];
            else
                t.y = y[side == EDGE_BOTTOM];
        }

        if( p.hasRegion )
        {
            t.x = std::clamp( t.x, p.region.GetLeft() - rel.GetLeft(),
                              std::max( p.region.GetLeft() - rel.GetLeft(), p.region.GetRight() - rel.GetRight() ) );
            t.y = std::clamp( t.y, p.region.GetTop() - rel.GetTop(),
                              std::max( p.region.GetTop() - rel.GetTop(), p.region.GetBottom() - rel.GetBottom() ) );
        }

        return t;
    }

    bool legalAt( int i, const VECTOR2I& aPos, int aRot ) const
    {
        if( !ruled( i, aPos, aRot ) )
            return false;

        return free( i, aPos, aRot ) && insideBoard( boxAt( i, aPos, aRot ) );
    }

    /// Nearest legal anchor to aTarget for part i in rotation aRot (square rings on the grid).
    bool nearestLegal( int i, const VECTOR2I& aTarget, int aRot, VECTOR2I& aOut, int aMaxRing = -1 ) const
    {
        const int g = m_opt.grid;
        VECTOR2I  t( KiROUND( (double) aTarget.x / g ) * g, KiROUND( (double) aTarget.y / g ) * g );

        // keep the search inside what could fit at all
        const BOX2I& rel = m_work[i].box[at( m_parts[i], aRot )];
        const int    xmin = m_area.GetLeft() - rel.GetLeft(), xmax = m_area.GetRight() - rel.GetRight();
        const int    ymin = m_area.GetTop() - rel.GetTop(), ymax = m_area.GetBottom() - rel.GetBottom();

        if( xmin > xmax || ymin > ymax )
            return false;

        t.x = std::clamp( t.x, xmin, xmax );
        t.y = std::clamp( t.y, ymin, ymax );

        int maxRing = (int) ( std::max( m_area.GetWidth(), m_area.GetHeight() ) / g ) + 2;

        if( aMaxRing >= 0 )
            maxRing = std::min( maxRing, aMaxRing );

        for( int r = 0; r <= maxRing; ++r )
        {
            if( expired() )
                return false;

            double   best = std::numeric_limits<double>::max();
            bool     found = false;
            VECTOR2I bestPos;

            auto tryAt = [&]( int dx, int dy )
            {
                VECTOR2I c( t.x + dx * g, t.y + dy * g );

                if( c.x < xmin || c.x > xmax || c.y < ymin || c.y > ymax )
                    return;

                double d = ( VECTOR2D( c ) - VECTOR2D( aTarget ) ).EuclideanNorm();

                if( d >= best )
                    return;

                if( legalAt( i, c, aRot ) )
                {
                    best = d;
                    bestPos = c;
                    found = true;
                }
            };

            if( r == 0 )
            {
                tryAt( 0, 0 );
            }
            else
            {
                for( int k = -r; k <= r; ++k )
                {
                    tryAt( k, -r );
                    tryAt( k, r );

                    if( k != -r && k != r )
                    {
                        tryAt( -r, k );
                        tryAt( r, k );
                    }
                }
            }

            if( found )
            {
                aOut = bestPos;
                return true;
            }
        }

        return false;
    }

    void global()
    {
        // Start: movable parts off the board at its centre on a small golden-angle spiral
        // (distinct, deterministic).
        const VECTOR2I c = m_area.Centre();
        const double   spread = 0.15 * std::min( m_area.GetWidth(), m_area.GetHeight() );
        int            k = 0;

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            PART& p = m_parts[i];

            if( !p.movable )
                continue;

            if( m_newcomer[i] )
            {
                double a = k * 2.39996 + m_variant * 1.1;
                double rr = spread * std::sqrt( ( k + 0.5 ) / ( m_parts.size() + 1.0 ) );

                p.pos = c + VECTOR2I( KiROUND( rr * std::cos( a ) ), KiROUND( rr * std::sin( a ) ) );
            }

            k++;
        }

        // Attraction, with every part counted as placed for the pull
        std::vector<bool> wasPlaced( m_parts.size() );

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            wasPlaced[i] = m_work[i].placed;
            m_work[i].placed = true;
        }

        // Parts already on the board keep their place as the target (auto_place on a laid-out
        // board improves it rather than starting over); newcomers are pulled in.
        for( int it = 0; it < 60; ++it )
        {
            refreshBlocks();

            for( size_t i = 0; i < m_parts.size(); ++i )
            {
                if( !m_parts[i].movable || !m_newcomer[i] )
                    continue;

                VECTOR2I want;

                if( pullPoint( (int) i, m_parts[i].rot, want ) )
                    m_parts[i].pos = ( m_parts[i].pos + want ) / 2;
            }
        }

        for( size_t i = 0; i < m_parts.size(); ++i )
            m_work[i].placed = wasPlaced[i];
    }

    /// With nothing fixed pulling on them, the placed group's wire length is the same anywhere:
    /// move it to the middle of the board (when it stays legal there).
    void centre()
    {
        BOX2I group;
        bool  any = false;

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( !m_parts[i].movable || !m_work[i].placed )
                continue;

            for( const PAD& pad : m_parts[i].pads )
            {
                if( pad.net < 0 )
                    continue;

                for( auto [q, b] : m_netPads[pad.net] )
                {
                    if( !m_parts[q].movable )
                        return;     // tied to something fixed: stay where the pull put it
                }
            }

            BOX2I box = boxAt( (int) i, m_parts[i].pos, m_parts[i].rot );

            if( any )
                group.Merge( box );
            else
                group = box;

            any = true;
        }

        if( !any )
            return;

        const int g = m_opt.grid;
        VECTOR2I  d = m_area.Centre() - group.Centre();
        d = VECTOR2I( KiROUND( (double) d.x / g ) * g, KiROUND( (double) d.y / g ) * g );

        if( d == VECTOR2I( 0, 0 ) )
            return;

        std::vector<VECTOR2I> keep;

        for( PART& p : m_parts )
        {
            keep.push_back( p.pos );

            if( p.movable )
                p.pos += d;
        }

        bucketRebuild();

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( !m_parts[i].movable || !m_work[i].placed )
                continue;

            if( !legalAt( (int) i, m_parts[i].pos, m_parts[i].rot ) )
            {
                for( size_t j = 0; j < m_parts.size(); ++j )
                    m_parts[j].pos = keep[j];

                bucketRebuild();
                return;
            }
        }
    }

    void legalize( RESULT& aRes )
    {
        bucketRebuild();
        std::vector<int> order;

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( m_parts[i].movable )
                order.push_back( (int) i );
        }

        // big parts first (odd variants: the most connected first), satellites after
        std::stable_sort( order.begin(), order.end(),
                          [&]( int a, int b )
                          {
                              double aa = (double) m_work[a].box[0].GetArea(), ab = (double) m_work[b].box[0].GetArea();
                              size_t pa = m_parts[a].pads.size(), pb = m_parts[b].pads.size();

                              if( m_variant & 1 )
                                  return pa != pb ? pa > pb : aa > ab;

                              return aa != ab ? aa > ab : pa > pb;
                          } );

        // Parts on no net (mounting holes, logos, fiducials) have nothing to pull them anywhere:
        // they go last, to the board's corners, then the middles of its edges.
        auto connected = [&]( int i )
        {
            return std::any_of( m_parts[i].pads.begin(), m_parts[i].pads.end(),
                                []( const PAD& p ) { return p.net >= 0; } );
        };

        std::stable_partition( order.begin(), order.end(), connected );

        // parts with a place given by the rules (connectors on an edge, parts in a region) first:
        // they frame the rest
        std::stable_partition( order.begin(), order.end(),
                               [&]( int i ) { return m_parts[i].edgeSide >= 0 || m_parts[i].hasRegion; } );

        const VECTOR2I spots[8] = { m_area.GetOrigin(),
                                    { m_area.GetRight(), m_area.GetTop() },
                                    m_area.GetEnd(),
                                    { m_area.GetLeft(), m_area.GetBottom() },
                                    { m_area.Centre().x, m_area.GetTop() },
                                    { m_area.GetRight(), m_area.Centre().y },
                                    { m_area.Centre().x, m_area.GetBottom() },
                                    { m_area.GetLeft(), m_area.Centre().y } };
        int spot = 0;
        int placedCount = 0;

        for( int i : order )
        {
            refreshBlocks();     // the block's parts placed so far
            VECTOR2I target = m_parts[i].pos, at;

            if( !connected( i ) && m_newcomer[i] )
                target = spots[spot++ % 8];

            // On each side it may use: satellites go to what they belong to, now that it has its
            // final place (on the back right under the pin: the part above doesn't block it);
            // in its rotation or, when that doesn't fit, the other three. The back has to win by
            // backPenalty.
            const bool startSide = m_parts[i].back;
            const int  startRot = m_parts[i].rot;
            const VECTOR2I start = target;
            bool       ok = false;
            double     bestD = 0;
            VECTOR2I   bestAt;
            int        bestRot = startRot;
            bool       bestSide = startSide;

            for( int s = 0; s < ( m_parts[i].canFlip ? 2 : 1 ); ++s )
            {
                m_parts[i].back = s == 0 ? startSide : !startSide;
                VECTOR2I tgt = start;

                if( m_work[i].affPart >= 0 && m_work[m_work[i].affPart].placed )
                    pullPoint( i, startRot, tgt );

                // a near rule with partners placed: right next to them
                nearTarget( i, startRot, tgt );

                bool     found = false;
                VECTOR2I spot;
                int      rot = startRot;

                if( nearestLegal( i, ruleTarget( i, tgt, startRot ), startRot, spot ) )
                    found = true;

                for( int k = 1; k < 4 && !found && m_opt.rotate && m_parts[i].fixedRot < 0; ++k )
                {
                    rot = ( startRot + k ) & 3;
                    found = nearestLegal( i, ruleTarget( i, tgt, rot ), rot, spot );
                }

                if( !found )
                    continue;

                double d = ( VECTOR2D( spot ) - VECTOR2D( tgt ) ).EuclideanNorm()
                           + ( m_parts[i].back ? m_opt.backPenalty : 0 );

                // a satellite: how close its pad gets to the pin is what counts
                if( m_work[i].affPart >= 0 && m_work[m_work[i].affPart].placed )
                {
                    VECTOR2I keepPos = m_parts[i].pos;
                    int      keepRot = m_parts[i].rot;
                    m_parts[i].pos = spot;
                    m_parts[i].rot = rot;
                    d = ( VECTOR2D( padPos( i, m_work[i].affPad ) )
                          - VECTOR2D( padPos( m_work[i].affPart, m_work[i].affOther ) ) ).EuclideanNorm()
                        + ( m_parts[i].back ? m_opt.backPenalty : 0 );
                    m_parts[i].pos = keepPos;
                    m_parts[i].rot = keepRot;
                }

                if( !ok || d < bestD )
                {
                    ok = true;
                    bestD = d;
                    bestAt = spot;
                    bestRot = rot;
                    bestSide = m_parts[i].back;
                }
            }

            m_parts[i].back = bestSide;
            m_parts[i].rot = bestRot;
            at = bestAt;

            if( cancelled() )
                return;

            if( ok )
            {
                m_parts[i].pos = at;
                m_work[i].placed = true;
                bucketUpdate( i );
            }
            else
            {
                aRes.unplaced.push_back( m_parts[i].ref );

                // It stays where it is (it isn't moved): when that is on the board (even partly:
                // a connector over the edge), the parts placed after it must keep clear of it,
                // as of a fixed part.
                m_parts[i].pos = m_startPos[i];
                m_parts[i].rot = m_startRot[i];
                m_parts[i].back = m_work[i].startBack;

                if( m_area.Intersects( boxAt( i, m_parts[i].pos, m_parts[i].rot ) ) )
                {
                    m_parts[i].movable = false;
                    m_work[i].placed = true;
                    bucketUpdate( i );
                }
            }

            if( m_opt.progress )
                raise( m_opt.progress->placed, ++placedCount );
        }
    }

    bool refine()
    {
        refreshBlocks();
        bool             improved = false;
        std::vector<int> order;

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( m_parts[i].movable && m_work[i].placed )
                order.push_back( (int) i );
        }

        std::stable_sort( order.begin(), order.end(),
                          [&]( int a, int b ) { return m_parts[a].pads.size() > m_parts[b].pads.size(); } );

        for( int i : order )
        {
            if( expired() )
                return improved;

            PART&    p = m_parts[i];
            VECTOR2I keepPos = p.pos;
            int      keepRot = p.rot;
            bool     keepBack = p.back;
            double   bestCost = localCost( i ) + ( p.back && p.canFlip ? m_opt.backPenalty : 0 );
            VECTOR2I bestPos = keepPos;
            int      bestRot = keepRot;
            bool     bestBack = keepBack;

            for( int t = 0; t < ( m_opt.rotate ? 4 : 1 ) * ( p.canFlip ? 2 : 1 ); ++t )
            {
                const int r = m_opt.rotate ? t % 4 : 0;
                int       rot = m_opt.rotate ? r : keepRot;

                if( p.fixedRot >= 0 && rot != ( p.fixedRot & 3 ) )
                    continue;
                p.back = ( t / ( m_opt.rotate ? 4 : 1 ) ) == 0 ? keepBack : !keepBack;
                VECTOR2I want, at;

                if( !pullPoint( i, rot, want ) )
                    continue;

                // where the wires pull it, and (near rules) next to its partners
                std::vector<VECTOR2I> cands = { ruleTarget( i, want, rot ) };
                VECTOR2I              nt;

                if( nearTarget( i, rot, nt ) )
                    cands.push_back( ruleTarget( i, nt, rot ) );

                for( const VECTOR2I& cand : cands )
                {
                    // a spot farther from where the wires pull than the part is now rarely
                    // helps: don't search the whole board for one (near targets: a bit more)
                    int maxRing = (int) ( ( VECTOR2D( keepPos ) - VECTOR2D( cand ) ).EuclideanNorm() / m_opt.grid ) + 3;

                    if( &cand != &cands[0] )
                        maxRing += (int) ( 10000000 / m_opt.grid );

                    if( !nearestLegal( i, cand, rot, at, maxRing ) )
                        continue;

                    p.pos = at;
                    p.rot = rot;
                    double cost = localCost( i ) + ( p.back && p.canFlip ? m_opt.backPenalty : 0 );

                    if( cost < bestCost - 1000.0 )      // at least 1 µm better
                    {
                        bestCost = cost;
                        bestPos = at;
                        bestRot = rot;
                        bestBack = p.back;
                    }
                }
            }

            p.pos = bestPos;
            p.rot = bestRot;
            p.back = bestBack;

            if( bestPos != keepPos || bestRot != keepRot || bestBack != keepBack )
            {
                improved = true;
                bucketUpdate( i );
            }
        }

        // Swap parts of about the same size (two resistors, two caps): kept when the wire
        // length of both goes down and both still fit.
        for( size_t a = 0; a < order.size() && !expired(); ++a )
        {
            for( size_t bi = a + 1; bi < order.size(); ++bi )
            {
                int   i = order[a], j = order[bi];
                PART &p = m_parts[i], &q = m_parts[j];
                if( p.back != q.back )
                    continue;

                BOX2I bp = m_work[i].box[at( p, p.rot )], bq = m_work[j].box[at( q, q.rot )];

                if( std::abs( bp.GetWidth() - bq.GetWidth() ) > m_opt.grid
                    || std::abs( bp.GetHeight() - bq.GetHeight() ) > m_opt.grid )
                    continue;

                double before = localCost( i ) + localCost( j );
                std::swap( p.pos, q.pos );

                bool fits = legalAt( i, p.pos, p.rot ) && legalAt( j, q.pos, q.rot );

                if( fits && localCost( i ) + localCost( j ) < before - 1000.0 )
                {
                    improved = true;
                    bucketUpdate( i );
                    bucketUpdate( j );
                }
                else
                {
                    std::swap( p.pos, q.pos );
                }
            }
        }

        return improved;
    }

    std::vector<PART>&                            m_parts;
    int                                           m_netCount;
    const SHAPE_POLY_SET&                         m_outline;
    OPTIONS                                       m_opt;
    BOX2I                                         m_area;
    std::vector<WORK>                             m_work;
    std::vector<std::vector<std::pair<int, int>>> m_netPads;
    std::vector<double>                           m_weight;
    std::vector<bool>                             m_newcomer;
    std::vector<VECTOR2I>                         m_startPos;   ///< where the parts were
    std::vector<int>                              m_startRot;
    int                                           m_variant = 0;
    std::vector<std::vector<int>>                 m_satellites;
    std::vector<std::vector<int>>                 m_buckets;
    std::vector<BOX2I>                            m_bucketAt;
    VECTOR2I                                      m_bucketOrigin;
    int                                           m_bucketW = 1, m_bucketH = 1;
    mutable std::vector<unsigned>                 m_stamp;
    mutable unsigned                              m_stampNow = 0;
    int                                           m_maxHoleGap = 0;
    std::vector<std::vector<int>>                 m_members;    ///< per block
    std::vector<std::vector<int>>                 m_nearOf;     ///< near rules a part is in
    bool                                          m_soft = false;
    std::vector<VECTOR2D>                         m_blockSum;
    std::vector<int>                              m_blockN;
    std::vector<VECTOR2D>                         m_cachedCentre;
    double                                        m_softWeight = m_opt.groupWeight;

    std::chrono::steady_clock::time_point         m_deadline;
    bool                                          m_hasDeadline = false;
    bool                                          m_limited = false;   ///< improving: the deadline applies
    mutable bool                                  m_late = false;
    std::function<bool( uint64_t )>               m_claim;
    bool                                          m_duplicate = false;
};

} // namespace


RESULT Place( std::vector<PART>& aParts, int aNetCount, const SHAPE_POLY_SET& aOutline, const OPTIONS& aOptions )
{
    // A placement depends on where it starts: a few starts (spiral phase, order), keep the one
    // that places the most parts with the shortest wire length.
    // The starts are independent (own copy of the parts, the outline only read): run them in
    // parallel.
    const int                      starts = aOptions.measureOnly ? 1 : std::max( 1, aOptions.starts );
    std::vector<std::vector<PART>> parts( starts, aParts );
    std::vector<RESULT>            results( starts );
    std::vector<char>              duplicate( starts, 0 );
    std::vector<std::thread>       threads;

    // one time limit for all starts (they run side by side)
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds( (int64_t) ( aOptions.timeLimit * 1000 ) );
    std::mutex         seenMutex;
    std::set<uint64_t> seen;

    auto claim = [&]( uint64_t aLayout )
    {
        std::lock_guard<std::mutex> lock( seenMutex );
        return seen.insert( aLayout ).second;
    };

    for( int v = 0; v < starts; ++v )
    {
        threads.emplace_back(
                [&, v]()
                {
                    PLACER placer( parts[v], aNetCount, aOutline, aOptions, v + aOptions.seed * starts );
                    placer.Limit( deadline, aOptions.timeLimit > 0, claim );
                    results[v] = placer.Run();
                    duplicate[v] = placer.Duplicate();
                } );
    }

    for( std::thread& t : threads )
        t.join();

    int best = 0;

    while( best + 1 < starts && duplicate[best] )
        best++;

    for( int v = best + 1; v < starts; ++v )
    {
        if( duplicate[v] )
            continue;

        const RESULT &a = results[v], &b = results[best];

        if( a.unplaced.size() < b.unplaced.size()
            || ( a.unplaced.size() == b.unplaced.size() && a.hpwlAfter < b.hpwlAfter ) )
            best = v;
    }

    if( aOptions.progress && aOptions.progress->cancel )
    {
        RESULT res;
        res.cancelled = true;
        return res;      // aParts as they were
    }

    aParts = std::move( parts[best] );
    return results[best];
}


double Hpwl( const std::vector<PART>& aParts, int aNetCount )
{
    PLACER placer( const_cast<std::vector<PART>&>( aParts ), aNetCount, SHAPE_POLY_SET(), OPTIONS() );
    return placer.hpwl();
}

} // namespace kicadd::autoplace
