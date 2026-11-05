/*
 * KiCadD - local (incremental) DRC around a set of footprints.
 *
 * Pairwise semantics follow DRC_TEST_PROVIDER_COPPER_CLEARANCE::testSingleLayerItemAgainstItem
 * (per copper layer, hole shape where a pad/via isn't flashed, net-tie exclusion, DRC epsilon)
 * and DRC_TEST_PROVIDER_COURTYARD_CLEARANCE (front/back courtyard collision).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "kicadd_local_drc.h"

#include <array>
#include <set>
#include <tuple>
#include <unordered_set>

#include <board.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <drc/drc_rule.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_track.h>
#include <geometry/shape.h>
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
    {}

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
    std::vector<DrcViolation>    m_results;
    std::set<std::tuple<int, const BOARD_ITEM*, const BOARD_ITEM*>> m_seen;
};

} // namespace


std::vector<DrcViolation> kicadd::RunLocalDrc( BOARD* aBoard, const std::vector<FOOTPRINT*>& aFootprints )
{
    LOCAL_DRC drc( aBoard );

    if( !drc.Ready() )
        return {};

    drc.Run( aFootprints );
    return drc.TakeResults();
}
