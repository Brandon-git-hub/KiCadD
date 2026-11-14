/*
 * KiCadD - compact, spatially scoped board API handler.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "kicadd_handler_pcb.h"
#include "kicadd_local_drc.h"
#include "kicadd_render.h"
#include "lcsc/kicadd_lcsc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <climits>
#include <set>
#include <unordered_map>

#include <api/pcb_context.h>
#include <eda_draw_frame.h>
#include <pcb_base_frame.h>
#include <pcb_base_edit_frame.h>
#include <class_draw_panel_gal.h>
#include <tool/tool_manager.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <frame_type.h>
#include <mail_type.h>
#include <lib_id.h>
#include <netlist_reader/pcb_netlist_utils.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <board.h>
#include <board_commit.h>
#include <footprint.h>
#include <view/view.h>
#include <pad.h>
#include <pcb_track.h>
#include <geometry/shape_circle.h>
#include <geometry/shape_arc.h>
#include <zone.h>
#include <geometry/seg.h>
#include <geometry/shape_poly_set.h>
#include <cmath>
#include <netinfo.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_algo.h>
#include <ratsnest/ratsnest_data.h>
#include <netlist_reader/board_netlist_updater.h>
#include "pcb/kicadd_autoplace.h"
#include "pcb/kicadd_fab.h"
#include "pcb/kicadd_picture.h"
#include "lcsc/kicadd_parts.h"
#include "pcb/kicadd_fpgen.h"
#include "common/kicadd_libtable.h"
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <wx/ffile.h>
#include <project/net_settings.h>
#include <project_pcb.h>
#include <footprint_library_adapter.h>
#include <netclass.h>
#include <pcb_text.h>
#include <pcb_field.h>
#include <trigo.h>
#include <netlist_reader/pcb_netlist.h>
#include <kiface_ids.h>
#include <kiface_services.h>
#include <kiway.h>
#include <reporter.h>
#include "pcb/kicadd_route.h"
#include <pcb_edit_frame.h>
#include <pcb_shape.h>
#include <zone_settings.h>
#include <wx/ffile.h>

using namespace kicadd::board;

namespace
{

constexpr int DEFAULT_RADIUS_LIMIT = 64;
constexpr int DEFAULT_NET_LIMIT    = 256;


ApiResponseStatus makeError( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}


void setPoint( Point* aOut, const VECTOR2I& aPt )
{
    aOut->set_x( aPt.x );
    aOut->set_y( aPt.y );
}


std::string shortId( const BOARD_ITEM* aItem )
{
    return aItem->m_Uuid.AsStdString().substr( 0, 8 );
}


std::string netOf( const BOARD_ITEM* aItem )
{
    if( const BOARD_CONNECTED_ITEM* c = dynamic_cast<const BOARD_CONNECTED_ITEM*>( aItem ) )
    {
        if( c->GetNetCode() > 0 )
            return c->GetNetname().ToStdString();
    }

    return {};
}


std::string netName( BOARD* aBoard, int aCode )
{
    const NETINFO_ITEM* net = aBoard->FindNet( aCode );
    return net ? net->GetNetname().ToStdString() : std::to_string( aCode );
}


/// Fill the fields shared by every item kind; caller fills geometry.
CompactItem makeItem( ItemKind aKind, const BOARD_ITEM* aItem, std::string aHandle )
{
    CompactItem out;
    out.set_kind( aKind );
    out.set_handle( std::move( aHandle ) );
    out.set_net( netOf( aItem ) );
    setPoint( out.mutable_pos(), aItem->GetPosition() );
    return out;
}


CompactItem describeFootprint( const FOOTPRINT* aFp )
{
    CompactItem out = makeItem( IK_FOOTPRINT, aFp, aFp->GetReference().ToStdString() );
    out.set_layer( LSET::Name( aFp->GetLayer() ).ToStdString() );
    out.set_rot_decideg( aFp->GetOrientation().AsTenthsOfADegree() );

    BOX2I body = aFp->GetBoundingBox( false );
    out.mutable_size()->set_x( body.GetWidth() );
    out.mutable_size()->set_y( body.GetHeight() );
    return out;
}


CompactItem describePad( const PAD* aPad )
{
    const FOOTPRINT* parent = aPad->GetParentFootprint();
    std::string      handle = parent ? parent->GetReference().ToStdString() : std::string( "?" );
    handle += "." + aPad->GetNumber().ToStdString();

    CompactItem out = makeItem( IK_PAD, aPad, std::move( handle ) );

    // Pads on a single copper layer report it; through-hole pads leave it empty.
    LSET cu = aPad->GetLayerSet() & LSET::AllCuMask();

    if( cu.count() == 1 )
        out.set_layer( LSET::Name( aPad->GetLayer() ).ToStdString() );

    return out;
}


CompactItem describeTrack( const PCB_TRACK* aTrack )
{
    if( aTrack->Type() == PCB_VIA_T )
    {
        const PCB_VIA* via = static_cast<const PCB_VIA*>( aTrack );
        CompactItem    out = makeItem( IK_VIA, via, shortId( via ) );
        out.set_width( via->GetWidth( F_Cu ) );
        return out;
    }

    CompactItem out = makeItem( aTrack->Type() == PCB_ARC_T ? IK_ARC : IK_TRACK, aTrack,
                                shortId( aTrack ) );
    out.set_layer( LSET::Name( aTrack->GetLayer() ).ToStdString() );
    setPoint( out.mutable_pos(), aTrack->GetStart() );
    setPoint( out.mutable_end(), aTrack->GetEnd() );
    out.set_width( aTrack->GetWidth() );
    return out;
}


/// Distance from aPt to the item's copper/body, 0 when inside.
int64_t distanceTo( const BOARD_ITEM* aItem, const VECTOR2I& aPt )
{
    switch( aItem->Type() )
    {
    case PCB_TRACE_T:
    {
        const PCB_TRACK* t = static_cast<const PCB_TRACK*>( aItem );
        int64_t          d = SEG( t->GetStart(), t->GetEnd() ).Distance( aPt ) - t->GetWidth() / 2;
        return std::max<int64_t>( d, 0 );
    }

    case PCB_VIA_T:
    {
        const PCB_VIA* v = static_cast<const PCB_VIA*>( aItem );
        int64_t        d = ( v->GetPosition() - aPt ).EuclideanNorm() - v->GetWidth( F_Cu ) / 2;
        return std::max<int64_t>( d, 0 );
    }

    case PCB_FOOTPRINT_T:
    {
        // Body hull (pads + graphics, no text fields): text bboxes of large parts can
        // span half the board and make every neighbour look like it's "on top".
        const FOOTPRINT* fp = static_cast<const FOOTPRINT*>( aItem );
        SHAPE_POLY_SET   hull = fp->GetBoundingHull();

        if( hull.OutlineCount() == 0 )
            return fp->GetBoundingBox( false ).Distance( aPt );

        return KiROUND( std::sqrt( static_cast<double>( hull.SquaredDistance( aPt ) ) ) );
    }

    default:
        // Arcs, pads, zones: bounding box is a good enough, cheap bound.
        return aItem->GetBoundingBox().Distance( aPt );
    }
}

} // namespace


KICADD_HANDLER_PCB::KICADD_HANDLER_PCB( std::shared_ptr<PCB_CONTEXT> aContext ) :
        API_HANDLER(),
        m_context( std::move( aContext ) )
{
    registerHandler<GetBoardSummary, BoardSummaryResponse>( &KICADD_HANDLER_PCB::handleGetBoardSummary );
    registerHandler<QueryRadius, QueryResponse>( &KICADD_HANDLER_PCB::handleQueryRadius );
    registerHandler<QueryNet, QueryResponse>( &KICADD_HANDLER_PCB::handleQueryNet );
    registerHandler<MoveDelta, MoveResponse>( &KICADD_HANDLER_PCB::handleMoveDelta );
    registerHandler<FpSetFields, MoveResponse>( &KICADD_HANDLER_PCB::handleSetFields );
    registerHandler<GetRules, RulesResponse>( &KICADD_HANDLER_PCB::handleGetRules );
    registerHandler<SetRules, RulesResponse>( &KICADD_HANDLER_PCB::handleSetRules );
    registerHandler<Zones, ZonesResponse>( &KICADD_HANDLER_PCB::handleZones );
    registerHandler<BoardOutline, BoardOutlineResponse>( &KICADD_HANDLER_PCB::handleBoardOutline );
    registerHandler<BoardSetup, BoardSetupResponse>( &KICADD_HANDLER_PCB::handleBoardSetup );
    registerHandler<DeleteFootprints, MoveResponse>( &KICADD_HANDLER_PCB::handleDeleteFootprints );
    registerHandler<DeleteTracks, MoveResponse>( &KICADD_HANDLER_PCB::handleDeleteTracks );
    registerHandler<RouteTrack, RouteResponse>( &KICADD_HANDLER_PCB::handleRoute );
    registerHandler<RouteShort, RouteResponse>( &KICADD_HANDLER_PCB::handleRouteShort );
    registerHandler<Fanout, FanoutResponse>( &KICADD_HANDLER_PCB::handleFanout );
    registerHandler<StitchVias, StitchViasResponse>( &KICADD_HANDLER_PCB::handleStitchVias );
    registerHandler<Teardrops, TeardropsResponse>( &KICADD_HANDLER_PCB::handleTeardrops );
    registerHandler<ExportFab, ExportFabResponse>( &KICADD_HANDLER_PCB::handleExportFab );
    registerHandler<RenderBoard, RenderBoardResponse>( &KICADD_HANDLER_PCB::handleRenderBoard );
    registerHandler<LcscSearch, LcscSearchResponse>( &KICADD_HANDLER_PCB::handleLcscSearch );
    registerHandler<FpCreate, FpCreateResponse>( &KICADD_HANDLER_PCB::handleFpCreate );
    registerHandler<AddFootprint, MoveResponse>( &KICADD_HANDLER_PCB::handleAddFootprint );
    registerHandler<FetchLcsc, FetchLcscResponse>( &KICADD_HANDLER_PCB::handleFetchLcsc );
    registerHandler<SyncFromSchematic, SyncFromSchematicResponse>( &KICADD_HANDLER_PCB::handleSyncFromSchematic );
    registerHandler<AutoPlace, AutoPlaceResponse>( &KICADD_HANDLER_PCB::handleAutoPlace );
    registerHandler<AutoPlaceStatus, AutoPlaceResponse>( &KICADD_HANDLER_PCB::handleAutoPlaceStatus );
    registerHandler<Undo, HistoryResponse>( &KICADD_HANDLER_PCB::handleUndo );
    registerHandler<Redo, HistoryResponse>( &KICADD_HANDLER_PCB::handleRedo );
    registerHandler<GetHistory, HistoryResponse>( &KICADD_HANDLER_PCB::handleGetHistory );
    registerHandler<DescribeItems, DescribeItemsResponse>( &KICADD_HANDLER_PCB::handleDescribeItems );
    registerHandler<CheckLocalDrc, LocalDrcResponse>( &KICADD_HANDLER_PCB::handleCheckLocalDrc );
    registerHandler<GetStateDigest, StateDigestResponse>( &KICADD_HANDLER_PCB::handleGetStateDigest );
    registerHandler<SetRatsnestDeferral, RatsnestDeferralResponse>(
            &KICADD_HANDLER_PCB::handleSetRatsnestDeferral );
    registerHandler<FlushRatsnest, FlushRatsnestResponse>( &KICADD_HANDLER_PCB::handleFlushRatsnest );
    registerHandler<VerifyConnectivity, VerifyConnectivityResponse>(
            &KICADD_HANDLER_PCB::handleVerifyConnectivity );
    registerHandler<kicadd::render::GetRenderSnapshot, kicadd::render::RenderSnapshot>(
            &KICADD_HANDLER_PCB::handleGetRenderSnapshot );
    registerHandler<kicadd::render::GetChanges, kicadd::render::RenderChanges>(
            &KICADD_HANDLER_PCB::handleGetChanges );
    registerHandler<kicadd::render::GetRatsnest, kicadd::render::Ratsnest>(
            &KICADD_HANDLER_PCB::handleGetRatsnest );
    registerHandler<kicadd::render::GetZoneFills, kicadd::render::ZoneFills>(
            &KICADD_HANDLER_PCB::handleGetZoneFills );
    registerHandler<kicadd::render::GetArt, kicadd::render::Art>( &KICADD_HANDLER_PCB::handleGetArt );

    if( board() )
        m_feed = std::make_unique<kicadd::RENDER_FEED>( board() );
}


KICADD_HANDLER_PCB::~KICADD_HANDLER_PCB()
{
    stopAutoPlace();
}


void KICADD_HANDLER_PCB::DetachBoard()
{
    stopAutoPlace();
    m_feed.reset();
    m_undo.clear();
    m_redo.clear();
    m_pendingNets.clear();
}


kicadd::RENDER_FEED* KICADD_HANDLER_PCB::feed()
{
    if( !m_feed && board() )
        m_feed = std::make_unique<kicadd::RENDER_FEED>( board() );

    return m_feed.get();
}


std::optional<ApiResponseStatus> KICADD_HANDLER_PCB::busy() const
{
    if( m_autoPlace )
    {
        return makeError( ApiStatusCode::AS_BUSY,
                          "auto_place is running on this board; the edit was not applied (wait for it, "
                          "or cancel it)" );
    }

    if( !m_context || m_context->CanAcceptApiCommands() )
        return std::nullopt;

    // say why (PCB_EDIT_FRAME::CanAcceptApiCommands' tests), so a refusal with no dialog in
    // sight can be told apart: a progress window, a tool left active
    std::string why = "the user is in an interactive tool (routing, moving, a dialog)";
    TOOL_MANAGER* mgr = m_context->GetToolManager();

    if( auto* frame = mgr ? dynamic_cast<PCB_BASE_EDIT_FRAME*>( mgr->GetToolHolder() ) : nullptr )
    {
        TOOL_BASE* tool = mgr->GetCurrentTool();

        if( !frame->IsEnabled() )
            why = "the editor window is disabled: a dialog or a progress window (loading libraries, "
                  "filling zones) is open";
        else if( frame->UndoRedoBlocked() )
            why = "the user is dragging or editing something";
        else if( tool && tool->GetName() != "common.InteractiveSelection"
                 && tool->GetName() != "pcbnew.PointEditor" )
            why = "the tool " + tool->GetName() + " is active in the editor (Esc ends it)";
        else
            why = "the editor is filling zones or routing";
    }

    return makeError( ApiStatusCode::AS_BUSY, "the editor is busy: " + why + "; the edit was not applied" );
}


bool KICADD_HANDLER_PCB::editorOwnsUndo() const
{
    // Same test as BOARD_COMMIT::Push, which keeps an undo list only with an editor frame
    TOOL_MANAGER* mgr = m_context ? m_context->GetToolManager() : nullptr;
    return mgr && dynamic_cast<PCB_BASE_FRAME*>( mgr->GetToolHolder() );
}


void KICADD_HANDLER_PCB::announce( const wxString& aWhat ) const
{
    TOOL_MANAGER* mgr = m_context ? m_context->GetToolManager() : nullptr;

    // The commit has updated the view items; an API request (unlike a tool event) does not
    // repaint the canvas by itself.
    if( EDA_DRAW_FRAME* frame = mgr ? dynamic_cast<EDA_DRAW_FRAME*>( mgr->GetToolHolder() ) : nullptr )
    {
        frame->SetStatusText( aWhat );
        frame->GetCanvas()->Refresh();
    }
}


BOARD* KICADD_HANDLER_PCB::board() const
{
    return m_context ? m_context->GetBoard() : nullptr;
}


HANDLER_RESULT<BoardSummaryResponse>
KICADD_HANDLER_PCB::handleGetBoardSummary( const HANDLER_CONTEXT<GetBoardSummary>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    BoardSummaryResponse out;
    out.set_file( b->GetFileName().ToStdString() );
    out.set_footprints( static_cast<int>( b->Footprints().size() ) );

    int tracks = 0, vias = 0;

    for( const PCB_TRACK* t : b->Tracks() )
        ( t->Type() == PCB_VIA_T ? vias : tracks )++;

    out.set_tracks( tracks );
    out.set_vias( vias );
    out.set_zones( static_cast<int>( b->Zones().size() ) );
    out.set_nets( static_cast<int>( b->GetNetCount() ) );
    out.set_copper_layers( b->GetCopperLayerCount() );

    BOX2I bbox = b->ComputeBoundingBox( true, true );   // board outline
    setPoint( out.mutable_bbox_min(), bbox.GetOrigin() );
    setPoint( out.mutable_bbox_max(), bbox.GetEnd() );

    std::map<wxString, int> refs;

    for( const FOOTPRINT* fp : b->Footprints() )
    {
        if( !fp->GetReference().IsEmpty() && !fp->GetReference().StartsWith( wxS( "#" ) ) )
            refs[fp->GetReference()]++;
    }

    for( const auto& [ref, n] : refs )
    {
        if( n > 1 )
            out.add_duplicate_refs( ref.ToStdString() + " x" + std::to_string( n ) );
    }

    return out;
}


HANDLER_RESULT<QueryResponse>
KICADD_HANDLER_PCB::handleQueryRadius( const HANDLER_CONTEXT<QueryRadius>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const QueryRadius& req = aCtx.Request;
    VECTOR2I           center;
    const FOOTPRINT*   anchorFp = nullptr;

    if( req.has_point() )
    {
        center = VECTOR2I( req.point().x(), req.point().y() );
    }
    else if( !req.reference().empty() )
    {
        anchorFp = b->FindFootprintByReference( wxString::FromUTF8( req.reference() ) );

        if( !anchorFp )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "unknown reference " + req.reference() ) );

        center = anchorFp->GetPosition();
    }
    else
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "QueryRadius needs point or reference" ) );
    }

    if( req.radius() <= 0 )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "radius must be > 0" ) );

    std::set<int> kinds( req.kinds().begin(), req.kinds().end() );

    if( kinds.empty() )
        kinds = { IK_FOOTPRINT, IK_PAD, IK_TRACK, IK_ARC, IK_VIA };

    auto want = [&]( ItemKind k ) { return kinds.count( k ) > 0; };

    // (item, bbox area): area breaks distance ties, so a module or heatsink whose body merely
    // contains the centre (distance 0) ranks after the small parts actually there.
    std::vector<std::pair<CompactItem, double>> hits;
    const int64_t                               radius = req.radius();

    auto consider = [&]( const BOARD_ITEM* aItem, auto&& aDescribe )
    {
        int64_t d = distanceTo( aItem, center );

        if( d <= radius )
        {
            CompactItem ci = aDescribe();
            ci.set_dist( d );

            if( req.with_ids() )
                ci.set_id( aItem->m_Uuid.AsStdString() );

            BOX2I bb = aItem->Type() == PCB_FOOTPRINT_T
                               ? static_cast<const FOOTPRINT*>( aItem )->GetBoundingBox( false )
                               : aItem->GetBoundingBox();
            hits.emplace_back( std::move( ci ), static_cast<double>( bb.GetArea() ) );
        }
    };

    for( const FOOTPRINT* fp : b->Footprints() )
    {
        // Cheap reject: the whole footprint is out of range, so are its pads.
        if( fp->GetBoundingBox( false ).Distance( center ) > radius )
            continue;

        if( want( IK_FOOTPRINT ) && fp != anchorFp )
            consider( fp, [&] { return describeFootprint( fp ); } );

        if( want( IK_PAD ) )
        {
            for( const PAD* pad : fp->Pads() )
                consider( pad, [&] { return describePad( pad ); } );
        }
    }

    for( const PCB_TRACK* t : b->Tracks() )
    {
        ItemKind k = t->Type() == PCB_VIA_T ? IK_VIA : t->Type() == PCB_ARC_T ? IK_ARC : IK_TRACK;

        if( want( k ) )
            consider( t, [&] { return describeTrack( t ); } );
    }

    if( want( IK_ZONE ) )
    {
        for( const ZONE* z : b->Zones() )
        {
            consider( z, [&]
                      {
                          CompactItem ci = makeItem( IK_ZONE, z, shortId( z ) );
                          ci.set_layer( LSET::Name( z->GetLayer() ).ToStdString() );
                          return ci;
                      } );
        }
    }

    std::stable_sort( hits.begin(), hits.end(),
                      []( const auto& a, const auto& b )
                      {
                          if( a.first.dist() != b.first.dist() )
                              return a.first.dist() < b.first.dist();

                          return a.second < b.second;
                      } );

    const int limit = req.limit() > 0 ? req.limit() : DEFAULT_RADIUS_LIMIT;

    QueryResponse out;
    out.set_total( static_cast<int>( hits.size() ) );
    out.set_truncated( static_cast<int>( hits.size() ) > limit );

    for( size_t i = 0; i < hits.size() && static_cast<int>( i ) < limit; ++i )
        *out.add_items() = std::move( hits[i].first );

    return out;
}


HANDLER_RESULT<QueryResponse>
KICADD_HANDLER_PCB::handleQueryNet( const HANDLER_CONTEXT<QueryNet>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const NETINFO_ITEM* net = b->FindNet( wxString::FromUTF8( aCtx.Request.net() ) );

    if( !net || net->GetNetCode() <= 0 )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "unknown net " + aCtx.Request.net() ) );

    const int code  = net->GetNetCode();
    const int limit = aCtx.Request.limit() > 0 ? aCtx.Request.limit() : DEFAULT_NET_LIMIT;

    QueryResponse out;
    int           total = 0;

    auto emit = [&]( CompactItem&& aItem )
    {
        if( total++ < limit )
            *out.add_items() = std::move( aItem );
    };

    for( const FOOTPRINT* fp : b->Footprints() )
    {
        for( const PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() == code )
                emit( describePad( pad ) );
        }
    }

    for( const PCB_TRACK* t : b->Tracks() )
    {
        if( t->GetNetCode() == code )
            emit( describeTrack( t ) );
    }

    for( const ZONE* z : b->Zones() )
    {
        if( z->GetNetCode() == code )
        {
            CompactItem ci = makeItem( IK_ZONE, z, shortId( z ) );
            ci.set_layer( LSET::Name( z->GetLayer() ).ToStdString() );
            emit( std::move( ci ) );
        }
    }

    out.set_total( total );
    out.set_truncated( total > limit );
    return out;
}


void KICADD_HANDLER_PCB::runCommit( const std::function<std::vector<KIID>( BOARD_COMMIT& )>& aBuild,
                                    const wxString& aCommitMessage, MoveResponse& aOut )
{
    auto   t0 = std::chrono::steady_clock::now();
    BOARD* b = board();

    std::vector<FOOTPRINT*> report;

    {
        std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();

        // Cleared on every exit path, so a failed commit can't leave the predicate installed.
        struct DEFERRAL_GUARD
        {
            CONNECTIVITY_DATA* c;
            ~DEFERRAL_GUARD() { c->SetRatsnestDeferral( nullptr ); }
        } deferralGuard{ conn.get() };

        // Large nets (GND, power) are the expensive part of the ratsnest on an unrouted or
        // half-routed board; defer them for the duration of this commit.
        if( m_deferLargeNets )
        {
            std::vector<int> padsPerNet( b->GetNetCount() + 1, 0 );

            for( const FOOTPRINT* fp : b->Footprints() )
            {
                for( const PAD* pad : fp->Pads() )
                {
                    int net = pad->GetNetCode();

                    if( net > 0 && net < (int) padsPerNet.size() )
                        padsPerNet[net]++;
                }
            }

            const int minPads = m_deferMinPads;
            conn->SetRatsnestDeferral(
                    [padsPerNet = std::move( padsPerNet ), minPads]( int aNet )
                    {
                        return aNet > 0 && aNet < (int) padsPerNet.size() && padsPerNet[aNet] > minPads;
                    } );
        }

        BOARD_COMMIT      commit( m_context->GetToolManager(), true, false );
        std::vector<KIID> ids = aBuild( commit );

        if( commit.Empty() )
            return;

        commit.Push( aCommitMessage );
        m_context->SetContentModified();
        announce( aCommitMessage );

        for( const KIID& id : ids )
        {
            BOARD_ITEM* item = b->ResolveItem( id, true );

            if( item && item->Type() == PCB_FOOTPRINT_T )
                report.push_back( static_cast<FOOTPRINT*>( item ) );
        }

        for( FOOTPRINT* fp : report )
        {
            fp->BuildCourtyardCaches();

            MoveResult* r = aOut.add_moved();
            r->set_ref( fp->GetReference().ToStdString() );
            setPoint( r->mutable_pos(), fp->GetPosition() );
            r->set_rot_decideg( fp->GetOrientation().AsTenthsOfADegree() );
            r->set_back( fp->IsFlipped() );
        }
    }

    aOut.set_elapsed_us( std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - t0 ).count() );
    {
        std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();
        aOut.set_unconnected( static_cast<int>( conn->GetUnconnectedCountNoFlush( false ) ) );

        for( int net : conn->GetPendingRatsnestNets() )
            aOut.add_ratsnest_pending( netName( b, net ) );
    }

    if( !report.empty() )
    {
        auto d0 = std::chrono::steady_clock::now();

        for( DrcViolation& v : kicadd::RunLocalDrc( b, report ) )
            *aOut.add_drc() = std::move( v );

        aOut.set_drc_us( std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - d0 ).count() );
    }
}


void KICADD_HANDLER_PCB::applyFootprintEdit( const std::vector<FOOTPRINT*>& aTargets,
                                             const std::function<void( FOOTPRINT* )>& aMutate,
                                             const wxString& aCommitMessage, MoveResponse& aOut )
{
    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                std::vector<KIID> ids;

                for( FOOTPRINT* fp : aTargets )
                {
                    aCommit.Modify( fp );
                    aMutate( fp );
                    ids.push_back( fp->m_Uuid );
                }

                return ids;
            },
            aCommitMessage, aOut );
}


KICADD_HANDLER_PCB::FP_STATE KICADD_HANDLER_PCB::stateOf( const FOOTPRINT* aFp, bool aWithFields )
{
    FP_STATE st{ aFp->m_Uuid, aFp->GetPosition(), aFp->GetOrientation(), aFp->IsFlipped() };

    if( aWithFields )
    {
        st.withFields = true;
        st.attrs = aFp->GetAttributes();

        for( const PCB_FIELD* field : aFp->GetFields() )
        {
            if( field && field->GetId() != FIELD_T::FOOTPRINT )
                st.fields.emplace_back( field->GetName(), field->GetText() );
        }

        std::sort( st.fields.begin(), st.fields.end() );
    }

    return st;
}


void KICADD_HANDLER_PCB::setState( FOOTPRINT* aFp, const FP_STATE& aState, KIGFX::VIEW* aView )
{
    // Side first: a flip mirrors the orientation, which is then set exactly.
    if( aFp->IsFlipped() != aState.back )
        aFp->Flip( aFp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

    if( aFp->GetOrientation() != aState.orient )
        aFp->SetOrientation( aState.orient );

    if( aFp->GetPosition() != aState.pos )
        aFp->SetPosition( aState.pos );

    if( !aState.withFields )
        return;

    if( aView )
        aView->Remove( aFp );

    aFp->SetAttributes( aState.attrs );
    std::map<wxString, wxString> want( aState.fields.begin(), aState.fields.end() );

    std::vector<PCB_FIELD*> fields( aFp->GetFields().begin(), aFp->GetFields().end() );

    for( PCB_FIELD* field : fields )
    {
        if( !field || field->GetId() == FIELD_T::FOOTPRINT )
            continue;

        auto it = want.find( field->GetName() );

        if( it == want.end() )
        {
            if( !field->IsMandatory() )
            {
                aFp->Remove( field );
                delete field;
            }

            continue;
        }

        field->SetText( it->second );
        want.erase( it );
    }

    for( const auto& [name, text] : want )
        addUserField( aFp, name, text );

    if( aView )
        aView->Add( aFp );
}


KIGFX::VIEW* KICADD_HANDLER_PCB::view() const
{
    TOOL_MANAGER* mgr = m_context ? m_context->GetToolManager() : nullptr;
    return mgr && editorOwnsUndo() ? mgr->GetView() : nullptr;
}


void KICADD_HANDLER_PCB::addUserField( FOOTPRINT* aFp, const wxString& aName, const wxString& aText )
{
    // as BOARD_NETLIST_UPDATER adds one: hidden, on the fab layer, at the anchor
    PCB_FIELD* field = new PCB_FIELD( aFp, FIELD_T::USER );
    aFp->Add( field );
    field->SetName( aName );
    field->SetText( aText );
    field->SetVisible( false );
    field->SetLayer( aFp->GetLayer() == F_Cu ? F_Fab : B_Fab );
    field->SetPosition( aFp->GetPosition() );
    field->Rotate( aFp->GetPosition(), aFp->GetOrientation() );

    if( BOARD* b = aFp->GetBoard() )
        field->StyleFromSettings( b->GetDesignSettings(), true );
}


KICADD_HANDLER_PCB::FP_SNAPSHOT KICADD_HANDLER_PCB::detach( FOOTPRINT* aFp )
{
    FP_SNAPSHOT snap;
    snap.fp.reset( aFp );

    for( PAD* pad : aFp->Pads() )
    {
        snap.padNets.push_back( pad->GetNetCode() > 0 ? pad->GetNetname() : wxString() );
        pad->SetNet( NETINFO_LIST::OrphanedItem() );
    }

    aFp->SetParent( nullptr );
    return snap;
}


KICADD_HANDLER_PCB::FP_SNAPSHOT KICADD_HANDLER_PCB::copyOf( const FOOTPRINT* aFp )
{
    FOOTPRINT* copy = static_cast<FOOTPRINT*>( aFp->Clone() );   // same KIIDs
    FP_SNAPSHOT snap = detach( copy );                           // nets by name
    return snap;
}


bool KICADD_HANDLER_PCB::matches( const FOOTPRINT* aFp, const FP_SNAPSHOT& aSnap )
{
    const FOOTPRINT& s = *aSnap.fp;

    if( aFp->GetPosition() != s.GetPosition() || aFp->GetOrientation() != s.GetOrientation()
        || aFp->GetLayer() != s.GetLayer() || aFp->GetAttributes() != s.GetAttributes()
        || aFp->GetPath() != s.GetPath() || aFp->GetFPID() != s.GetFPID() || !( *aFp == s ) )
        return false;

    size_t i = 0;

    for( const PAD* pad : aFp->Pads() )
    {
        wxString net = pad->GetNetCode() > 0 ? pad->GetNetname() : wxString();

        if( i >= aSnap.padNets.size() || aSnap.padNets[i++] != net )
            return false;
    }

    return i == aSnap.padNets.size();
}


NETINFO_ITEM* KICADD_HANDLER_PCB::netFor( const wxString& aName, BOARD_COMMIT& aCommit, MoveResponse& aOut )
{
    BOARD* b = board();

    if( aName.IsEmpty() )
        return b->FindNet( NETINFO_LIST::UNCONNECTED );

    if( NETINFO_ITEM* net = b->FindNet( aName ) )
        return net;

    // Created earlier in this commit, not on the board until it's pushed.
    for( NETINFO_ITEM* pending : m_pendingNets )
    {
        if( pending->GetNetname() == aName )
            return pending;
    }

    NETINFO_ITEM* net = new NETINFO_ITEM( b, aName );

    // its net class by the name patterns (a new net is Default otherwise, until the board is
    // loaded again: the router would take Default's width and clearance, DRC the pattern's)
    if( b->GetDesignSettings().m_NetSettings )
        net->SetNetClass( b->GetDesignSettings().m_NetSettings->GetEffectiveNetClass( aName ) );

    aCommit.Add( net );
    m_pendingNets.push_back( net );
    aOut.add_created_nets( aName.ToStdString() );
    return net;
}


FOOTPRINT* KICADD_HANDLER_PCB::instantiate( const FP_SNAPSHOT& aSnap, BOARD_COMMIT& aCommit, MoveResponse& aOut )
{
    FOOTPRINT* fp = static_cast<FOOTPRINT*>( aSnap.fp->Clone() );   // same KIIDs
    fp->SetParent( board() );

    size_t i = 0;

    for( PAD* pad : fp->Pads() )
    {
        pad->SetNet( netFor( i < aSnap.padNets.size() ? aSnap.padNets[i] : wxString(), aCommit, aOut ) );
        ++i;
    }

    return fp;
}


void KICADD_HANDLER_PCB::record( HISTORY_ENTRY&& aEntry )
{
    m_undo.push_back( std::move( aEntry ) );
    m_redo.clear();

    if( m_undo.size() > MAX_HISTORY )
        m_undo.erase( m_undo.begin() );
}


namespace
{

std::string mmText( int64_t aNm )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%+.2f", aNm / 1e6 );
    return buf;
}


std::string refList( const std::vector<std::string>& aRefs )
{
    std::string refs;

    for( size_t i = 0; i < aRefs.size() && i < 3; ++i )
        refs += ( i ? "," : "" ) + aRefs[i];

    if( aRefs.size() > 3 )
        refs += ",+" + std::to_string( aRefs.size() - 3 );

    return refs;
}


std::string refList( const std::vector<FOOTPRINT*>& aTargets )
{
    std::vector<std::string> refs;

    for( const FOOTPRINT* fp : aTargets )
        refs.push_back( fp->GetReference().ToStdString() );

    return refList( refs );
}


/// Label of a move/rotate/flip, computed before it is applied.
std::string describeMove( const std::vector<FOOTPRINT*>& aTargets, const VECTOR2I& aDelta, int aDrotDecideg,
                          bool aFlip )
{
    const std::string refs = refList( aTargets );
    std::string       label;

    if( aDelta.x || aDelta.y )
        label = "move " + refs + " " + mmText( aDelta.x ) + "," + mmText( aDelta.y ) + " mm";

    if( aDrotDecideg )
    {
        char buf[32];
        std::snprintf( buf, sizeof( buf ), "%+g", aDrotDecideg / 10.0 );
        label += ( label.empty() ? "rotate " + refs + " " : ", rotate " ) + std::string( buf ) + "°";
    }

    if( aFlip )
    {
        // Side after the flip (called before it: the label is the commit message), when all
        // targets end up on the same one.
        bool        back = !aTargets.empty() && !aTargets.front()->IsFlipped();
        bool        same = std::all_of( aTargets.begin(), aTargets.end(),
                                        [&]( const FOOTPRINT* fp ) { return !fp->IsFlipped() == back; } );
        std::string side = same ? ( back ? " to B.Cu" : " to F.Cu" ) : "";
        label += ( label.empty() ? "flip " + refs : ", flip" ) + side;
    }

    return label;
}


/// Next free "<prefix><n>" designator.
wxString nextReference( BOARD* aBoard, const wxString& aPrefix )
{
    long highest = 0;

    for( const FOOTPRINT* fp : aBoard->Footprints() )
    {
        const wxString& ref = fp->GetReference();
        long            n = 0;

        if( ref.StartsWith( aPrefix ) && ref.Mid( aPrefix.length() ).ToLong( &n ) && n > highest )
            highest = n;
    }

    return aPrefix + wxString::Format( wxS( "%ld" ), highest + 1 );
}


wxString referencePrefix( const wxString& aRef )
{
    size_t n = aRef.length();

    while( n > 0 && wxIsdigit( aRef[n - 1] ) )
        --n;

    return aRef.Left( n );
}


/// A footprint from the library table. The library is loaded first if it is not yet: the adapter
/// is made on first use and loads its libraries in the background, so one registered moments ago
/// (lib_create_footprint, lib_register) would not be found yet.
FOOTPRINT* loadFromTable( BOARD* aBoard, const LIB_ID& aLibId )
{
    try
    {
        FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( nullptr /* ignored */ );
        const wxString             nick = aLibId.GetLibNickname().wx_str();

        if( adapter && !adapter->IsLibraryLoaded( nick ) )
            adapter->LoadLibraryEntry( nick );
    }
    catch( ... )
    {
    }

    return LoadFootprintFromProject( aBoard, aLibId );
}


