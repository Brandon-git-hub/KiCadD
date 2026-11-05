/*
 * KiCadD - local (incremental) DRC around a set of footprints.
 *
 * Pairwise semantics follow DRC_TEST_PROVIDER_COPPER_CLEARANCE::testSingleLayerItemAgainstItem
 * (per copper layer, hole shape where a pad/via isn't flashed, net-tie exclusion, DRC epsilon)
 * DRC_TEST_PROVIDER_COURTYARD_CLEARANCE (front/back courtyard collision) and
 * DRC_TEST_PROVIDER_EDGE_CLEARANCE (copper vs Edge.Cuts / Margin, copper outside the board).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "kicadd_local_drc.h"

#include <array>
#include <cstdlib>
#include <set>
#include <tuple>
#include <unordered_set>

#include <board.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <drc/drc_rule.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_board_outline.h>
#include <pcb_shape.h>
#include <pcb_track.h>
#include <geometry/shape.h>
#include <geometry/shape_arc.h>
#include <geometry/shape_poly_set.h>
#include <geometry/shape_segment.h>

using namespace kicadd::board;

namespace
{

std::string handleOf( const BOARD_ITEM* aItem )
{
    if( aItem->Type() == PCB_PAD_T )
    {
        const PAD*       pad = static_cast<const PAD*>( aItem );
        const FOOTPRINT* fp = pad->GetParentFootprint();
        return ( fp ? fp->GetReference().ToStdString() : std::string( "?" ) ) + "."
               + pad->GetNumber().ToStdString();
    }

    if( aItem->Type() == PCB_FOOTPRINT_T )
        return static_cast<const FOOTPRINT*>( aItem )->GetReference().ToStdString();

    if( aItem->Type() == PCB_SHAPE_T && ( aItem->IsOnLayer( Edge_Cuts ) || aItem->IsOnLayer( Margin ) ) )
    {
        std::string what = aItem->IsOnLayer( Edge_Cuts ) ? "edge" : "margin";

        if( const FOOTPRINT* fp = aItem->GetParentFootprint() )
            return fp->GetReference().ToStdString() + "." + what;

        return what;
    }

    return aItem->m_Uuid.AsStdString().substr( 0, 8 );
}


class LOCAL_DRC
{
public:
    LOCAL_DRC( BOARD* aBoard ) :
            m_board( aBoard ),
            m_bds( aBoard->GetDesignSettings() ),
            m_engine( m_bds.m_DRCEngine.get() ),
            m_epsilon( m_bds.GetDRCEpsilon() ),
            m_maxClearance( m_bds.GetBiggestClearanceValue() )
    {
        DRC_CONSTRAINT worst;

        if( m_engine && m_engine->QueryWorstConstraint( EDGE_CLEARANCE_CONSTRAINT, worst ) )
            m_largestEdgeClearance = worst.GetValue().Min();
    }

    bool Ready() const { return m_engine != nullptr; }

    void Run( const std::vector<FOOTPRINT*>& aFootprints )
    {
        std::unordered_set<const FOOTPRINT*> moved( aFootprints.begin(), aFootprints.end() );

        for( FOOTPRINT* fp : aFootprints )
        {
            BOX2I area = fp->GetBoundingBox( false );
            area.Inflate( m_maxClearance );

            for( FOOTPRINT* other : m_board->Footprints() )
            {
                if( other == fp )
                    continue;

                // Each moved/moved pair once.
                if( moved.count( other ) && other < fp )
                    continue;

                if( !area.Intersects( other->GetBoundingBox( false ) ) )
                    continue;

                for( PAD* pad : fp->Pads() )
                {
                    for( PAD* otherPad : other->Pads() )
                        testCopperPair( pad, otherPad );
                }

                testCourtyards( fp, other );
            }

            for( PCB_TRACK* track : m_board->Tracks() )
            {
                if( !area.Intersects( track->GetBoundingBox() ) )
                    continue;

                for( PAD* pad : fp->Pads() )
                    testCopperPair( pad, track );
            }
        }

        if( !std::getenv( "KICADD_NO_EDGE_DRC" ) )
            testEdges( aFootprints );
    }

    std::vector<DrcViolation> TakeResults() { return std::move( m_results ); }

private:
    void report( DrcType aType, const BOARD_ITEM* aA, const BOARD_ITEM* aB, PCB_LAYER_ID aLayer,
                 int aRequired, int aActual, const VECTOR2I& aPos )
    {
        // One entry per (type, pair): through-hole pairs would otherwise repeat per layer.
        auto key = std::make_tuple( static_cast<int>( aType ), std::min( aA, aB ), std::max( aA, aB ) );

        if( !m_seen.insert( key ).second )
            return;

        DrcViolation v;
        v.set_type( aType );
        v.set_a( handleOf( aA ) );
        v.set_b( handleOf( aB ) );
        v.set_a_id( aA->m_Uuid.AsStdString() );
        v.set_b_id( aB->m_Uuid.AsStdString() );

        if( aA->IsConnected() )
            v.set_net_a( static_cast<const BOARD_CONNECTED_ITEM*>( aA )->GetNetname().ToStdString() );

        if( aB->IsConnected() )
            v.set_net_b( static_cast<const BOARD_CONNECTED_ITEM*>( aB )->GetNetname().ToStdString() );

        v.set_layer( m_board->GetLayerName( aLayer ).ToStdString() );
        v.set_required( aRequired );
        v.set_actual( aActual );
        v.mutable_pos()->set_x( aPos.x );
        v.mutable_pos()->set_y( aPos.y );
        m_results.push_back( std::move( v ) );
    }

    int subEpsilon( int aClearance ) const { return std::max( 0, aClearance - m_epsilon ); }

    /// Hole of aItem on aLayer for the hole-clearance test (null if none there).
    static std::shared_ptr<SHAPE_SEGMENT> holeOn( const BOARD_ITEM* aItem, PCB_LAYER_ID aLayer )
    {
        if( aItem->Type() == PCB_VIA_T )
        {
            if( !aItem->GetLayerSet().Contains( aLayer ) )
                return nullptr;
        }
        else if( !aItem->HasHole() )
        {
            return nullptr;
        }

        return aItem->GetEffectiveHoleShape( aLayer, HOLE_CLEARANCE_CONSTRAINT );
    }

    static int netOf( const BOARD_ITEM* aItem )
    {
        return aItem->IsConnected() ? static_cast<const BOARD_CONNECTED_ITEM*>( aItem )->GetNetCode() : 0;
    }

    bool netTieExcluded( const BOARD_ITEM* aA, const BOARD_ITEM* aB, PCB_LAYER_ID aLayer,
                         const VECTOR2I& aPos )
    {
        return m_engine->IsNetTieExclusion( netOf( aA ), aLayer, aPos, const_cast<BOARD_ITEM*>( aB ) )
               || m_engine->IsNetTieExclusion( netOf( aB ), aLayer, aPos, const_cast<BOARD_ITEM*>( aA ) );
    }

    void testCopperPair( PAD* aPad, BOARD_ITEM* aOther )
    {
        if( !aPad->GetBoundingBox().GetInflated( m_maxClearance ).Intersects( aOther->GetBoundingBox() ) )
            return;

        if( aOther->Type() == PCB_PAD_T )
        {
            testPadPath( aPad, static_cast<PAD*>( aOther ) );
        }
        else
        {
            testTrackPath( static_cast<PCB_TRACK*>( aOther ), aPad );

            if( aOther->Type() == PCB_VIA_T )
                testPadPath( aPad, aOther );
        }
    }

    /// Mirrors DRC_TEST_PROVIDER_COPPER_CLEARANCE::testPadAgainstItem for pad-vs-pad (full)
    /// and pad-vs-via (hole part only; via copper is tested from the track side).
    void testPadPath( PAD* aPad, BOARD_ITEM* aOther )
    {
        PAD*     pad = aPad;
        PAD*     otherPad = aOther->Type() == PCB_PAD_T ? static_cast<PAD*>( aOther ) : nullptr;
        PCB_VIA* otherVia = aOther->Type() == PCB_VIA_T ? static_cast<PCB_VIA*>( aOther ) : nullptr;

        // Pad pairs are visited in UUID order upstream.
        if( otherPad && pad->m_Uuid > otherPad->m_Uuid )
            std::swap( pad, otherPad );

        BOARD_ITEM* other = otherPad ? static_cast<BOARD_ITEM*>( otherPad ) : aOther;
        const int   padNet = pad->GetNetCode();
        const int   otherNet = netOf( other );

        // Other objects of the same (defined) net get a waiver on clearance and hole tests.
        if( otherNet && otherNet == padNet )
            return;

        const bool anyDrill = pad->GetDrillSize().x > 0
                              || ( otherPad && otherPad->GetDrillSize().x > 0 )
                              || ( otherVia && otherVia->GetDrill() > 0 );

        LSET layers = pad->GetLayerSet() & other->GetLayerSet()
                      & LSET::AllCuMask( m_board->GetCopperLayerCount() );

        for( PCB_LAYER_ID layer : layers.Seq() )
        {
            std::shared_ptr<SHAPE> padShape = pad->GetEffectiveShape( layer );
            std::shared_ptr<SHAPE> otherShape = other->GetEffectiveShape( layer );

            bool testClearance = otherPad != nullptr;   // via copper: track path

            if( pad->GetAttribute() == PAD_ATTRIB::NPTH && !pad->FlashLayer( layer ) )
                testClearance = false;

            if( otherPad && otherPad->GetAttribute() == PAD_ATTRIB::NPTH && !otherPad->FlashLayer( layer ) )
                testClearance = false;

            bool testHoles = anyDrill;

            if( testClearance )
            {
                DRC_CONSTRAINT c = m_engine->EvalRules( CLEARANCE_CONSTRAINT, pad, other, layer );
                int            clearance = c.GetValue().Min();
                int            actual = 0;
                VECTOR2I       pos;

                if( c.GetSeverity() != RPT_SEVERITY_IGNORE && clearance > 0
                    && padShape->Collide( otherShape.get(), subEpsilon( clearance ), &actual, &pos )
                    && !m_engine->IsNetTieExclusion( padNet, layer, pos, other ) )
                {
                    DrcType type = ( actual == 0 && padNet && otherNet ) ? DT_SHORT : DT_CLEARANCE;
                    report( type, aPad, aOther, layer, clearance, actual, pos );
                    testHoles = false;   // no need for multiple violations
                }
            }

            if( !testHoles )
                continue;

            DRC_CONSTRAINT hc = m_engine->EvalRules( HOLE_CLEARANCE_CONSTRAINT, pad, other, layer );

            if( hc.GetSeverity() == RPT_SEVERITY_IGNORE )
                continue;

            int clearance = hc.GetValue().Min();

            if( otherVia && !otherVia->IsOnLayer( layer ) )
                clearance = 0;

            if( clearance <= 0 )
                continue;

            auto tryHole = [&]( const SHAPE* aCopper, const BOARD_ITEM* aHolder ) -> bool
            {
                std::shared_ptr<SHAPE_SEGMENT> hole =
                        aHolder->GetEffectiveHoleShape( layer, HOLE_CLEARANCE_CONSTRAINT );
                int      actual = 0;
                VECTOR2I pos;

                if( hole && aCopper->Collide( hole.get(), subEpsilon( clearance ), &actual, &pos ) )
                {
                    report( DT_HOLE_CLEARANCE, aPad, aOther, layer, clearance, actual, pos );
                    return true;
                }

                return false;
            };

            if( otherPad && otherPad->HasHole() && pad->FlashLayer( layer )
                && tryHole( padShape.get(), otherPad ) )
            {
                continue;
            }

            if( otherPad && pad->HasHole() && otherPad->FlashLayer( layer )
                && tryHole( otherShape.get(), pad ) )
            {
                continue;
            }

            if( otherVia && otherVia->HasHole() )
                tryHole( padShape.get(), otherVia );
        }
    }

    /// Mirrors the track loop + testSingleLayerItemAgainstItem for a track/arc/via vs a pad.
    void testTrackPath( PCB_TRACK* aTrack, PAD* aPad )
    {
        // The track loop skips same-net candidates entirely (net 0 included).
        if( aTrack->GetNetCode() == aPad->GetNetCode() )
            return;

        LSET layers = aTrack->GetLayerSet() & aPad->GetLayerSet()
                      & LSET::AllCuMask( m_board->GetCopperLayerCount() );

        for( PCB_LAYER_ID layer : layers.Seq() )
        {
            BOARD_ITEM*            item = aTrack;
            BOARD_ITEM*            other = aPad;
            std::shared_ptr<SHAPE> itemShape = aTrack->GetEffectiveShape( layer );
            std::shared_ptr<SHAPE> otherShape;
            bool                   testClearance = true;

            if( !aPad->FlashLayer( layer ) )
            {
                if( aPad->GetAttribute() == PAD_ATTRIB::NPTH )
                    testClearance = false;

                otherShape = aPad->GetEffectiveHoleShape( layer, HOLE_CLEARANCE_CONSTRAINT );
            }
            else
            {
                otherShape = aPad->GetEffectiveShape( layer );
            }

            if( !itemShape || !otherShape )
                continue;

            if( item->m_Uuid > other->m_Uuid )
            {
                std::swap( item, other );
                std::swap( itemShape, otherShape );
            }

            if( testClearance )
            {
                DRC_CONSTRAINT c = m_engine->EvalRules( CLEARANCE_CONSTRAINT, item, other, layer );
                int            clearance = c.GetValue().Min();
                int            actual = 0;
                VECTOR2I       pos;

                if( c.GetSeverity() != RPT_SEVERITY_IGNORE && clearance > 0
                    && itemShape->Collide( otherShape.get(), subEpsilon( clearance ), &actual, &pos )
                    && !netTieExcluded( item, other, layer, pos ) )
                {
                    DrcType type = ( actual == 0 && netOf( other ) ) ? DT_SHORT : DT_CLEARANCE;
                    report( type, aPad, aTrack, layer, clearance, actual, pos );
                }
            }

            // Hole clearance, both directions, tested even at 0 clearance.
            std::array<BOARD_ITEM*, 2>            a{ item, other };
            std::array<BOARD_ITEM*, 2>            b{ other, item };
            std::array<std::shared_ptr<SHAPE>, 2> aShape{ itemShape, otherShape };

            for( size_t ii = 0; ii < 2; ++ii )
            {
                std::shared_ptr<SHAPE_SEGMENT> hole = holeOn( b[ii], layer );

                if( !hole )
                    continue;

                if( m_engine->IsNetTieExclusion( netOf( b[ii] ), layer, hole->Centre(), a[ii] ) )
                    continue;

                DRC_CONSTRAINT hc = m_engine->EvalRules( HOLE_CLEARANCE_CONSTRAINT, b[ii], a[ii], layer );

                if( hc.GetSeverity() == RPT_SEVERITY_IGNORE )
                    continue;

                int      actual = 0;
                VECTOR2I pos;

                if( aShape[ii]->Collide( hole.get(), subEpsilon( hc.GetValue().Min() ), &actual, &pos ) )
                {
                    report( DT_HOLE_CLEARANCE, aPad, aTrack, layer, hc.GetValue().Min(), actual, pos );
                    break;
                }
            }
        }
    }

    /// Edge.Cuts / Margin shapes near aArea, split like DRC_TEST_PROVIDER_EDGE_CLEARANCE does
    /// (rectangles and polygons into their sides, Edge.Cuts stroke width 0).
    void collectEdges( const BOX2I& aArea )
    {
        auto take = [&]( BOARD_ITEM* aItem )
        {
            if( aItem->Type() != PCB_SHAPE_T || !( aItem->IsOnLayer( Edge_Cuts ) || aItem->IsOnLayer( Margin ) ) )
                return;

            PCB_SHAPE* shape = static_cast<PCB_SHAPE*>( aItem );

            if( !aArea.Intersects( shape->GetBoundingBox() ) )
                return;

            STROKE_PARAMS stroke = shape->GetStroke();

            if( shape->IsOnLayer( Edge_Cuts ) )
                stroke.SetWidth( 0 );

            auto piece = [&]() -> PCB_SHAPE*
            {
                m_edges.emplace_back( static_cast<PCB_SHAPE*>( shape->Clone() ) );
                m_edges.back()->SetStroke( stroke );
                return m_edges.back().get();
            };

            auto segment = [&]( const VECTOR2I& aA, const VECTOR2I& aB )
            {
                PCB_SHAPE* e = piece();
                e->SetShape( SHAPE_T::SEGMENT );
                e->SetStart( aA );
                e->SetEnd( aB );
            };

            if( shape->GetShape() == SHAPE_T::RECTANGLE && !shape->IsSolidFill() )
            {
                if( shape->GetCornerRadius() > 0 )
                {
                    for( SHAPE* sub : shape->MakeEffectiveShapes( true ) )
                    {
                        if( SHAPE_SEGMENT* seg = dynamic_cast<SHAPE_SEGMENT*>( sub ) )
                        {
                            segment( seg->GetStart(), seg->GetEnd() );
                        }
                        else if( SHAPE_ARC* arc = dynamic_cast<SHAPE_ARC*>( sub ) )
                        {
                            PCB_SHAPE* e = piece();
                            e->SetShape( SHAPE_T::ARC );
                            e->SetArcGeometry( arc->GetP0(), arc->GetArcMid(), arc->GetP1() );
                        }

                        delete sub;
                    }
                }
                else
                {
                    VECTOR2I s = shape->GetStart(), e = shape->GetEnd();
                    segment( s, { s.x, e.y } );
                    segment( s, { e.x, s.y } );
                    segment( { e.x, s.y }, e );
                    segment( { s.x, e.y }, e );
                }
            }
            else if( shape->GetShape() == SHAPE_T::POLY && !shape->IsSolidFill()
                     && !shape->GetPolyShape().IsEmpty() )
            {
                const SHAPE_LINE_CHAIN& poly = shape->GetPolyShape().COutline( 0 );

                for( int ii = 0; ii < (int) poly.GetSegmentCount(); ++ii )
                    segment( poly.CSegment( ii ).A, poly.CSegment( ii ).B );
            }
            else
            {
                piece();
            }
        };

        for( BOARD_ITEM* item : m_board->Drawings() )
            take( item );

        for( FOOTPRINT* fp : m_board->Footprints() )
        {
            if( !aArea.Intersects( fp->GetBoundingBox( false ) ) )
                continue;

            for( BOARD_ITEM* item : fp->GraphicalItems() )
                take( item );
        }

        // Drop pieces of a split outline that are nowhere near the footprints.
        std::erase_if( m_edges, [&]( const std::unique_ptr<PCB_SHAPE>& e )
                                { return !aArea.Intersects( e->GetBoundingBox() ); } );
    }

    /// Copper of the footprints against the board edge, following
    /// DRC_TEST_PROVIDER_EDGE_CLEARANCE::testAgainstEdge and testCopperOutsideBoard.
    void testEdges( const std::vector<FOOTPRINT*>& aFootprints )
    {
        if( aFootprints.empty() )
            return;

        BOX2I area = aFootprints.front()->GetBoundingBox( false );

        for( const FOOTPRINT* fp : aFootprints )
            area.Merge( fp->GetBoundingBox( false ) );

        area.Inflate( std::max( m_largestEdgeClearance, 0 ) + m_epsilon );
        collectEdges( area );

        const PCB_BOARD_OUTLINE* outline = m_board->BoardOutline();
        const bool hasOutline = outline && outline->HasOutline();

        // Castellated pads and edge-connector fingers are meant to reach the edge (upstream
        // exempts them, and tracks inside castellated holes; KiCadD only moves footprints).
        auto testItem = [&]( BOARD_ITEM* aItem, const std::vector<PCB_LAYER_ID>& aLayers )
        {
            bool touchesBoard = !hasOutline;

            for( PCB_LAYER_ID shapeLayer : aLayers )
            {
                std::shared_ptr<SHAPE> itemShape = aItem->GetEffectiveShape( shapeLayer );

                if( !itemShape )
                    continue;

                if( !touchesBoard && outline->GetOutline().Collide( itemShape.get() ) )
                    touchesBoard = true;

                const BOX2I itemBox = itemShape->BBox().GetInflated( std::max( m_largestEdgeClearance, 0 ) );

                for( PCB_LAYER_ID testLayer : { Edge_Cuts, Margin } )
                {
                    for( const std::unique_ptr<PCB_SHAPE>& edge : m_edges )
                    {
                        if( !edge->IsOnLayer( testLayer ) || !itemBox.Intersects( edge->GetBoundingBox() ) )
                            continue;

                        DRC_CONSTRAINT c = m_engine->EvalRules( EDGE_CLEARANCE_CONSTRAINT, edge.get(), aItem,
                                                                UNDEFINED_LAYER );
                        int clearance = c.GetValue().Min();

                        if( c.GetSeverity() == RPT_SEVERITY_IGNORE || clearance < 0 )
                            continue;

                        std::shared_ptr<SHAPE> edgeShape = edge->GetEffectiveShape( Edge_Cuts );
                        int                    actual = 0;
                        VECTOR2I               pos;

                        if( itemShape->Collide( edgeShape.get(), std::max( 0, clearance - m_epsilon ), &actual,
                                                &pos ) )
                        {
                            report( DT_EDGE_CLEARANCE, aItem, edge.get(),
                                    shapeLayer == UNDEFINED_LAYER ? aItem->GetLayer() : shapeLayer, clearance,
                                    actual, pos );
                            break;   // one edge per item is enough, as upstream
                        }
                    }
                }
            }

            if( !touchesBoard && aItem->IsOnCopperLayer() )
            {
                PCB_LAYER_ID layer = aLayers.empty() || aLayers.front() == UNDEFINED_LAYER ? aItem->GetLayer()
                                                                                            : aLayers.front();
                report( DT_COPPER_OUTSIDE_BOARD, aItem, aItem, layer, 0, 0, aItem->GetPosition() );
            }
        };

        for( FOOTPRINT* fp : aFootprints )
        {
            for( PAD* pad : fp->Pads() )
            {
                if( pad->GetProperty() == PAD_PROP::CASTELLATED || pad->GetAttribute() == PAD_ATTRIB::CONN )
                    continue;

                if( !pad->IsOnCopperLayer() )
                    continue;

                testItem( pad, pad->Padstack().UniqueLayers() );
            }

            for( BOARD_ITEM* item : fp->GraphicalItems() )
            {
                if( item->Type() == PCB_SHAPE_T && item->IsOnCopperLayer() )
                    testItem( item, { UNDEFINED_LAYER } );
            }
        }
    }

    void testCourtyards( FOOTPRINT* aFp, FOOTPRINT* aOther )
    {
        for( PCB_LAYER_ID layer : { F_CrtYd, B_CrtYd } )
        {
            const SHAPE_POLY_SET& mine = aFp->GetCourtyard( layer );
            const SHAPE_POLY_SET& theirs = aOther->GetCourtyard( layer );

            if( mine.OutlineCount() == 0 || theirs.OutlineCount() == 0 )
                continue;

            if( !mine.BBoxFromCaches().Intersects( theirs.BBoxFromCaches() ) )
                continue;

            DRC_CONSTRAINT constraint =
                    m_engine->EvalRules( COURTYARD_CLEARANCE_CONSTRAINT, aFp, aOther, layer );

            if( constraint.GetSeverity() == RPT_SEVERITY_IGNORE )
                continue;

            int      clearance = std::max( 0, constraint.GetValue().Min() );
            int      actual = 0;
            VECTOR2I pos;

            if( clearance > 0 )
            {
                if( !mine.Collide( &theirs, clearance - m_epsilon, &actual, &pos ) )
                    continue;
            }
            else
            {
                // Plain overlap: courtyards whose outlines merely touch are fine.
                SHAPE_POLY_SET overlap;
                overlap.BooleanIntersection( mine, theirs );

                if( overlap.OutlineCount() == 0 || overlap.Area() <= 0 )
                    continue;

                pos = overlap.BBox().Centre();
            }

            report( DT_COURTYARD_OVERLAP, aFp, aOther, layer, clearance, actual, pos );
        }
    }

    BOARD*                       m_board;
    BOARD_DESIGN_SETTINGS&       m_bds;
    DRC_ENGINE*                  m_engine;
    int                          m_epsilon;
    int                          m_maxClearance;
    int                          m_largestEdgeClearance = 0;
    std::vector<std::unique_ptr<PCB_SHAPE>> m_edges;
    std::vector<DrcViolation>    m_results;
    std::set<std::tuple<int, const BOARD_ITEM*, const BOARD_ITEM*>> m_seen;
};

} // namespace


std::vector<DrcViolation> kicadd::RunLocalDrc( BOARD* aBoard, const std::vector<FOOTPRINT*>& aFootprints )
{
    LOCAL_DRC drc( aBoard );

    if( !drc.Ready() )
        return {};

    // A headless board has no cached outline until something (full DRC, an Edge.Cuts commit)
    // builds it. Build it once per outline generation; a board without a closed outline
    // isn't retried until its edges change.
    static std::pair<const BOARD*, uint64_t> tried{ nullptr, 0 };

    if( !aBoard->BoardOutline()->HasOutline()
        && !( tried.first == aBoard && tried.second == aBoard->GetBoardOutlineGeneration() ) )
    {
        aBoard->UpdateBoardOutline();
        tried = { aBoard, aBoard->GetBoardOutlineGeneration() };
    }

    drc.Run( aFootprints );
    return drc.TakeResults();
}
