/*
 * KiCadD M5c: automatic placement. See kicadd_autoplace.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_autoplace.h"

#include <trigo.h>
#include <geometry/eda_angle.h>

#include <algorithm>
#include <cmath>
#include <limits>
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


/// Everything the placer needs per part, precomputed for the four rotations.
struct WORK
{
    BOX2I                 box[4];        ///< courtyard (+ silk) box relative to the anchor
    BOX2I                 cu[4];         ///< pads' box relative to the anchor
    std::vector<VECTOR2I> pad[4];        ///< pad offsets
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

            for( int r = 0; r < 4; ++r )
            {
                m_work[i].box[r] = boxOf( p.outline, r, unit );
                m_work[i].cu[r] = boxOf( p.copper, r, m_work[i].box[r] );

                for( const PAD& pad : p.pads )
                    m_work[i].pad[r].push_back( rotated( pad.offset, r ) );
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
    }

    RESULT Run( bool aRefine = true )
    {
        RESULT res;
        res.hpwlBefore = hpwl();

        global();
        legalize( res );

        for( int pass = 0; aRefine && pass < m_opt.refinePasses; ++pass )
        {
            if( !refine() )
                break;
        }

        centre();

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( m_parts[i].movable && m_work[i].placed )
                res.moved++;
        }

        res.hpwlAfter = hpwl();
        return res;
    }

    double hpwl() const
    {
        double total = 0;

        for( int n = 0; n < m_netCount; ++n )
            total += netHpwl( n );

        return total / 1e6;
    }

private:
    VECTOR2I padPos( int aPart, int aPad ) const
    {
        const PART& p = m_parts[aPart];
        return p.pos + m_work[aPart].pad[p.rot & 3][aPad];
    }

    BOX2I boxAt( int aPart, const VECTOR2I& aPos, int aRot ) const
    {
        BOX2I b = m_work[aPart].box[aRot & 3];
        b.Move( aPos );
        return b;
    }

    BOX2I cuAt( int aPart, const VECTOR2I& aPos, int aRot ) const
    {
        BOX2I b = m_work[aPart].cu[aRot & 3];
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

                VECTOR2I want = padPos( q, b ) - m_work[i].pad[aRot & 3][k];
                sx += m_weight[n] * want.x;
                sy += m_weight[n] * want.y;
                sw += m_weight[n];
            }
        }

        const WORK& w = m_work[i];

        if( w.affPart >= 0 && m_work[w.affPart].placed )
        {
            VECTOR2I want = padPos( w.affPart, w.affOther ) - w.pad[aRot & 3][w.affPad];
            sx += 2.0 * want.x;
            sy += 2.0 * want.y;
            sw += 2.0;
        }

        if( sw <= 0 )
            return false;

        aOut = VECTOR2I( KiROUND( sx / sw ), KiROUND( sy / sw ) );
        return true;
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
    bool free( int i, const VECTOR2I& aPos, int aRot ) const
    {
        if( !m_opt.obstacles.empty() )
        {
            BOX2I cu = cuAt( i, aPos, aRot );

            for( const BOX2I& o : m_opt.obstacles )
            {
                if( cu.Intersects( o ) )
                    return false;
            }
        }

        BOX2I b = boxAt( i, aPos, aRot );
        b.Inflate( m_opt.spacing );

        for( size_t j = 0; j < m_parts.size(); ++j )
        {
            if( (int) j == i || !m_work[j].placed )
                continue;

            if( b.Intersects( boxAt( (int) j, m_parts[j].pos, m_parts[j].rot ) ) )
                return false;
        }

        return true;
    }

    bool legalAt( int i, const VECTOR2I& aPos, int aRot ) const
    {
        return insideBoard( boxAt( i, aPos, aRot ) ) && free( i, aPos, aRot );
    }

    /// Nearest legal anchor to aTarget for part i in rotation aRot (square rings on the grid).
    bool nearestLegal( int i, const VECTOR2I& aTarget, int aRot, VECTOR2I& aOut, int aMaxRing = -1 ) const
    {
        const int g = m_opt.grid;
        VECTOR2I  t( KiROUND( (double) aTarget.x / g ) * g, KiROUND( (double) aTarget.y / g ) * g );

        // keep the search inside what could fit at all
        const BOX2I& rel = m_work[i].box[aRot & 3];
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

            if( !insideBoard( boxAt( (int) i, p.pos, p.rot ) ) )
            {
                double a = k * 2.39996 + m_variant * 1.1;
                double rr = spread * std::sqrt( ( k + 0.5 ) / ( m_parts.size() + 1.0 ) );
                p.pos = c + VECTOR2I( KiROUND( rr * std::cos( a ) ), KiROUND( rr * std::sin( a ) ) );
                m_newcomer[i] = true;
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

        for( size_t i = 0; i < m_parts.size(); ++i )
        {
            if( !m_parts[i].movable || !m_work[i].placed )
                continue;

            if( !legalAt( (int) i, m_parts[i].pos, m_parts[i].rot ) )
            {
                for( size_t j = 0; j < m_parts.size(); ++j )
                    m_parts[j].pos = keep[j];

                return;
            }
        }
    }

    void legalize( RESULT& aRes )
    {
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

        const VECTOR2I spots[8] = { m_area.GetOrigin(),
                                    { m_area.GetRight(), m_area.GetTop() },
                                    m_area.GetEnd(),
                                    { m_area.GetLeft(), m_area.GetBottom() },
                                    { m_area.Centre().x, m_area.GetTop() },
                                    { m_area.GetRight(), m_area.Centre().y },
                                    { m_area.Centre().x, m_area.GetBottom() },
                                    { m_area.GetLeft(), m_area.Centre().y } };
        int spot = 0;

        for( int i : order )
        {
            VECTOR2I target = m_parts[i].pos, at;

            if( !connected( i ) && m_newcomer[i] )
                target = spots[spot++ % 8];

            // satellites go to what they belong to, now that it has its final place
            if( m_work[i].affPart >= 0 && m_work[m_work[i].affPart].placed )
                pullPoint( i, m_parts[i].rot, target );

            bool ok = nearestLegal( i, target, m_parts[i].rot, at );

            // doesn't fit as it is turned: try the other three ways round
            for( int k = 1; k < 4 && !ok && m_opt.rotate; ++k )
            {
                int rot = ( m_parts[i].rot + k ) & 3;

                if( nearestLegal( i, target, rot, at ) )
                {
                    m_parts[i].rot = rot;
                    ok = true;
                }
            }

            if( ok )
            {
                m_parts[i].pos = at;
                m_work[i].placed = true;
            }
            else
            {
                aRes.unplaced.push_back( m_parts[i].ref );
            }
        }
    }

    bool refine()
    {
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
            PART&    p = m_parts[i];
            VECTOR2I keepPos = p.pos;
            int      keepRot = p.rot;
            double   bestCost = localCost( i );
            VECTOR2I bestPos = keepPos;
            int      bestRot = keepRot;

            for( int r = 0; r < ( m_opt.rotate ? 4 : 1 ); ++r )
            {
                int      rot = m_opt.rotate ? r : keepRot;
                VECTOR2I want, at;

                if( !pullPoint( i, rot, want ) )
                    continue;

                if( !nearestLegal( i, want, rot, at ) )
                    continue;

                p.pos = at;
                p.rot = rot;
                double cost = localCost( i );

                if( cost < bestCost - 1000.0 )      // at least 1 µm better
                {
                    bestCost = cost;
                    bestPos = at;
                    bestRot = rot;
                }
            }

            p.pos = bestPos;
            p.rot = bestRot;

            if( bestPos != keepPos || bestRot != keepRot )
                improved = true;
        }

        // Swap parts of about the same size (two resistors, two caps): kept when the wire
        // length of both goes down and both still fit.
        for( size_t a = 0; a < order.size(); ++a )
        {
            for( size_t bi = a + 1; bi < order.size(); ++bi )
            {
                int   i = order[a], j = order[bi];
                PART &p = m_parts[i], &q = m_parts[j];
                BOX2I bp = m_work[i].box[p.rot & 3], bq = m_work[j].box[q.rot & 3];

                if( std::abs( bp.GetWidth() - bq.GetWidth() ) > m_opt.grid
                    || std::abs( bp.GetHeight() - bq.GetHeight() ) > m_opt.grid )
                    continue;

                double before = localCost( i ) + localCost( j );
                std::swap( p.pos, q.pos );

                bool fits = legalAt( i, p.pos, p.rot ) && legalAt( j, q.pos, q.rot );

                if( fits && localCost( i ) + localCost( j ) < before - 1000.0 )
                    improved = true;
                else
                    std::swap( p.pos, q.pos );
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
    int                                           m_variant = 0;
    std::vector<std::vector<int>>                 m_satellites;
};

} // namespace


RESULT Place( std::vector<PART>& aParts, int aNetCount, const SHAPE_POLY_SET& aOutline, const OPTIONS& aOptions )
{
    // A placement depends on where it starts: a few starts (spiral phase, order), keep the one
    // that places the most parts with the shortest wire length.
    // The starts are independent (own copy of the parts, the outline only read): run them in
    // parallel.
    const int                      starts = std::max( 1, aOptions.starts );
    std::vector<std::vector<PART>> parts( starts, aParts );
    std::vector<RESULT>            results( starts );
    std::vector<std::thread>       threads;

    for( int v = 0; v < starts; ++v )
    {
        threads.emplace_back(
                [&, v]()
                {
                    PLACER placer( parts[v], aNetCount, aOutline, aOptions, v );
                    results[v] = placer.Run();
                } );
    }

    for( std::thread& t : threads )
        t.join();

    int best = 0;

    for( int v = 1; v < starts; ++v )
    {
        const RESULT &a = results[v], &b = results[best];

        if( a.unplaced.size() < b.unplaced.size()
            || ( a.unplaced.size() == b.unplaced.size() && a.hpwlAfter < b.hpwlAfter ) )
            best = v;
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