/// The directories searched after the library table, in order.
std::vector<wxString> footprintDirectories()
{
    std::vector<wxString> dirs;

    if( const char* env = std::getenv( "KICADD_FOOTPRINT_DIRS" ) )
    {
#ifdef _WIN32
        wxStringTokenizer tok( wxString::FromUTF8( env ), wxS( ";" ) );
#else
        wxStringTokenizer tok( wxString::FromUTF8( env ), wxS( ":" ) );
#endif

        while( tok.HasMoreTokens() )
            dirs.push_back( tok.GetNextToken() );
    }

#ifndef _WIN32
    dirs.push_back( wxS( "/usr/share/kicad/footprints" ) );
#endif
    return dirs;
}


/// A footprint from a .pretty directory: the project's lcsc.pretty for LCSC:..., then
/// KICADD_FOOTPRINT_DIRS, then the system library.
std::unique_ptr<FOOTPRINT> loadFromDirectories( const BOARD* aBoard, const LIB_ID& aLibId, std::string& aSource )
{
    if( aLibId.GetLibNickname().compare( kicadd::lcsc::LIB_NICKNAME ) == 0 && aBoard
        && !aBoard->GetFileName().IsEmpty() )
    {
        wxFileName lib( wxFileName( aBoard->GetFileName() ).GetPath(), wxEmptyString );
        lib.AppendDir( wxS( "lcsc.pretty" ) );
        wxFileName file( lib.GetPath(), aLibId.GetLibItemName().wx_str(), wxS( "kicad_mod" ) );

        if( file.FileExists() )
        {
            try
            {
                PCB_IO_KICAD_SEXPR         io;
                std::unique_ptr<FOOTPRINT> fp = io.FootprintLoad( lib.GetPath(), aLibId.GetLibItemName().wx_str() );

                if( fp )
                {
                    aSource = file.GetFullPath().ToStdString();
                    return fp;
                }
            }
            catch( const IO_ERROR& )
            {
            }
        }
    }

    for( const wxString& dir : footprintDirectories() )
    {
        wxFileName lib( dir, wxEmptyString );
        lib.AppendDir( aLibId.GetLibNickname().wx_str() + wxS( ".pretty" ) );

        wxFileName file( lib.GetPath(), aLibId.GetLibItemName().wx_str(), wxS( "kicad_mod" ) );

        if( !file.FileExists() )
            continue;

        try
        {
            PCB_IO_KICAD_SEXPR         io;
            std::unique_ptr<FOOTPRINT> fp = io.FootprintLoad( lib.GetPath(), aLibId.GetLibItemName().wx_str() );

            if( fp )
            {
                aSource = file.GetFullPath().ToStdString();
                return fp;
            }
        }
        catch( const IO_ERROR& )
        {
        }
    }

    return nullptr;
}


/// A copy of a board footprint as a fresh, unplaced part: new KIIDs, no nets, front side, 0°.
std::unique_ptr<FOOTPRINT> copyAsNew( const FOOTPRINT* aSource )
{
    std::unique_ptr<FOOTPRINT> fp( static_cast<FOOTPRINT*>( aSource->Clone() ) );

    fp->SetUuid( KIID() );
    fp->RunOnChildren( []( BOARD_ITEM* aChild ) { aChild->ResetUuid(); }, RECURSE_MODE::RECURSE );
    fp->ClearAllNets();

    // a new part, placed by hand: not the schematic symbol the original belongs to (sync_pcb
    // would take the copy for that symbol)
    fp->SetPath( KIID_PATH() );
    fp->SetSheetname( wxEmptyString );
    fp->SetSheetfile( wxEmptyString );

    if( fp->IsFlipped() )
        fp->Flip( fp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

    fp->SetOrientation( ANGLE_0 );
    return fp;
}

} // namespace


HANDLER_RESULT<MoveResponse>
KICADD_HANDLER_PCB::handleSetFields( const HANDLER_CONTEXT<FpSetFields>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    MoveResponse                                         out;
    std::vector<std::pair<FOOTPRINT*, const FpFieldEdit*>> edits;
    std::map<const FOOTPRINT*, wxString>                 newRefs;   // the parts renumbered
    auto bad = [&]( const std::string& aMsg ) { return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, aMsg ) ); };
    auto at = []( const FOOTPRINT* aFp )
    {
        char buf[64];
        std::snprintf( buf, sizeof( buf ), "%.4g,%.4g", aFp->GetPosition().x / 1e6, aFp->GetPosition().y / 1e6 );
        return std::string( buf );
    };

    for( const FpFieldEdit& e : aCtx.Request.parts() )
    {
        std::optional<wxString> newRef;

        for( const auto& [name, text] : e.fields() )
        {
            std::string n = name;
            std::transform( n.begin(), n.end(), n.begin(), ::toupper );

            if( n == "FOOTPRINT" )
                return bad( e.ref() + ": " + name + " is not changed here" );

            if( n == "REFERENCE" )
            {
                newRef = wxString::FromUTF8( text ).Trim().Trim( false );

                if( newRef->IsEmpty() || newRef->Contains( wxS( " " ) ) )
                    return bad( e.ref() + ": Reference is a designator like R12" );
            }
        }

        std::vector<FOOTPRINT*> same;

        for( FOOTPRINT* fp : b->Footprints() )
        {
            if( fp->GetReference() == wxString::FromUTF8( e.ref() ) )
                same.push_back( fp );
        }

        if( same.empty() )
        {
            out.add_missing( e.ref() );
            continue;
        }

        FOOTPRINT* fp = same.front();

        // Renumbering one of two parts of the same reference (a sync that added a second one):
        // the one that is not from the schematic, which keeps its symbol's reference
        if( newRef && same.size() > 1 )
        {
            auto byHand = std::find_if( same.begin(), same.end(), []( const FOOTPRINT* aFp ) { return aFp->GetPath().empty(); } );

            if( byHand != same.end() )
                fp = *byHand;

            out.add_notes( std::to_string( same.size() ) + " parts are " + e.ref() + ": renumbered the one at " + at( fp )
                           + ( fp->GetPath().empty() ? " (placed by hand, not from the schematic)" : "" ) );
        }

        if( newRef && *newRef != fp->GetReference() )
            newRefs[fp] = *newRef;

        edits.emplace_back( fp, &e );
    }

    // A new reference is one no other part has (or gives up in this same edit)
    for( const auto& [fp, ref] : newRefs )
    {
        for( const FOOTPRINT* other : b->Footprints() )
        {
            auto     moved = newRefs.find( other );
            wxString theirs = moved != newRefs.end() ? moved->second : other->GetReference();

            if( other != fp && theirs.CmpNoCase( ref ) == 0 )
            {
                return bad( fp->GetReference().ToStdString() + ": another part is " + ref.ToStdString() + " already (at "
                            + at( other ) + "); give a reference that is free" );
            }
        }

        if( !fp->GetPath().empty() )
        {
            out.add_notes( fp->GetReference().ToStdString() + " -> " + ref.ToStdString() + ": this part comes from the "
                           "schematic, where it is still " + fp->GetReference().ToStdString() + ": the next sync_pcb puts "
                           "that back. To keep the new reference, renumber the symbol (sch_set_fields), then sync_pcb" );
        }
    }

    if( edits.empty() )
        return out;

    HISTORY_ENTRY           entry;
    std::vector<FOOTPRINT*> targets;

    for( auto& [fp, e] : edits )
    {
        entry.before.push_back( stateOf( fp, true ) );
        targets.push_back( fp );
    }

    entry.label = "set fields of " + std::to_string( targets.size() ) + " footprint(s)";

    if( newRefs.size() == 1 )
        entry.label += " (" + newRefs.begin()->first->GetReference().ToStdString() + " is now " + newRefs.begin()->second.ToStdString() + ")";
    else if( !newRefs.empty() )
        entry.label += " (" + std::to_string( newRefs.size() ) + " renumbered)";

    std::map<const FOOTPRINT*, const FpFieldEdit*> byFp( edits.begin(), edits.end() );

    applyFootprintEdit( targets,
                        [&]( FOOTPRINT* fp )
                        {
                            const FpFieldEdit& e = *byFp[fp];
                            KIGFX::VIEW*       v = view();

                            if( v )
                                v->Remove( fp );

                            for( const auto& [name, text] : e.fields() )
                            {
                                wxString n = wxString::FromUTF8( name );

                                if( n.CmpNoCase( wxS( "Value" ) ) == 0 )
                                    fp->SetValue( wxString::FromUTF8( text ) );
                                else if( n.CmpNoCase( wxS( "Reference" ) ) == 0 )
                                    fp->SetReference( wxString::FromUTF8( text ).Trim().Trim( false ) );
                                else if( PCB_FIELD* f = fp->GetField( n ) )
                                    f->SetText( wxString::FromUTF8( text ) );
                                else
                                    addUserField( fp, n, wxString::FromUTF8( text ) );
                            }

                            for( const std::string& name : e.remove() )
                            {
                                PCB_FIELD* f = fp->GetField( wxString::FromUTF8( name ) );

                                if( f && !f->IsMandatory() )
                                {
                                    fp->Remove( f );
                                    delete f;
                                }
                            }

                            if( e.has_dnp() )
                                fp->SetDNP( e.dnp() );

                            if( e.has_exclude_from_bom() )
                                fp->SetExcludedFromBOM( e.exclude_from_bom() );

                            if( e.has_exclude_from_pos() )
                                fp->SetExcludedFromPosFiles( e.exclude_from_pos() );

                            if( v )
                                v->Add( fp );
                        },
                        wxString::FromUTF8( "Agent: " + entry.label ), out );

    for( const FOOTPRINT* fp : targets )
        entry.after.push_back( stateOf( fp, true ) );

    out.set_history_label( entry.label );
    record( std::move( entry ) );
    return out;
}


HANDLER_RESULT<MoveResponse>
KICADD_HANDLER_PCB::handleMoveDelta( const HANDLER_CONTEXT<MoveDelta>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const MoveDelta& req = aCtx.Request;

    if( req.refs().empty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "MoveDelta needs refs" ) );

    MoveResponse            out;
    std::vector<FOOTPRINT*> targets;

    for( const std::string& ref : req.refs() )
    {
        if( FOOTPRINT* fp = b->FindFootprintByReference( wxString::FromUTF8( ref ) ) )
            targets.push_back( fp );
        else
            out.add_missing( ref );
    }

    const VECTOR2I  delta( req.dx(), req.dy() );
    const EDA_ANGLE drot( req.drot_decideg() / 10.0, DEGREES_T );
    const bool      changes = delta.x || delta.y || req.drot_decideg() || req.flip();

    HISTORY_ENTRY entry;

    for( const FOOTPRINT* fp : targets )
        entry.before.push_back( stateOf( fp ) );

    // Group pivot: centre of the bodies after the move (the move applies first).
    const bool group = req.group_pivot() && targets.size() > 1;
    VECTOR2I   pivot;

    if( group )
    {
        BOX2I box = targets.front()->GetBoundingBox( false );

        for( const FOOTPRINT* fp : targets )
            box.Merge( fp->GetBoundingBox( false ) );

        pivot = box.Centre() + delta;
    }

    const std::string label = changes ? describeMove( targets, delta, req.drot_decideg(), req.flip() )
                                      : std::string();

    applyFootprintEdit( changes ? targets : std::vector<FOOTPRINT*>(),
                        [&]( FOOTPRINT* fp )
                        {
                            if( delta.x != 0 || delta.y != 0 )
                                fp->Move( delta );

                            if( req.drot_decideg() != 0 )
                                fp->Rotate( group ? pivot : fp->GetPosition(), drot );

                            // Mirrored left-right: Flip swaps every layer of the footprint
                            // (copper padstacks, silk, fab, courtyard, mask, paste).
                            if( req.flip() )
                                fp->Flip( group ? pivot : fp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );
                        },
                        wxString::FromUTF8( "Agent: " + label ), out );

    if( !targets.empty() && changes )
    {
        for( const FOOTPRINT* fp : targets )
            entry.after.push_back( stateOf( fp ) );

        entry.label = label;
        out.set_history_label( entry.label );
        record( std::move( entry ) );
    }

    return out;
}


HANDLER_RESULT<MoveResponse>
KICADD_HANDLER_PCB::handleDeleteFootprints( const HANDLER_CONTEXT<DeleteFootprints>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    if( aCtx.Request.refs().empty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "DeleteFootprints needs refs" ) );

    MoveResponse            out;
    std::vector<FOOTPRINT*> targets;

    for( const std::string& ref : aCtx.Request.refs() )
    {
        FOOTPRINT* fp = b->FindFootprintByReference( wxString::FromUTF8( ref ) );

        if( !fp )
            out.add_missing( ref );
        else if( std::find( targets.begin(), targets.end(), fp ) == targets.end() )
            targets.push_back( fp );
    }

    if( targets.empty() )
        return out;

    HISTORY_ENTRY entry;
    entry.label = "delete " + refList( targets );

    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                for( FOOTPRINT* fp : targets )
                {
                    out.add_removed( fp->GetReference().ToStdString() );
                    aCommit.Remove( fp );
                }

                return std::vector<KIID>();
            },
            wxString::FromUTF8( "Agent: " + entry.label ), out );

    // Headless commits keep no undo list, so the removed footprints are ours now: off the
    // board and out of connectivity (BOARD::Remove), they become the history's snapshots.
    // In the desktop editor the commit hands them to the editor's undo list (Ctrl+Z puts that
    // very object back), so the history keeps a copy instead.
    for( FOOTPRINT* fp : targets )
        entry.removed.push_back( detach( editorOwnsUndo() ? static_cast<FOOTPRINT*>( fp->Clone() ) : fp ) );

    out.set_history_label( entry.label );
    record( std::move( entry ) );
    return out;
}


HANDLER_RESULT<FpCreateResponse>
KICADD_HANDLER_PCB::handleFpCreate( const HANDLER_CONTEXT<FpCreate>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    std::string name, text, err;
    int         pads = 0;

    if( !kicadd::MakeFootprint( aCtx.Request, name, text, pads, err ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    if( name.find_first_of( ":/\\\"" ) != std::string::npos )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "name must not contain : / \\ \"" ) );

    wxString projectDir = b->GetProject() && !b->GetProject()->GetProjectPath().IsEmpty()
                                  ? b->GetProject()->GetProjectPath()
                                  : wxFileName( b->GetFileName() ).GetPath();
    wxFileName lib = wxFileName::DirName( projectDir );
    lib.AppendDir( wxS( "kicadd.pretty" ) );

    if( !lib.DirExists() && !wxFileName::Mkdir( lib.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "cannot create " + lib.GetPath().ToStdString() ) );

    wxFileName file( lib.GetPath(), wxString::FromUTF8( name ), wxS( "kicad_mod" ) );

    if( file.FileExists() && !aCtx.Request.replace() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "kicadd:" + name + " exists already (replace=true overwrites it)" ) );

    {
        wxFFile out( file.GetFullPath(), wxS( "wb" ) );

        if( !out.IsOpened() || out.Write( text.data(), text.size() ) != text.size() )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "cannot write " + file.GetFullPath().ToStdString() ) );
    }

    FpCreateResponse out;

    // read it back: the file has to be one KiCad loads
    try
    {
        PCB_IO_KICAD_SEXPR         io;
        std::unique_ptr<FOOTPRINT> fp( io.FootprintLoad( lib.GetPath(), wxString::FromUTF8( name ) ) );

        if( !fp )
            throw std::runtime_error( "not loaded" );

        fp->BuildCourtyardCaches();
        BOX2I cy = fp->GetCourtyard( F_CrtYd ).BBox();
        out.mutable_courtyard()->set_x( cy.GetWidth() );
        out.mutable_courtyard()->set_y( cy.GetHeight() );
        out.set_pads( (int) fp->Pads().size() );
    }
    catch( const std::exception& e )
    {
        wxRemoveFile( file.GetFullPath() );
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, std::string( "footprint not made: " ) + e.what() ) );
    }
    catch( const IO_ERROR& e )
    {
        wxRemoveFile( file.GetFullPath() );
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "footprint not made: " + e.What().ToStdString() ) );
    }

    std::string note;

    switch( kicadd::AddProjectLibrary( projectDir, false, "kicadd", "${KIPRJMOD}/kicadd.pretty",
                                       "footprints made by KiCadD", note ) )
    {
    case kicadd::LIBTABLE_RESULT::ADDED:    out.set_registered( true ); break;
    case kicadd::LIBTABLE_RESULT::CONFLICT: note = "the project has another library named kicadd"; break;
    default: break;
    }

    kicadd::ReloadProjectLibrary( false, "kicadd" );
    out.set_lib_id( "kicadd:" + name );
    out.set_file( file.GetFullPath().ToStdString() );
    out.set_note( note );
    return out;
}


