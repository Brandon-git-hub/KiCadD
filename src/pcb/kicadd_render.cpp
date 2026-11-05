/*
 * KiCadD - render snapshot and change journal for remote views.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "kicadd_render.h"

#include <board_design_settings.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_poly_set.h>

using namespace kicadd::render;

namespace kicadd
{

namespace
{

constexpr size_t MAX_REMOVED_ENTRIES = 50000;


void setPoint( kicadd::board::Point* aOut, const VECTOR2I& aPt )
{
    aOut->set_x( aPt.x );
    aOut->set_y( aPt.y );
}


void addChain( const SHAPE_LINE_CHAIN& aChain, Polyline* aOut )
{
    for( int i = 0; i < aChain.PointCount(); ++i )
    {
        aOut->add_xy( aChain.CPoint( i ).x );
        aOut->add_xy( aChain.CPoint( i ).y );
    }
}


std::string netName( const BOARD_CONNECTED_ITEM* aItem )
{
    return aItem->GetNetCode() > 0 ? aItem->GetNetname().ToStdString() : std::string();
}


void serializePad( const PAD* aPad, int aMaxError, Pad* aOut )
{
    aOut->set_number( aPad->GetNumber().ToStdString() );
    aOut->set_net( netName( aPad ) );

    const bool front = aPad->IsOnLayer( F_Cu );
    const bool back = aPad->IsOnLayer( B_Cu );

    if( front && back )
        aOut->set_side( SIDE_THROUGH );
    else
        aOut->set_side( back ? SIDE_BACK : SIDE_FRONT );

    PCB_LAYER_ID layer = front ? F_Cu : back ? B_Cu : aPad->GetLayerSet().Seq().front();

    setPoint( aOut->mutable_pos(), aPad->GetPosition() );
    setPoint( aOut->mutable_size(), aPad->GetSize( layer ) );
    aOut->set_rot_decideg( aPad->GetOrientation().AsTenthsOfADegree() );

    if( aPad->HasHole() )
        aOut->set_drill( aPad->GetDrillSize().x );

    switch( aPad->GetShape( layer ) )
    {
    case PAD_SHAPE::CIRCLE:    aOut->set_shape( PS_CIRCLE ); break;
    case PAD_SHAPE::RECTANGLE: aOut->set_shape( PS_RECT ); break;
    case PAD_SHAPE::OVAL:      aOut->set_shape( PS_OVAL ); break;

    case PAD_SHAPE::ROUNDRECT:
        aOut->set_shape( PS_ROUNDRECT );
        aOut->set_rr_ratio( static_cast<float>( aPad->GetRoundRectRadiusRatio( layer ) ) );
        break;

    default:
    {
        aOut->set_shape( PS_POLY );
        SHAPE_POLY_SET poly;
        aPad->TransformShapeToPolygon( poly, layer, 0, aMaxError, ERROR_INSIDE );

        if( poly.OutlineCount() > 0 )
            addChain( poly.COutline( 0 ), aOut->mutable_poly() );

        break;
    }
    }
}


void serializeFootprint( const FOOTPRINT* aFp, int aMaxError, Footprint* aOut )
{
    aOut->set_id( aFp->m_Uuid.AsStdString() );
    aOut->set_ref( aFp->GetReference().ToStdString() );
    aOut->set_value( aFp->GetValue().ToStdString() );
    aOut->set_back( aFp->GetLayer() == B_Cu );
    setPoint( aOut->mutable_pos(), aFp->GetPosition() );
    aOut->set_rot_decideg( aFp->GetOrientation().AsTenthsOfADegree() );
    aOut->set_locked( aFp->IsLocked() );

    for( const PAD* pad : aFp->Pads() )
        serializePad( pad, aMaxError, aOut->add_pads() );

    const SHAPE_POLY_SET& cy = aFp->GetCourtyard( aOut->back() ? B_CrtYd : F_CrtYd );

    if( cy.OutlineCount() > 0 )
        addChain( cy.COutline( 0 ), aOut->mutable_courtyard() );

    BOX2I bb = aFp->GetBoundingBox( false );
    setPoint( aOut->mutable_bbox_min(), bb.GetOrigin() );
    setPoint( aOut->mutable_bbox_max(), bb.GetEnd() );
}


void serializeTrack( const PCB_TRACK* aTrack, RenderItems& aOut )
{
    if( aTrack->Type() == PCB_VIA_T )
    {
        const PCB_VIA* via = static_cast<const PCB_VIA*>( aTrack );
        Via*           v = aOut.add_vias();
        v->set_id( via->m_Uuid.AsStdString() );
        v->set_net( netName( via ) );
        setPoint( v->mutable_pos(), via->GetPosition() );
        v->set_diameter( via->GetWidth( F_Cu ) );
        v->set_drill( via->GetDrillValue() );
        return;
    }

    Track* t = aOut.add_tracks();
    t->set_id( aTrack->m_Uuid.AsStdString() );
    t->set_layer( aTrack->GetLayerName().ToStdString() );
    t->set_net( netName( aTrack ) );
    setPoint( t->mutable_a(), aTrack->GetStart() );
    setPoint( t->mutable_b(), aTrack->GetEnd() );
    t->set_width( aTrack->GetWidth() );

    if( aTrack->Type() == PCB_ARC_T )
    {
        t->set_arc( true );
        setPoint( t->mutable_mid(), static_cast<const PCB_ARC*>( aTrack )->GetMid() );
    }
}


bool rendersZone( const ZONE* aZone )
{
    return !aZone->IsTeardropArea();
}


void serializeZone( const ZONE* aZone, Zone* aOut )
{
    aOut->set_id( aZone->m_Uuid.AsStdString() );
    aOut->set_net( netName( aZone ) );
    aOut->set_keepout( aZone->GetIsRuleArea() );

    for( PCB_LAYER_ID layer : aZone->GetLayerSet().Seq() )
        aOut->add_layers( aZone->GetBoard()->GetLayerName( layer ).ToStdString() );

    const SHAPE_POLY_SET* outline = aZone->Outline();

    for( int i = 0; i < outline->OutlineCount(); ++i )
        addChain( outline->COutline( i ), aOut->add_outlines() );
}


void serializeItem( const BOARD_ITEM* aItem, int aMaxError, RenderItems& aOut )
{
    switch( aItem->Type() )
    {
    case PCB_FOOTPRINT_T:
        serializeFootprint( static_cast<const FOOTPRINT*>( aItem ), aMaxError, aOut.add_footprints() );
        break;

    case PCB_TRACE_T:
    case PCB_ARC_T:
    case PCB_VIA_T:
        serializeTrack( static_cast<const PCB_TRACK*>( aItem ), aOut );
        break;

    case PCB_ZONE_T:
        if( rendersZone( static_cast<const ZONE*>( aItem ) ) )
            serializeZone( static_cast<const ZONE*>( aItem ), aOut.add_zones() );

        break;

    default:
        break;
    }
}

} // namespace


RENDER_FEED::RENDER_FEED( BOARD* aBoard ) :
        m_board( aBoard )
{
    if( m_board )
        m_board->AddListener( this );
}


RENDER_FEED::~RENDER_FEED()
{
    if( m_board )
        m_board->RemoveListener( this );
}


const BOARD_ITEM* RENDER_FEED::trackedItem( const BOARD_ITEM* aItem )
{
    if( !aItem )
        return nullptr;

    switch( aItem->Type() )
    {
    case PCB_FOOTPRINT_T:
    case PCB_TRACE_T:
    case PCB_ARC_T:
    case PCB_VIA_T:
    case PCB_ZONE_T:
        return aItem;

    default:
        // Pads, fields, footprint graphics: the owning footprint changed.
        if( const FOOTPRINT* fp = aItem->GetParentFootprint() )
            return fp;

        return nullptr;   // board graphics, markers, groups: not part of the view (yet)
    }
}


void RENDER_FEED::touch( const BOARD_ITEM* aItem )
{
    if( const BOARD_ITEM* item = trackedItem( aItem ) )
    {
        ENTRY& e = m_journal[item->m_Uuid.AsStdString()];

        if( e.removed )
            --m_removedCount;

        e = { ++m_revision, false };
    }
}


void RENDER_FEED::remove( const BOARD_ITEM* aItem )
{
    const BOARD_ITEM* item = trackedItem( aItem );

    if( !item )
        return;

    // A removed pad means its footprint changed, not that the footprint went away.
    if( item != aItem )
    {
        touch( item );
        return;
    }

    ENTRY& e = m_journal[item->m_Uuid.AsStdString()];

    if( !e.removed )
        ++m_removedCount;

    e = { ++m_revision, true };
    compactRemovals();
}


void RENDER_FEED::compactRemovals()
{
    if( m_removedCount <= MAX_REMOVED_ENTRIES )
        return;

    // Forget the older half of the removals; views older than that resync.
    uint64_t cutoff = m_floor + ( m_revision - m_floor ) / 2;

    for( auto it = m_journal.begin(); it != m_journal.end(); )
    {
        if( it->second.removed && it->second.revision <= cutoff )
        {
            it = m_journal.erase( it );
            --m_removedCount;
        }
        else
        {
            ++it;
        }
    }

    m_floor = cutoff;
}


void RENDER_FEED::OnBoardItemAdded( BOARD&, BOARD_ITEM* aItem ) { touch( aItem ); }
void RENDER_FEED::OnBoardItemChanged( BOARD&, BOARD_ITEM* aItem ) { touch( aItem ); }
void RENDER_FEED::OnBoardItemRemoved( BOARD&, BOARD_ITEM* aItem ) { remove( aItem ); }

void RENDER_FEED::OnBoardItemsAdded( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    for( BOARD_ITEM* item : aItems )
        touch( item );
}

void RENDER_FEED::OnBoardItemsChanged( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    for( BOARD_ITEM* item : aItems )
        touch( item );
}

void RENDER_FEED::OnBoardItemsRemoved( BOARD&, std::vector<BOARD_ITEM*>& aItems )
{
    for( BOARD_ITEM* item : aItems )
        remove( item );
}

void RENDER_FEED::OnBoardCompositeUpdate( BOARD&, std::vector<BOARD_ITEM*>& aAdded,
                                          std::vector<BOARD_ITEM*>& aRemoved,
                                          std::vector<BOARD_ITEM*>& aChanged )
{
    for( BOARD_ITEM* item : aRemoved )
        remove( item );

    for( BOARD_ITEM* item : aAdded )
        touch( item );

    for( BOARD_ITEM* item : aChanged )
        touch( item );
}


void RENDER_FEED::Snapshot( RenderSnapshot& aOut ) const
{
    const int maxError = m_board->GetDesignSettings().m_MaxError;

    aOut.set_revision( m_revision );
    aOut.set_file( m_board->GetFileName().ToStdString() );

    int order = 0;

    for( PCB_LAYER_ID layer : m_board->GetEnabledLayers().CuStack() )
    {
        LayerInfo* li = aOut.add_layers();
        li->set_name( m_board->GetLayerName( layer ).ToStdString() );
        li->set_copper( true );
        li->set_order( order++ );
    }

    SHAPE_POLY_SET outlines;

    if( m_board->GetBoardPolygonOutlines( outlines, true ) )
    {
        for( int i = 0; i < outlines.OutlineCount(); ++i )
        {
            addChain( outlines.COutline( i ), aOut.add_outline() );

            for( int h = 0; h < outlines.HoleCount( i ); ++h )
                addChain( outlines.CHole( i, h ), aOut.add_outline() );
        }
    }

    RenderItems* items = aOut.mutable_items();

    for( const FOOTPRINT* fp : m_board->Footprints() )
        serializeItem( fp, maxError, *items );

    for( const PCB_TRACK* t : m_board->Tracks() )
        serializeItem( t, maxError, *items );

    for( const ZONE* z : m_board->Zones() )
        serializeItem( z, maxError, *items );
}


void RENDER_FEED::Changes( uint64_t aSince, RenderChanges& aOut ) const
{
    aOut.set_revision( m_revision );

    if( aSince == m_revision )
        return;

    // aSince ahead of us: the view saw a previous board (open_board restarts revisions).
    if( aSince > m_revision || aSince < m_floor )
    {
        aOut.set_resync( true );
        return;
    }

    const int maxError = m_board->GetDesignSettings().m_MaxError;

    for( const auto& [id, entry] : m_journal )
    {
        if( entry.revision <= aSince )
            continue;

        if( entry.removed )
        {
            aOut.add_removed( id );
            continue;
        }

        // The item may have been deleted again without our noticing (e.g. never added
        // through the listener); then report it as removed.
        if( BOARD_ITEM* item = m_board->ResolveItem( KIID( id ), true ) )
            serializeItem( item, maxError, *aOut.mutable_upsert() );
        else
            aOut.add_removed( id );
    }
}

} // namespace kicadd
