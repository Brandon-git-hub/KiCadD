/*
 * KiCadD: where a connector opens and where its footprint wants the board edge (see
 * kicadd_mouth.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_mouth.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <footprint.h>
#include <pad.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <trigo.h>

#include "pcb/kicadd_autoplace.h"

namespace kicadd::mouth
{

using namespace kicadd::autoplace;

namespace
{

constexpr int MM = 1000000;

// a coordinate along aSide, from the anchor (positive the way the side faces)
int along( const VECTOR2I& aLocal, int aSide )
{
    switch( aSide )
    {
    case EDGE_LEFT:  return -aLocal.x;
    case EDGE_RIGHT: return aLocal.x;
    case EDGE_TOP:   return -aLocal.y;
    default:         return aLocal.y;
    }
}


int along( const BOX2I& aBox, int aSide )
{
    switch( aSide )
    {
    case EDGE_LEFT:  return -aBox.GetLeft();
    case EDGE_RIGHT: return aBox.GetRight();
    case EDGE_TOP:   return -aBox.GetTop();
    default:         return aBox.GetBottom();
    }
}

} // namespace


const char* SideName( int aSide )
{
    static const char* names[4] = { "left", "right", "top", "bottom" };
    return aSide >= 0 && aSide < 4 ? names[aSide] : "none";
}


MOUTH Find( const FOOTPRINT& aFp, std::optional<double> aOverhang )
{
    MOUTH out;

    const VECTOR2I  anchor = aFp.GetPosition();
    const EDA_ANGLE orient = aFp.GetOrientation();
    const bool      flipped = aFp.IsFlipped();

    auto toLocal = [&]( const VECTOR2I& aWorld )
    {
        VECTOR2I p = aWorld - anchor;
        RotatePoint( p, -orient );
        return p;
    };

    auto merge = [&]( std::optional<BOX2I>& aBox, const VECTOR2I& aLocal )
    {
        if( aBox )
            aBox->Merge( aLocal );
        else
            aBox = BOX2I( aLocal, VECTOR2I( 0, 0 ) );
    };

    // the pads' copper and the courtyard (else the footprint's body), rotation 0
    std::optional<BOX2I> cu, crt;
    VECTOR2D             padSum;

    for( const ::PAD* pad : aFp.Pads() )
    {
        const BOX2I b = pad->GetBoundingBox();

        for( const VECTOR2I& c : { b.GetOrigin(), b.GetEnd(), VECTOR2I( b.GetRight(), b.GetTop() ),
                                   VECTOR2I( b.GetLeft(), b.GetBottom() ) } )
            merge( cu, toLocal( c ) );

        padSum += VECTOR2D( toLocal( pad->GetPosition() ) );
    }

    if( !cu )
        return out;

    const VECTOR2D padCentre = padSum / (double) aFp.Pads().size();
    const SHAPE_POLY_SET& courtyard = aFp.GetCourtyard( flipped ? B_CrtYd : F_CrtYd );

    if( courtyard.OutlineCount() > 0 )
    {
        for( auto it = courtyard.CIterate(); it; ++it )
            merge( crt, toLocal( *it ) );
    }
    else
    {
        const BOX2I b = aFp.GetBoundingBox( false );

        for( const VECTOR2I& c : { b.GetOrigin(), b.GetEnd(), VECTOR2I( b.GetRight(), b.GetTop() ),
                                   VECTOR2I( b.GetLeft(), b.GetBottom() ) } )
            merge( crt, toLocal( c ) );
    }

    // the footprint's own edge line: a "PCB Edge" text, the longest straight line on its layer
    // running along it (within 1.5 mm, across where the text is)
    for( const BOARD_ITEM* item : aFp.GraphicalItems() )
    {
        const PCB_TEXT* text = dynamic_cast<const PCB_TEXT*>( item );

        if( !text || ( !text->IsOnLayer( Dwgs_User ) && !text->IsOnLayer( Cmts_User ) ) )
            continue;

        wxString t = text->GetText().Lower();
        t.Replace( wxS( " " ), wxEmptyString );

        if( !t.Contains( wxS( "pcbedge" ) ) && !t.Contains( wxS( "boardedge" ) ) )
            continue;

        const VECTOR2I at = toLocal( text->GetPosition() );
        int            bestLen = 0;

        for( const BOARD_ITEM* other : aFp.GraphicalItems() )
        {
            const PCB_SHAPE* line = dynamic_cast<const PCB_SHAPE*>( other );

            if( !line || line->GetShape() != SHAPE_T::SEGMENT || line->GetLayer() != text->GetLayer() )
                continue;

            const VECTOR2I a = toLocal( line->GetStart() ), b = toLocal( line->GetEnd() );
            const bool     horizontal = std::abs( a.y - b.y ) < 1000, vertical = std::abs( a.x - b.x ) < 1000;

            if( horizontal == vertical )
                continue;

            const int len = horizontal ? std::abs( a.x - b.x ) : std::abs( a.y - b.y );
            const int off = horizontal ? std::abs( at.y - a.y ) : std::abs( at.x - a.x );
            const int lo = horizontal ? std::min( a.x, b.x ) : std::min( a.y, b.y );
            const int hi = horizontal ? std::max( a.x, b.x ) : std::max( a.y, b.y );
            const int pos = horizontal ? at.x : at.y;

            if( off > 1.5 * MM || pos < lo - MM || pos > hi + MM || len < MM || len <= bestLen )
                continue;

            bestLen = len;

            // the opening is on the side of the line away from the pins
            if( horizontal )
                out.side = padCentre.y < a.y ? EDGE_BOTTOM : EDGE_TOP;
            else
                out.side = padCentre.x < a.x ? EDGE_RIGHT : EDGE_LEFT;

            out.edgeLine = along( a, out.side );
            out.marked = true;
        }

        if( out.marked )
            break;
    }

    // by shape: the courtyard well past the pads on one side only
    if( !out.marked )
    {
        int reach[4];

        for( int s = 0; s < 4; ++s )
            reach[s] = along( *crt, s ) - along( *cu, s );

        const int best = (int) ( std::max_element( reach, reach + 4 ) - reach );
        int       second = 0;

        for( int s = 0; s < 4; ++s )
        {
            if( s != best )
                second = std::max( second, reach[s] );
        }

        if( reach[best] < 1.5 * MM || reach[best] < 2 * second )
            return out;

        out.side = best;
    }

    out.front = along( *crt, out.side );

    if( aOverhang || !out.marked )
    {
        out.edgeLine = out.front - KiROUND( aOverhang.value_or( 0.0 ) * MM );
        out.marked = out.marked && !aOverhang;
    }

    // its pins stay on the board
    if( along( *cu, out.side ) > out.edgeLine )
    {
        char buf[160];
        std::snprintf( buf, sizeof( buf ), "its pads would reach %.2f mm past the board edge",
                       ( along( *cu, out.side ) - out.edgeLine ) / 1e6 );
        out.error = buf;
    }

    return out;
}

} // namespace kicadd::mouth