HANDLER_RESULT<LcscSearchResponse>
KICADD_HANDLER_PCB::handleLcscSearch( const HANDLER_CONTEXT<LcscSearch>& aCtx )
{
    LcscSearchResponse out;
    std::string        err;

    // runs on the editor's thread: the first call downloads the index (seconds); later ones are
    // milliseconds plus the live checks
    if( !kicadd::lcsc::SearchParts( aCtx.Request, out, err ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    return out;
}


HANDLER_RESULT<ExportFabResponse>
KICADD_HANDLER_PCB::handleExportFab( const HANDLER_CONTEXT<ExportFab>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    if( aCtx.Request.check_only() )
    {
        ExportFabResponse out;

        for( const std::string& p : kicadd::FabProblems( b ) )
            out.add_problems( p );

        return out;
    }

    wxString dir = wxString::FromUTF8( aCtx.Request.dir() );
    wxFileName boardFile( b->GetFileName() );

    if( dir.IsEmpty() )
    {
        dir = boardFile.GetPath() + wxFileName::GetPathSeparator() + wxS( "fab" );
    }
    else if( !wxFileName( dir ).IsAbsolute() )
    {
        wxFileName d = wxFileName::DirName( dir );
        d.MakeAbsolute( boardFile.GetPath() );
        dir = d.GetPath();
    }

    ExportFabResponse out;
    std::string       err;

    if( !kicadd::ExportFab( b, dir, !aCtx.Request.no_zip(), out, err ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    return out;
}


HANDLER_RESULT<RenderBoardResponse>
KICADD_HANDLER_PCB::handleRenderBoard( const HANDLER_CONTEXT<RenderBoard>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    RenderBoardResponse out;
    std::string         err;
    RenderBoard         req = aCtx.Request;

    // a relative path is in the project's folder
    if( !req.path().empty() && !wxFileName( wxString::FromUTF8( req.path() ) ).IsAbsolute() )
    {
        wxFileName f( wxString::FromUTF8( req.path() ) );
        f.MakeAbsolute( wxFileName( b->GetFileName() ).GetPath() );
        req.set_path( f.GetFullPath().ToStdString( wxConvUTF8 ) );
    }

    if( !kicadd::RenderBoardPng( b, req, out, err ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    return out;
}


HANDLER_RESULT<MoveResponse>
KICADD_HANDLER_PCB::handleDeleteTracks( const HANDLER_CONTEXT<DeleteTracks>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const DeleteTracks& req = aCtx.Request;
    const bool          byRegion = req.has_region_min() && req.has_region_max();

    if( !req.all() && req.nets().empty() && !byRegion && req.refs().empty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "DeleteTracks needs all, nets, region or refs" ) );

    MoveResponse                out;
    std::set<const PCB_TRACK*>  match;
    std::set<wxString>          nets;

    for( const std::string& n : req.nets() )
    {
        if( b->FindNet( wxString::FromUTF8( n ) ) )
            nets.insert( wxString::FromUTF8( n ) );
        else
            out.add_missing( "net " + n );
    }

    BOX2I region;

    if( byRegion )
    {
        VECTOR2I a( req.region_min().x(), req.region_min().y() );
        VECTOR2I c( req.region_max().x(), req.region_max().y() );
        region = BOX2I( a, c - a );
        region.Normalize();
    }

    for( PCB_TRACK* t : b->Tracks() )
    {
        if( req.all() || nets.count( t->GetNetname() ) || ( byRegion && t->HitTest( region, false ) ) )
            match.insert( t );
    }

    // refs: the copper running from their pads, through tracks and vias, up to the next pad
    if( !req.refs().empty() )
    {
        std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();
        std::vector<const BOARD_CONNECTED_ITEM*> todo;

        for( const std::string& ref : req.refs() )
        {
            FOOTPRINT* fp = b->FindFootprintByReference( wxString::FromUTF8( ref ) );

            if( !fp )
            {
                out.add_missing( ref );
                continue;
            }

            for( const PAD* pad : fp->Pads() )
                todo.push_back( pad );
        }

        std::set<const FOOTPRINT*> mine;

        for( const BOARD_CONNECTED_ITEM* item : todo )
            mine.insert( item->GetParentFootprint() );

        while( !todo.empty() )
        {
            const BOARD_CONNECTED_ITEM* item = todo.back();
            todo.pop_back();

            // direct neighbours (CONNECTIVITY_DATA::GetConnectedItems gives the whole island)
            std::vector<BOARD_CONNECTED_ITEM*> adjacent;

            if( const CN_CONNECTIVITY_ALGO::ITEM_MAP_ENTRY* entry = conn->GetConnectivityAlgo()->FindItemEntry( item ) )
            {
                for( const CN_ITEM* cn : entry->GetItems() )
                {
                    for( const CN_ITEM* other : cn->ConnectedItems() )
                    {
                        BOARD_CONNECTED_ITEM* parent = other->Parent();

                        if( parent && parent != item && parent->Type() != PCB_ZONE_T
                            && std::find( adjacent.begin(), adjacent.end(), parent ) == adjacent.end() )
                        {
                            adjacent.push_back( parent );
                        }
                    }
                }
            }

            // a track that reaches another part's pad is the end of the connection: taken, but
            // not followed further (else a GND tree would go all the way round the board)
            if( item->Type() != PCB_PAD_T )
            {
                bool end = std::any_of( adjacent.begin(), adjacent.end(),
                                        [&]( const BOARD_CONNECTED_ITEM* n )
                                        {
                                            return n->Type() == PCB_PAD_T && !mine.count( n->GetParentFootprint() );
                                        } );

                if( end )
                    continue;
            }

            for( BOARD_CONNECTED_ITEM* next : adjacent )
            {
                KICAD_T type = next->Type();

                if( ( type == PCB_TRACE_T || type == PCB_ARC_T || type == PCB_VIA_T )
                    && match.insert( static_cast<PCB_TRACK*>( next ) ).second )
                {
                    todo.push_back( next );
                }
            }
        }
    }

    std::vector<PCB_TRACK*> targets;
    int                     locked = 0;

    for( PCB_TRACK* t : b->Tracks() )     // board order: the same every time
    {
        if( !match.count( t ) )
            continue;

        if( req.keep_vias() && t->Type() == PCB_VIA_T )
            continue;

        if( t->IsLocked() && !req.include_locked() )
        {
            locked++;
            continue;
        }

        targets.push_back( t );
    }

    out.set_tracks_locked( locked );

    if( targets.empty() )
    {
        std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();
        out.set_unconnected( static_cast<int>( conn->GetUnconnectedCountNoFlush( false ) ) );
        return out;
    }

    HISTORY_ENTRY entry;
    entry.label = "delete " + std::to_string( targets.size() ) + " track(s)/via(s)";

    for( PCB_TRACK* t : targets )
        entry.tracksRemoved.push_back( { nullptr, t->GetNetCode() > 0 ? t->GetNetname() : wxString() } );

    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                for( PCB_TRACK* t : targets )
                    aCommit.Remove( t );

                return std::vector<KIID>();
            },
            wxString::FromUTF8( "Agent: " + entry.label ), out );

    // As with footprints: headless, the removed tracks are ours now; in the editor its undo
    // list holds them, so the history keeps copies.
    for( size_t i = 0; i < targets.size(); ++i )
    {
        PCB_TRACK* t = editorOwnsUndo() ? static_cast<PCB_TRACK*>( targets[i]->Clone() ) : targets[i];
        t->SetNet( NETINFO_LIST::OrphanedItem() );
        t->SetParent( nullptr );
        entry.tracksRemoved[i].track.reset( t );
    }

    out.set_tracks_removed( (int) targets.size() );
    out.set_history_label( entry.label );
    record( std::move( entry ) );
    return out;
}


namespace
{

/// A route end: a pad ("U1.5"), or the copper at a point (a pad, track or via there; for the
/// far end also nothing: a free end).
std::string ptText( const VECTOR2I& aPos )
{
    char buf[64];
    std::snprintf( buf, sizeof( buf ), "%.4g,%.4g", aPos.x / 1e6, aPos.y / 1e6 );
    return buf;
}


struct ROUTE_END
{
    BOARD_CONNECTED_ITEM* item = nullptr;
    VECTOR2I              pos;
    std::string           name;
};


std::optional<ROUTE_END> resolveEnd( BOARD* aBoard, const RouteEnd& aEnd, PCB_LAYER_ID aLayer, int aNet,
                                     bool aFreeOk, std::string& aError )
{
    ROUTE_END out;

    if( !aEnd.pad().empty() )
    {
        const std::string& name = aEnd.pad();
        size_t             dot = name.rfind( '.' );
        FOOTPRINT*         fp = dot == std::string::npos
                                        ? nullptr
                                        : aBoard->FindFootprintByReference( wxString::FromUTF8( name.substr( 0, dot ) ) );
        PAD*               pad = fp ? fp->FindPadByNumber( wxString::FromUTF8( name.substr( dot + 1 ) ) ) : nullptr;

        if( !pad )
        {
            aError = "no pad " + name + ( dot == std::string::npos ? " (write REF.PAD, e.g. U1.5)" : "" );
            return std::nullopt;
        }

        out.item = pad;
        out.pos = pad->GetPosition();
        out.name = name;
        return out;
    }

    if( !aEnd.has_pos() )
    {
        aError = "an end needs pad or pos";
        return std::nullopt;
    }

    out.pos = VECTOR2I( aEnd.pos().x(), aEnd.pos().y() );
    out.name = ptText( out.pos );

    // The copper there: a pad, via or track (of the net, when it is known)
    BOARD_CONNECTED_ITEM* best = nullptr;
    int                   bestRank = 99;

    auto consider = [&]( BOARD_CONNECTED_ITEM* aItem, int aRank )
    {
        if( aNet > 0 && aItem->GetNetCode() != aNet )
            return;

        if( aLayer != UNDEFINED_LAYER && !aItem->IsOnLayer( aLayer ) )
            return;

        if( aRank < bestRank && aItem->HitTest( out.pos, 0 ) )
        {
            best = aItem;
            bestRank = aRank;
        }
    };

    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        if( !fp->GetBoundingBox( false ).Contains( out.pos ) )
            continue;

        for( PAD* pad : fp->Pads() )
            consider( pad, 0 );
    }

    for( PCB_TRACK* t : aBoard->Tracks() )
        consider( t, t->Type() == PCB_VIA_T ? 1 : 2 );

    if( best )
    {
        out.item = best;

        // a via is entered at its centre, a pad where the point is (off the centre: a way out
        // between close pads), a track where the point is, on its axis
        if( best->Type() == PCB_VIA_T )
            out.pos = best->GetPosition();
        else if( best->Type() == PCB_TRACE_T )
            out.pos = static_cast<PCB_TRACK*>( best )->GetEffectiveShape( UNDEFINED_LAYER )->Centre() == out.pos
                              ? out.pos
                              : SEG( static_cast<PCB_TRACK*>( best )->GetStart(),
                                     static_cast<PCB_TRACK*>( best )->GetEnd() ).NearestPoint( out.pos );

        out.name = kicadd::route::Describe( best );

        if( best->Type() == PCB_TRACE_T || best->Type() == PCB_ARC_T
            || ( best->Type() == PCB_PAD_T && out.pos != best->GetPosition() ) )
            out.name += " @" + ptText( out.pos );

        return out;
    }

    if( !aFreeOk )
    {
        aError = "no copper" + std::string( aNet > 0 ? " of the net" : "" ) + " at " + out.name
                 + ( aLayer != UNDEFINED_LAYER ? " on " + LSET::Name( aLayer ).ToStdString() : std::string() );
        return std::nullopt;
    }

    return out;
}


void fillResult( RouteResult& aOut, const kicadd::route::RESULT& aRes )
{
    aOut.set_ok( aRes.ok );
    aOut.set_error( aRes.error );
    aOut.set_net( aRes.net );
    aOut.set_layer( aRes.layer );
    aOut.set_width( aRes.width );
    aOut.set_length( aRes.length );
    aOut.set_direct( aRes.direct );
    aOut.set_segments( aRes.segments );
    aOut.set_vias( aRes.vias );
    aOut.set_min_gap( aRes.minGap );
    aOut.set_gap_to( aRes.gapTo );
    aOut.set_blocked_by( aRes.blockedBy );

    if( aRes.reached )
    {
        aOut.mutable_reached()->set_x( aRes.reached->x );
        aOut.mutable_reached()->set_y( aRes.reached->y );
    }

    if( aRes.blockedAt )
    {
        aOut.mutable_blocked_at()->set_x( aRes.blockedAt->x );
        aOut.mutable_blocked_at()->set_y( aRes.blockedAt->y );
    }
}

/// The pads and vias joined to aEnd (other than aEnd and aStart), nearest to aFrom first: other
/// ways to reach the same copper when aEnd itself can't be (a track on the far layer)
std::vector<BOARD_CONNECTED_ITEM*> alternatives( CONNECTIVITY_DATA& aConn, BOARD_CONNECTED_ITEM* aEnd,
                                                 const BOARD_CONNECTED_ITEM* aStart, const VECTOR2I& aFrom )
{
    std::vector<std::pair<long long, BOARD_CONNECTED_ITEM*>> alt;

    for( BOARD_CONNECTED_ITEM* item : aConn.GetConnectedItems( aEnd ) )
    {
        if( ( item->Type() == PCB_PAD_T || item->Type() == PCB_VIA_T ) && item != aEnd && item != aStart
            && item->GetNetCode() == aStart->GetNetCode() )
            alt.emplace_back( ( item->GetPosition() - aFrom ).EuclideanNorm(), item );
    }

    std::sort( alt.begin(), alt.end(), []( const auto& x, const auto& y ) { return x.first < y.first; } );

    std::vector<BOARD_CONNECTED_ITEM*> out;

    for( size_t i = 0; i < alt.size() && i < 4; ++i )
        out.push_back( alt[i].second );

    return out;
}

} // namespace


HANDLER_RESULT<RouteResponse> KICADD_HANDLER_PCB::handleRoute( const HANDLER_CONTEXT<RouteTrack>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const RouteTrack& req = aCtx.Request;
    std::string       err;

    auto layerOf = [&]( const std::string& aName, PCB_LAYER_ID& aLayer ) -> bool
    {
        aLayer = UNDEFINED_LAYER;

        if( aName.empty() )
            return true;

        int id = b->GetLayerID( wxString::FromUTF8( aName ) );

        if( id < 0 || !IsCopperLayer( id ) || !b->IsLayerEnabled( ToLAYER_ID( id ) ) )
        {
            err = "no copper layer " + aName;
            return false;
        }

        aLayer = ToLAYER_ID( id );
        return true;
    };

    kicadd::route::REQUEST r;

    if( !layerOf( req.layer(), r.layer ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    std::optional<ROUTE_END> from = resolveEnd( b, req.from(), r.layer, 0, false, err );

    if( !from )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "from: " + err ) );

    std::optional<ROUTE_END> to;

    if( req.has_to() && ( !req.to().pad().empty() || req.to().has_pos() ) )
    {
        to = resolveEnd( b, req.to(), UNDEFINED_LAYER, from->item->GetNetCode(), true, err );

        if( !to )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "to: " + err ) );
    }
    else
    {
        // The nearest unrouted connection of the start (its ratsnest line)
        std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();
        RN_NET*                            rn = conn->GetRatsnestForNet( from->item->GetNetCode() );
        unsigned                           best = UINT_MAX;

        for( const CN_EDGE& edge : rn ? rn->GetEdges() : std::vector<CN_EDGE>() )
        {
            std::shared_ptr<const CN_ANCHOR> a = edge.GetSourceNode(), z = edge.GetTargetNode();

            if( !a || !z || a->Dirty() || z->Dirty() )
                continue;

            std::shared_ptr<const CN_ANCHOR> other = a->Parent() == from->item ? z : z->Parent() == from->item ? a : nullptr;

            if( other && other->Parent() && edge.GetLength() < best )
            {
                best = edge.GetLength();
                to = ROUTE_END{ other->Parent(), other->Pos(), kicadd::route::Describe( other->Parent() ) };

                if( other->Parent()->Type() == PCB_PAD_T || other->Parent()->Type() == PCB_VIA_T )
                    to->pos = other->Parent()->GetPosition();
                else
                    to->name += " @" + ptText( to->pos );
            }
        }

        if( !to )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              from->name + " has no unrouted connection: give `to`" ) );
    }

    r.fromItem = from->item;
    r.from = from->pos;
    r.toItem = to->item;
    r.to = to->pos;
    r.width = (int) req.width();

    for( const RouteStop& st : req.stops() )
    {
        kicadd::route::STOP stop;
        stop.pos = VECTOR2I( st.pos().x(), st.pos().y() );

        if( st.via_to() == "other" )
            stop.otherSide = true;
        else if( !layerOf( st.via_to(), stop.viaTo ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

        r.stops.push_back( stop );
    }

    RouteResponse                 out;
    kicadd::route::SESSION        session( b );
    kicadd::route::RESULT         res = session.Route( r );

    // The end taken from the ratsnest can't be reached (a track on the other layer): the pads and
    // vias joined to it may be
    const bool autoTo = !( req.has_to() && ( !req.to().pad().empty() || req.to().has_pos() ) );

    if( !res.ok && autoTo && to->item && r.stops.empty() )
    {
        for( BOARD_CONNECTED_ITEM* alt : alternatives( *b->GetConnectivity(), to->item, from->item, from->pos ) )
        {
            r.toItem = alt;
            r.to = alt->GetPosition();
            kicadd::route::RESULT attempt = session.Route( r );

            if( attempt.ok )
            {
                res = attempt;
                to = ROUTE_END{ alt, alt->GetPosition(), kicadd::route::Describe( alt ) };

                if( alt->Type() != PCB_PAD_T )
                    to->name += " @" + ptText( alt->GetPosition() );
                break;
            }
        }
    }

    RouteResult*                  rr = out.add_results();

    rr->set_from( from->name );
    rr->set_to( to->name );
    fillResult( *rr, res );

    for( const DrcViolation& v : res.drc )
        *out.add_drc() = v;

    out.set_dry_run( req.dry_run() );

    // Not put on the board: a dry run, a failure, or tracks that would break a rule (the router
    // keeps clearances; this is the check that it did)
    if( !res.ok || req.dry_run() )
    {
        out.set_unconnected( (int) b->GetConnectivity()->GetUnconnectedCountNoFlush( false ) );
        return out;
    }

    commitRoutes( session.TakeAdded(), session.Replaced(),
                  "route " + res.net + " " + from->name + " -> " + to->name, out );
    return out;
}


namespace
{

/// "R9.1" for a pad, else the router's description and where: "track GND (B.Cu) @12.5,30",
/// "via GND @14,30"
std::string endName( const BOARD_CONNECTED_ITEM* aItem, const VECTOR2I& aPos )
{
    if( aItem && aItem->Type() == PCB_PAD_T )
    {
        const PAD* pad = static_cast<const PAD*>( aItem );
        return pad->GetParentFootprint()->GetReference().ToStdString() + "." + pad->GetNumber().ToStdString();
    }

    return kicadd::route::Describe( aItem ) + " @" + ptText( aPos );
}


/// Where a route enters an item: a pad's or via's centre, else the anchor
VECTOR2I entry( const BOARD_CONNECTED_ITEM* aItem, const VECTOR2I& aAnchor )
{
    if( aItem->Type() == PCB_PAD_T || aItem->Type() == PCB_VIA_T )
        return aItem->GetPosition();

    return aAnchor;
}


bool throughAll( const BOARD_CONNECTED_ITEM* aItem )
{
    return ( aItem->GetLayerSet() & LSET::AllCuMask() ).count() > 1;
}

} // namespace


HANDLER_RESULT<RouteResponse> KICADD_HANDLER_PCB::handleRouteShort( const HANDLER_CONTEXT<RouteShort>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const RouteShort& req = aCtx.Request;

    if( req.refs().empty() && req.nets().empty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "RouteShort needs refs or nets" ) );

    PCB_LAYER_ID layer = UNDEFINED_LAYER;

    if( !req.layer().empty() )
    {
        int id = b->GetLayerID( wxString::FromUTF8( req.layer() ) );

        if( id < 0 || !IsCopperLayer( id ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no copper layer " + req.layer() ) );

        layer = ToLAYER_ID( id );
    }

    RouteResponse            out;
    std::set<const FOOTPRINT*> parts;
    std::set<int>            nets;

    for( const std::string& ref : req.refs() )
    {
        FOOTPRINT* fp = b->FindFootprintByReference( wxString::FromUTF8( ref ) );

        if( !fp )
        {
            RouteResult* r = out.add_results();
            r->set_from( ref );
            r->set_error( "no such part" );
            continue;
        }

        parts.insert( fp );

        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 )
                nets.insert( pad->GetNetCode() );
        }
    }

    std::set<int> onlyNets;

    for( const std::string& name : req.nets() )
    {
        NETINFO_ITEM* net = b->FindNet( wxString::FromUTF8( name ) );

        if( !net )
        {
            RouteResult* r = out.add_results();
            r->set_net( name );
            r->set_error( "no such net" );
            continue;
        }

        onlyNets.insert( net->GetNetCode() );
    }

    if( parts.empty() )
        nets = onlyNets;

    struct CONN
    {
        BOARD_CONNECTED_ITEM* a;
        BOARD_CONNECTED_ITEM* z;
        VECTOR2I              pa, pz;
        long long             length;
    };

    std::vector<CONN>                  conns;
    std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();

    for( int code : nets )
    {
        if( !onlyNets.empty() && !onlyNets.count( code ) )
            continue;

        RN_NET* rn = conn->GetRatsnestForNet( code );

        if( !rn )
            continue;

        for( const CN_EDGE& edge : rn->GetEdges() )
        {
            std::shared_ptr<const CN_ANCHOR> a = edge.GetSourceNode(), z = edge.GetTargetNode();

            if( !a || !z || a->Dirty() || z->Dirty() || !a->Parent() || !z->Parent() )
                continue;

            if( !parts.empty() && !parts.count( a->Parent()->GetParentFootprint() )
                && !parts.count( z->Parent()->GetParentFootprint() ) )
                continue;

            if( a->Parent()->Type() == PCB_ZONE_T || z->Parent()->Type() == PCB_ZONE_T )
                continue;

            CONN c{ a->Parent(), z->Parent(), entry( a->Parent(), a->Pos() ), entry( z->Parent(), z->Pos() ), 0 };
            c.length = ( c.pz - c.pa ).EuclideanNorm();

            if( req.max_length() > 0 && c.length > req.max_length() )
                continue;

            // start from the pad of the parts asked for, or from a pad rather than a track
            if( ( !parts.empty() && !parts.count( c.a->GetParentFootprint() ) )
                || ( c.a->Type() != PCB_PAD_T && c.z->Type() == PCB_PAD_T ) )
            {
                std::swap( c.a, c.z );
                std::swap( c.pa, c.pz );
            }

            conns.push_back( c );
        }
    }

    // shortest first: a short connection has the fewest ways round, a long one the most
    std::stable_sort( conns.begin(), conns.end(), []( const CONN& x, const CONN& y ) { return x.length < y.length; } );

    constexpr size_t MAX_ROUTES = 300;

    if( conns.size() > MAX_ROUTES )
        conns.resize( MAX_ROUTES );

    kicadd::route::SESSION session( b );
    int                    routed = 0;

    for( const CONN& c : conns )
    {
        kicadd::route::REQUEST r;
        r.fromItem = c.a;
        r.from = c.pa;
        r.toItem = c.z;
        r.to = c.pz;

        // the layer asked for; else as route chooses, and for through-hole at both ends F.Cu
        // first, then B.Cu
        auto layersFor = [&]( const BOARD_CONNECTED_ITEM* aEnd ) -> std::vector<PCB_LAYER_ID>
        {
            if( layer == UNDEFINED_LAYER && throughAll( c.a ) && throughAll( aEnd ) )
                return { F_Cu, B_Cu };

            return { layer };
        };

        kicadd::route::RESULT res;
        BOARD_CONNECTED_ITEM* zItem = c.z;
        VECTOR2I              zPos = c.pz;

        auto attempt = [&]( BOARD_CONNECTED_ITEM* aEnd, const VECTOR2I& aPos )
        {
            r.toItem = aEnd;
            r.to = aPos;

            for( PCB_LAYER_ID l : layersFor( aEnd ) )
            {
                r.layer = l;
                kicadd::route::RESULT a = session.Route( r );

                if( a.ok || res.error.empty() )
                    res = a;

                if( a.ok )
                {
                    zItem = aEnd;
                    zPos = aPos;
                    return true;
                }
            }

            return false;
        };

        // Not to the nearest copper of the other side (a track on the far layer, say): the
        // other pads and vias it is joined to, nearest first, may be reachable on another layer
        if( !attempt( c.z, c.pz ) )
        {
            for( BOARD_CONNECTED_ITEM* alt : alternatives( *conn, c.z, c.a, c.pa ) )
            {
                if( attempt( alt, alt->GetPosition() ) )
                    break;
            }
        }

        RouteResult* rr = out.add_results();
        rr->set_from( endName( c.a, c.pa ) );
        rr->set_to( endName( zItem, zPos ) );
        fillResult( *rr, res );
        routed += res.ok ? 1 : 0;
    }

    out.set_dry_run( req.dry_run() );

    if( req.dry_run() || routed == 0 )
    {
        out.set_unconnected( (int) conn->GetUnconnectedCountNoFlush( false ) );
        return out;
    }

    commitRoutes( session.TakeAdded(), session.Replaced(), "route " + std::to_string( routed ) + " connection(s)", out );
    return out;
}


HANDLER_RESULT<FanoutResponse> KICADD_HANDLER_PCB::handleFanout( const HANDLER_CONTEXT<Fanout>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const Fanout& req = aCtx.Request;

    if( req.nets().empty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "Fanout needs nets (e.g. GND)" ) );

    PCB_LAYER_ID onlyLayer = UNDEFINED_LAYER;

    if( !req.layer().empty() )
    {
        int id = b->GetLayerID( wxString::FromUTF8( req.layer() ) );

        if( id < 0 || !IsCopperLayer( id ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no copper layer " + req.layer() ) );

        onlyLayer = ToLAYER_ID( id );
    }

    FanoutResponse out;
    RouteResponse& routes = *out.mutable_routes();
    std::set<int>  nets;

    for( const std::string& name : req.nets() )
    {
        if( NETINFO_ITEM* net = b->FindNet( wxString::FromUTF8( name ) ) )
            nets.insert( net->GetNetCode() );
        else
            out.add_no_zone( name + " (no such net)" );
    }

    std::vector<FOOTPRINT*> parts;

    if( req.refs().empty() )
    {
        parts.assign( b->Footprints().begin(), b->Footprints().end() );
    }
    else
    {
        for( const std::string& ref : req.refs() )
        {
            if( FOOTPRINT* fp = b->FindFootprintByReference( wxString::FromUTF8( ref ) ) )
                parts.push_back( fp );
            else
                out.add_no_zone( ref + " (no such part)" );
        }
    }

    std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();
    kicadd::route::SESSION             session( b );
    std::map<std::string, std::vector<std::string>> noZone;   ///< "VCC" -> the pads left alone
    int                                routed = 0;

    static const VECTOR2D dirs[8] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
                                      { M_SQRT1_2, M_SQRT1_2 }, { -M_SQRT1_2, M_SQRT1_2 },
                                      { M_SQRT1_2, -M_SQRT1_2 }, { -M_SQRT1_2, -M_SQRT1_2 } };

    for( FOOTPRINT* fp : parts )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( !nets.count( pad->GetNetCode() ) )
                continue;

            if( throughAll( pad ) )
            {
                out.set_skipped_tht( out.skipped_tht() + 1 );
                continue;
            }

            PCB_LAYER_ID padLayer = LSET( pad->GetLayerSet() & LSET::AllCuMask() ).Seq().front();

            // the zones of the net on another layer (or the layer asked for)
            std::vector<std::pair<ZONE*, PCB_LAYER_ID>> zones;

            for( ZONE* z : b->Zones() )
            {
                if( z->GetIsRuleArea() || z->GetNetCode() != pad->GetNetCode() )
                    continue;

                for( PCB_LAYER_ID l : LSET( z->GetLayerSet() & LSET::AllCuMask() ).Seq() )
                {
                    if( l != padLayer && ( onlyLayer == UNDEFINED_LAYER || l == onlyLayer ) )
                        zones.emplace_back( z, l );
                }
            }

            if( zones.empty() )
            {
                noZone[pad->GetNetname().ToStdString()
                       + ( onlyLayer != UNDEFINED_LAYER ? " on " + LSET::Name( onlyLayer ).ToStdString() : "" )]
                        .push_back( endName( pad, pad->GetPosition() ) );
                continue;
            }

            // already joined to such a zone (its fill, a via, a track)
            bool done = false;

            for( BOARD_CONNECTED_ITEM* item : conn->GetConnectedItems( pad ) )
            {
                for( auto& [z, l] : zones )
                    done = done || item == z;
            }

            if( done )
            {
                out.set_skipped_done( out.skipped_done() + 1 );
                continue;
            }

            // the nearest layer first (a through via reaches all of them; the nearest is the
            // shortest way down)
            auto layerDist = [&]( PCB_LAYER_ID aLayer )
            {
                return std::abs( (int) CopperLayerToOrdinal( aLayer ) - (int) CopperLayerToOrdinal( padLayer ) );
            };

            std::stable_sort( zones.begin(), zones.end(),
                              [&]( const auto& x, const auto& y ) { return layerDist( x.second ) < layerDist( y.second ); } );

            // inside the copper of one of them (its fill; the outline while it is unfilled)
            auto zoneAt = [&]( const VECTOR2I& aPos ) -> std::optional<PCB_LAYER_ID>
            {
                for( auto& [z, l] : zones )
                {
                    bool inside = z->IsFilled() && z->HasFilledPolysForLayer( l )
                                          ? z->GetFilledPolysList( l )->Contains( aPos )
                                          : z->Outline()->Contains( aPos );

                    if( inside )
                        return l;
                }

                return std::nullopt;
            };

            PCB_LAYER_ID target = zones.front().second;

            // a via next to the pad: outwards from the part first, nearer first
            const NETCLASS* cls = pad->GetEffectiveNetClass();
            const int       viaR = ( cls && cls->HasViaDiameter() ? cls->GetViaDiameter()
                                                                    : b->GetDesignSettings().GetCurrentViaSize() ) / 2;
            const int       clr = cls && cls->HasClearance() ? cls->GetClearance() : b->GetDesignSettings().m_MinClearance;
            const BOX2I     box = pad->GetBoundingBox();
            VECTOR2D        out0 = VECTOR2D( pad->GetPosition() - fp->GetPosition() );

            std::vector<VECTOR2D> order( std::begin( dirs ), std::end( dirs ) );
            std::stable_sort( order.begin(), order.end(),
                              [&]( const VECTOR2D& x, const VECTOR2D& y ) { return x.Dot( out0 ) > y.Dot( out0 ); } );

            kicadd::route::RESULT res, first;
            bool                  found = false;

            for( int step = 0; step < 7 && !found; ++step )
            {
                for( const VECTOR2D& d : order )
                {
                    double   reach = std::abs( d.x ) * box.GetWidth() / 2.0 + std::abs( d.y ) * box.GetHeight() / 2.0;
                    double   dist = reach + viaR + clr + step * 250000.0;
                    VECTOR2I at = pad->GetPosition() + VECTOR2I( KiROUND( d.x * dist ), KiROUND( d.y * dist ) );

                    std::optional<PCB_LAYER_ID> here = zoneAt( at );

                    if( !here )
                        continue;

                    kicadd::route::REQUEST r;
                    r.fromItem = pad;
                    r.from = pad->GetPosition();
                    r.to = at;
                    r.layer = padLayer;
                    r.endViaTo = *here;
                    res = session.Route( r );

                    if( first.error.empty() && !first.ok )
                        first = res;

                    if( res.ok && res.drc.empty() )
                    {
                        found = true;
                        target = *here;
                        break;
                    }
                }
            }

            RouteResult* rr = routes.add_results();
            rr->set_from( endName( pad, pad->GetPosition() ) );
            rr->set_to( "via to " + LSET::Name( target ).ToStdString() );

            if( found )
            {
                fillResult( *rr, res );
                routed++;
            }
            else
            {
                fillResult( *rr, first );
                rr->set_net( pad->GetNetname().ToStdString() );
                rr->set_ok( false );
                rr->set_error( first.error.empty() ? "no spot near the pad inside the copper of a " + pad->GetNetname().ToStdString()
                                                             + " zone on another layer"
                                                   : "no room for a via near the pad (" + first.error + ")" );
            }
        }
    }

    for( const auto& [n, pads] : noZone )
    {
        std::string list;

        for( size_t i = 0; i < pads.size() && i < 8; ++i )
            list += ( i ? ", " : "" ) + pads[i];

        if( pads.size() > 8 )
            list += " +" + std::to_string( pads.size() - 8 ) + " more";

        out.add_no_zone( n + ": " + list );
    }

    routes.set_dry_run( req.dry_run() );

    if( req.dry_run() || routed == 0 )
    {
        routes.set_unconnected( (int) conn->GetUnconnectedCountNoFlush( false ) );
        return out;
    }

    commitRoutes( session.TakeAdded(), session.Replaced(), "fan out " + std::to_string( routed ) + " pad(s)", routes );
    return out;
}


