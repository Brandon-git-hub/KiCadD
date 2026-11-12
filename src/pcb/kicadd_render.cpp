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
#include <pcb_field.h>
#include <pcb_text.h>
#include <geometry/shape_arc.h>
#include <geometry/shape_circle.h>
#include <geometry/shape_compound.h>
#include <geometry/shape_line_chain.h>
#include <geometry/shape_poly_set.h>
#include <geometry/shape_rect.h>
#include <geometry/shape_segment.h>
#include <geometry/shape_simple.h>

#include <algorithm>
#include <cmath>

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


void addChain( const SHAPE_LINE_CHAIN& aChain, kicadd::render::Polyline* aOut )
{
    for( int i = 0; i < aChain.PointCount(); ++i )
    {
        aOut->add_xy( aChain.CPoint( i ).x );
        aOut->add_xy( aChain.CPoint( i ).y );
    }
}


/// Silk and fab layers shown by the view, under fixed names (users may rename layers).
const std::vector<std::pair<PCB_LAYER_ID, const char*>>& artLayers()
{
    static const std::vector<std::pair<PCB_LAYER_ID, const char*>> layers = {
        { F_SilkS, "F.SilkS" }, { B_SilkS, "B.SilkS" }, { F_Fab, "F.Fab" }, { B_Fab, "B.Fab" } };
    return layers;
}


/// Flattens SHAPEs into Graphic polylines / polygons, joining consecutive segments of the
/// same width into one polyline (stroke-font text arrives as hundreds of short segments).
class ART_SINK
{
public:
    ART_SINK( google::protobuf::RepeatedPtrField<Graphic>* aOut, const char* aLayer, int aMaxError ) :
            m_out( aOut ), m_layer( aLayer ), m_maxError( std::max( aMaxError, 1000 ) )
    {}

    void Add( const SHAPE* aShape )
    {
        switch( aShape->Type() )
        {
        case SH_SEGMENT:
        {
            const SHAPE_SEGMENT* seg = static_cast<const SHAPE_SEGMENT*>( aShape );
            stroke( seg->GetSeg().A, seg->GetSeg().B, seg->GetWidth() );
            break;
        }

        case SH_ARC:
        {
            const SHAPE_ARC* arc = static_cast<const SHAPE_ARC*>( aShape );
            SHAPE_LINE_CHAIN chain = arc->ConvertToPolyline( m_maxError );

            for( int i = 0; i + 1 < chain.PointCount(); ++i )
                stroke( chain.CPoint( i ), chain.CPoint( i + 1 ), arc->GetWidth() );

            break;
        }

        case SH_LINE_CHAIN:
        {
            const SHAPE_LINE_CHAIN* chain = static_cast<const SHAPE_LINE_CHAIN*>( aShape );

            for( int i = 0; i < chain->SegmentCount(); ++i )
                stroke( chain->CSegment( i ).A, chain->CSegment( i ).B, chain->Width() );

            break;
        }

        case SH_CIRCLE:
        {
            const SHAPE_CIRCLE* c = static_cast<const SHAPE_CIRCLE*>( aShape );
            int    r = std::max( c->GetRadius(), 1 );
            double step = 2 * std::acos( std::max( 0.0, 1.0 - (double) m_maxError / r ) );
            int    n = std::clamp( step > 0 ? (int) std::ceil( 2 * M_PI / step ) : 64, 8, 64 );
            Graphic* g = begin( true, 0 );

            for( int i = 0; i < n; ++i )
            {
                double a = 2 * M_PI * i / n;
                g->add_xy( c->GetCenter().x + std::lround( r * std::cos( a ) ) );
                g->add_xy( c->GetCenter().y + std::lround( r * std::sin( a ) ) );
            }

            break;
        }

        case SH_RECT:
        {
            const SHAPE_RECT* rect = static_cast<const SHAPE_RECT*>( aShape );
            fill( rect->Outline() );
            break;
        }

        case SH_SIMPLE:
            fill( static_cast<const SHAPE_SIMPLE*>( aShape )->Vertices() );
            break;

        case SH_POLY_SET:
        {
            SHAPE_POLY_SET polys = *static_cast<const SHAPE_POLY_SET*>( aShape );
            polys.Fracture();   // holes joined into the outline: fill with the nonzero rule

            for( int i = 0; i < polys.OutlineCount(); ++i )
                fill( polys.COutline( i ) );

            break;
        }

        case SH_COMPOUND:
            for( const SHAPE* sub : static_cast<const SHAPE_COMPOUND*>( aShape )->Shapes() )
                Add( sub );

            break;

        default:
            break;
        }
    }

private:
    Graphic* begin( bool aFilled, int aWidth )
    {
        Graphic* g = m_out->Add();
        g->set_layer( m_layer );
        g->set_filled( aFilled );
        g->set_width( aWidth );
        m_open = aFilled ? nullptr : g;
        return g;
    }

    void stroke( const VECTOR2I& aA, const VECTOR2I& aB, int aWidth )
    {
        int n = m_open ? m_open->xy_size() : 0;

        if( !( m_open && m_open->width() == aWidth && n >= 2 && m_open->xy( n - 2 ) == aA.x
               && m_open->xy( n - 1 ) == aA.y ) )
        {
            begin( false, aWidth );
            m_open->add_xy( aA.x );
            m_open->add_xy( aA.y );
        }

        m_open->add_xy( aB.x );
        m_open->add_xy( aB.y );
    }

