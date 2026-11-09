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
#include <set>
#include <unordered_map>

#include <api/pcb_context.h>
#include <eda_draw_frame.h>
#include <pcb_base_frame.h>
#include <class_draw_panel_gal.h>
#include <tool/tool_manager.h>
#include <board_design_settings.h>
#include <lib_id.h>
#include <netlist_reader/pcb_netlist_utils.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <board.h>
#include <board_commit.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/seg.h>
#include <geometry/shape_poly_set.h>
#include <cmath>
#include <netinfo.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_algo.h>
#include <ratsnest/ratsnest_data.h>

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
    out.set_layer( aFp->GetLayerName().ToStdString() );
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
        out.set_layer( aPad->GetLayerName().ToStdString() );

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
    out.set_layer( aTrack->GetLayerName().ToStdString() );
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
    registerHandler<DeleteFootprints, MoveResponse>( &KICADD_HANDLER_PCB::handleDeleteFootprints );
    registerHandler<AddFootprint, MoveResponse>( &KICADD_HANDLER_PCB::handleAddFootprint );
    registerHandler<FetchLcsc, FetchLcscResponse>( &KICADD_HANDLER_PCB::handleFetchLcsc );
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


KICADD_HANDLER_PCB::~KICADD_HANDLER_PCB() = default;


void KICADD_HANDLER_PCB::DetachBoard()
{
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
    if( !m_context || m_context->CanAcceptApiCommands() )
        return std::nullopt;

    return makeError( ApiStatusCode::AS_BUSY,
                      "the editor is busy: the user is in an interactive tool (routing, moving, a "
                      "dialog); the edit was not applied" );
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
                          ci.set_layer( z->GetLayerName().ToStdString() );
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
            ci.set_layer( z->GetLayerName().ToStdString() );
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


KICADD_HANDLER_PCB::FP_STATE KICADD_HANDLER_PCB::stateOf( const FOOTPRINT* aFp )
{
    return { aFp->m_Uuid, aFp->GetPosition(), aFp->GetOrientation(), aFp->IsFlipped() };
}


void KICADD_HANDLER_PCB::setState( FOOTPRINT* aFp, const FP_STATE& aState )
{
    // Side first: a flip mirrors the orientation, which is then set exactly.
    if( aFp->IsFlipped() != aState.back )
        aFp->Flip( aFp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

    if( aFp->GetOrientation() != aState.orient )
        aFp->SetOrientation( aState.orient );

    if( aFp->GetPosition() != aState.pos )
        aFp->SetPosition( aState.pos );
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


/// A footprint from a .pretty directory: the project's lcsc.pretty for LCSC:..., then
/// KICADD_FOOTPRINT_DIRS, then the system library.
std::unique_ptr<FOOTPRINT> loadFromDirectories( const BOARD* aBoard, const LIB_ID& aLibId, std::string& aSource )
{
    std::vector<wxString> dirs;

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

    if( const char* env = std::getenv( "KICADD_FOOTPRINT_DIRS" ) )
    {
        wxStringTokenizer tok( wxString::FromUTF8( env ), wxS( ":" ) );

        while( tok.HasMoreTokens() )
            dirs.push_back( tok.GetNextToken() );
    }

    dirs.push_back( wxS( "/usr/share/kicad/footprints" ) );

    for( const wxString& dir : dirs )
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

    if( fp->IsFlipped() )
        fp->Flip( fp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

    fp->SetOrientation( ANGLE_0 );
    return fp;
}

} // namespace


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
            source += ", saved as " + saved.libId + ( saved.registered ? ", LCSC library added to fp-lib-table" : "" );
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

        if( FOOTPRINT* loaded = LoadFootprintFromProject( b, libId ) )
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
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST,
                                              "footprint " + req.lib_id()
                                                      + " not found (library table, KICADD_FOOTPRINT_DIRS, "
                                                        "/usr/share/kicad/footprints, board)" ) );
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


bool KICADD_HANDLER_PCB::applyHistory( const HISTORY_ENTRY& aEntry, bool aUndo, MoveResponse& aOut )
{
    BOARD* b = board();

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

    for( const FP_SNAPSHOT& snap : toRemove )
    {
        if( FOOTPRINT* fp = resolve( snap.fp->m_Uuid ) )
            removing.push_back( fp );
        else
            aOut.add_missing( snap.fp->GetReference().ToStdString() );
    }

    for( const FP_SNAPSHOT& snap : toAdd )
    {
        if( resolve( snap.fp->m_Uuid ) )
            aOut.add_missing( snap.fp->GetReference().ToStdString() + " (already on the board)" );
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
                FP_STATE now = stateOf( aFp );
                return now.pos == e.pos && now.orient == e.orient && now.back == e.back;
            }
        }

        return true;
    };

    for( const FP_STATE& st : states )
    {
        if( FOOTPRINT* fp = resolve( st.id ) )
        {
            if( unchanged( fp ) )
                modifying.emplace_back( fp, &st );
            else
                aOut.add_missing( fp->GetReference().ToStdString() + " (changed since; left as is)" );
        }
        else
        {
            aOut.add_missing( st.id.AsStdString() );
        }
    }

    if( removing.empty() && adding.empty() && modifying.empty() )
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
                    setState( fp, *st );
                    ids.push_back( fp->m_Uuid );
                }

                return ids;
            },
            wxString::FromUTF8( std::string( "Agent: " ) + ( aUndo ? "undo " : "redo " ) + aEntry.label ),
            aOut );

    m_pendingNets.clear();

    // The snapshots stay in the history (undo/redo can repeat); the board copies we just
    // removed are no longer referenced by anything.
    for( FOOTPRINT* fp : removing )
        delete fp;

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
            zf->set_layer( b->GetLayerName( layer ).ToStdString() );

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
                d->set_handle( fp->GetReference().ToStdString() + ":" + item->GetLayerName().ToStdString() );
            }
            else if( item->GetLayer() == Edge_Cuts )
            {
                d->set_kind( IK_SHAPE );
                d->set_handle( "edge" );
            }
            else
            {
                d->set_kind( item->Type() == PCB_SHAPE_T ? IK_SHAPE : IK_TEXT );
                d->set_handle( item->GetLayerName().ToStdString() + ":" + shortId( item ) );
            }

            break;
        }
    }

    return out;
}
