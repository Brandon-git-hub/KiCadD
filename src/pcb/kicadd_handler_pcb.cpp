/*
 * KiCadD - compact, spatially scoped board API handler.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "kicadd_handler_pcb.h"
#include "kicadd_local_drc.h"

#include <algorithm>
#include <chrono>
#include <set>

#include <api/pcb_context.h>
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
    registerHandler<CheckLocalDrc, LocalDrcResponse>( &KICADD_HANDLER_PCB::handleCheckLocalDrc );
    registerHandler<GetStateDigest, StateDigestResponse>( &KICADD_HANDLER_PCB::handleGetStateDigest );
    registerHandler<SetRatsnestDeferral, RatsnestDeferralResponse>(
            &KICADD_HANDLER_PCB::handleSetRatsnestDeferral );
    registerHandler<FlushRatsnest, FlushRatsnestResponse>( &KICADD_HANDLER_PCB::handleFlushRatsnest );
    registerHandler<VerifyConnectivity, VerifyConnectivityResponse>(
            &KICADD_HANDLER_PCB::handleVerifyConnectivity );
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


HANDLER_RESULT<MoveResponse>
KICADD_HANDLER_PCB::handleMoveDelta( const HANDLER_CONTEXT<MoveDelta>& aCtx )
{
    auto   t0 = std::chrono::steady_clock::now();
    BOARD* b  = board();

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

    if( !targets.empty() )
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

        BOARD_COMMIT    commit( m_context->GetToolManager(), true, false );
        const VECTOR2I  delta( req.dx(), req.dy() );
        const EDA_ANGLE drot( req.drot_decideg() / 10.0, DEGREES_T );

        for( FOOTPRINT* fp : targets )
        {
            commit.Modify( fp );

            if( delta.x != 0 || delta.y != 0 )
                fp->Move( delta );

            if( req.drot_decideg() != 0 )
                fp->Rotate( fp->GetPosition(), drot );
        }

        commit.Push( wxS( "KiCadD MoveDelta" ) );
        m_context->SetContentModified();

        for( FOOTPRINT* fp : targets )
            fp->BuildCourtyardCaches();

        for( const FOOTPRINT* fp : targets )
        {
            MoveResult* r = out.add_moved();
            r->set_ref( fp->GetReference().ToStdString() );
            setPoint( r->mutable_pos(), fp->GetPosition() );
            r->set_rot_decideg( fp->GetOrientation().AsTenthsOfADegree() );
        }
    }

    out.set_elapsed_us( std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0 ).count() );
    {
        std::shared_ptr<CONNECTIVITY_DATA> conn = b->GetConnectivity();
        out.set_unconnected( static_cast<int>( conn->GetUnconnectedCountNoFlush( false ) ) );

        for( int net : conn->GetPendingRatsnestNets() )
            out.add_ratsnest_pending( netName( b, net ) );
    }

    if( !targets.empty() )
    {
        auto d0 = std::chrono::steady_clock::now();

        for( DrcViolation& v : kicadd::RunLocalDrc( b, targets ) )
            *out.add_drc() = std::move( v );

        out.set_drc_us( std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - d0 ).count() );
    }

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