HANDLER_RESULT<StitchViasResponse> KICADD_HANDLER_PCB::handleStitchVias( const HANDLER_CONTEXT<StitchVias>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const StitchVias& req = aCtx.Request;
    const wxString    netName = req.net().empty() ? wxString( wxS( "GND" ) ) : wxString::FromUTF8( req.net() );
    NETINFO_ITEM*     net = b->FindNet( netName );

    if( !net || net->GetNetCode() <= 0 )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no net " + netName.ToStdString() ) );

    const int              code = net->GetNetCode();
    BOARD_DESIGN_SETTINGS& bds = b->GetDesignSettings();
    const NETCLASS*        nc = net->GetNetClass();
    const int dia = nc && nc->HasViaDiameter() ? nc->GetViaDiameter() : bds.GetCurrentViaSize();
    const int drill = nc && nc->HasViaDrill() ? nc->GetViaDrill() : bds.GetCurrentViaDrill();
    const int clr = std::max( nc && nc->HasClearance() ? nc->GetClearance() : 0, bds.m_MinClearance );
    const int r = dia / 2;

    StitchViasResponse out;
    out.set_via_diameter( dia );
    out.set_via_drill( drill );
    out.set_dry_run( req.dry_run() );

    // --- the net's copper, layer by layer: its zones' fills; `inner` keeps a via's radius from their edge
    std::map<PCB_LAYER_ID, SHAPE_POLY_SET> copper, inner;
    int                                    unfilled = 0;

    for( ZONE* z : b->Zones() )
    {
        if( z->GetIsRuleArea() || z->IsTeardropArea() || z->GetNetCode() != code )
            continue;

        for( PCB_LAYER_ID l : LSET( z->GetLayerSet() & LSET::AllCuMask() ).Seq() )
        {
            if( z->IsFilled() && z->HasFilledPolysForLayer( l ) && z->GetFilledPolysList( l )->OutlineCount() )
                copper[l].Append( *z->GetFilledPolysList( l ) );
            else
                unfilled++;
        }
    }

    for( auto& [l, poly] : copper )
    {
        poly.Simplify();
        poly.BuildBBoxCaches();
        inner[l] = poly.CloneDropTriangulation();
        inner[l].Deflate( r, CORNER_STRATEGY::ROUND_ALL_CORNERS, bds.m_MaxError );
        inner[l].BuildBBoxCaches();
        out.add_layers( LSET::Name( l ).ToStdString() );
    }

    if( copper.size() < 2 )
    {
        std::string have;

        for( auto& [l, poly] : copper )
            have += LSET::Name( l ).ToStdString();

        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                "stitching vias join " + netName.ToStdString() + "'s copper on two layers or more; it has filled copper "
                + ( have.empty() ? std::string( "on no layer" ) : "only on " + have )
                + ( unfilled ? " (refill_zones first)" : " (zones create {\"net\": \"" + netName.ToStdString()
                                                                + "\"} pours F.Cu and B.Cu)" ) ) );
    }

    if( unfilled )
        out.add_notes( std::to_string( unfilled ) + " zone layer(s) of " + netName.ToStdString()
                       + " not filled: refill_zones first, or they are left out" );

    auto layersAt = [&]( const VECTOR2I& aP )
    {
        int n = 0;

        for( auto& [l, poly] : inner )
            n += poly.Contains( aP ) ? 1 : 0;

        return n;
    };

    // --- what a via may not sit on: pads (any net), tracks (any net), courtyards, rule areas
    //     with no vias. Bucketed by a 2 mm grid, each item in the cells its box touches.
    const int                                              CELL = pcbIUScale.mmToIU( 2 );
    std::unordered_map<int64_t, std::vector<BOARD_ITEM*>>  cells;
    auto key = []( int64_t cx, int64_t cy ) { return ( cx << 32 ) ^ ( cy & 0xffffffff ); };
    auto bucket = [&]( BOARD_ITEM* aItem, BOX2I aBox )
    {
        aBox.Inflate( r + clr );

        for( int64_t cx = aBox.GetX() / CELL - 1; cx <= aBox.GetRight() / CELL; ++cx )
        {
            for( int64_t cy = aBox.GetY() / CELL - 1; cy <= aBox.GetBottom() / CELL; ++cy )
                cells[key( cx, cy )].push_back( aItem );
        }
    };

    std::vector<VECTOR2I> netHoles;   // vias and through-holes of the net: a stitching via near one is not needed

    for( FOOTPRINT* fp : b->Footprints() )
    {
        if( !req.under_parts() )
            bucket( fp, fp->GetBoundingBox( false ) );

        for( PAD* pad : fp->Pads() )
        {
            bucket( pad, pad->GetBoundingBox() );

            if( pad->GetNetCode() == code && pad->HasHole() )
                netHoles.push_back( pad->GetPosition() );
        }
    }

    for( PCB_TRACK* t : b->Tracks() )
    {
        bucket( t, t->GetBoundingBox() );

        if( t->Type() == PCB_VIA_T && t->GetNetCode() == code )
            netHoles.push_back( t->GetPosition() );
    }

    for( ZONE* z : b->Zones() )
    {
        if( z->GetIsRuleArea() && z->GetDoNotAllowVias() )
            bucket( z, z->GetBoundingBox() );
    }

    auto blockedAt = [&]( const VECTOR2I& aP ) -> bool
    {
        auto it = cells.find( key( aP.x / CELL - ( aP.x < 0 ? 1 : 0 ), aP.y / CELL - ( aP.y < 0 ? 1 : 0 ) ) );

        if( it == cells.end() )
            return false;

        const SHAPE_CIRCLE via( aP, r );

        for( BOARD_ITEM* item : it->second )
        {
            switch( item->Type() )
            {
            case PCB_FOOTPRINT_T:
            {
                FOOTPRINT* fp = static_cast<FOOTPRINT*>( item );
                const SHAPE_POLY_SET& crt = fp->GetCourtyard( fp->IsFlipped() ? B_CrtYd : F_CrtYd );

                if( crt.OutlineCount() ? crt.Contains( aP ) : fp->GetBoundingBox( false ).Contains( aP ) )
                    return true;

                break;
            }

            case PCB_PAD_T:
            {
                PAD* pad = static_cast<PAD*>( item );

                for( PCB_LAYER_ID l : LSET( pad->GetLayerSet() & LSET::AllCuMask() ).Seq() )
                {
                    std::shared_ptr<SHAPE> shape = pad->GetEffectiveShape( l );

                    if( shape && shape->Collide( &via, clr ) )
                        return true;
                }

                if( std::shared_ptr<SHAPE_SEGMENT> hole = pad->GetEffectiveHoleShape() )
                {
                    if( hole->Collide( &via, std::max( clr, bds.m_HoleToHoleMin ) ) )
                        return true;
                }

                break;
            }

            case PCB_TRACE_T:
            case PCB_ARC_T:
            case PCB_VIA_T:
            {
                PCB_TRACK* t = static_cast<PCB_TRACK*>( item );
                std::shared_ptr<SHAPE> shape = t->GetEffectiveShape( t->Type() == PCB_VIA_T ? F_Cu : t->GetLayer() );

                if( shape && shape->Collide( &via, clr ) )
                    return true;

                break;
            }

            case PCB_ZONE_T:
                if( static_cast<ZONE*>( item )->Outline()->Collide( aP, r ) )
                    return true;

                break;

            default:
                break;
            }
        }

        return false;
    };

    const int64_t spacing = req.spacing() > 0 ? std::max<int64_t>( req.spacing(), dia + clr ) : pcbIUScale.mmToIU( 5 );
    const int     maxVias = req.max() > 0 ? req.max() : 500;
    const int64_t nearest = spacing / 2;

    auto nearHole = [&]( const VECTOR2I& aP, int64_t aDist )
    {
        for( const VECTOR2I& h : netHoles )
        {
            if( ( h - aP ).SquaredEuclideanNorm() < aDist * aDist )
                return true;
        }

        return false;
    };

    BOX2I area;

    if( req.has_area_min() && req.has_area_max() )
    {
        VECTOR2I a( req.area_min().x(), req.area_min().y() ), c( req.area_max().x(), req.area_max().y() );
        area = BOX2I( a, c - a );
        area.Normalize();
    }
    else
    {
        bool first = true;

        for( auto& [l, poly] : copper )
        {
            if( first )
                area = poly.BBox();
            else
                area.Merge( poly.BBox() );

            first = false;
        }
    }

    kicadd::route::SESSION session( b );
    int                    placed = 0;

    auto tryAt = [&]( const VECTOR2I& aP ) -> bool
    {
        if( blockedAt( aP ) )
            return false;

        kicadd::route::RESULT res = session.PlaceVia( aP, code, dia, drill );

        if( !res.ok )
            return false;

        netHoles.push_back( aP );
        placed++;
        return true;
    };

    // --- the grid (absolute: multiples of the spacing)
    for( int64_t y = ( area.GetY() + spacing - 1 ) / spacing * spacing; y <= area.GetBottom() && placed < maxVias; y += spacing )
    {
        for( int64_t x = ( area.GetX() + spacing - 1 ) / spacing * spacing; x <= area.GetRight() && placed < maxVias; x += spacing )
        {
            VECTOR2I p( (int) x, (int) y );
            out.set_grid_points( out.grid_points() + 1 );

            if( layersAt( p ) < 2 )
                continue;

            out.set_in_copper( out.in_copper() + 1 );

            if( nearHole( p, nearest ) )
                out.set_near_existing( out.near_existing() + 1 );
            else if( !tryAt( p ) )
                out.set_blocked( out.blocked() + 1 );
        }
    }

    // --- islands: copper of the net with no via or through-hole of it in it
    if( !req.no_islands() )
    {
        const double minArea = std::pow( pcbIUScale.mmToIU( 2 ), 2 );   // 4 mm²: room for a via
        std::vector<std::string> left;

        for( auto& [l, poly] : copper )
        {
            for( int i = 0; i < poly.OutlineCount() && placed < maxVias; ++i )
            {
                if( area.GetWidth() > 0 && !area.Intersects( poly.COutline( i ).BBox() ) )
                    continue;

                bool has = false;

                for( const VECTOR2I& h : netHoles )
                {
                    if( poly.Contains( h, i ) )
                    {
                        has = true;
                        break;
                    }
                }

                if( has )
                    continue;

                SHAPE_POLY_SET island;
                island.AddPolygon( poly.CPolygon( i ) );
                const double a = island.Area();

                if( a < minArea )
                    continue;

                // the free points of the island, nearest its middle first
                const BOX2I    box = island.BBox();
                const int      step = std::max( dia, (int) pcbIUScale.mmToIU( 0.5 ) );
                const VECTOR2I mid = box.Centre();
                std::vector<VECTOR2I> pts;

                for( int y = box.GetY() + step / 2; y < box.GetBottom(); y += step )
                {
                    for( int x = box.GetX() + step / 2; x < box.GetRight(); x += step )
                    {
                        VECTOR2I p( x, y );

                        if( inner[l].Contains( p ) && poly.Contains( p, i ) )
                            pts.push_back( p );
                    }
                }

                std::sort( pts.begin(), pts.end(), [&]( const VECTOR2I& p, const VECTOR2I& q )
                           { return ( p - mid ).SquaredEuclideanNorm() < ( q - mid ).SquaredEuclideanNorm(); } );

                bool joined = false;

                for( size_t k = 0; k < pts.size() && k < 400 && !joined; ++k )
                {
                    if( layersAt( pts[k] ) >= 2 && tryAt( pts[k] ) )
                        joined = true;
                }

                if( joined )
                {
                    out.set_islands_joined( out.islands_joined() + 1 );
                }
                else
                {
                    char buf[160];
                    std::snprintf( buf, sizeof( buf ), "%s @%.2f,%.2f (%.1f mm²)", LSET::Name( l ).ToStdString().c_str(),
                                   pcbIUScale.IUTomm( mid.x ), pcbIUScale.IUTomm( mid.y ), a / 1e12 );
                    left.push_back( buf );
                }
            }
        }

        for( size_t k = 0; k < left.size() && k < 10; ++k )
            out.add_islands_left( left[k] );

        if( left.size() > 10 )
            out.add_islands_left( "+" + std::to_string( left.size() - 10 ) + " more" );
    }

    if( placed >= maxVias )
        out.add_notes( "stopped at max=" + std::to_string( maxVias ) + " vias" );

    out.set_vias_added( placed );
    std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();

    if( req.dry_run() || placed == 0 )
    {
        out.set_unconnected( (int) conn->GetUnconnectedCountNoFlush( false ) );
        return out;
    }

    RouteResponse routes;
    commitRoutes( session.TakeAdded(), {}, "stitch " + std::to_string( placed ) + " " + netName.ToStdString() + " via(s)",
                  routes );
    out.set_history_label( routes.history_label() );
    out.set_unconnected( routes.unconnected() );
    return out;
}


HANDLER_RESULT<TeardropsResponse> KICADD_HANDLER_PCB::handleTeardrops( const HANDLER_CONTEXT<Teardrops>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const Teardrops&  req = aCtx.Request;
    const bool        on = !req.remove();
    TeardropsResponse out;
    std::set<int>     nets;
    std::set<const FOOTPRINT*> parts;

    for( const std::string& n : req.nets() )
    {
        NETINFO_ITEM* net = b->FindNet( wxString::FromUTF8( n ) );

        if( net && net->GetNetCode() > 0 )
            nets.insert( net->GetNetCode() );
        else
            out.add_missing( "net " + n );
    }

    for( const std::string& ref : req.refs() )
    {
        if( FOOTPRINT* fp = b->FindFootprintByReference( wxString::FromUTF8( ref ) ) )
            parts.insert( fp );
        else
            out.add_missing( ref );
    }

    if( ( !req.nets().empty() && nets.empty() ) || ( !req.refs().empty() && parts.empty() ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "not found: none of the nets / parts given" ) );

    std::vector<BOARD_CONNECTED_ITEM*> todo;
    int                                already = 0;

    auto consider = [&]( BOARD_CONNECTED_ITEM* aItem )
    {
        if( aItem->GetNetCode() <= 0 || ( !nets.empty() && !nets.count( aItem->GetNetCode() ) ) )
            return;

        if( aItem->GetTeardropParams().m_Enabled == on )
            already++;
        else
            todo.push_back( aItem );
    };

    for( FOOTPRINT* fp : b->Footprints() )
    {
        if( !parts.empty() && !parts.count( fp ) )
            continue;

        for( PAD* pad : fp->Pads() )
        {
            const bool pth = pad->GetAttribute() == PAD_ATTRIB::PTH;
            const bool smd = pad->GetAttribute() == PAD_ATTRIB::SMD;

            if( ( pth && !req.no_pth() ) || ( smd && req.smd() ) )
                consider( pad );
        }
    }

    if( !req.no_vias() && ( parts.empty() || !nets.empty() ) )
    {
        for( PCB_TRACK* t : b->Tracks() )
        {
            if( t->Type() == PCB_VIA_T )
                consider( t );
        }
    }

    auto count = [&]()
    {
        int n = 0;

        for( ZONE* z : b->Zones() )
            n += z->IsTeardropArea() ? 1 : 0;

        return n;
    };

    out.set_before( count() );
    out.set_already( already );
    out.set_changed( (int) todo.size() );
    out.set_dry_run( req.dry_run() );

    if( req.dry_run() || todo.empty() )
    {
        out.set_after( out.before() );
        return out;
    }

    HISTORY_ENTRY entry;
    entry.label = std::string( on ? "teardrops on " : "teardrops off " ) + std::to_string( todo.size() ) + " pad(s) / via(s)";
    MoveResponse moved;

    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                for( BOARD_CONNECTED_ITEM* item : todo )
                {
                    aCommit.Modify( item );
                    item->SetTeardropsEnabled( on );
                    entry.teardropFlags.push_back( { item->m_Uuid, !on, on } );
                }

                return std::vector<KIID>();
            },
            wxString::FromUTF8( "Agent: " + entry.label ), moved );

    out.set_after( count() );
    out.set_history_label( entry.label );
    record( std::move( entry ) );
    return out;
}


void KICADD_HANDLER_PCB::commitRoutes( std::vector<std::unique_ptr<PCB_TRACK>>&& aAdded,
                                       const std::vector<PCB_TRACK*>& aReplaced, const std::string& aLabel,
                                       RouteResponse& aOut )
{
    BOARD* b = board();

    if( aAdded.empty() && aReplaced.empty() )
        return;

    HISTORY_ENTRY entry;
    entry.label = aLabel;
    MoveResponse moved;
    int          tracks = 0, vias = 0;

    for( const std::unique_ptr<PCB_TRACK>& t : aAdded )
        ( t->Type() == PCB_VIA_T ? vias : tracks )++;

    std::vector<PCB_TRACK*> onBoard;

    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                for( PCB_TRACK* t : aReplaced )
                {
                    entry.tracksRemoved.push_back( { nullptr, t->GetNetCode() > 0 ? t->GetNetname() : wxString() } );
                    aCommit.Remove( t );
                }

                for( std::unique_ptr<PCB_TRACK>& t : aAdded )
                {
                    PCB_TRACK* raw = t.release();
                    onBoard.push_back( raw );
                    aCommit.Add( raw );
                }

                return std::vector<KIID>();
            },
            wxString::FromUTF8( "Agent: " + aLabel ), moved );

    // the history's own copies: undo removes the board's by id, redo adds copies back
    for( PCB_TRACK* t : onBoard )
    {
        PCB_TRACK* copy = static_cast<PCB_TRACK*>( t->Clone() );
        copy->SetNet( NETINFO_LIST::OrphanedItem() );
        copy->SetParent( nullptr );
        entry.tracksAdded.push_back( { std::shared_ptr<PCB_TRACK>( copy ), t->GetNetCode() > 0 ? t->GetNetname() : wxString() } );
    }

    for( size_t i = 0; i < aReplaced.size(); ++i )
    {
        PCB_TRACK* t = editorOwnsUndo() ? static_cast<PCB_TRACK*>( aReplaced[i]->Clone() ) : aReplaced[i];
        t->SetNet( NETINFO_LIST::OrphanedItem() );
        t->SetParent( nullptr );
        entry.tracksRemoved[i].track.reset( t );
    }

    aOut.set_unconnected( moved.unconnected() );
    aOut.set_tracks_added( tracks );
    aOut.set_vias_added( vias );
    aOut.set_history_label( entry.label );
    record( std::move( entry ) );
}


HANDLER_RESULT<MoveResponse>
KICADD_HANDLER_PCB::handleAddFootprint( const HANDLER_CONTEXT<AddFootprint>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const AddFootprint&        req = aCtx.Request;
    std::unique_ptr<FOOTPRINT> fp;
    std::string                source;
    const FOOTPRINT*           like = nullptr;

    if( !req.lcsc().empty() )
    {
        kicadd::lcsc::PART  part;
        kicadd::lcsc::SAVED saved;
        std::string         err;

        if( !kicadd::lcsc::FetchPart( req.lcsc(), false, part, err ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

        source = "LCSC " + part.lcsc + " (" + part.title + "; JLCEDA/EasyEDA Official Library";

        if( kicadd::lcsc::SaveToProjectLibrary( b, part, saved, err ) )
        {
            source += ", saved as " + saved.libId + ( saved.registered ? ", LCSC library added to fp-lib-table" : "" );

            // the symbol too, so the schematic side finds the part later (best effort)
            kicadd::lcsc::SAVED_SYMBOL sym;

            if( kicadd::lcsc::SaveSymbolToProjectLibrary( b, m_context->GetKiway(), part, sym, err ) )
                source += ", symbol " + sym.libId;
        }
        else
            source += ", not saved: " + err;

        source += ")";

        if( !part.note.empty() )
            source += "; " + part.note;

        fp.reset( static_cast<FOOTPRINT*>( part.footprint->Duplicate( IGNORE_PARENT_GROUP ) ) );
        fp->SetFPID( part.footprint->GetFPID() );
        BOARD_DESIGN_SETTINGS& bds = b->GetDesignSettings();
        fp->ApplyDefaultSettings( *b, bds.m_StyleFPFields, bds.m_StyleFPText, bds.m_StyleFPShapes,
                                  bds.m_StyleFPDimensions, bds.m_StyleFPBarcodes );
    }
    else if( !req.like().empty() )
    {
        like = b->FindFootprintByReference( wxString::FromUTF8( req.like() ) );

        if( !like )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no footprint " + req.like() ) );

        fp = copyAsNew( like );
        source = "copy of " + req.like() + " (" + like->GetFPID().Format().wx_str().ToStdString() + ")";
    }
    else if( !req.lib_id().empty() )
    {
        LIB_ID libId;

        if( libId.Parse( wxString::FromUTF8( req.lib_id() ) ) >= 0 || libId.GetLibNickname().empty()
            || libId.GetLibItemName().empty() )
        {
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "lib_id must be Nickname:Name, got " + req.lib_id() ) );
        }

        if( FOOTPRINT* loaded = loadFromTable( b, libId ) )
        {
            fp.reset( loaded );
            source = "library table";
        }
        else if( ( fp = loadFromDirectories( b, libId, source ) ) )
        {
            fp->ClearAllNets();
            BOARD_DESIGN_SETTINGS& bds = b->GetDesignSettings();
            fp->ApplyDefaultSettings( *b, bds.m_StyleFPFields, bds.m_StyleFPText, bds.m_StyleFPShapes,
                                      bds.m_StyleFPDimensions, bds.m_StyleFPBarcodes );
        }
        else
        {
            for( const FOOTPRINT* candidate : b->Footprints() )
            {
                if( candidate->GetFPID() == libId )
                {
                    fp = copyAsNew( candidate );
                    source = "copy of " + candidate->GetReference().ToStdString() + " (no library has it)";
                    break;
                }
            }
        }

        if( !fp )
        {
            std::string where = "the library table";

            for( const wxString& dir : footprintDirectories() )
                where += ", " + dir.ToStdString();

            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "footprint " + req.lib_id() + " not found (searched " + where
                                                      + " and the board)" ) );
        }

        fp->SetFPID( libId );
    }
    else
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "AddFootprint needs lib_id, lcsc or like" ) );
    }

    // Reference designator.
    wxString ref = wxString::FromUTF8( req.ref() );

    if( ref.IsEmpty() )
    {
        if( !like )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "AddFootprint needs ref (e.g. \"R?\" for the next free R)" ) );

        ref = referencePrefix( like->GetReference() ) + wxS( "?" );
    }

    if( ref.EndsWith( wxS( "?" ) ) )
        ref = nextReference( b, ref.Left( ref.length() - 1 ) );

    if( b->FindFootprintByReference( ref ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, ref.ToStdString() + " already exists" ) );

    // Pad numbers must exist; report the valid ones otherwise.
    std::set<wxString> numbers;

    for( const PAD* pad : fp->Pads() )
        numbers.insert( pad->GetNumber() );

    for( const auto& [number, net] : req.nets() )
    {
        if( !numbers.count( wxString::FromUTF8( number ) ) )
        {
            std::string valid;

            for( const wxString& n : numbers )
                valid += ( valid.empty() ? "" : "," ) + n.ToStdString();

            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "no pad " + number + " on " + req.lib_id() + req.like() + req.lcsc()
                                                      + " (pads: " + valid + ")" ) );
        }
    }

    fp->SetReference( ref );

    if( !req.value().empty() )
        fp->SetValue( wxString::FromUTF8( req.value() ) );

    fp->SetParent( b );
    fp->SetPosition( VECTOR2I( req.pos().x(), req.pos().y() ) );

    if( req.back() )
        fp->Flip( fp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

    fp->SetOrientation( EDA_ANGLE( req.rot_decideg() / 10.0, DEGREES_T ) );

    MoveResponse out;
    out.set_source( source );
    FOOTPRINT* placed = fp.release();
    const std::string label = "add " + ref.ToStdString() + " (" + placed->GetFPID().Format().wx_str().ToStdString() + ")";

    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                for( PAD* pad : placed->Pads() )
                {
                    auto it = req.nets().find( pad->GetNumber().ToStdString() );
                    pad->SetNet( netFor( it != req.nets().end() ? wxString::FromUTF8( it->second ) : wxString(),
                                         aCommit, out ) );
                }

                aCommit.Add( placed );
                out.add_added( ref.ToStdString() );
                return std::vector<KIID>{ placed->m_Uuid };
            },
            wxString::FromUTF8( "Agent: " + label ), out );

    m_pendingNets.clear();

    // Redo puts this exact footprint back; keep a detached copy of it.
    HISTORY_ENTRY entry;
    entry.label = label;
    entry.added.push_back( detach( static_cast<FOOTPRINT*>( placed->Clone() ) ) );

    // the nets it made: undo takes out those nothing is on any more (as for sync_pcb)
    for( const std::string& n : out.created_nets() )
        entry.netsCreated.push_back( wxString::FromUTF8( n ) );

    out.set_history_label( entry.label );
    record( std::move( entry ) );
    return out;
}


