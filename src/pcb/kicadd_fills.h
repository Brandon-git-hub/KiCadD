/*
 * KiCadD: the zone fills of one copper layer, indexed for walking tracks over them: where a
 * segment crosses a fill's edge (a grid of the edges) and which fills are over a point. Shared by
 * the reference-plane check and the impedance profile.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <geometry/seg.h>
#include <geometry/shape_poly_set.h>
#include <math/box2.h>

namespace kicadd::fills
{

constexpr int CELL = 1000000;   // nm, the fill-edge grid


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

    /// (aLayer, net) of each fill over p.
    void refsAt( int aLayer, const VECTOR2I& p, std::vector<std::pair<int, int>>& aOut ) const
    {
        for( const FILL& f : fills )
        {
            if( f.box.Contains( p ) && f.poly->Contains( p, -1, 0, true ) )
                aOut.push_back( { aLayer, f.net } );
        }
    }

    /// Any fill over p.
    bool any( const VECTOR2I& p ) const
    {
        for( const FILL& f : fills )
        {
            if( f.box.Contains( p ) && f.poly->Contains( p, -1, 0, true ) )
                return true;
        }

        return false;
    }
};

} // namespace kicadd::fills
