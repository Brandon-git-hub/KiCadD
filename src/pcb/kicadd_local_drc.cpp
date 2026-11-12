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

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <tuple>
#include <unordered_set>

#include <board.h>
#include <connectivity/connectivity_algo.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_items.h>
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

        // Upper bounds of every CLEARANCE / HOLE_CLEARANCE rule (netclass rules included): a
        // pair that doesn't collide at the bound can't violate any rule, so its rules need no
        // evaluation. Upstream's copper provider sizes its R-tree query by the same value.
        m_copper = LSET::AllCuMask( m_board->GetCopperLayerCount() );
        m_worstClearance = m_maxClearance;
        m_worstHoleClearance = m_maxClearance;

        if( m_engine && m_engine->QueryWorstConstraint( CLEARANCE_CONSTRAINT, worst ) )
            m_worstClearance = std::max( 0, worst.GetValue().Min() );

        if( m_engine && m_engine->QueryWorstConstraint( HOLE_CLEARANCE_CONSTRAINT, worst ) )
            m_worstHoleClearance = std::max( 0, worst.GetValue().Min() );
    }

    bool Ready() const { return m_engine != nullptr; }

    void Run( const std::vector<FOOTPRINT*>& aFootprints )
    {
        std::unordered_set<const FOOTPRINT*> moved( aFootprints.begin(), aFootprints.end() );
        const bool prof = std::getenv( "KICADD_DRC_PROFILE" ) != nullptr;
        using clk = std::chrono::steady_clock;
        double tPads = 0, tTracks = 0, tQuery = 0;
        long   nTracks = 0;

        for( FOOTPRINT* fp : aFootprints )
        {
            BOX2I area = fp->GetBoundingBox( false );
            area.Inflate( m_maxClearance );

            // The footprint's pads in a grid, so each neighbour item only meets the pads
            // near it (a 700-pin connector would otherwise test every pad against everything).
            PAD_GRID grid( fp, m_maxClearance );

            auto t0 = clk::now();

            for( FOOTPRINT* other : m_board->Footprints() )
            {
                if( other == fp )
                    continue;

                // Each moved/moved pair once.
                if( moved.count( other ) && other < fp )
                    continue;

                if( !area.Intersects( other->GetBoundingBox( false ) ) )
                    continue;

                for( PAD* otherPad : other->Pads() )
                    grid.Visit( otherPad->GetBoundingBox(), [&]( PAD* pad ) { testPadPair( pad, otherPad ); } );

                testCourtyards( fp, other );
            }

            auto t1 = clk::now();
            tPads += std::chrono::duration<double, std::milli>( t1 - t0 ).count();

            std::vector<std::pair<PCB_TRACK*, BOX2I>> nearby = tracksIn( area );
            tQuery += std::chrono::duration<double, std::milli>( clk::now() - t1 ).count();

            for( const auto& [track, box] : nearby )
            {
                nTracks++;
                grid.Visit( box, [&]( PAD* pad ) { testTrackPair( pad, track ); } );
            }

            tTracks += std::chrono::duration<double, std::milli>( clk::now() - t1 ).count();
        }

        auto t2 = clk::now();

        if( !std::getenv( "KICADD_NO_EDGE_DRC" ) )
            testEdges( aFootprints );

        double tEdges = std::chrono::duration<double, std::milli>( clk::now() - t2 ).count();

        if( prof )
        {
            std::fprintf( stderr, "local drc: pads %.2f ms, tracks %.2f ms (query %.2f ms, %ld tracks), edges %.2f ms, "
                                  "%ld bbox hits, %ld rule evals, %ld layer tests\n",
                          tPads, tTracks, tQuery, nTracks, tEdges, m_bboxHits, m_ruleEvals, m_layerTests );
        }
    }

    std::vector<DrcViolation> TakeResults() { return std::move( m_results ); }