HANDLER_RESULT<FetchLcscResponse>
KICADD_HANDLER_PCB::handleFetchLcsc( const HANDLER_CONTEXT<FetchLcsc>& aCtx )
{
    kicadd::lcsc::PART part;
    std::string        err;

    if( !kicadd::lcsc::FetchPart( aCtx.Request.lcsc(), aCtx.Request.refresh(), part, err ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    FetchLcscResponse out;
    out.set_lcsc( part.lcsc );
    out.set_title( part.title );
    out.set_package( part.package );
    out.set_description( part.description );
    out.set_datasheet( part.datasheet );
    out.set_from_cache( part.fromCache );
    out.set_attribution( kicadd::lcsc::ATTRIBUTION );

    LcscStock* st = out.mutable_stock();
    st->set_stock( part.stock );
    st->set_price_usd( part.priceUsd );
    st->set_min_qty( part.minQty );
    st->set_url( part.url );
    st->set_jlc_smt( part.jlcSmt );
    st->set_jlc_class( part.jlcClass );
    st->set_as_of( part.asOf );

    std::string note = part.note;

    if( BOARD* b = board() )
    {
        kicadd::lcsc::SAVED saved;

        if( kicadd::lcsc::SaveToProjectLibrary( b, part, saved, err ) )
        {
            out.set_lib_id( saved.libId );
            out.set_file( saved.file );
            out.set_registered( saved.registered );

            if( !saved.note.empty() )
                note += ( note.empty() ? "" : "; " ) + saved.note;

            // M4b: the schematic symbol, into the project's lcsc.kicad_sym
            kicadd::lcsc::SAVED_SYMBOL sym;

            if( kicadd::lcsc::SaveSymbolToProjectLibrary( b, m_context->GetKiway(), part, sym, err ) )
            {
                LcscSymbol* s = out.mutable_symbol();
                s->set_lib_id( sym.libId );
                s->set_file( sym.file );
                s->set_reference( sym.reference );
                s->set_units( sym.units );
                s->set_registered( sym.registered );

                std::set<std::string> pads;

                for( const PAD* pad : part.footprint->Pads() )
                {
                    if( !pad->GetNumber().IsEmpty() )
                        pads.insert( pad->GetNumber().ToStdString() );
                }

                std::set<std::string> pins( sym.pins.begin(), sym.pins.end() );

                for( const std::string& p : sym.pins )
                {
                    s->add_pins( p );

                    if( !pads.count( p ) )
                        s->add_pins_without_pad( p );
                }

                for( const std::string& p : pads )
                {
                    if( !pins.count( p ) )
                        s->add_pads_without_pin( p );
                }

                if( !sym.note.empty() )
                    note += ( note.empty() ? "" : "; " ) + sym.note;
            }
            else
            {
                note += ( note.empty() ? "" : "; " ) + std::string( "symbol not saved: " ) + err;
            }
        }
        else
        {
            note += ( note.empty() ? "" : "; " ) + std::string( "not saved: " ) + err;
        }
    }

    out.set_note( note );

    for( const BOARD_ITEM* item : part.footprint->GraphicalItems() )
    {
        if( item->GetLayer() == F_CrtYd )
        {
            BOX2I box = item->GetBoundingBox();
            out.mutable_courtyard()->set_x( box.GetWidth() );
            out.mutable_courtyard()->set_y( box.GetHeight() );
            break;
        }
    }

    for( const PAD* pad : part.footprint->Pads() )
    {
        LcscPad* p = out.add_pads();
        p->set_number( pad->GetNumber().ToStdString() );
        p->mutable_pos()->set_x( pad->GetPosition().x );
        p->mutable_pos()->set_y( pad->GetPosition().y );
        BOX2I box = pad->GetBoundingBox();
        p->mutable_size()->set_x( box.GetWidth() );
        p->mutable_size()->set_y( box.GetHeight() );
        p->set_tht( pad->GetAttribute() == PAD_ATTRIB::PTH || pad->GetAttribute() == PAD_ATTRIB::NPTH );
    }

    return out;
}


void KICADD_HANDLER_PCB::noteStaleFills( const HISTORY_ENTRY& aEntry, MoveResponse& aOut ) const
{
    // An edit of zones, the outline or the layers: the fills there are were made for the other state
    bool touches = aEntry.setupBefore || !aEntry.zonesBefore.empty() || !aEntry.teardropFlags.empty()
                   || aEntry.label.rfind( "stitch", 0 ) == 0;

    for( const std::vector<HISTORY_ENTRY::ITEM_SNAPSHOT>* list : { &aEntry.itemsRemoved, &aEntry.itemsAdded } )
    {
        for( const HISTORY_ENTRY::ITEM_SNAPSHOT& snap : *list )
            touches = touches || snap.item->Type() == PCB_ZONE_T || snap.item->GetLayer() == Edge_Cuts;
    }

    if( !touches )
        return;

    for( const ZONE* zone : board()->Zones() )
    {
        if( !zone->GetIsRuleArea() && zone->IsFilled() )
        {
            aOut.add_notes( "the zone fills are out of date after this: refill_zones (until then unconnected counts "
                            "what the old fills leave open)" );
            return;
        }
    }
}


bool KICADD_HANDLER_PCB::applyHistory( const HISTORY_ENTRY& aEntry, bool aUndo, MoveResponse& aOut )
{
    BOARD* b = board();

    if( aEntry.rulesBefore && aEntry.rulesAfter )
    {
        const kicadd::rules::STATE& expect = aUndo ? *aEntry.rulesAfter : *aEntry.rulesBefore;
        const kicadd::rules::STATE& target = aUndo ? *aEntry.rulesBefore : *aEntry.rulesAfter;
        kicadd::rules::STATE        now = kicadd::rules::Capture( *b );

        if( now == target )
        {
            aOut.set_already_there( 1 );
            return false;
        }

        if( !( now == expect ) )
        {
            aOut.add_missing( "design rules (changed since; left as they are)" );
            return false;
        }

        kicadd::rules::Apply( *b, target );
        rulesChanged();
        return true;
    }

    if( aEntry.setupBefore && aEntry.setupAfter )
    {
        const kicadd::setup::STATE& expect = aUndo ? *aEntry.setupAfter : *aEntry.setupBefore;
        const kicadd::setup::STATE& target = aUndo ? *aEntry.setupBefore : *aEntry.setupAfter;
        kicadd::setup::STATE        now = kicadd::setup::Capture( *b );

        if( now == target )
        {
            aOut.set_already_there( 1 );
            return false;
        }

        if( !( now == expect ) )
        {
            aOut.add_missing( "layers / stackup (changed since; left as they are)" );
            return false;
        }

        std::string err;

        if( !kicadd::setup::Changed( *b, now, { target.copperLayers }, err ) )
        {
            aOut.add_missing( "layers / stackup (" + err + ")" );
            return false;
        }

        kicadd::setup::Apply( *b, target );
        setupChanged();
        return true;
    }

    if( !aEntry.teardropFlags.empty() )
    {
        std::vector<std::pair<BOARD_CONNECTED_ITEM*, bool>> todo;
        int already = 0, changed = 0;

        for( const HISTORY_ENTRY::TEARDROP_FLAG& f : aEntry.teardropFlags )
        {
            BOARD_ITEM* item = b->ResolveItem( f.id, true );
            auto*       ci = item && ( item->Type() == PCB_PAD_T || item->Type() == PCB_VIA_T )
                                     ? static_cast<BOARD_CONNECTED_ITEM*>( item ) : nullptr;
            const bool  target = aUndo ? f.before : f.after;

            if( !ci )
                continue;    // the part or via is gone: nothing to switch
            else if( ci->GetTeardropParams().m_Enabled == target )
                already++;
            else
                todo.emplace_back( ci, target );
        }

        aOut.set_already_there( already );

        if( todo.empty() )
            return false;

        runCommit(
                [&]( BOARD_COMMIT& aCommit )
                {
                    for( auto& [ci, on] : todo )
                    {
                        aCommit.Modify( ci );
                        ci->SetTeardropsEnabled( on );
                        changed++;
                    }

                    return std::vector<KIID>();
                },
                wxString::FromUTF8( std::string( "Agent: " ) + ( aUndo ? "undo " : "redo " ) + aEntry.label ), aOut );
        return changed > 0;
    }

    if( !aEntry.zonesBefore.empty() )
    {
        const std::vector<ZONE_STATE>& expect = aUndo ? aEntry.zonesAfter : aEntry.zonesBefore;
        const std::vector<ZONE_STATE>& target = aUndo ? aEntry.zonesBefore : aEntry.zonesAfter;
        std::vector<std::pair<ZONE*, const ZONE_STATE*>> todo;
        int already = 0;

        for( size_t i = 0; i < target.size(); ++i )
        {
            BOARD_ITEM* item = b->ResolveItem( target[i].id, true );
            ZONE*       zone = item && item->Type() == PCB_ZONE_T ? static_cast<ZONE*>( item ) : nullptr;

            if( !zone )
                aOut.add_missing( "zone " + target[i].id.AsStdString() + " (gone)" );
            else if( zoneStateOf( zone ) == target[i] )
                already++;
            else if( zoneStateOf( zone ) == expect[i] )
                todo.emplace_back( zone, &target[i] );
            else
                aOut.add_missing( "zone " + zone->GetNetname().ToStdString() + " (changed since; left as is)" );
        }

        aOut.set_already_there( already );

        if( todo.empty() )
            return false;

        runCommit(
                [&]( BOARD_COMMIT& aCommit )
                {
                    for( auto& [zone, st] : todo )
                    {
                        aCommit.Modify( zone );
                        setZoneState( zone, *st );
                    }

                    return std::vector<KIID>();
                },
                wxString::FromUTF8( std::string( "Agent: " ) + ( aUndo ? "undo " : "redo " ) + aEntry.label ), aOut );
        return true;
    }

    const std::vector<FP_SNAPSHOT>& toRemove = aUndo ? aEntry.added : aEntry.removed;
    const std::vector<FP_SNAPSHOT>& toAdd = aUndo ? aEntry.removed : aEntry.added;
    const std::vector<FP_STATE>&    states = aUndo ? aEntry.before : aEntry.after;

    auto resolve = [&]( const KIID& aId ) -> FOOTPRINT*
    {
        BOARD_ITEM* item = b->ResolveItem( aId, true );
        return item && item->Type() == PCB_FOOTPRINT_T ? static_cast<FOOTPRINT*>( item ) : nullptr;
    };

    std::vector<FOOTPRINT*>                                removing;
    std::vector<std::pair<FOOTPRINT*, const FP_STATE*>>    modifying;
    std::vector<const FP_SNAPSHOT*>                        adding;

    // A footprint to remove that is gone, or one to put back that is there, was already taken
    // care of (the editor's undo / redo of the agent's commit).
    int alreadyDone = 0;

    // Tracks deleted by the entry: undo puts them back, redo takes them out again.
    std::vector<const TRACK_SNAPSHOT*> trackAdd;
    std::vector<PCB_TRACK*>            trackRemove;

    // ... and tracks it added (route): undo takes them out, redo puts them back.
    for( const std::vector<TRACK_SNAPSHOT>* list : { &aEntry.tracksRemoved, &aEntry.tracksAdded } )
    {
        const bool putBack = ( list == &aEntry.tracksRemoved ) == aUndo;

        for( const TRACK_SNAPSHOT& snap : *list )
        {
            BOARD_ITEM* item = b->ResolveItem( snap.track->m_Uuid, true );
            PCB_TRACK*  onBoard = item && ( item->Type() == PCB_TRACE_T || item->Type() == PCB_ARC_T
                                            || item->Type() == PCB_VIA_T )
                                          ? static_cast<PCB_TRACK*>( item ) : nullptr;

            if( putBack && !onBoard )
                trackAdd.push_back( &snap );
            else if( !putBack && onBoard )
                trackRemove.push_back( onBoard );
            else
                ++alreadyDone;
        }
    }

    // Board graphics and zones it added (taken out by undo) / removed (put back)
    std::vector<const HISTORY_ENTRY::ITEM_SNAPSHOT*> itemAdd;
    std::vector<BOARD_ITEM*>                         itemRemove;

    for( const std::vector<HISTORY_ENTRY::ITEM_SNAPSHOT>* list : { &aEntry.itemsRemoved, &aEntry.itemsAdded } )
    {
        const bool putBack = ( list == &aEntry.itemsRemoved ) == aUndo;

        for( const HISTORY_ENTRY::ITEM_SNAPSHOT& snap : *list )
        {
            BOARD_ITEM* onBoard = b->ResolveItem( snap.item->m_Uuid, true );

            if( putBack && !onBoard )
                itemAdd.push_back( &snap );
            else if( !putBack && onBoard )
                itemRemove.push_back( onBoard );
            else
                ++alreadyDone;
        }
    }

    for( const FP_SNAPSHOT& snap : toRemove )
    {
        if( FOOTPRINT* fp = resolve( snap.fp->m_Uuid ) )
            removing.push_back( fp );
        else
            ++alreadyDone;
    }

    for( const FP_SNAPSHOT& snap : toAdd )
    {
        if( resolve( snap.fp->m_Uuid ) )
            ++alreadyDone;
        else
            adding.push_back( &snap );
    }

    // A footprint someone else changed after this entry (the user in the editor or the web
    // view) is left alone: undo/redo only takes back the agent's own edit.
    const std::vector<FP_STATE>& expected = aUndo ? aEntry.after : aEntry.before;

    auto unchanged = [&]( const FOOTPRINT* aFp )
    {
        for( const FP_STATE& e : expected )
        {
            if( e.id == aFp->m_Uuid )
            {
                return stateOf( aFp, e.withFields ) == e;
            }
        }

        return true;
    };

    // Already where this entry would put it: someone undid / redid it elsewhere (the editor's
    // Ctrl+Z on the agent's commit). Nothing to do for it, and not "changed by someone else".
    auto atTarget = [&]( const FOOTPRINT* aFp, const FP_STATE& aSt )
    {
        return stateOf( aFp, aSt.withFields ) == aSt;
    };

    // sync_pcb: footprints it changed are swapped back / forth whole; tracks and zones it moved
    // to another net go back to the old one
    std::vector<std::pair<FOOTPRINT*, const FP_SNAPSHOT*>> swapping;
    std::vector<std::pair<BOARD_CONNECTED_ITEM*, wxString>> renetting;

    for( const auto& [was, is] : aEntry.replaced )
    {
        const FP_SNAPSHOT& target = aUndo ? was : is;
        const FP_SNAPSHOT& expect = aUndo ? is : was;

        if( FOOTPRINT* fp = resolve( target.fp->m_Uuid ) )
        {
            if( matches( fp, target ) )
                ++alreadyDone;
            else if( matches( fp, expect ) )
                swapping.emplace_back( fp, &target );
            else
                aOut.add_missing( fp->GetReference().ToStdString() + " (changed since; left as is)" );
        }
        else
        {
            aOut.add_missing( target.fp->GetReference().ToStdString() + " (gone)" );
        }
    }

    for( const HISTORY_ENTRY::NET_CHANGE& nc : aEntry.netChanges )
    {
        BOARD_ITEM*           item = b->ResolveItem( nc.id, true );
        BOARD_CONNECTED_ITEM* ci = item && item->IsConnected() ? static_cast<BOARD_CONNECTED_ITEM*>( item ) : nullptr;
        const wxString&       target = aUndo ? nc.before : nc.after;
        const wxString&       expect = aUndo ? nc.after : nc.before;

        if( !ci )
            continue;   // deleted since: nothing to put on a net

        wxString now = ci->GetNetCode() > 0 ? ci->GetNetname() : wxString();

        if( now == target )
            ++alreadyDone;
        else if( now == expect )
            renetting.emplace_back( ci, target );
    }

    int alreadyThere = alreadyDone;

    for( const FP_STATE& st : states )
    {
        if( FOOTPRINT* fp = resolve( st.id ) )
        {
            if( atTarget( fp, st ) )
                ++alreadyThere;
            else if( unchanged( fp ) )
                modifying.emplace_back( fp, &st );
            else
                aOut.add_missing( fp->GetReference().ToStdString() + " (changed since; left as is)" );
        }
        else
        {
            aOut.add_missing( st.id.AsStdString() );
        }
    }

    aOut.set_already_there( alreadyThere );

    // Undoing a sync: the nets it made that nothing will be on any more
    std::vector<NETINFO_ITEM*> netsGone;

    if( aUndo && !aEntry.netsCreated.empty() )
    {
        std::set<const BOARD_ITEM*> leaving( removing.begin(), removing.end() );
        std::set<wxString>          used;

        for( const PCB_TRACK* t : trackRemove )
            leaving.insert( t );

        for( const auto& [fp, snap] : swapping )
            leaving.insert( fp );

        std::map<const BOARD_ITEM*, wxString> renamed;

        for( const auto& [item, net] : renetting )
            renamed[item] = net;

        auto use = [&]( const BOARD_CONNECTED_ITEM* aItem )
        {
            if( leaving.count( aItem ) || leaving.count( aItem->GetParentFootprint() ) )
                return;

            auto it = renamed.find( aItem );
            used.insert( it != renamed.end() ? it->second : aItem->GetNetname() );
        };

        for( const FOOTPRINT* fp : b->Footprints() )
        {
            for( const PAD* pad : fp->Pads() )
                use( pad );
        }

        for( const PCB_TRACK* t : b->Tracks() )
            use( t );

        for( const ZONE* z : b->Zones() )
            use( z );

        for( const FP_SNAPSHOT* snap : adding )
            used.insert( snap->padNets.begin(), snap->padNets.end() );

        for( const auto& [fp, snap] : swapping )
            used.insert( snap->padNets.begin(), snap->padNets.end() );

        for( const TRACK_SNAPSHOT* snap : trackAdd )
            used.insert( snap->net );

        for( const wxString& name : aEntry.netsCreated )
        {
            if( NETINFO_ITEM* net = b->FindNet( name ); net && !used.count( name ) )
                netsGone.push_back( net );
        }
    }

    if( removing.empty() && adding.empty() && modifying.empty() && trackAdd.empty() && trackRemove.empty()
        && swapping.empty() && renetting.empty() && netsGone.empty() && itemAdd.empty() && itemRemove.empty() )
        return false;

    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                std::vector<KIID> ids;

                for( FOOTPRINT* fp : removing )
                {
                    aOut.add_removed( fp->GetReference().ToStdString() );
                    aCommit.Remove( fp );
                }

                for( const FP_SNAPSHOT* snap : adding )
                {
                    FOOTPRINT* fp = instantiate( *snap, aCommit, aOut );
                    aCommit.Add( fp );
                    aOut.add_added( fp->GetReference().ToStdString() );
                    ids.push_back( fp->m_Uuid );
                }

                for( auto& [fp, st] : modifying )
                {
                    aCommit.Modify( fp );
                    setState( fp, *st, view() );
                    ids.push_back( fp->m_Uuid );
                }

                for( const TRACK_SNAPSHOT* snap : trackAdd )
                {
                    PCB_TRACK* t = static_cast<PCB_TRACK*>( snap->track->Clone() );   // same KIID
                    t->SetParent( b );
                    t->SetNet( netFor( snap->net, aCommit, aOut ) );
                    aCommit.Add( t );
                }

                for( PCB_TRACK* t : trackRemove )
                    aCommit.Remove( t );

                for( auto& [fp, snap] : swapping )
                {
                    aCommit.Remove( fp );
                    FOOTPRINT* back = instantiate( *snap, aCommit, aOut );
                    aCommit.Add( back );
                    ids.push_back( back->m_Uuid );
                }

                for( auto& [item, net] : renetting )
                {
                    aCommit.Modify( item );
                    item->SetNet( netFor( net, aCommit, aOut ) );
                }

                for( NETINFO_ITEM* net : netsGone )
                    aCommit.Remove( net );

                for( const HISTORY_ENTRY::ITEM_SNAPSHOT* snap : itemAdd )
                {
                    BOARD_ITEM* item = static_cast<BOARD_ITEM*>( snap->item->Clone() );   // same KIID
                    item->SetParent( b );

                    if( item->IsConnected() )
                        static_cast<BOARD_CONNECTED_ITEM*>( item )->SetNet( netFor( snap->net, aCommit, aOut ) );

                    aCommit.Add( item );
                }

                for( BOARD_ITEM* item : itemRemove )
                    aCommit.Remove( item );

                return ids;
            },
            wxString::FromUTF8( std::string( "Agent: " ) + ( aUndo ? "undo " : "redo " ) + aEntry.label ),
            aOut );

    m_pendingNets.clear();

    aOut.set_changed( aOut.changed() + (int) swapping.size() );

    // The snapshots stay in the history (undo/redo can repeat). Headless, the board copies we
    // just removed are referenced by nothing and ours to delete. In the desktop editor its undo
    // list owns them (Ctrl+Z puts that very object back): deleting them there would leave the
    // editor's undo pointing at freed memory.
    if( !editorOwnsUndo() )
    {
        for( FOOTPRINT* fp : removing )
            delete fp;

        for( PCB_TRACK* t : trackRemove )
            delete t;

        for( auto& [fp, snap] : swapping )
            delete fp;

        for( BOARD_ITEM* item : itemRemove )
            delete item;
    }

    aOut.set_tracks_removed( (int) trackRemove.size() );
    return true;
}


void KICADD_HANDLER_PCB::fillHistory( HistoryResponse& aOut ) const
{
    for( const HISTORY_ENTRY& e : m_undo )
        aOut.add_undo( e.label );

    for( const HISTORY_ENTRY& e : m_redo )
        aOut.add_redo( e.label );

    aOut.set_modified( m_context && m_context->IsContentModified() );
}


HANDLER_RESULT<HistoryResponse>
KICADD_HANDLER_PCB::handleUndo( const HANDLER_CONTEXT<Undo>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    if( !board() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    HistoryResponse out;

    if( !m_undo.empty() )
    {
        HISTORY_ENTRY entry = std::move( m_undo.back() );
        m_undo.pop_back();

        out.set_ok( applyHistory( entry, true, *out.mutable_result() ) );
        out.set_label( entry.label );

        if( out.ok() )
            noteStaleFills( entry, *out.mutable_result() );

        // Nothing applied because it had all been done already (the editor's Ctrl+Z / Ctrl+Y on
        // the agent's commit): the entry still moves across, so the history matches the board.
        bool alreadyDone = !out.ok() && out.result().missing().empty()
                           && out.result().already_there() > 0;

        if( out.ok() || alreadyDone )
            m_redo.push_back( std::move( entry ) );
    }

    fillHistory( out );
    return out;
}


HANDLER_RESULT<HistoryResponse>
KICADD_HANDLER_PCB::handleRedo( const HANDLER_CONTEXT<Redo>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    if( !board() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    HistoryResponse out;

    if( !m_redo.empty() )
    {
        HISTORY_ENTRY entry = std::move( m_redo.back() );
        m_redo.pop_back();

        out.set_ok( applyHistory( entry, false, *out.mutable_result() ) );
        out.set_label( entry.label );

        if( out.ok() )
            noteStaleFills( entry, *out.mutable_result() );

        // Nothing applied because it had all been done already (the editor's Ctrl+Z / Ctrl+Y on
        // the agent's commit): the entry still moves across, so the history matches the board.
        bool alreadyDone = !out.ok() && out.result().missing().empty()
                           && out.result().already_there() > 0;

        if( out.ok() || alreadyDone )
            m_undo.push_back( std::move( entry ) );
    }

    fillHistory( out );
    return out;
}


HANDLER_RESULT<HistoryResponse>
KICADD_HANDLER_PCB::handleGetHistory( const HANDLER_CONTEXT<GetHistory>& aCtx )
{
    HistoryResponse out;
    out.set_ok( true );
    fillHistory( out );
    return out;
}


HANDLER_RESULT<LocalDrcResponse>
KICADD_HANDLER_PCB::handleCheckLocalDrc( const HANDLER_CONTEXT<CheckLocalDrc>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    LocalDrcResponse        out;
    std::vector<FOOTPRINT*> targets;

    for( const std::string& ref : aCtx.Request.refs() )
    {
        if( FOOTPRINT* fp = b->FindFootprintByReference( wxString::FromUTF8( ref ) ) )
            targets.push_back( fp );
        else
            out.add_missing( ref );
    }

    auto d0 = std::chrono::steady_clock::now();

    for( DrcViolation& v : kicadd::RunLocalDrc( b, targets ) )
        *out.add_drc() = std::move( v );

    out.set_drc_us( std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - d0 ).count() );
    return out;
}


namespace
{

struct CONN_SNAPSHOT
{
    std::vector<std::pair<const BOARD_CONNECTED_ITEM*, int>> itemNets;
    std::vector<size_t>                                      netEdges;
    unsigned                                                 unconnected = 0;
};


CONN_SNAPSHOT snapshotConnectivity( BOARD* aBoard )
{
    CONN_SNAPSHOT snap;

    for( const FOOTPRINT* fp : aBoard->Footprints() )
    {
        for( const PAD* pad : fp->Pads() )
            snap.itemNets.emplace_back( pad, pad->GetNetCode() );
    }

    for( const PCB_TRACK* t : aBoard->Tracks() )
        snap.itemNets.emplace_back( t, t->GetNetCode() );

    for( const ZONE* z : aBoard->Zones() )
        snap.itemNets.emplace_back( z, z->GetNetCode() );

    std::shared_ptr<CONNECTIVITY_DATA> conn = aBoard->GetConnectivity();

    for( int net = 1; net < conn->GetNetCount(); ++net )
    {
        RN_NET* rn = conn->GetRatsnestForNet( net );
        snap.netEdges.push_back( rn ? rn->GetEdges().size() : 0 );
    }

    snap.unconnected = conn->GetUnconnectedCount( false );
    return snap;
}


std::string handleOf( const BOARD_CONNECTED_ITEM* aItem )
{
    if( aItem->Type() == PCB_PAD_T )
        return describePad( static_cast<const PAD*>( aItem ) ).handle();

    return shortId( aItem );
}

} // namespace


HANDLER_RESULT<VerifyConnectivityResponse>
KICADD_HANDLER_PCB::handleVerifyConnectivity( const HANDLER_CONTEXT<VerifyConnectivity>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    constexpr int MAX_REPORTED = 50;

    CONN_SNAPSHOT before = snapshotConnectivity( b );

    auto t0 = std::chrono::steady_clock::now();
    b->BuildConnectivity();
    auto t1 = std::chrono::steady_clock::now();

    CONN_SNAPSHOT after = snapshotConnectivity( b );

    VerifyConnectivityResponse out;
    out.set_items_checked( static_cast<int>( before.itemNets.size() ) );
    out.set_nets_checked( static_cast<int>( std::min( before.netEdges.size(), after.netEdges.size() ) ) );
    out.set_unconnected_before( static_cast<int>( before.unconnected ) );
    out.set_unconnected_after( static_cast<int>( after.unconnected ) );
    out.set_rebuild_us( std::chrono::duration_cast<std::chrono::microseconds>( t1 - t0 ).count() );

    for( size_t i = 0; i < before.itemNets.size() && i < after.itemNets.size(); ++i )
    {
        if( before.itemNets[i].second != after.itemNets[i].second
                && out.item_mismatches_size() < MAX_REPORTED )
        {
            out.add_item_mismatches( handleOf( before.itemNets[i].first ) + ": "
                                     + netName( b, before.itemNets[i].second ) + " -> "
                                     + netName( b, after.itemNets[i].second ) );
        }
    }

    for( size_t i = 0; i < before.netEdges.size() && i < after.netEdges.size(); ++i )
    {
        if( before.netEdges[i] != after.netEdges[i] && out.net_mismatches_size() < MAX_REPORTED )
        {
            out.add_net_mismatches( netName( b, static_cast<int>( i ) + 1 ) + ": "
                                    + std::to_string( before.netEdges[i] ) + " -> "
                                    + std::to_string( after.netEdges[i] ) );
        }
    }

    return out;
}


namespace
{

/// FNV-1a over a byte range, combined order-independently by the caller (sum of item hashes).
uint64_t fnv1a( const void* aData, size_t aLen, uint64_t aSeed = 1469598103934665603ULL )
{
    const unsigned char* p = static_cast<const unsigned char*>( aData );
    uint64_t             h = aSeed;

    for( size_t i = 0; i < aLen; ++i )
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }

    return h;
}


uint64_t hashStr( const std::string& aStr, uint64_t aSeed = 1469598103934665603ULL )
{
    return fnv1a( aStr.data(), aStr.size(), aSeed );
}


template <typename T>
uint64_t hashPod( const T& aValue, uint64_t aSeed )
{
    return fnv1a( &aValue, sizeof( T ), aSeed );
}

} // namespace


HANDLER_RESULT<StateDigestResponse>
KICADD_HANDLER_PCB::handleGetStateDigest( const HANDLER_CONTEXT<GetStateDigest>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    StateDigestResponse out;
    uint64_t            netHash = 0, tdHash = 0, rnHash = 0;
    int                 teardrops = 0;

    auto addNet = [&]( const BOARD_CONNECTED_ITEM* aItem )
    {
        netHash += hashPod( aItem->GetNetCode(), hashStr( aItem->m_Uuid.AsStdString() ) );
    };

    for( const FOOTPRINT* fp : b->Footprints() )
    {
        for( const PAD* pad : fp->Pads() )
            addNet( pad );
    }

    for( const PCB_TRACK* t : b->Tracks() )
        addNet( t );

    for( const ZONE* z : b->Zones() )
    {
        addNet( z );

        if( !z->IsTeardropArea() )
            continue;

        ++teardrops;
        uint64_t h = hashStr( z->m_Uuid.AsStdString() );
        h = hashPod( z->GetAssignedPriority(), h );

        for( auto it = z->Outline()->CIterate(); it; ++it )
        {
            h = hashPod( it->x, h );
            h = hashPod( it->y, h );
        }

        tdHash += h;
    }

    std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();

    for( int net = 1; net < conn->GetNetCount(); ++net )
    {
        RN_NET* rn = conn->GetRatsnestForNet( net );
        size_t  edges = rn ? rn->GetEdges().size() : 0;
        rnHash += hashPod( edges, hashPod( net, 1469598103934665603ULL ) );
    }

    out.set_net_hash( netHash );
    out.set_ratsnest_hash( rnHash );
    out.set_teardrop_hash( tdHash );
    out.set_teardrops( teardrops );
    out.set_unconnected( static_cast<int>( conn->GetUnconnectedCount( false ) ) );
    return out;
}


HANDLER_RESULT<RatsnestDeferralResponse>
KICADD_HANDLER_PCB::handleSetRatsnestDeferral( const HANDLER_CONTEXT<SetRatsnestDeferral>& aCtx )
{
    m_deferLargeNets = aCtx.Request.enabled();

    if( aCtx.Request.min_pads() > 0 )
        m_deferMinPads = aCtx.Request.min_pads();

    // Turning deferral off must not leave anything pending.
    if( !m_deferLargeNets && board() )
        board()->GetConnectivity()->FlushDeferredRatsnest();

    RatsnestDeferralResponse out;
    out.set_enabled( m_deferLargeNets );
    out.set_min_pads( m_deferMinPads );
    return out;
}


HANDLER_RESULT<FlushRatsnestResponse>
KICADD_HANDLER_PCB::handleFlushRatsnest( const HANDLER_CONTEXT<FlushRatsnest>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();

    FlushRatsnestResponse out;
    out.set_nets_flushed( conn->FlushDeferredRatsnest() );
    out.set_unconnected( static_cast<int>( conn->GetUnconnectedCount( false ) ) );
    out.set_elapsed_us( std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0 ).count() );
    return out;
}