    void fill( const SHAPE_LINE_CHAIN& aChain )
    {
        if( aChain.PointCount() < 3 )
            return;

        Graphic* g = begin( true, aChain.Width() );

        for( int i = 0; i < aChain.PointCount(); ++i )
        {
            g->add_xy( aChain.CPoint( i ).x );
            g->add_xy( aChain.CPoint( i ).y );
        }
    }

    google::protobuf::RepeatedPtrField<Graphic>* m_out;
    const char*                                  m_layer;
    int                                          m_maxError;
    Graphic*                                     m_open = nullptr;
};


/// Text that the board shows: hidden fields and texts are skipped.
bool artVisible( const BOARD_ITEM* aItem )
{
    if( const EDA_TEXT* text = dynamic_cast<const EDA_TEXT*>( aItem ) )
        return text->IsVisible();

    return true;
}


constexpr unsigned ALL_ART = 0xF;   ///< bit i = artLayers()[i]


void addArt( const BOARD_ITEM* aItem, int aMaxError, google::protobuf::RepeatedPtrField<Graphic>* aOut,
             unsigned aMask = ALL_ART )
{
    if( aItem->Type() == PCB_REFERENCE_IMAGE_T || !artVisible( aItem ) )
        return;

    for( size_t i = 0; i < artLayers().size(); ++i )
    {
        const auto& [layer, name] = artLayers()[i];

        if( !( aMask & ( 1u << i ) ) || !aItem->IsOnLayer( layer ) )
            continue;

        std::shared_ptr<SHAPE> shape = aItem->GetEffectiveShape( layer );

        if( shape )
        {
            ART_SINK sink( aOut, name, aMaxError );
            sink.Add( shape.get() );
        }
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


void footprintArt( const FOOTPRINT* aFp, int aMaxError, google::protobuf::RepeatedPtrField<Graphic>* aOut,
                   unsigned aMask )
{
    for( const PCB_FIELD* field : aFp->GetFields() )
        addArt( field, aMaxError, aOut, aMask );

    for( const BOARD_ITEM* item : aFp->GraphicalItems() )
        addArt( item, aMaxError, aOut, aMask );
}


void serializeFootprint( const FOOTPRINT* aFp, int aMaxError, Footprint* aOut, unsigned aArtMask )
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

    if( aArtMask )
        footprintArt( aFp, aMaxError, aOut->mutable_gfx(), aArtMask );
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
    t->set_layer( LSET::Name( aTrack->GetLayer() ).ToStdString() );
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
        aOut->add_layers( LSET::Name( layer ).ToStdString() );

    const SHAPE_POLY_SET* outline = aZone->Outline();

    for( int i = 0; i < outline->OutlineCount(); ++i )
        addChain( outline->COutline( i ), aOut->add_outlines() );
}


void serializeItem( const BOARD_ITEM* aItem, int aMaxError, RenderItems& aOut, unsigned aArtMask = ALL_ART )
{
    switch( aItem->Type() )
    {
    case PCB_FOOTPRINT_T:
        serializeFootprint( static_cast<const FOOTPRINT*>( aItem ), aMaxError, aOut.add_footprints(), aArtMask );
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


void RENDER_FEED::Snapshot( RenderSnapshot& aOut, bool aWithArt ) const
{
    const int maxError = m_board->GetDesignSettings().m_MaxError;

    aOut.set_revision( m_revision );
    aOut.set_file( m_board->GetFileName().ToStdString() );

    int order = 0;

    for( PCB_LAYER_ID layer : m_board->GetEnabledLayers().CuStack() )
    {
        LayerInfo* li = aOut.add_layers();
        li->set_name( LSET::Name( layer ).ToStdString() );
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
        serializeItem( fp, maxError, *items, aWithArt ? ALL_ART : 0 );

    for( const PCB_TRACK* t : m_board->Tracks() )
        serializeItem( t, maxError, *items );

    for( const ZONE* z : m_board->Zones() )
        serializeItem( z, maxError, *items );

    if( aWithArt )
    {
        for( const BOARD_ITEM* item : m_board->Drawings() )
            addArt( item, maxError, aOut.mutable_drawings() );
    }
}


void RENDER_FEED::GetArt( const std::vector<std::string>& aLayers, Art& aOut ) const
{
    const int maxError = m_board->GetDesignSettings().m_MaxError;
    unsigned  mask = aLayers.empty() ? ALL_ART : 0;

    for( size_t i = 0; i < artLayers().size(); ++i )
    {
        if( std::find( aLayers.begin(), aLayers.end(), artLayers()[i].second ) != aLayers.end() )
            mask |= 1u << i;
    }

    for( size_t i = 0; i < artLayers().size(); ++i )
    {
        if( mask & ( 1u << i ) )
            aOut.add_layers( artLayers()[i].second );
    }

    aOut.set_revision( m_revision );

    for( const FOOTPRINT* fp : m_board->Footprints() )
    {
        FootprintArt* fa = aOut.add_footprints();
        fa->set_id( fp->m_Uuid.AsStdString() );
        footprintArt( fp, maxError, fa->mutable_gfx(), mask );
    }

    for( const BOARD_ITEM* item : m_board->Drawings() )
        addArt( item, maxError, aOut.mutable_drawings(), mask );
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