private:
    /// Pads of one footprint bucketed by their clearance-inflated bounding boxes.
    class PAD_GRID
    {
    public:
        PAD_GRID( FOOTPRINT* aFp, int aClearance )
        {
            for( PAD* pad : aFp->Pads() )
                m_pads.push_back( { pad, pad->GetBoundingBox().GetInflated( aClearance ) } );

            if( m_pads.empty() )
                return;

            m_bounds = m_pads.front().box;

            for( const ENTRY& e : m_pads )
                m_bounds.Merge( e.box );

            // About 4 pads per cell on a dense part, never finer than 0.5 mm.
            double area = std::max( 1.0, (double) m_bounds.GetWidth() * (double) m_bounds.GetHeight() );
            m_cell = std::max<int64_t>( 500000, (int64_t) std::sqrt( area * 4 / m_pads.size() ) );
            m_nx = std::max<int64_t>( 1, m_bounds.GetWidth() / m_cell + 1 );
            m_ny = std::max<int64_t>( 1, m_bounds.GetHeight() / m_cell + 1 );
            m_cells.resize( m_nx * m_ny );
            m_stamp.assign( m_pads.size(), 0 );

            for( size_t i = 0; i < m_pads.size(); ++i )
            {
                auto [x0, y0, x1, y1] = cellRange( m_pads[i].box );

                for( int64_t y = y0; y <= y1; ++y )
                    for( int64_t x = x0; x <= x1; ++x )
                        m_cells[y * m_nx + x].push_back( (int) i );
            }
        }

        /// Call aFunc for each pad whose inflated box intersects aBox (each pad once).
        template <class F>
        void Visit( const BOX2I& aBox, F aFunc )
        {
            if( m_pads.empty() || !m_bounds.Intersects( aBox ) )
                return;

            ++m_epoch;
            auto [x0, y0, x1, y1] = cellRange( aBox );

            for( int64_t y = y0; y <= y1; ++y )
            {
                for( int64_t x = x0; x <= x1; ++x )
                {
                    for( int i : m_cells[y * m_nx + x] )
                    {
                        if( m_stamp[i] == m_epoch )
                            continue;

                        m_stamp[i] = m_epoch;

                        if( m_pads[i].box.Intersects( aBox ) )
                            aFunc( m_pads[i].pad );
                    }
                }
            }
        }

    private:
        struct ENTRY
        {
            PAD*  pad;
            BOX2I box;
        };

        std::tuple<int64_t, int64_t, int64_t, int64_t> cellRange( const BOX2I& aBox ) const
        {
            auto clampX = [&]( int64_t v ) { return std::clamp<int64_t>( v, 0, m_nx - 1 ); };
            auto clampY = [&]( int64_t v ) { return std::clamp<int64_t>( v, 0, m_ny - 1 ); };
            return { clampX( ( (int64_t) aBox.GetLeft() - m_bounds.GetLeft() ) / m_cell ),
                     clampY( ( (int64_t) aBox.GetTop() - m_bounds.GetTop() ) / m_cell ),
                     clampX( ( (int64_t) aBox.GetRight() - m_bounds.GetLeft() ) / m_cell ),
                     clampY( ( (int64_t) aBox.GetBottom() - m_bounds.GetTop() ) / m_cell ) };
        }

        std::vector<ENTRY>            m_pads;
        BOX2I                         m_bounds;
        int64_t                       m_cell = 1;
        int64_t                       m_nx = 1, m_ny = 1;
        std::vector<std::vector<int>> m_cells;
        std::vector<unsigned>         m_stamp;
        unsigned                      m_epoch = 0;
    };

    /// Tracks, arcs and vias whose bounding box intersects aArea, from the connectivity
    /// R-tree (kept current by every commit) instead of a scan of every track on the board.
    std::vector<std::pair<PCB_TRACK*, BOX2I>> tracksIn( const BOX2I& aArea )
    {
        std::vector<std::pair<PCB_TRACK*, BOX2I>> out;
        std::shared_ptr<CONNECTIVITY_DATA>       conn = m_board->GetConnectivity();

        if( conn && conn->GetConnectivityAlgo() && !std::getenv( "KICADD_DRC_SCAN_TRACKS" ) )
        {
            std::unordered_set<PCB_TRACK*> seen;

            conn->GetConnectivityAlgo()->ItemList().QueryArea( aArea,
                    [&]( CN_ITEM* aItem ) -> bool
                    {
                        if( !aItem->Valid() )
                            return true;

                        BOARD_CONNECTED_ITEM* parent = aItem->Parent();

                        if( parent->Type() == PCB_TRACE_T || parent->Type() == PCB_ARC_T
                            || parent->Type() == PCB_VIA_T )
                        {
                            PCB_TRACK*   t = static_cast<PCB_TRACK*>( parent );
                            const BOX2I& box = aItem->BBox();   // the parent's GetBoundingBox(), cached

                            if( aArea.Intersects( box ) && seen.insert( t ).second )
                                out.emplace_back( t, box );
                        }

                        return true;
                    } );

            return out;
        }

        for( PCB_TRACK* track : m_board->Tracks() )
        {
            BOX2I box = track->GetBoundingBox();

            if( aArea.Intersects( box ) )
                out.emplace_back( track, box );
        }

        return out;
    }

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

        v.set_layer( LSET::Name( aLayer ).ToStdString() );
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

    /// The grid already applied testCopperPair's bounding-box filter.
    void testPadPair( PAD* aPad, PAD* aOther )
    {
        m_bboxHits++;
        testPadPath( aPad, aOther );
    }

    void testTrackPair( PAD* aPad, PCB_TRACK* aTrack )
    {
        m_bboxHits++;
        testTrackPath( aTrack, aPad );

        if( aTrack->Type() == PCB_VIA_T )
            testPadPath( aPad, aTrack );
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

        LSET layers = LSET( pad->GetLayerSet() & other->GetLayerSet() & m_copper );

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

            if( testClearance && padShape->Collide( otherShape.get(), m_worstClearance ) )
            {
                DRC_CONSTRAINT c = ( ++m_ruleEvals, m_engine )->EvalRules( CLEARANCE_CONSTRAINT, pad, other, layer );
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

            auto holeNear = [&]( const SHAPE* aCopper, const BOARD_ITEM* aHolder )
            {
                std::shared_ptr<SHAPE_SEGMENT> hole = aHolder->GetEffectiveHoleShape( layer, HOLE_CLEARANCE_CONSTRAINT );
                return hole && aCopper->Collide( hole.get(), m_worstHoleClearance );
            };

            // Same candidates as the tests below, at the worst hole clearance of any rule.
            if( !( otherPad && otherPad->HasHole() && pad->FlashLayer( layer ) && holeNear( padShape.get(), otherPad ) )
                && !( otherPad && pad->HasHole() && otherPad->FlashLayer( layer ) && holeNear( otherShape.get(), pad ) )
                && !( otherVia && otherVia->HasHole() && holeNear( padShape.get(), otherVia ) ) )
            {
                continue;
            }

            DRC_CONSTRAINT hc = ( ++m_ruleEvals, m_engine )->EvalRules( HOLE_CLEARANCE_CONSTRAINT, pad, other, layer );

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

        // Same layers as GetLayerSet() & pad layers & copper, without building LSETs (which
        // allocate) for every pair: a trace or arc has exactly one layer.
        PCB_LAYER_ID              single = UNDEFINED_LAYER;
        std::vector<PCB_LAYER_ID> viaLayers;

        if( aTrack->Type() != PCB_VIA_T )
        {
            single = aTrack->GetLayer();

            if( !m_copper.Contains( single ) || !aPad->IsOnLayer( single ) )
                return;
        }
        else
        {
            viaLayers = LSET( aTrack->GetLayerSet() & aPad->GetLayerSet() & m_copper ).Seq();
        }

        const std::vector<PCB_LAYER_ID>& layerList = single != UNDEFINED_LAYER ? m_one( single ) : viaLayers;

        for( PCB_LAYER_ID layer : layerList )
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

            m_layerTests++;

            if( item->m_Uuid > other->m_Uuid )
            {
                std::swap( item, other );
                std::swap( itemShape, otherShape );
            }

            // Hole clearance, both directions, tested even at 0 clearance.
            std::array<BOARD_ITEM*, 2>                    a{ item, other };
            std::array<BOARD_ITEM*, 2>                    b{ other, item };
            std::array<std::shared_ptr<SHAPE>, 2>         aShape{ itemShape, otherShape };
            std::array<std::shared_ptr<SHAPE_SEGMENT>, 2> holes{ holeOn( b[0], layer ), holeOn( b[1], layer ) };

            // Rules are only evaluated for shapes within the worst clearance of their kind.
            if( testClearance && itemShape->Collide( otherShape.get(), m_worstClearance ) )
            {
                DRC_CONSTRAINT c = ( ++m_ruleEvals, m_engine )->EvalRules( CLEARANCE_CONSTRAINT, item, other, layer );
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

            for( size_t ii = 0; ii < 2; ++ii )
            {
                const std::shared_ptr<SHAPE_SEGMENT>& hole = holes[ii];

                if( !hole || !aShape[ii]->Collide( hole.get(), m_worstHoleClearance ) )
                    continue;

                if( m_engine->IsNetTieExclusion( netOf( b[ii] ), layer, hole->Centre(), a[ii] ) )
                    continue;

                DRC_CONSTRAINT hc = ( ++m_ruleEvals, m_engine )->EvalRules( HOLE_CLEARANCE_CONSTRAINT, b[ii], a[ii], layer );

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
    long                         m_bboxHits = 0;
    long                         m_ruleEvals = 0;
    long                         m_layerTests = 0;
    int                          m_worstClearance = 0;
    LSET                         m_copper;
    std::vector<PCB_LAYER_ID>    m_oneLayer{ UNDEFINED_LAYER };

    const std::vector<PCB_LAYER_ID>& m_one( PCB_LAYER_ID aLayer )
    {
        m_oneLayer[0] = aLayer;
        return m_oneLayer;
    }

    int                          m_worstHoleClearance = 0;
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