HANDLER_RESULT<kicadd::render::RenderSnapshot>
KICADD_HANDLER_PCB::handleGetRenderSnapshot( const HANDLER_CONTEXT<kicadd::render::GetRenderSnapshot>& aCtx )
{
    if( !board() || !feed() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    kicadd::render::RenderSnapshot out;
    m_feed->Snapshot( out, !aCtx.Request.skip_art() );
    return out;
}


HANDLER_RESULT<kicadd::render::Art>
KICADD_HANDLER_PCB::handleGetArt( const HANDLER_CONTEXT<kicadd::render::GetArt>& aCtx )
{
    if( !board() || !feed() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    kicadd::render::Art out;
    m_feed->GetArt( { aCtx.Request.layers().begin(), aCtx.Request.layers().end() }, out );
    return out;
}


HANDLER_RESULT<kicadd::render::RenderChanges>
KICADD_HANDLER_PCB::handleGetChanges( const HANDLER_CONTEXT<kicadd::render::GetChanges>& aCtx )
{
    if( !board() || !feed() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    kicadd::render::RenderChanges out;
    m_feed->Changes( aCtx.Request.since(), out );
    return out;
}


HANDLER_RESULT<kicadd::render::Ratsnest>
KICADD_HANDLER_PCB::handleGetRatsnest( const HANDLER_CONTEXT<kicadd::render::GetRatsnest>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();

    if( aCtx.Request.flush() )
        conn->FlushDeferredRatsnest();

    kicadd::render::Ratsnest           out;
    std::unordered_map<int, int>       netIndex;

    conn->RunOnUnconnectedEdgesNoFlush(
            [&]( int aNet, const CN_EDGE& aEdge )
            {
                if( !aEdge.IsVisible() || !aEdge.GetSourceNode() || !aEdge.GetTargetNode() )
                    return true;

                auto [it, inserted] = netIndex.try_emplace( aNet, out.nets_size() );

                if( inserted )
                    out.add_nets( netName( b, aNet ) );

                const VECTOR2I a = aEdge.GetSourcePos(), z = aEdge.GetTargetPos();
                out.add_xy( a.x ); out.add_xy( a.y ); out.add_xy( z.x ); out.add_xy( z.y );
                out.add_net( it->second );
                return true;
            } );

    for( int net : conn->GetPendingRatsnestNets() )
        out.add_pending( netName( b, net ) );

    out.set_unconnected( static_cast<int>( conn->GetUnconnectedCountNoFlush( false ) ) );
    return out;
}


HANDLER_RESULT<kicadd::render::ZoneFills>
KICADD_HANDLER_PCB::handleGetZoneFills( const HANDLER_CONTEXT<kicadd::render::GetZoneFills>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    std::set<std::string> wanted( aCtx.Request.ids().begin(), aCtx.Request.ids().end() );

    auto addChain = []( const SHAPE_LINE_CHAIN& aChain, kicadd::render::Polyline* aOut )
    {
        for( int i = 0; i < aChain.PointCount(); ++i )
        {
            aOut->add_xy( aChain.CPoint( i ).x );
            aOut->add_xy( aChain.CPoint( i ).y );
        }
    };

    kicadd::render::ZoneFills out;

    for( const ZONE* zone : b->Zones() )
    {
        if( zone->GetIsRuleArea() || !zone->IsOnCopperLayer() || !zone->IsFilled() )
            continue;

        const std::string id = zone->m_Uuid.AsStdString();

        if( !wanted.empty() && !wanted.count( id ) )
            continue;

        for( PCB_LAYER_ID layer : LSET( zone->GetLayerSet() & LSET::AllCuMask() ).Seq() )
        {
            std::shared_ptr<SHAPE_POLY_SET> fill = zone->GetFilledPolysList( layer );

            if( !fill || fill->OutlineCount() == 0 )
                continue;

            kicadd::render::ZoneFill* zf = out.add_fills();
            zf->set_id( id );
            zf->set_net( zone->GetNetCode() > 0 ? zone->GetNetname().ToStdString() : std::string() );
            zf->set_layer( LSET::Name( layer ).ToStdString() );

            for( int i = 0; i < fill->OutlineCount(); ++i )
            {
                kicadd::render::FillPoly* fp = zf->add_polys();
                addChain( fill->COutline( i ), fp->mutable_outline() );

                for( int h = 0; h < fill->HoleCount( i ); ++h )
                    addChain( fill->CHole( i, h ), fp->add_holes() );
            }
        }
    }

    return out;
}


HANDLER_RESULT<DescribeItemsResponse>
KICADD_HANDLER_PCB::handleDescribeItems( const HANDLER_CONTEXT<DescribeItems>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    DescribeItemsResponse out;

    for( const std::string& id : aCtx.Request.ids() )
    {
        DescribedItem* d = out.add_items();
        d->set_id( id );

        BOARD_ITEM* item = b->ResolveItem( KIID( id ), true );

        if( !item )
            continue;

        d->set_net( netOf( item ) );

        switch( item->Type() )
        {
        case PCB_FOOTPRINT_T:
            d->set_kind( IK_FOOTPRINT );
            d->set_handle( static_cast<FOOTPRINT*>( item )->GetReference().ToStdString() );
            break;

        case PCB_PAD_T:
            d->set_kind( IK_PAD );
            d->set_handle( describePad( static_cast<PAD*>( item ) ).handle() );
            break;

        case PCB_TRACE_T: d->set_kind( IK_TRACK ); d->set_handle( shortId( item ) ); break;
        case PCB_ARC_T:   d->set_kind( IK_ARC );   d->set_handle( shortId( item ) ); break;
        case PCB_VIA_T:   d->set_kind( IK_VIA );   d->set_handle( "via:" + shortId( item ) ); break;

        case PCB_ZONE_T:
            d->set_kind( IK_ZONE );
            d->set_handle( "zone:" + ( d->net().empty() ? shortId( item ) : d->net() ) );
            break;

        default:
            if( const FOOTPRINT* fp = item->GetParentFootprint() )
            {
                d->set_kind( IK_FOOTPRINT );
                d->set_handle( fp->GetReference().ToStdString() + ":" + LSET::Name( item->GetLayer() ).ToStdString() );
            }
            else if( item->GetLayer() == Edge_Cuts )
            {
                d->set_kind( IK_SHAPE );
                d->set_handle( "edge" );
            }
            else
            {
                d->set_kind( item->Type() == PCB_SHAPE_T ? IK_SHAPE : IK_TEXT );
                d->set_handle( LSET::Name( item->GetLayer() ).ToStdString() + ":" + shortId( item ) );
            }

            break;
        }
    }

    return out;
}


HANDLER_RESULT<SyncFromSchematicResponse>
KICADD_HANDLER_PCB::handleSyncFromSchematic( const HANDLER_CONTEXT<SyncFromSchematic>& aCtx )
{
    BOARD* b = board();

    if( !b || b->GetFileName().IsEmpty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no saved board loaded" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    // The project's root schematic: next to the board, same name
    wxFileName sch( b->GetFileName() );
    sch.SetExt( wxS( "kicad_sch" ) );

    KIWAY*      kiway = m_context->GetKiway();
    std::string text;
    bool        fromEditor = false;

    // An open Schematic Editor (desktop, project manager): its schematic as it is now, unsaved
    // edits included, the way Update PCB from Schematic asks for it
    if( kiway && kiway->Player( FRAME_SCH, false ) )
    {
        kiway->ExpressMail( FRAME_SCH, MAIL_SCH_GET_NETLIST, text );
        fromEditor = !text.empty() && text != MAIL_SCH_GET_NETLIST_CANCELLED;

        if( !fromEditor )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "the Schematic Editor gave no netlist" ) );
    }

    if( !fromEditor && !sch.FileExists() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "no schematic " + sch.GetFullName().ToStdString() + " next to the board" ) );

    KIFACE_NETLIST_SCHEMATIC_SERVICE::Type netlister = nullptr;

    try
    {
        if( kiway && !fromEditor )
            netlister = GetKifaceService<KIFACE_NETLIST_SCHEMATIC_SERVICE>( kiway->KiFACE( KIWAY::FACE_SCH ) );
    }
    catch( const IO_ERROR& e )
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_UNIMPLEMENTED,
                                          "the schematic module could not be loaded: " + e.What().ToStdString() ) );
    }

    if( !fromEditor && !netlister )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNIMPLEMENTED, "no schematic netlister available" ) );

    if( !fromEditor && ( !( *netlister )( sch.GetFullPath(), text, kiway ) || text.empty() ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "the schematic could not be netlisted: " + text.substr( 0, 300 ) ) );

    // The netlist reader (and its footprint preloading) works from a file
    wxString netFile = wxFileName::CreateTempFileName( wxS( "kicadd-netlist" ) );

    {
        wxFFile f( netFile, wxS( "wb" ) );

        if( !f.IsOpened() || !f.Write( text.data(), text.size() ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_UNKNOWN, "cannot write a temporary netlist" ) );
    }

    // The updater's report, each warning and error marked as such (the reply lists them first)
    struct MARKING_REPORTER : public WX_STRING_REPORTER
    {
        REPORTER& Report( const wxString& aText, SEVERITY aSeverity = RPT_SEVERITY_UNDEFINED ) override
        {
            wxString mark;

            if( aSeverity == RPT_SEVERITY_ERROR )
                mark = wxS( "error: " );
            else if( aSeverity == RPT_SEVERITY_WARNING )
                mark = wxS( "warning: " );

            return WX_STRING_REPORTER::Report( mark + aText, aSeverity );
        }
    };

    MARKING_REPORTER reporter;
    NETLIST          netlist;
    netlist.SetFindByTimeStamp( true );
    netlist.SetReplaceFootprints( aCtx.Request.update_footprints() );

    bool read = m_context->ReadNetlistFromFile( netFile, netlist, reporter );
    wxRemoveFile( netFile );

    if( !read )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "reading the netlist failed: " + reporter.GetMessages().ToStdString() ) );

    std::unique_ptr<BOARD_NETLIST_UPDATER> updater = m_context->MakeNetlistUpdater();
    updater->SetReporter( &reporter );
    updater->SetIsDryRun( aCtx.Request.dry_run() );
    updater->SetWillDrag( false );
    updater->SetLookupByTimestamp( true );
    updater->SetDeleteUnusedFootprints( aCtx.Request.delete_extra() );
    updater->SetReplaceFootprints( aCtx.Request.update_footprints() );
    updater->SetTransferGroups( false );
    updater->SetOverrideLocks( false );
    updater->SetUpdateFields( true );

    // A footprint from the LCSC library may still carry the part number it was fetched for
    // (libraries saved before that was fixed): a symbol without an LCSC number clears it, so a
    // 1k resistor on the 10k's R0603 doesn't go into the BOM as the 10k. Only where there can be
    // a number (LCSC footprints, board footprints with one): an empty field on every other part
    // would make the updater report "Update fields" for all of them.
    std::set<wxString> withLcsc;

    for( FOOTPRINT* fp : b->Footprints() )
    {
        if( const PCB_FIELD* f = fp->GetField( wxS( "LCSC" ) ); f && !f->GetText().IsEmpty() )
            withLcsc.insert( fp->GetReference() );
    }

    for( unsigned i = 0; i < netlist.GetCount(); ++i )
    {
        COMPONENT* comp = netlist.GetComponent( i );
        auto       fields = comp->GetFields();

        if( fields.find( wxS( "LCSC" ) ) == fields.end()
            && ( withLcsc.count( comp->GetReference() ) || comp->GetFPID().GetLibNickname() == "LCSC" ) )
        {
            fields[wxS( "LCSC" )] = wxEmptyString;
            comp->SetFields( fields );
        }
    }

    // A symbol whose footprint is not on the board yet, while a footprint of the same reference
    // is (placed by hand, or linked to another symbol): the updater adds a second one, and the
    // BOM and the placement file get the reference twice.
    int ownWarnings = 0;

    {
        std::map<wxString, std::vector<const FOOTPRINT*>> byRef;
        std::set<KIID_PATH>                               paths;

        for( const FOOTPRINT* fp : b->Footprints() )
        {
            byRef[fp->GetReference().Upper()].push_back( fp );
            paths.insert( fp->GetPath() );
        }

        for( unsigned i = 0; i < netlist.GetCount(); ++i )
        {
            COMPONENT* comp = netlist.GetComponent( i );
            bool       linked = false;

            for( const KIID& uuid : comp->GetKIIDs() )
            {
                KIID_PATH path = comp->GetPath();
                path.push_back( uuid );
                linked = linked || paths.count( path );
            }

            auto same = byRef.find( comp->GetReference().Upper() );

            if( linked || same == byRef.end() || comp->GetProperties().count( wxT( "exclude_from_board" ) ) )
                continue;

            const FOOTPRINT* other = same->second.front();
            wxString         msg = wxString::Format(
                    wxS( "%s is already on the board, %s: the sync %s a second %s (the BOM and the "
                         "placement file would list it twice). Delete or renumber the board's one "
                         "first (delete_footprints, or pcb_set_fields {\"%s\": {\"Reference\": \"...\"}}), "
                         "or delete_extra if it should go" ),
                    comp->GetReference(),
                    other->GetPath().empty() ? wxS( "placed by hand (not from the schematic)" )
                                             : wxS( "from another symbol" ),
                    aCtx.Request.dry_run() ? wxS( "would add" ) : wxS( "adds" ), comp->GetReference(),
                    comp->GetReference() );
            reporter.Report( msg, RPT_SEVERITY_WARNING );
            ownWarnings++;
        }
    }

    // What the board is before, for the agent's undo (the updater's own commit goes into the
    // editor's undo only; headless there is none)
    std::map<KIID, FP_SNAPSHOT> fpsBefore;
    std::map<KIID, wxString>    netsBefore;
    std::set<wxString>          netNamesBefore;

    auto netName = []( const BOARD_CONNECTED_ITEM* aItem )
    {
        return aItem->GetNetCode() > 0 ? aItem->GetNetname() : wxString();
    };

    if( !aCtx.Request.dry_run() )
    {
        for( const FOOTPRINT* fp : b->Footprints() )
            fpsBefore.emplace( fp->m_Uuid, copyOf( fp ) );

        for( const PCB_TRACK* t : b->Tracks() )
            netsBefore[t->m_Uuid] = netName( t );

        for( const ZONE* z : b->Zones() )
            netsBefore[z->m_Uuid] = netName( z );

        for( const NETINFO_ITEM* net : b->GetNetInfo() )
            netNamesBefore.insert( net->GetNetname() );
    }

    bool ok = updater->UpdateNetlist( netlist );

    SyncFromSchematicResponse out;
    out.set_from_editor( fromEditor );

    if( !aCtx.Request.dry_run() && ok )
    {
        m_context->OnNetlistChanged( *updater );

        for( FOOTPRINT* fp : updater->GetAddedFootprints() )
            out.add_new_refs( fp->GetReference().ToStdString() );

        HISTORY_ENTRY entry;
        entry.label = "sync_pcb";

        for( const FOOTPRINT* fp : b->Footprints() )
        {
            auto it = fpsBefore.find( fp->m_Uuid );

            if( it == fpsBefore.end() )
            {
                entry.added.push_back( copyOf( fp ) );
            }
            else
            {
                if( !matches( fp, it->second ) )
                    entry.replaced.emplace_back( std::move( it->second ), copyOf( fp ) );

                fpsBefore.erase( it );
            }
        }

        for( auto& [id, snap] : fpsBefore )   // the ones it deleted (delete_extra)
            entry.removed.push_back( std::move( snap ) );

        auto netMoved = [&]( const BOARD_CONNECTED_ITEM* aItem )
        {
            auto it = netsBefore.find( aItem->m_Uuid );

            if( it != netsBefore.end() && it->second != netName( aItem ) )
                entry.netChanges.push_back( { aItem->m_Uuid, it->second, netName( aItem ) } );
        };

        for( const PCB_TRACK* t : b->Tracks() )
            netMoved( t );

        for( const ZONE* z : b->Zones() )
            netMoved( z );

        for( const NETINFO_ITEM* net : b->GetNetInfo() )
        {
            if( !netNamesBefore.count( net->GetNetname() ) )
                entry.netsCreated.push_back( net->GetNetname() );
        }

        if( !entry.added.empty() || !entry.removed.empty() || !entry.replaced.empty() || !entry.netChanges.empty()
            || !entry.netsCreated.empty() )
        {
            out.set_history_label( entry.label );
            record( std::move( entry ) );
        }
    }

    out.set_added( updater->GetNewFootprintCount() );
    out.set_errors( updater->GetErrorCount() );
    out.set_warnings( updater->GetWarningCount() + ownWarnings );
    out.set_report( reporter.GetMessages().ToStdString( wxConvUTF8 ) );
    out.set_footprints( (int) b->Footprints().size() );
    out.set_nets( (int) b->GetNetCount() - 1 );
    return out;
}


HANDLER_RESULT<AutoPlaceResponse> KICADD_HANDLER_PCB::handleAutoPlace( const HANDLER_CONTEXT<AutoPlace>& aCtx )
{
    namespace ap = kicadd::autoplace;

    if( auto err = busy() )
        return tl::unexpected( *err );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    SHAPE_POLY_SET outline;

    if( !b->GetBoardPolygonOutlines( outline, false ) || outline.OutlineCount() == 0 )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "the board has no closed outline on Edge.Cuts to place into" ) );

    const AutoPlace&      req = aCtx.Request;
    AutoPlaceResponse     out;
    std::set<wxString>    asked, fixed;

    for( const std::string& r : req.refs() )
        asked.insert( wxString::FromUTF8( r ) );

    for( const std::string& r : req.fixed() )
        fixed.insert( wxString::FromUTF8( r ) );

    for( const wxString& r : asked )
    {
        if( !b->FindFootprintByReference( r ) )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no footprint " + r.ToStdString() ) );
    }

    // The model: every footprint, with its courtyard (or body) and its pads' nets
    std::vector<ap::PART>   parts;
    std::vector<FOOTPRINT*> fps;
    std::map<int, int>      netIndex;

    auto inside = [&]( FOOTPRINT* fp )
    {
        BOX2I box = fp->GetBoundingBox( false );
        return outline.Contains( box.GetOrigin() ) && outline.Contains( box.GetEnd() )
               && outline.Contains( VECTOR2I( box.GetRight(), box.GetTop() ) )
               && outline.Contains( VECTOR2I( box.GetLeft(), box.GetBottom() ) );
    };

    std::vector<wxString> back;

    for( const std::string& g : req.back() )
        back.push_back( wxString::FromUTF8( g ) );

    if( ( !back.empty() || req.back_decaps() ) && b->GetCopperLayerCount() < 2 )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "a one-layer board has no bottom side to place on" ) );

    // A footprint's shape at rotation 0, relative to its anchor: courtyard (or body) and the
    // silkscreen on its side, each pad's position and copper.
    auto shapeOf = [&]( const FOOTPRINT* f, std::vector<VECTOR2I>& aOutline, std::vector<VECTOR2I>& aCopper,
                        std::vector<VECTOR2I>& aPadOffset, std::vector<BOX2I>& aPadBox )
    {
        const VECTOR2I  anchor = f->GetPosition();
        const EDA_ANGLE orient = f->GetOrientation();
        const bool      flipped = f->IsFlipped();

        auto toLocal = [&]( const VECTOR2I& aWorld )
        {
            VECTOR2I p = aWorld - anchor;
            RotatePoint( p, -orient );
            return p;
        };

        auto corners = [&]( const BOX2I& aBox, std::vector<VECTOR2I>& aOut )
        {
            for( const VECTOR2I& c : { aBox.GetOrigin(), aBox.GetEnd(), VECTOR2I( aBox.GetRight(), aBox.GetTop() ),
                                       VECTOR2I( aBox.GetLeft(), aBox.GetBottom() ) } )
                aOut.push_back( toLocal( c ) );
        };

        const SHAPE_POLY_SET& crt = f->GetCourtyard( flipped ? B_CrtYd : F_CrtYd );

        if( crt.OutlineCount() > 0 )
        {
            for( auto it = crt.CIterate(); it; ++it )
                aOutline.push_back( toLocal( *it ) );
        }
        else
        {
            corners( f->GetBoundingBox( false ), aOutline );
        }

        // Silkscreen (outline, polarity marks, the visible reference) often sticks out of the
        // courtyard: keep it clear of the neighbours' silk too (DRC silk_clearance).
        auto addSilk = [&]( const BOARD_ITEM* aItem )
        {
            if( !aItem->IsOnLayer( flipped ? B_SilkS : F_SilkS ) )
                return;

            if( const PCB_TEXT* text = dynamic_cast<const PCB_TEXT*>( aItem ); text && !text->IsVisible() )
                return;

            corners( aItem->GetBoundingBox(), aOutline );
        };

        for( const BOARD_ITEM* item : f->GraphicalItems() )
            addSilk( item );

        for( const PCB_FIELD* field : f->GetFields() )
            addSilk( field );

        for( const PAD* pad : f->Pads() )
        {
            aPadOffset.push_back( toLocal( pad->GetPosition() ) );

            std::vector<VECTOR2I> c;
            corners( pad->GetBoundingBox(), c );
            BOX2I box( c[0], VECTOR2I( 0, 0 ) );

            for( const VECTOR2I& v : c )
            {
                box.Merge( v );
                aCopper.push_back( v );
            }

            aPadBox.push_back( box );
        }
    };

    for( FOOTPRINT* fp : b->Footprints() )
    {
        ap::PART  part;
        std::vector<VECTOR2I> padOffset;
        std::vector<BOX2I>    padBox;
        EDA_ANGLE orient = fp->GetOrientation();
        VECTOR2I  anchor = fp->GetPosition();
        double    quarter = orient.AsDegrees() / 90.0;
        bool      square = std::abs( quarter - std::round( quarter ) ) < 1e-6;

        part.ref = fp->GetReference().ToStdString();
        part.pos = anchor;
        part.rot = ( ( (int) std::lround( quarter ) % 4 ) + 4 ) % 4;

        bool wanted = !asked.empty() ? asked.count( fp->GetReference() ) > 0
                                     : ( req.all() || !inside( fp ) );

        part.movable = wanted && !fixed.count( fp->GetReference() );

        if( part.movable && ( fp->IsLocked() || !square ) )
        {
            part.movable = false;
            out.add_skipped( part.ref + ( fp->IsLocked() ? " (locked)" : " (not at a right angle)" ) );
        }

        part.back = fp->IsFlipped();
        shapeOf( fp, part.outline, part.copper, padOffset, padBox );

        // may go to the other side: its shape there, from a flipped copy (the same flip as
        // applying it does)
        bool backAsked = std::any_of( back.begin(), back.end(),
                                  [&]( const wxString& g ) { return fp->GetReference().Matches( g ); } );
        bool smd = std::none_of( fp->Pads().begin(), fp->Pads().end(), []( PAD* p ) { return p->HasHole(); } );
        bool small = fp->Pads().size() <= 3;

        if( part.movable && smd && ( backAsked || ( req.back_decaps() && small ) ) )
        {
            std::unique_ptr<FOOTPRINT> copy( static_cast<FOOTPRINT*>( fp->Clone() ) );
            copy->Flip( copy->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );
            shapeOf( copy.get(), part.flipOutline, part.flipCopper, part.flipPadOffset, part.flipPadBox );
            part.canFlip = true;
            part.flipIfSatellite = !backAsked;
        }
        else if( backAsked && !smd )
        {
            out.add_skipped( part.ref + " (through-hole: stays on its side)" );
        }

        for( size_t k = 0; k < fp->Pads().size(); ++k )
        {
            PAD*    pad = fp->Pads()[k];
            ap::PAD p;
            p.offset = padOffset[k];
            p.box = padBox[k];

            if( pad->HasHole() )
            {
                part.tht = true;
                wxString src;

                if( std::optional<int> c = pad->GetLocalClearance( &src ) )
                    part.holeGap = std::max( part.holeGap, *c );
            }

            p.hole = pad->HasHole();

            if( pad->GetNetCode() > 0 )
            {
                auto [it, added] = netIndex.emplace( pad->GetNetCode(), (int) netIndex.size() );
                p.net = it->second;
            }

            part.pads.push_back( p );
        }

        parts.push_back( std::move( part ) );
        fps.push_back( fp );
    }

    ap::OPTIONS opt;

    // Functional blocks kept together: asked for, else the footprints' Block field (from the
    // symbol, sch_add_part block), or the schematic sheets when asked. Sheets are not the default:
    // on the demo boards they are rarely the blocks a layout keeps apart, and pulling them
    // together cost 10-35% wire length.
    std::map<std::string, int> groupIndex;
    std::string                groupSource;

    auto assign = [&]( size_t aPart, const std::string& aName )
    {
        auto [it, added] = groupIndex.emplace( aName, (int) groupIndex.size() );
        parts[aPart].group = it->second;
    };

    if( !req.no_groups() && req.groups_size() > 0 )
    {
        groupSource = "request";

        for( const kicadd::board::PartGroup& g : req.groups() )
        {
            for( const std::string& r : g.refs() )
            {
                auto it = std::find_if( parts.begin(), parts.end(), [&]( const ap::PART& p ) { return p.ref == r; } );

                if( it == parts.end() )
                    return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                                      "groups: no footprint " + r ) );

                assign( it - parts.begin(), g.name() );
            }
        }
    }
    else if( !req.no_groups() && req.groups_from_sheets() )
    {
        groupSource = "sheets";

        for( size_t i = 0; i < fps.size(); ++i )
        {
            KIID_PATH path = fps[i]->GetPath();

            if( path.empty() )
                continue;

            path.pop_back();
            assign( i, path.AsString().ToStdString() );
        }
    }
    else if( !req.no_groups() )
    {
        groupSource = "Block field";

        for( size_t i = 0; i < fps.size(); ++i )
        {
            if( const PCB_FIELD* f = fps[i]->GetField( wxS( "Block" ) ); f && !f->GetText().IsEmpty() )
                assign( i, f->GetText().ToStdString( wxConvUTF8 ) );
        }
    }

    if( req.group_weight() > 0 )
        opt.groupWeight = req.group_weight();

    if( groupIndex.size() >= 2 )
    {
        opt.groups = (int) groupIndex.size();
    }
    else
    {
        for( ap::PART& p : parts )
            p.group = -1;
    }

    if( req.spacing() > 0 )
        opt.spacing = (int) req.spacing();

    if( req.edge() > 0 )
        opt.edge = (int) req.edge();

    opt.rotate = !req.no_rotate();
    opt.backSatellites = req.back_decaps();

    // The agent's rules: refs may be patterns ("J*"); each must match something
    auto matching = [&]( const std::string& aPattern, std::vector<int>& aOut ) -> bool
    {
        wxString pat = wxString::FromUTF8( aPattern );

        for( size_t i = 0; i < fps.size(); ++i )
        {
            if( fps[i]->GetReference().Matches( pat ) )
                aOut.push_back( (int) i );
        }

        return !aOut.empty();
    };

    auto bad = [&]( const std::string& aMsg ) { return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, aMsg ) ); };

    for( const kicadd::board::EdgeRule& r : req.edge_rules() )
    {
        static const std::map<std::string, int> sides = { { "left", ap::EDGE_LEFT },   { "right", ap::EDGE_RIGHT },
                                                          { "top", ap::EDGE_TOP },     { "bottom", ap::EDGE_BOTTOM },
                                                          { "any", ap::EDGE_ANY } };
        std::vector<int> hit;
        auto             side = sides.find( r.side() );

        if( side == sides.end() )
            return bad( "edge rule " + r.ref() + ": side is left, right, top, bottom or any" );

        if( !matching( r.ref(), hit ) )
            return bad( "edge rule: no footprint " + r.ref() );

        for( int i : hit )
            parts[i].edgeSide = side->second;
    }

    for( const kicadd::board::RegionRule& r : req.region_rules() )
    {
        std::vector<int> hit;

        if( !matching( r.ref(), hit ) )
            return bad( "region rule: no footprint " + r.ref() );

        BOX2I box( VECTOR2I( (int) std::min( r.min().x(), r.max().x() ), (int) std::min( r.min().y(), r.max().y() ) ),
                   VECTOR2I( (int) std::abs( r.max().x() - r.min().x() ), (int) std::abs( r.max().y() - r.min().y() ) ) );

        for( int i : hit )
        {
            parts[i].hasRegion = true;
            parts[i].region = box;
        }
    }

    for( const kicadd::board::RotationRule& r : req.rotation_rules() )
    {
        std::vector<int> hit;
        int              deg = ( ( r.degrees() % 360 ) + 360 ) % 360;

        if( deg % 90 )
            return bad( "rotation rule " + r.ref() + ": 0, 90, 180 or 270" );

        if( !matching( r.ref(), hit ) )
            return bad( "rotation rule: no footprint " + r.ref() );

        for( int i : hit )
            parts[i].fixedRot = deg / 90;
    }

    for( const kicadd::board::NearRule& r : req.near_rules() )
    {
        ap::NEAR_RULE rule;
        rule.maxDist = (int) std::max<int64_t>( 0, r.max_distance() );

        for( const std::string& ref : r.refs() )
        {
            if( !matching( ref, rule.parts ) )
                return bad( "near rule: no footprint " + ref );

            rule.label += ( rule.label.empty() ? "" : " " ) + ref;
        }

        if( rule.parts.size() < 2 )
            return bad( "near rule " + rule.label + ": two parts or more" );

        opt.nearRules.push_back( std::move( rule ) );
    }
    opt.measureOnly = req.measure_only();

    if( const char* seed = getenv( "KICADD_AP_SEED" ) )     // benchmarks only
        opt.seed = atoi( seed );

    // Board items the parts must keep clear of: copper drawings and text, tracks, vias (grown
    // by the default clearance), keep-outs that forbid footprints.
    {
        int clearance = b->GetDesignSettings().m_MinClearance;

        if( std::shared_ptr<NETCLASS> nc = b->GetDesignSettings().m_NetSettings->GetDefaultNetclass() )
            clearance = std::max( clearance, nc->GetClearance() );

        // which sides an item's copper is on: SMD pads meet their own side's only
        auto sides = []( const LSET& aLayers )
        {
            int m = 0;

            if( aLayers.test( F_Cu ) )
                m |= ap::OBST_FRONT;

            if( aLayers.test( B_Cu ) )
                m |= ap::OBST_BACK;

            if( ( aLayers & LSET::InternalCuMask() ).any() )
                m |= ap::OBST_INNER;

            return m;
        };

        auto add = [&]( const BOX2I& aBox, int aSides )
        {
            opt.obstacles.push_back( aBox );
            opt.obstacleLayers.push_back( aSides );
        };

        for( BOARD_ITEM* item : b->Drawings() )
        {
            if( ( item->GetLayerSet() & LSET::AllCuMask() ).any() )
            {
                BOX2I box = item->GetBoundingBox();
                box.Inflate( clearance );
                add( box, sides( item->GetLayerSet() ) );
            }
        }

        // A diagonal track's bounding box covers far more than the track: cover it with a row
        // of small boxes instead (arcs: along their polyline). Still never less than the track.
        auto addLine = [&]( const VECTOR2I& aA, const VECTOR2I& aB, int aHalf, int aSides )
        {
            const VECTOR2I d = aB - aA;

            int n = 1;

            if( d.x != 0 && d.y != 0 )
                n = std::clamp( (int) std::ceil( d.EuclideanNorm() / std::max( 2 * aHalf, 250000 ) ), 1, 256 );

            for( int k = 0; k < n; ++k )
            {
                VECTOR2I p0 = aA + VECTOR2I( (int64_t) d.x * k / n, (int64_t) d.y * k / n );
                VECTOR2I p1 = aA + VECTOR2I( (int64_t) d.x * ( k + 1 ) / n, (int64_t) d.y * ( k + 1 ) / n );
                BOX2I    box( p0 );
                box.Merge( p1 );
                box.Inflate( aHalf );
                add( box, aSides );
            }
        };

        for( PCB_TRACK* track : b->Tracks() )
        {
            // a via goes through every layer between its ends: count it on all sides
            if( track->Type() == PCB_VIA_T )
            {
                BOX2I box = track->GetBoundingBox();
                box.Inflate( std::max( clearance, track->GetOwnClearance( track->GetLayer() ) ) );
                add( box, ap::OBST_FRONT | ap::OBST_BACK | ap::OBST_INNER | ap::OBST_PADS );
                continue;
            }

            // its own net class may ask for more than the default (POWER: 0.28 mm)
            const int half = track->GetWidth() / 2 + std::max( clearance, track->GetOwnClearance( track->GetLayer() ) );
            const int side = sides( track->GetLayerSet() ) | ap::OBST_PADS;

            if( track->Type() == PCB_ARC_T )
            {
                auto*                 arc = static_cast<PCB_ARC*>( track );
                const int             maxError = 10000;
                SHAPE_LINE_CHAIN      chain = SHAPE_ARC( arc->GetStart(), arc->GetMid(), arc->GetEnd(), 0 )
                                                 .ConvertToPolyline( maxError );

                for( int k = 0; k + 1 < chain.PointCount(); ++k )
                    addLine( chain.CPoint( k ), chain.CPoint( k + 1 ), half + maxError, side );
            }
            else
            {
                addLine( track->GetStart(), track->GetEnd(), half, side );
            }
        }

        for( ZONE* zone : b->Zones() )
        {
            if( zone->GetIsRuleArea() && zone->GetDoNotAllowFootprints() )
            {
                int m = sides( zone->GetLayerSet() );
                add( zone->GetBoundingBox(), m ? m : ( ap::OBST_FRONT | ap::OBST_BACK | ap::OBST_INNER ) );
            }
        }
    }

    // Pads must keep the hole clearance from the holes of through-hole parts, on both sides
    // (board rules may ask for much more than the courtyard gap)
    {
        BOARD_DESIGN_SETTINGS& bds = b->GetDesignSettings();
        int                    hole = bds.m_HoleClearance;

        if( bds.m_DRCEngine )
        {
            DRC_CONSTRAINT c;

            if( bds.m_DRCEngine->QueryWorstConstraint( HOLE_CLEARANCE_CONSTRAINT, c ) )
                hole = std::max( hole, c.Value().Min() );

            if( bds.m_DRCEngine->QueryWorstConstraint( PHYSICAL_HOLE_CLEARANCE_CONSTRAINT, c ) )
                hole = std::max( hole, c.Value().Min() );
        }

        opt.holeClearance = std::min( hole, 5000000 );
    }

    // a finer grid for small boards, a coarser one for big ones
    BOX2I area = outline.BBox();
    opt.grid = std::max( 100000, (int) ( std::max( area.GetWidth(), area.GetHeight() ) / 800 ) / 50000 * 50000 );

    // Improving is cut off after time_limit (the placement is legal from the start): fewer
    // cores make it much slower.
    opt.timeLimit = req.time_limit() > 0 ? req.time_limit() : req.time_limit() < 0 ? 0 : 10.0;
    opt.keepBetterStart = !req.force() && !req.measure_only();

    // Where the parts are now counts as a placement only without overlapping courtyards (the
    // real shapes: the placer's boxes are coarser than a hand placement)
    if( opt.keepBetterStart )
    {
        for( size_t i = 0; i < fps.size() && !opt.startOverlaps; ++i )
        {
            if( !parts[i].movable )
                continue;

            fps[i]->BuildCourtyardCaches();

            for( FOOTPRINT* other : b->Footprints() )
            {
                if( other == fps[i] || opt.startOverlaps )
                    continue;

                for( PCB_LAYER_ID layer : { F_CrtYd, B_CrtYd } )
                {
                    const SHAPE_POLY_SET& mine = fps[i]->GetCourtyard( layer );
                    const SHAPE_POLY_SET& theirs = other->GetCourtyard( layer );

                    if( mine.OutlineCount() == 0 || theirs.OutlineCount() == 0
                        || !mine.BBox().Intersects( theirs.BBox() ) )
                    {
                        continue;
                    }

                    SHAPE_POLY_SET common = mine.CloneDropTriangulation();
                    common.BooleanIntersection( theirs );

                    if( common.Area() > 0.01 * 1e12 )     // more than 0.01 mm² (touching is fine)
                    {
                        opt.startOverlaps = true;
                        break;
                    }
                }
            }
        }
    }

    auto job = std::make_unique<AUTOPLACE_JOB>();
    job->parts = std::move( parts );
    job->netCount = (int) netIndex.size();
    job->outline = outline;
    job->opt = opt;
    job->opt.progress = &job->progress;
    job->measureOnly = req.measure_only();
    job->groupSource = groupSource;
    job->board = b;
    job->revision = feed() ? feed()->Revision() : 0;
    job->started = std::chrono::steady_clock::now();
    job->out = std::move( out );
    job->out.set_track_obstacles( (int) b->Tracks().size() );

    for( FOOTPRINT* fp : fps )
        job->ids.push_back( fp->m_Uuid );

    for( const ap::PART& p : job->parts )
        job->movable += p.movable ? 1 : 0;

    if( !req.background() )
    {
        runAutoPlace( *job );
        return finishAutoPlace( *job );
    }

    // In the background: the editor stays responsive. The placer works on its own copy of
    // the parts, never on the board; the result is applied on this thread when asked for
    // (AutoPlaceStatus), and only if the board hasn't changed meanwhile.
    AUTOPLACE_JOB* raw = job.get();
    job->worker = std::thread( [raw]() { runAutoPlace( *raw ); } );
    m_autoPlace = std::move( job );

    announce( wxString::Format( wxS( "Agent: auto-placing %d parts..." ), raw->movable ) );
    return autoPlaceRunning( *raw );
}


void KICADD_HANDLER_PCB::runAutoPlace( AUTOPLACE_JOB& aJob )
{
    aJob.result = kicadd::autoplace::Place( aJob.parts, aJob.netCount, aJob.outline, aJob.opt );
    aJob.seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - aJob.started ).count();
    aJob.done = true;
}


AutoPlaceResponse KICADD_HANDLER_PCB::autoPlaceRunning( const AUTOPLACE_JOB& aJob ) const
{
    AutoPlaceResponse r = aJob.out;
    r.set_running( true );
    r.set_seconds( std::chrono::duration<double>( std::chrono::steady_clock::now() - aJob.started ).count() );

    if( aJob.progress.improving )
        r.set_progress( "improving, " + std::to_string( aJob.progress.passes.load() ) + " pass(es) done" );
    else
        r.set_progress( "placing, " + std::to_string( aJob.progress.placed.load() ) + "/"
                        + std::to_string( aJob.movable ) + " parts" );

    return r;
}


void KICADD_HANDLER_PCB::stopAutoPlace()
{
    if( !m_autoPlace )
        return;

    m_autoPlace->progress.cancel = true;

    if( m_autoPlace->worker.joinable() )
        m_autoPlace->worker.join();

    m_autoPlace.reset();
}


HANDLER_RESULT<AutoPlaceResponse>
KICADD_HANDLER_PCB::handleAutoPlaceStatus( const HANDLER_CONTEXT<AutoPlaceStatus>& aCtx )
{
    if( !m_autoPlace )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no auto_place is running" ) );

    if( aCtx.Request.cancel() )
    {
        AutoPlaceResponse r = m_autoPlace->out;
        stopAutoPlace();
        r.set_cancelled( true );
        return r;
    }

    if( !m_autoPlace->done )
        return autoPlaceRunning( *m_autoPlace );

    // Done: apply it, unless the user is in the middle of something (asked again later)
    if( m_context && !m_context->CanAcceptApiCommands() )
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_BUSY,
                                          "auto_place is done, but the user is in an interactive tool; "
                                          "ask again to apply it (or cancel)" ) );
    }

    m_autoPlace->worker.join();
    std::unique_ptr<AUTOPLACE_JOB> job = std::move( m_autoPlace );
    return finishAutoPlace( *job );
}


HANDLER_RESULT<AutoPlaceResponse> KICADD_HANDLER_PCB::finishAutoPlace( AUTOPLACE_JOB& aJob )
{
    namespace ap = kicadd::autoplace;

    AutoPlaceResponse& out = aJob.out;
    const ap::RESULT&  res = aJob.result;
    BOARD*             b = board();

    out.set_seconds( aJob.seconds );
    out.set_timed_out( res.timedOut );
    out.set_refine_passes( res.refinePasses );

    if( res.cancelled )
    {
        out.set_cancelled( true );
        return out;
    }

    // The positions are for the board as it was: anything changed since (the user moved a part,
    // routed, the board was replaced) and they may no longer fit.
    if( b != aJob.board || !feed() || feed()->Revision() != aJob.revision )
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                          "the board changed while auto_place was running; nothing was "
                                          "moved, run it again" ) );
    }

    std::unordered_map<KIID, FOOTPRINT*> byId;

    for( FOOTPRINT* fp : b->Footprints() )
        byId[fp->m_Uuid] = fp;

    std::vector<ap::PART>&  parts = aJob.parts;
    std::vector<FOOTPRINT*> fps;

    for( const KIID& id : aJob.ids )
    {
        auto it = byId.find( id );

        if( it == byId.end() )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "a footprint went away while auto_place was running; run it again" ) );

        fps.push_back( it->second );
    }

    std::vector<FOOTPRINT*> targets;
    std::set<std::string>   unplaced( res.unplaced.begin(), res.unplaced.end() );
    std::map<FOOTPRINT*, size_t> index;

    for( size_t i = 0; i < parts.size(); ++i )
    {
        if( parts[i].movable && !unplaced.count( parts[i].ref ) && !aJob.measureOnly && !res.keptStart )
        {
            targets.push_back( fps[i] );
            index[fps[i]] = i;
            out.add_refs( parts[i].ref );
        }
    }

    HISTORY_ENTRY entry;

    for( const FOOTPRINT* fp : targets )
        entry.before.push_back( stateOf( fp ) );

    std::string label = "auto-place " + std::to_string( targets.size() ) + " part" + ( targets.size() == 1 ? "" : "s" );

    applyFootprintEdit( targets,
                        [&]( FOOTPRINT* fp )
                        {
                            const ap::PART& p = parts[index[fp]];

                            // the other side: the same flip its shape there was taken with
                            if( p.back != fp->IsFlipped() )
                                fp->Flip( fp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

                            fp->SetOrientation( EDA_ANGLE( 90.0 * p.rot, DEGREES_T ) );
                            fp->SetPosition( p.pos );
                        },
                        wxString::FromUTF8( "Agent: " + label ), *out.mutable_edit() );

    if( !targets.empty() )
    {
        for( const FOOTPRINT* fp : targets )
            entry.after.push_back( stateOf( fp ) );

        entry.label = label;
        out.mutable_edit()->set_history_label( label );
        record( std::move( entry ) );
    }

    out.set_moved( (int) targets.size() );
    out.set_kept_current( res.keptStart );

    int zones = 0;

    for( const ZONE* z : b->Zones() )
        zones += !z->GetIsRuleArea() && !z->IsTeardropArea();

    out.set_zones( zones );

    for( const std::string& r : res.unplaced )
        out.add_unplaced( r );

    if( aJob.opt.groups > 0 )
    {
        out.set_groups( aJob.opt.groups );
        out.set_group_source( aJob.groupSource );
        out.set_mixed( res.mixed );
        out.set_mixed_before( res.mixedBefore );
    }

    for( const std::string& v : res.violations )
        out.add_rule_violations( v );

    out.set_on_back( res.onBack );
    out.set_flipped( res.flipped );
    out.set_decaps( res.satellites );
    out.set_decap_before_mm( res.satBefore );
    out.set_decap_after_mm( res.satAfter );
    out.set_hpwl_before_mm( res.hpwlBefore );
    out.set_hpwl_after_mm( res.hpwlAfter );
    return out;
}


void KICADD_HANDLER_PCB::rulesChanged()
{
    m_context->SetContentModified();
    TOOL_MANAGER* mgr = m_context->GetToolManager();

    // as after Board Setup: the editor saves the project with the board, redraws clearances
    if( auto* frame = mgr ? dynamic_cast<PCB_BASE_EDIT_FRAME*>( mgr->GetToolHolder() ) : nullptr )
    {
        frame->Prj().IncrementNetclassesTicker();
        frame->OnModify();
        frame->GetCanvas()->Refresh();
    }
}


void KICADD_HANDLER_PCB::packRules( RulesResponse& aOut )
{
    BOARD*               b = board();
    kicadd::rules::STATE st = kicadd::rules::Capture( *b );
    std::map<wxString, int> nets;

    // A net in two classes has a composite one ("POWER,Power"): count it in each. KiCad also
    // adds Default to a composite to fill in the values a class leaves unset; that does not
    // make the net a Default net, so it counts there only when it is in no other class.
    for( NETINFO_ITEM* net : b->GetNetInfo() )
    {
        if( net->GetNetCode() <= 0 || !net->GetNetClass() )
            continue;

        bool inClass = false;

        for( const NETCLASS* nc : net->GetNetClass()->GetConstituentNetclasses() )
        {
            if( nc && nc->GetName() != NETCLASS::Default && st.classes.count( nc->GetName() ) )
            {
                nets[nc->GetName()]++;
                inClass = true;
            }
        }

        if( !inClass )
            nets[NETCLASS::Default]++;
    }

    auto mmOf = []( int aNm ) { return aNm / 1e6; };

    for( const auto& [name, c] : st.classes )
    {
        NetclassRule* r = aOut.add_classes();
        r->set_name( name.ToStdString() );

        if( c.clearance )     r->set_clearance( mmOf( *c.clearance ) );
        if( c.trackWidth )    r->set_track_width( mmOf( *c.trackWidth ) );
        if( c.viaDiameter )   r->set_via_diameter( mmOf( *c.viaDiameter ) );
        if( c.viaDrill )      r->set_via_drill( mmOf( *c.viaDrill ) );
        if( c.diffPairWidth ) r->set_diff_pair_width( mmOf( *c.diffPairWidth ) );
        if( c.diffPairGap )   r->set_diff_pair_gap( mmOf( *c.diffPairGap ) );

        r->set_nets( nets.count( name ) ? nets[name] : 0 );
    }

    for( const auto& [pattern, nc] : st.patterns )
    {
        NetclassAssign* a = aOut.add_patterns();
        a->set_pattern( pattern.ToStdString() );
        a->set_netclass( nc.ToStdString() );
    }

    BoardConstraints* k = aOut.mutable_constraints();
    k->set_min_clearance( mmOf( st.minClearance ) );
    k->set_min_track_width( mmOf( st.minTrackWidth ) );
    k->set_min_via_diameter( mmOf( st.minViaDiameter ) );
    k->set_min_through_drill( mmOf( st.minThroughDrill ) );
    k->set_min_annular_width( mmOf( st.minAnnularWidth ) );
    k->set_hole_clearance( mmOf( st.holeClearance ) );
    k->set_hole_to_hole( mmOf( st.holeToHole ) );
    k->set_copper_edge_clearance( mmOf( st.copperEdgeClearance ) );
    k->set_min_silk_text_height( mmOf( st.minSilkTextHeight ) );
    k->set_min_silk_text_thickness( mmOf( st.minSilkTextThickness ) );
}


HANDLER_RESULT<RulesResponse> KICADD_HANDLER_PCB::handleGetRules( const HANDLER_CONTEXT<GetRules>& aCtx )
{
    if( !board() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    RulesResponse out;
    packRules( out );
    return out;
}


HANDLER_RESULT<RulesResponse> KICADD_HANDLER_PCB::handleSetRules( const HANDLER_CONTEXT<SetRules>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    using kicadd::rules::STATE;
    const SetRules& req = aCtx.Request;
    const STATE     before = kicadd::rules::Capture( *b );
    STATE           st = before;
    const wxString  dflt = NETCLASS::Default;

    auto bad = [&]( const std::string& aMsg ) { return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, aMsg ) ); };

    // a fab's process first; what else is asked goes on top of it
    std::string presetNote;

    if( !req.preset().empty() )
    {
        std::optional<STATE> p = kicadd::setup::Preset( req.preset(), st, presetNote );

        if( !p )
            return bad( "no preset " + req.preset() + "; " + presetNote );

        st = *p;
    }
    auto nm = []( double aMm ) { return (int) std::llround( aMm * 1e6 ); };

    for( const NetclassRule& c : req.classes() )
    {
        if( c.name().empty() || c.name().find( ',' ) != std::string::npos )
            return bad( "a net class needs a name without commas" );

        const wxString          name = wxString::FromUTF8( c.name() );
        kicadd::rules::CLASS&   k = st.classes[name];

        auto set = [&]( bool aHas, double aMm, std::optional<int>& aField, const char* aWhat ) -> std::string
        {
            if( !aHas )
                return "";

            if( aMm < 0 && name == dflt )
                return std::string( "the Default class needs a " ) + aWhat;

            aField = aMm < 0 ? std::nullopt : std::optional<int>( nm( aMm ) );
            return "";
        };

        for( const std::string& e : { set( c.has_clearance(), c.clearance(), k.clearance, "clearance" ),
                                      set( c.has_track_width(), c.track_width(), k.trackWidth, "track width" ),
                                      set( c.has_via_diameter(), c.via_diameter(), k.viaDiameter, "via diameter" ),
                                      set( c.has_via_drill(), c.via_drill(), k.viaDrill, "via drill" ),
                                      set( c.has_diff_pair_width(), c.diff_pair_width(), k.diffPairWidth, "diff pair width" ),
                                      set( c.has_diff_pair_gap(), c.diff_pair_gap(), k.diffPairGap, "diff pair gap" ) } )
        {
            if( !e.empty() )
                return bad( e );
        }

        if( k.viaDiameter && k.viaDrill && *k.viaDrill >= *k.viaDiameter )
            return bad( c.name() + ": the via drill must be smaller than its diameter" );
    }

    for( const std::string& r : req.remove_classes() )
    {
        const wxString name = wxString::FromUTF8( r );

        if( name == dflt )
            return bad( "the Default class cannot be removed" );

        if( !st.classes.erase( name ) )
            return bad( "no net class " + r );

        std::erase_if( st.patterns, [&]( const auto& p ) { return p.second == name; } );
    }

    for( const NetclassAssign& a : req.assign() )
    {
        const wxString pattern = wxString::FromUTF8( a.pattern() ), nc = wxString::FromUTF8( a.netclass() );

        if( pattern.IsEmpty() )
            return bad( "an assignment needs a net name pattern" );

        auto it = std::find_if( st.patterns.begin(), st.patterns.end(),
                                [&]( const auto& p ) { return p.first == pattern; } );

        if( nc.IsEmpty() )
        {
            if( it == st.patterns.end() )
                return bad( "no assignment for the pattern " + a.pattern() );

            st.patterns.erase( it );
            continue;
        }

        if( !st.classes.count( nc ) )
            return bad( "no net class " + a.netclass() + " (add it in classes)" );

        if( it != st.patterns.end() )
            it->second = nc;
        else
            st.patterns.emplace_back( pattern, nc );
    }

    const BoardConstraints& k = req.constraints();

    if( k.has_min_clearance() )         st.minClearance = nm( k.min_clearance() );
    if( k.has_min_track_width() )       st.minTrackWidth = nm( k.min_track_width() );
    if( k.has_min_via_diameter() )      st.minViaDiameter = nm( k.min_via_diameter() );
    if( k.has_min_through_drill() )     st.minThroughDrill = nm( k.min_through_drill() );
    if( k.has_min_annular_width() )     st.minAnnularWidth = nm( k.min_annular_width() );
    if( k.has_hole_clearance() )        st.holeClearance = nm( k.hole_clearance() );
    if( k.has_hole_to_hole() )          st.holeToHole = nm( k.hole_to_hole() );
    if( k.has_copper_edge_clearance() ) st.copperEdgeClearance = nm( k.copper_edge_clearance() );
    if( k.has_min_silk_text_height() )  st.minSilkTextHeight = nm( k.min_silk_text_height() );
    if( k.has_min_silk_text_thickness() ) st.minSilkTextThickness = nm( k.min_silk_text_thickness() );

    RulesResponse out;
    out.set_note( presetNote );

    if( !( st == before ) )
    {
        kicadd::rules::Apply( *b, st );
        rulesChanged();

        std::vector<std::string> what;

        if( req.classes_size() )
            what.push_back( std::to_string( req.classes_size() ) + " net class(es)" );

        if( req.remove_classes_size() )
            what.push_back( std::to_string( req.remove_classes_size() ) + " removed" );

        if( req.assign_size() )
            what.push_back( std::to_string( req.assign_size() ) + " assignment(s)" );

        if( req.has_constraints() )
            what.push_back( "constraints" );

        if( !req.preset().empty() )
            what.insert( what.begin(), "preset " + req.preset() );

        std::string label = "set rules:";

        for( size_t i = 0; i < what.size(); ++i )
            label += ( i ? ", " : " " ) + what[i];

        HISTORY_ENTRY entry;
        entry.label = label;
        entry.rulesBefore = std::make_shared<STATE>( before );
        entry.rulesAfter = std::make_shared<STATE>( st );
        record( std::move( entry ) );
        out.set_history_label( label );
    }

    packRules( out );
    return out;
}


KICADD_HANDLER_PCB::ZONE_STATE KICADD_HANDLER_PCB::zoneStateOf( const ZONE* aZone )
{
    ZONE_STATE st;
    st.id = aZone->m_Uuid;
    st.clearance = aZone->GetLocalClearance().value_or( 0 );
    st.minWidth = aZone->GetMinThickness();
    st.gap = aZone->GetThermalReliefGap();
    st.spoke = aZone->GetThermalReliefSpokeWidth();
    st.priority = aZone->GetAssignedPriority();
    st.connection = (int) aZone->GetPadConnection();
    return st;
}


void KICADD_HANDLER_PCB::setZoneState( ZONE* aZone, const ZONE_STATE& aState )
{
    aZone->SetLocalClearance( aState.clearance );
    aZone->SetMinThickness( aState.minWidth );
    aZone->SetThermalReliefGap( aState.gap );
    aZone->SetThermalReliefSpokeWidth( aState.spoke );
    aZone->SetAssignedPriority( aState.priority );
    aZone->SetPadConnection( (ZONE_CONNECTION) aState.connection );
}


HANDLER_RESULT<ZonesResponse> KICADD_HANDLER_PCB::handleZones( const HANDLER_CONTEXT<Zones>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const Zones& req = aCtx.Request;
    const bool   change = req.has_clearance() || req.has_min_width() || req.has_priority() || !req.connection().empty()
                        || req.has_thermal_gap() || req.has_thermal_spoke();
    const bool   chosen = req.all() || !req.nets().empty() || !req.layers().empty() || !req.ids().empty();

    auto bad = [&]( const std::string& aMsg ) { return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, aMsg ) ); };

    if( change && !chosen )
        return bad( "which zones: nets, layers, ids or all=true" );

    std::optional<ZONE_CONNECTION> conn;

    if( req.connection() == "solid" )             conn = ZONE_CONNECTION::FULL;
    else if( req.connection() == "thermal" )      conn = ZONE_CONNECTION::THERMAL;
    else if( req.connection() == "none" )         conn = ZONE_CONNECTION::NONE;
    else if( req.connection() == "thermal_tht" )  conn = ZONE_CONNECTION::THT_THERMAL;
    else if( !req.connection().empty() )
        return bad( "connection is solid, thermal, none or thermal_tht" );

    std::vector<ZONE*> zones;

    for( ZONE* zone : b->Zones() )
    {
        if( zone->GetIsRuleArea() || zone->IsTeardropArea() )
            continue;

        bool take = req.all() || !chosen;

        for( const std::string& n : req.nets() )
            take |= zone->GetNetname() == wxString::FromUTF8( n ) || zone->GetNetname() == wxS( "/" ) + wxString::FromUTF8( n );

        for( const std::string& l : req.layers() )
            take |= zone->GetLayerSet().Contains( b->GetLayerID( wxString::FromUTF8( l ) ) );

        for( const std::string& id : req.ids() )
            take |= zone->m_Uuid.AsStdString().rfind( id, 0 ) == 0;

        if( take )
            zones.push_back( zone );
    }

    if( change && zones.empty() )
        return bad( "no copper zone matches" );

    ZonesResponse out;

    if( req.has_create() )
    {
        if( change || chosen )
            return bad( "create makes a new zone: give its settings inside create, not the zones to change" );

        if( std::optional<ApiResponseStatus> wait = busy() )
            return tl::unexpected( *wait );

        std::string err, label;
        std::vector<std::string> notes;
        ZONE*                    zone = createZone( req.create(), err, label, notes );

        if( !zone )
            return bad( err );

        for( const std::string& n : notes )
            out.add_notes( n );

        zones = { zone };
        out.set_history_label( label );
        out.set_changed( 1 );
    }
    else if( change )
    {
        if( std::optional<ApiResponseStatus> wait = busy() )
            return tl::unexpected( *wait );

        auto nm = []( double aMm ) { return (int) std::llround( aMm * 1e6 ); };
        HISTORY_ENTRY entry;
        MoveResponse  ignored;

        for( ZONE* zone : zones )
            entry.zonesBefore.push_back( zoneStateOf( zone ) );

        runCommit(
                [&]( BOARD_COMMIT& aCommit )
                {
                    for( ZONE* zone : zones )
                    {
                        aCommit.Modify( zone );

                        if( req.has_clearance() )     zone->SetLocalClearance( nm( req.clearance() ) );
                        if( req.has_min_width() )     zone->SetMinThickness( nm( req.min_width() ) );
                        if( req.has_priority() )      zone->SetAssignedPriority( (unsigned) std::max( 0, req.priority() ) );
                        if( conn )                    zone->SetPadConnection( *conn );
                        if( req.has_thermal_gap() )   zone->SetThermalReliefGap( nm( req.thermal_gap() ) );
                        if( req.has_thermal_spoke() ) zone->SetThermalReliefSpokeWidth( nm( req.thermal_spoke() ) );
                    }

                    return std::vector<KIID>();
                },
                wxString::Format( wxS( "Agent: zone settings (%d)" ), (int) zones.size() ), ignored );

        for( ZONE* zone : zones )
            entry.zonesAfter.push_back( zoneStateOf( zone ) );

        if( entry.zonesAfter != entry.zonesBefore )
        {
            entry.label = "zone settings of " + std::to_string( zones.size() ) + " zone(s)";
            out.set_history_label( entry.label );
            out.set_changed( (int) zones.size() );
            record( std::move( entry ) );
        }
    }

    auto mmOf = []( int aNm ) { return aNm / 1e6; };

    for( ZONE* zone : zones )
    {
        ZoneInfo* z = out.add_zones();
        z->set_id( zone->m_Uuid.AsStdString() );   // older files have ids that differ only at the end
        z->set_name( zone->GetZoneName().ToStdString() );
        z->set_net( zone->GetNetname().ToStdString() );

        for( PCB_LAYER_ID layer : zone->GetLayerSet().Seq() )
            z->add_layers( LSET::Name( layer ).ToStdString() );

        z->set_priority( (int) zone->GetAssignedPriority() );
        z->set_clearance( mmOf( zone->GetLocalClearance().value_or( 0 ) ) );
        z->set_min_width( mmOf( zone->GetMinThickness() ) );

        switch( zone->GetPadConnection() )
        {
        case ZONE_CONNECTION::FULL:        z->set_connection( "solid" ); break;
        case ZONE_CONNECTION::THERMAL:     z->set_connection( "thermal" ); break;
        case ZONE_CONNECTION::NONE:        z->set_connection( "none" ); break;
        case ZONE_CONNECTION::THT_THERMAL: z->set_connection( "thermal_tht" ); break;
        default:                           z->set_connection( "inherited" ); break;
        }

        z->set_thermal_gap( mmOf( zone->GetThermalReliefGap() ) );
        z->set_thermal_spoke( mmOf( zone->GetThermalReliefSpokeWidth() ) );
        z->set_filled( zone->IsFilled() );
    }

    return out;
}


KICADD_HANDLER_PCB::HISTORY_ENTRY::ITEM_SNAPSHOT KICADD_HANDLER_PCB::snapshotItem( const BOARD_ITEM* aItem )
{
    BOARD_ITEM* copy = static_cast<BOARD_ITEM*>( aItem->Clone() );
    wxString    net;

    if( copy->IsConnected() )
    {
        BOARD_CONNECTED_ITEM* ci = static_cast<BOARD_CONNECTED_ITEM*>( copy );
        net = ci->GetNetCode() > 0 ? ci->GetNetname() : wxString();
        ci->SetNet( NETINFO_LIST::OrphanedItem() );
    }

    copy->SetParent( nullptr );
    return { std::shared_ptr<BOARD_ITEM>( copy ), net };
}


namespace
{

std::string mm4( double aMm )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.4g", aMm );
    return buf;
}

} // namespace


ZONE* KICADD_HANDLER_PCB::createZone( const ZoneCreate& aReq, std::string& aError, std::string& aLabel,
                                      std::vector<std::string>& aNotes )
{
    BOARD* b = board();
    auto   nm = []( double aMm ) { return (int) std::llround( aMm * 1e6 ); };

    NETINFO_ITEM* net = b->FindNet( NETINFO_LIST::UNCONNECTED );

    if( !aReq.net().empty() )
    {
        net = b->FindNet( wxString::FromUTF8( aReq.net() ) );

        if( !net )
            net = b->FindNet( wxS( "/" ) + wxString::FromUTF8( aReq.net() ) );

        if( !net )
        {
            aError = "no net " + aReq.net();
            return nullptr;
        }
    }

    // layers: default the outer ones
    LSET layers;
    const LSET copper = b->GetEnabledLayers() & LSET::AllCuMask();

    if( aReq.layers().empty() )
    {
        layers = LSET( { F_Cu, B_Cu } ) & copper;
    }
    else
    {
        for( const std::string& l : aReq.layers() )
        {
            if( l == "all" )
            {
                layers |= copper;
                continue;
            }

            int id = b->GetLayerID( wxString::FromUTF8( l ) );

            if( id < 0 || !copper.Contains( ToLAYER_ID( id ) ) )
            {
                aError = "no copper layer " + l + " on this board";
                return nullptr;
            }

            layers.set( ToLAYER_ID( id ) );
        }
    }

    std::optional<ZONE_CONNECTION> conn;

    if( aReq.connection() == "solid" )             conn = ZONE_CONNECTION::FULL;
    else if( aReq.connection() == "thermal" )      conn = ZONE_CONNECTION::THERMAL;
    else if( aReq.connection() == "none" )         conn = ZONE_CONNECTION::NONE;
    else if( aReq.connection() == "thermal_tht" )  conn = ZONE_CONNECTION::THT_THERMAL;
    else if( !aReq.connection().empty() )
    {
        aError = "connection is solid, thermal, none or thermal_tht";
        return nullptr;
    }

    // the outline: the points given, else the board's (its biggest piece)
    SHAPE_LINE_CHAIN chain;

    if( aReq.outline_size() > 0 )
    {
        if( aReq.outline_size() < 3 )
        {
            aError = "an outline needs at least 3 points";
            return nullptr;
        }

        for( const Point& p : aReq.outline() )
            chain.Append( VECTOR2I( p.x(), p.y() ) );
    }
    else
    {
        SHAPE_POLY_SET outline;

        if( !b->GetBoardPolygonOutlines( outline, false ) || outline.OutlineCount() == 0 )
        {
            aError = "the board has no closed outline on Edge.Cuts (board_outline draws one), or give outline points";
            return nullptr;
        }

        int best = 0;

        for( int i = 1; i < outline.OutlineCount(); ++i )
        {
            if( std::abs( outline.Outline( i ).Area() ) > std::abs( outline.Outline( best ).Area() ) )
                best = i;
        }

        chain = outline.Outline( best );
    }

    chain.SetClosed( true );

    // The zones of other nets it shares copper with. Where two overlap, the one of the higher
    // priority is filled and the other keeps clear of it; at the same priority KiCad breaks the
    // tie by the zones' ids, which is no one's intent.
    struct OVERLAP { const ZONE* zone; double area; std::string layers; };
    std::vector<OVERLAP> overlaps;
    SHAPE_POLY_SET       mine;
    mine.AddOutline( chain );
    mine.Simplify();

    for( const ZONE* other : b->Zones() )
    {
        LSET shared = other->GetLayerSet() & layers;

        if( other->GetIsRuleArea() || other->IsTeardropArea() || other->GetNetCode() == net->GetNetCode()
            || shared.none() || !other->GetBoundingBox().Intersects( mine.BBox() ) )
        {
            continue;
        }

        SHAPE_POLY_SET theirs = *other->Outline();
        theirs.ClearArcs();
        SHAPE_POLY_SET both = theirs;
        both.BooleanIntersection( mine );

        if( both.Area() < 1e10 )   // 0.01 mm^2
            continue;

        std::string names;

        for( PCB_LAYER_ID l : shared.Seq() )
            names += ( names.empty() ? "" : "+" ) + LSET::Name( l ).ToStdString();

        overlaps.push_back( { other, theirs.Area(), names } );
    }

    auto zoneName = []( const OVERLAP& aOverlap )
    {
        return ( aOverlap.zone->GetNetCode() > 0 ? aOverlap.zone->GetNetname().ToStdString() : std::string( "(no net)" ) )
               + " on " + aOverlap.layers;
    };

    unsigned priority = (unsigned) std::max( 0, aReq.priority() );

    // No priority given: a zone inside bigger ones goes above them (an island of +3V3 in the GND pour)
    if( !aReq.has_priority() )
    {
        std::string under;

        for( const OVERLAP& o : overlaps )
        {
            if( o.area > mine.Area() * 1.05 )
            {
                priority = std::max( priority, o.zone->GetAssignedPriority() + 1 );
                under += ( under.empty() ? "" : ", " ) + zoneName( o );
            }
        }

        if( priority > 0 )
        {
            aNotes.push_back( "priority " + std::to_string( priority ) + ": above the bigger zone(s) it lies in (" + under
                              + "), which keep clear of it" );
        }
    }

    for( const OVERLAP& o : overlaps )
    {
        if( o.zone->GetAssignedPriority() == priority )
        {
            aNotes.push_back( "warning: it overlaps the zone " + zoneName( o ) + " at the same priority "
                              + std::to_string( priority ) + ": which of the two is filled where they overlap is "
                              "arbitrary. Give one the higher priority (it is filled, the other keeps clear of it): "
                              "zones ids=[\"" + o.zone->m_Uuid.AsStdString() + "\"] priority="
                              + std::to_string( priority + 1 ) + " for that one, or undo and create with priority / "
                              "an outline that does not overlap" );
        }
    }

    ZONE* zone = new ZONE( b );
    b->GetDesignSettings().GetDefaultZoneSettings().ExportSetting( *zone );
    zone->SetLayerSet( layers );
    zone->SetNet( net );
    zone->Outline()->RemoveAllContours();
    zone->Outline()->AddOutline( chain );
    zone->SetAssignedPriority( priority );
    zone->SetHatchStyle( ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_EDGE );
    zone->SetIsFilled( false );

    if( aReq.has_clearance() )  zone->SetLocalClearance( nm( aReq.clearance() ) );
    if( aReq.has_min_width() )  zone->SetMinThickness( nm( aReq.min_width() ) );
    if( conn )                  zone->SetPadConnection( *conn );
    if( !aReq.name().empty() )  zone->SetZoneName( wxString::FromUTF8( aReq.name() ) );

    std::string layerNames;

    for( PCB_LAYER_ID l : layers.Seq() )
        layerNames += ( layerNames.empty() ? "" : "+" ) + LSET::Name( l ).ToStdString();

    aLabel = "new zone " + ( net->GetNetCode() > 0 ? net->GetNetname().ToStdString() : std::string( "(no net)" ) )
             + " on " + layerNames;

    MoveResponse ignored;
    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                aCommit.Add( zone );
                return std::vector<KIID>();
            },
            wxString::FromUTF8( "Agent: " + aLabel ), ignored );

    HISTORY_ENTRY entry;
    entry.label = aLabel;
    entry.itemsAdded.push_back( snapshotItem( zone ) );
    record( std::move( entry ) );
    return zone;
}


HANDLER_RESULT<BoardOutlineResponse> KICADD_HANDLER_PCB::handleBoardOutline( const HANDLER_CONTEXT<BoardOutline>& aCtx )
{
    if( std::optional<ApiResponseStatus> wait = busy() )
        return tl::unexpected( *wait );

    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const BoardOutline& req = aCtx.Request;
    auto bad = [&]( const std::string& aMsg ) { return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, aMsg ) ); };
    auto nm = []( double aMm ) { return (int) std::llround( aMm * 1e6 ); };
    const int lineWidth = 100000;   // 0.1 mm, as KiCad's default for Edge.Cuts

    // One shape (rect / rounded rect / circle) on Edge.Cuts
    auto makeShape = [&]( const OutlineShape& aShape, const std::string& aWhat, std::string& aErr ) -> PCB_SHAPE*
    {
        if( aShape.rect_size() == 4 && aShape.circle_size() == 0 )
        {
            double x0 = std::min( aShape.rect( 0 ), aShape.rect( 2 ) ), x1 = std::max( aShape.rect( 0 ), aShape.rect( 2 ) );
            double y0 = std::min( aShape.rect( 1 ), aShape.rect( 3 ) ), y1 = std::max( aShape.rect( 1 ), aShape.rect( 3 ) );

            if( x1 - x0 < 1 || y1 - y0 < 1 )
            {
                aErr = aWhat + ": a rect of at least 1 x 1 mm";
                return nullptr;
            }

            if( aShape.corner_radius() < 0 || aShape.corner_radius() * 2 > std::min( x1 - x0, y1 - y0 ) )
            {
                aErr = aWhat + ": the corner radius is at most half the shorter side";
                return nullptr;
            }

            PCB_SHAPE* s = new PCB_SHAPE( b, SHAPE_T::RECTANGLE );
            s->SetStart( VECTOR2I( nm( x0 ), nm( y0 ) ) );
            s->SetEnd( VECTOR2I( nm( x1 ), nm( y1 ) ) );
            s->SetCornerRadius( nm( aShape.corner_radius() ) );
            s->SetLayer( Edge_Cuts );
            s->SetWidth( lineWidth );
            return s;
        }

        if( aShape.circle_size() == 3 && aShape.rect_size() == 0 )
        {
            if( aShape.circle( 2 ) < 0.5 )
            {
                aErr = aWhat + ": a circle of at least 0.5 mm";
                return nullptr;
            }

            PCB_SHAPE* s = new PCB_SHAPE( b, SHAPE_T::CIRCLE );
            VECTOR2I   c( nm( aShape.circle( 0 ) ), nm( aShape.circle( 1 ) ) );
            s->SetCenter( c );
            s->SetEnd( c + VECTOR2I( nm( aShape.circle( 2 ) / 2 ), 0 ) );
            s->SetLayer( Edge_Cuts );
            s->SetWidth( lineWidth );
            return s;
        }

        aErr = aWhat + ": rect [x0, y0, x1, y1] (corner_radius) or circle [cx, cy, diameter], in mm";
        return nullptr;
    };

    std::vector<std::unique_ptr<PCB_SHAPE>> shapes;
    std::string                             err;

    if( req.has_outline() )
    {
        PCB_SHAPE* s = makeShape( req.outline(), "outline", err );

        if( !s )
            return bad( err );

        shapes.emplace_back( s );
    }
    else if( !req.keep_old() )
    {
        return bad( "outline: rect [x0, y0, x1, y1] or circle [cx, cy, d] (keep_old adds cut-outs / holes to the outline there is)" );
    }

    for( int i = 0; i < req.cutouts_size(); ++i )
    {
        PCB_SHAPE* s = makeShape( req.cutouts( i ), "cut-out " + std::to_string( i + 1 ), err );

        if( !s )
            return bad( err );

        shapes.emplace_back( s );
    }

    // The outline as it will be: the new shapes (and the old ones, kept)
    std::vector<PCB_SHAPE*> old;

    for( BOARD_ITEM* item : b->Drawings() )
    {
        if( item->Type() == PCB_SHAPE_T && item->GetLayer() == Edge_Cuts )
            old.push_back( static_cast<PCB_SHAPE*>( item ) );
    }

    SHAPE_POLY_SET              outline, outer;
    std::vector<SHAPE_POLY_SET> cuts;   // the new cut-outs, one each
    BOX2I                       box;

    {
        // the outer shape and the cut-outs as holes in it
        SHAPE_POLY_SET holes;

        // the area a shape closes (an outline is drawn as a line: filled for this)
        auto polyOf = [&]( const PCB_SHAPE* aShape, SHAPE_POLY_SET& aInto )
        {
            std::unique_ptr<PCB_SHAPE> area( static_cast<PCB_SHAPE*>( aShape->Clone() ) );
            area->SetFilled( true );
            area->SetWidth( 0 );
            area->TransformShapeToPolygon( aInto, Edge_Cuts, 0, ARC_HIGH_DEF, ERROR_INSIDE, true );
            aInto.Simplify();
        };

        if( req.has_outline() )
        {
            polyOf( shapes.front().get(), outer );

            for( size_t i = 1; i < shapes.size(); ++i )
                polyOf( shapes[i].get(), cuts.emplace_back() );
        }
        else
        {
            if( !b->GetBoardPolygonOutlines( outer, false ) || outer.OutlineCount() == 0 )
                return bad( "the board has no closed outline to add to: give outline" );

            for( const auto& s : shapes )
                polyOf( s.get(), cuts.emplace_back() );
        }

        for( const SHAPE_POLY_SET& cut : cuts )
            holes.BooleanAdd( cut );

        outline = outer;

        if( holes.OutlineCount() )
        {
            SHAPE_POLY_SET inside = holes;
            inside.BooleanSubtract( outer );

            if( inside.OutlineCount() )
                return bad( "a cut-out reaches outside the outline" );

            outline.BooleanSubtract( holes );
        }

        box = outer.BBox();
    }

    // Mounting holes: given, and / or at the corners
    struct HOLE { VECTOR2I pos; int drill; bool plated; wxString net; wxString ref; };
    std::vector<HOLE> holes;

    for( const MountingHole& h : req.holes() )
    {
        double d = h.diameter() > 0 ? h.diameter() : 3.2;
        holes.push_back( { VECTOR2I( nm( h.x() ), nm( h.y() ) ), nm( d ), h.plated(), wxString::FromUTF8( h.net() ) } );
    }

    if( req.hole_inset() > 0 )
    {
        int d = nm( req.hole_diameter() > 0 ? req.hole_diameter() : 3.2 ), in = nm( req.hole_inset() );

        if( 2 * in >= box.GetWidth() || 2 * in >= box.GetHeight() )
            return bad( "hole_inset: the board is too small for holes that far in" );

        for( const VECTOR2I& c : { VECTOR2I( box.GetLeft() + in, box.GetTop() + in ), VECTOR2I( box.GetRight() - in, box.GetTop() + in ),
                                   VECTOR2I( box.GetRight() - in, box.GetBottom() - in ), VECTOR2I( box.GetLeft() + in, box.GetBottom() - in ) } )
        {
            holes.push_back( { c, d, false, wxString() } );
        }
    }

    for( const HOLE& h : holes )
    {
        if( h.drill < 500000 )
            return bad( "a mounting hole of at least 0.5 mm" );

        if( !outline.Contains( h.pos ) )
            return bad( "a hole at " + mm4( h.pos.x / 1e6 ) + "," + mm4( h.pos.y / 1e6 ) + " is not on the board" );

        if( !h.net.IsEmpty() && !b->FindNet( h.net ) )
            return bad( "no net " + h.net.ToStdString() );
    }

    // H1, H2 ...: the next free numbers
    int next = 1;

    for( const FOOTPRINT* fp : b->Footprints() )
    {
        wxString ref = fp->GetReference();
        long     n = 0;

        if( ref.StartsWith( wxS( "H" ) ) && ref.Mid( 1 ).ToLong( &n ) )
            next = std::max( next, (int) n + 1 );
    }

    BoardOutlineResponse out;
    HISTORY_ENTRY        entry;
    std::vector<KIID>    holeIds;
    MoveResponse         ignored;
    auto xy = []( const VECTOR2I& aPos ) { return mm4( aPos.x / 1e6 ) + "," + mm4( aPos.y / 1e6 ); };

    // A hole where a part already has one (a board that has its mounting holes): left out
    holes.erase( std::remove_if( holes.begin(), holes.end(),
                                 [&]( const HOLE& h )
                                 {
                                     for( const FOOTPRINT* fp : b->Footprints() )
                                     {
                                         for( const PAD* pad : fp->Pads() )
                                         {
                                             int theirs = std::max( pad->GetDrillSize().x, pad->GetDrillSize().y );

                                             if( pad->HasHole() && ( pad->GetPosition() - h.pos ).EuclideanNorm() < ( theirs + h.drill ) / 2 )
                                             {
                                                 out.add_problems( "no hole at " + xy( h.pos ) + ": " + fp->GetReference().ToStdString()
                                                                   + " has a hole there already (" + mm4( theirs / 1e6 ) + " mm)" );
                                                 return true;
                                             }
                                         }
                                     }

                                     return false;
                                 } ),
                 holes.end() );

    for( HOLE& h : holes )
        h.ref = wxString::Format( wxS( "H%d" ), next++ );

    // What the new holes and cut-outs run into (DRC says the same, later and in more words)
    auto netsOf = []( const std::set<std::string>& aNets )
    {
        std::string list;
        int         n = 0;

        for( const std::string& net : aNets )
        {
            if( n++ < 6 )
                list += ( list.empty() ? "" : ", " ) + net;
        }

        return list.empty() ? std::string() : " (" + list + ( n > 6 ? ", ..." : "" ) + ")";
    };

    auto count = []( int aTracks, int aVias )
    {
        std::string t = aTracks ? std::to_string( aTracks ) + " track(s)" : std::string();

        if( aVias )
            t += ( t.empty() ? "" : " and " ) + std::to_string( aVias ) + " via(s)";

        return t;
    };

    // the tracks and vias on an area
    auto on = [&]( const std::function<bool( const PCB_TRACK*, const SHAPE* )>& aHit, int& aTracks, int& aVias,
                   std::set<std::string>& aNets )
    {
        for( const PCB_TRACK* track : b->Tracks() )
        {
            std::shared_ptr<SHAPE> shape = track->GetEffectiveShape();

            if( !aHit( track, shape.get() ) )
                continue;

            ( track->Type() == PCB_VIA_T ? aVias : aTracks )++;

            if( track->GetNetCode() > 0 )
                aNets.insert( track->GetNetname().ToStdString() );
        }
    };

    for( const HOLE& h : holes )
    {
        const int    size = h.plated ? KiROUND( h.drill * 1.8 ) : h.drill;
        const int    keep = KiROUND( h.drill * 1.8 ) / 2 + 250000;   // its courtyard
        SHAPE_CIRCLE body( h.pos, size / 2 );
        std::string  parts;
        int          nParts = 0;

        for( FOOTPRINT* fp : b->Footprints() )
        {
            bool hit = false;

            for( PCB_LAYER_ID side : { F_CrtYd, B_CrtYd } )
            {
                const SHAPE_POLY_SET& courtyard = fp->GetCourtyard( side );
                hit = hit || ( courtyard.OutlineCount() && courtyard.Collide( h.pos, keep ) );
            }

            for( const PAD* pad : fp->Pads() )
            {
                for( PCB_LAYER_ID side : { F_Cu, B_Cu } )
                    hit = hit || ( pad->IsOnLayer( side ) && pad->GetEffectiveShape( side )->Collide( &body, 0 ) );
            }

            if( hit && nParts++ < 6 )
                parts += ( parts.empty() ? "" : ", " ) + fp->GetReference().ToStdString();
        }

        if( nParts )
        {
            out.add_problems( h.ref.ToStdString() + " @" + xy( h.pos ) + " is on " + parts + ( nParts > 6 ? ", ..." : "" )
                              + ": move the hole or the part" );
        }

        int                   tracks = 0, vias = 0;
        std::set<std::string> nets;

        on( [&]( const PCB_TRACK* aTrack, const SHAPE* aShape )
            {
                // a plated hole takes the copper of its own net
                return !( h.plated && !h.net.IsEmpty() && aTrack->GetNetname() == h.net ) && aShape->Collide( &body, 0 );
            },
            tracks, vias, nets );

        if( tracks + vias )
        {
            out.add_problems( h.ref.ToStdString() + " @" + xy( h.pos ) + " goes through " + count( tracks, vias ) + netsOf( nets )
                              + ": move the hole, or reroute them" );
        }
    }

    for( size_t i = 0; i < cuts.size(); ++i )
    {
        int                   tracks = 0, vias = 0;
        std::set<std::string> nets;

        on( [&]( const PCB_TRACK*, const SHAPE* aShape ) { return cuts[i].Collide( aShape, 0 ); }, tracks, vias, nets );

        if( tracks + vias )
        {
            out.add_problems( "cut-out " + std::to_string( i + 1 ) + " cuts through " + count( tracks, vias ) + netsOf( nets )
                              + ": reroute them clear of it, or move the cut-out" );
        }
    }

    if( req.has_outline() )
    {
        // what the new outline leaves outside, or cuts at the edge
        BOX2I around = box;
        around.Merge( b->ComputeBoundingBox( false ) );
        around.Inflate( 1000000 );

        SHAPE_POLY_SET off;
        off.NewOutline();
        off.Append( around.GetLeft(), around.GetTop() );
        off.Append( around.GetRight(), around.GetTop() );
        off.Append( around.GetRight(), around.GetBottom() );
        off.Append( around.GetLeft(), around.GetBottom() );
        off.BooleanSubtract( outer );

        int                   tracks = 0, vias = 0;
        std::set<std::string> nets;

        on( [&]( const PCB_TRACK*, const SHAPE* aShape ) { return off.Collide( aShape, 0 ); }, tracks, vias, nets );

        if( tracks + vias )
            out.add_problems( count( tracks, vias ) + netsOf( nets ) + " are outside the new outline, or cross it" );
    }

    // Only holes asked, and they are all there already: nothing to do
    if( shapes.empty() && holes.empty() )
    {
        out.set_width( box.GetWidth() / 1e6 );
        out.set_height( box.GetHeight() / 1e6 );
        out.set_area( outline.Area() / 1e12 );
        return out;
    }

    if( !req.keep_old() )
    {
        for( PCB_SHAPE* s : old )
            entry.itemsRemoved.push_back( snapshotItem( s ) );
    }

    runCommit(
            [&]( BOARD_COMMIT& aCommit )
            {
                if( !req.keep_old() )
                {
                    for( PCB_SHAPE* s : old )
                        aCommit.Remove( s );
                }

                for( auto& s : shapes )
                {
                    entry.itemsAdded.push_back( snapshotItem( s.get() ) );
                    aCommit.Add( s.release() );
                }

                for( const HOLE& h : holes )
                {
                    FOOTPRINT* fp = new FOOTPRINT( b );
                    char       name[64];
                    std::snprintf( name, sizeof( name ), "MountingHole_%gmm%s", h.drill / 1e6, h.plated ? "_Pad" : "" );
                    fp->SetFPID( LIB_ID( wxEmptyString, wxString::FromUTF8( name ) ) );
                    fp->SetReference( h.ref );
                    fp->SetValue( wxS( "MountingHole" ) );
                    fp->Reference().SetVisible( false );
                    fp->Value().SetVisible( false );
                    fp->SetAttributes( FP_EXCLUDE_FROM_BOM | FP_EXCLUDE_FROM_POS_FILES | FP_BOARD_ONLY );

                    PAD* pad = new PAD( fp );
                    int  size = h.plated ? KiROUND( h.drill * 1.8 ) : h.drill;
                    pad->SetAttribute( h.plated ? PAD_ATTRIB::PTH : PAD_ATTRIB::NPTH );
                    pad->SetShape( PADSTACK::ALL_LAYERS, PAD_SHAPE::CIRCLE );
                    pad->SetSize( PADSTACK::ALL_LAYERS, VECTOR2I( size, size ) );
                    pad->SetDrillShape( PAD_DRILL_SHAPE::CIRCLE );
                    pad->SetDrillSize( VECTOR2I( h.drill, h.drill ) );
                    pad->SetLayerSet( h.plated ? PAD::PTHMask() : PAD::UnplatedHoleMask() );
                    pad->SetNumber( wxString( h.plated ? wxS( "1" ) : wxS( "" ) ) );
                    fp->Add( pad );

                    // a courtyard round it, so nothing is placed on the screw head
                    PCB_SHAPE* crt = new PCB_SHAPE( fp, SHAPE_T::CIRCLE );
                    crt->SetCenter( VECTOR2I( 0, 0 ) );
                    crt->SetEnd( VECTOR2I( std::max( size, KiROUND( h.drill * 1.8 ) ) / 2 + 250000, 0 ) );
                    crt->SetLayer( F_CrtYd );
                    crt->SetWidth( 50000 );
                    fp->Add( crt );

                    fp->SetPosition( h.pos );

                    if( h.plated && !h.net.IsEmpty() )
                        pad->SetNet( b->FindNet( h.net ) );

                    aCommit.Add( fp );
                    holeIds.push_back( fp->m_Uuid );

                    out.add_holes( fp->GetReference().ToStdString() + " @" + mm4( h.pos.x / 1e6 ) + ","
                                   + mm4( h.pos.y / 1e6 ) + " d" + mm4( h.drill / 1e6 )
                                   + ( h.plated ? " plated" + ( h.net.IsEmpty() ? std::string() : " " + h.net.ToStdString() )
                                                : std::string() ) );
                }

                return std::vector<KIID>();
            },
            wxS( "Agent: board outline" ), ignored );

    for( const KIID& id : holeIds )
    {
        if( BOARD_ITEM* item = b->ResolveItem( id, true ); item && item->Type() == PCB_FOOTPRINT_T )
            entry.added.push_back( copyOf( static_cast<FOOTPRINT*>( item ) ) );
    }

    // Headless, the old shapes are ours to delete; the editor's undo owns them otherwise
    if( !editorOwnsUndo() && !req.keep_old() )
    {
        for( PCB_SHAPE* s : old )
            delete s;
    }

    entry.label = req.has_outline() ? "board outline" : "cut-outs / holes";
    out.set_history_label( entry.label );
    out.set_removed_shapes( req.keep_old() ? 0 : (int) old.size() );
    record( std::move( entry ) );

    out.set_width( box.GetWidth() / 1e6 );
    out.set_height( box.GetHeight() / 1e6 );
    out.set_area( outline.Area() / 1e12 );

    // parts (partly) off the new outline
    std::set<KIID> mine( holeIds.begin(), holeIds.end() );

    for( FOOTPRINT* fp : b->Footprints() )
    {
        if( mine.count( fp->m_Uuid ) )
            continue;

        BOX2I fb = fp->GetBoundingBox( false );
        bool  in = outline.Contains( fb.GetOrigin() ) && outline.Contains( fb.GetEnd() )
                  && outline.Contains( VECTOR2I( fb.GetRight(), fb.GetTop() ) )
                  && outline.Contains( VECTOR2I( fb.GetLeft(), fb.GetBottom() ) );

        // ... or on a cut-out
        for( size_t i = 0; in && i < cuts.size(); ++i )
        {
            if( !cuts[i].BBox().Intersects( fb ) )
                continue;

            SHAPE_POLY_SET part;
            part.NewOutline();
            part.Append( fb.GetLeft(), fb.GetTop() );
            part.Append( fb.GetRight(), fb.GetTop() );
            part.Append( fb.GetRight(), fb.GetBottom() );
            part.Append( fb.GetLeft(), fb.GetBottom() );
            part.BooleanIntersection( cuts[i] );
            in = part.OutlineCount() == 0;
        }

        if( !in )
            out.add_outside( fp->GetReference().ToStdString() );
    }

    return out;
}


void KICADD_HANDLER_PCB::setupChanged()
{
    m_context->SetContentModified();
    board()->BuildConnectivity();

    TOOL_MANAGER* mgr = m_context->GetToolManager();

    // as after Board Setup: the layer lists and the canvas redone
    if( auto* frame = mgr ? dynamic_cast<PCB_EDIT_FRAME*>( mgr->GetToolHolder() ) : nullptr )
    {
        frame->OnModify();
        frame->UpdateUserInterface();
        frame->ReCreateLayerBox();
        frame->GetCanvas()->Refresh();
    }
}


HANDLER_RESULT<BoardSetupResponse> KICADD_HANDLER_PCB::handleBoardSetup( const HANDLER_CONTEXT<BoardSetup>& aCtx )
{
    BOARD* b = board();

    if( !b )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const BoardSetup& req = aCtx.Request;
    BoardSetupResponse out;

    if( req.has_stackup() && ( req.stackup() == "list" || req.stackup().empty() ) )
    {
        std::string names;

        for( const std::string& n : kicadd::setup::FabStackups() )
            names += ( names.empty() ? "" : " " ) + n;

        out.set_note( "fab stackups: " + names + "\n" + kicadd::setup::FabStackupSource() );
    }

    const bool fab = req.has_stackup() && !out.note().size();
    const bool change = req.has_copper_layers() || req.has_thickness() || req.has_outer_copper_oz()
                        || req.has_inner_copper_oz() || fab;

    if( change )
    {
        if( std::optional<ApiResponseStatus> wait = busy() )
            return tl::unexpected( *wait );

        kicadd::setup::CHANGE c;

        if( req.has_copper_layers() )   c.copperLayers = req.copper_layers();
        if( req.has_thickness() )       c.thickness = (int) std::llround( req.thickness() * 1e6 );
        if( req.has_outer_copper_oz() ) c.outerOz = req.outer_copper_oz();
        if( req.has_inner_copper_oz() ) c.innerOz = req.inner_copper_oz();
        if( fab )                       c.fab = req.stackup();

        kicadd::setup::STATE before = kicadd::setup::Capture( *b );
        std::string          err;
        std::optional<kicadd::setup::STATE> after = kicadd::setup::Changed( *b, before, c, err );

        if( !after )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

        if( !( *after == before ) )
        {
            kicadd::setup::Apply( *b, *after );
            setupChanged();

            char buf[128];
            std::snprintf( buf, sizeof( buf ), "board setup: %s%d layers, %.3g mm",
                           fab ? ( wxString::FromUTF8( req.stackup() ).Upper().ToStdString() + " stackup, " ).c_str() : "",
                           after->copperLayers,
                           after->thickness / 1e6 );

            HISTORY_ENTRY entry;
            entry.label = buf;
            entry.setupBefore = std::make_shared<kicadd::setup::STATE>( before );
            entry.setupAfter = std::make_shared<kicadd::setup::STATE>( *after );
            out.set_history_label( entry.label );
            record( std::move( entry ) );
        }
    }

    kicadd::setup::STATE now = kicadd::setup::Capture( *b );

    if( fab )
    {
        char buf[200];
        std::snprintf( buf, sizeof( buf ), "%s is one of JLCPCB's 1.6 mm stackups; the layers in its table add up "
                       "to %.3f mm (the thickness above). ",
                       wxString::FromUTF8( req.stackup() ).Upper().ToStdString().c_str(), now.thickness / 1e6 );
        out.set_note( buf + kicadd::setup::FabStackupSource() );
    }

    out.set_copper_layers( now.copperLayers );
    out.set_thickness( now.thickness / 1e6 );

    for( const std::string& line : kicadd::setup::Describe( now ) )
        out.add_stackup( line );

    return out;
}
