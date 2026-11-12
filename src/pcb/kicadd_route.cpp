/*
 * KiCadD: tracks drawn by KiCad's router without a canvas (see kicadd_route.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_route.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <board.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_shape.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_arc.h>
#include <geometry/shape_segment.h>

#include <router/pns_arc.h>
#include <router/pns_debug_decorator.h>
#include <router/pns_kicad_iface.h>
#include <router/pns_line.h>
#include <router/pns_node.h>
#include <router/pns_placement_algo.h>
#include <router/pns_router.h>
#include <router/pns_routing_settings.h>
#include <router/pns_segment.h>
#include <router/pns_sizes_settings.h>
#include <router/pns_solid.h>
#include <router/pns_via.h>

namespace kicadd::route
{

namespace
{

constexpr int GAP_RANGE = 5000000;   // nm: nearer copper / edge than this is measured

std::string str( const wxString& aText )
{
    return aText.ToStdString( wxConvUTF8 );
}


std::string netOf( const BOARD_CONNECTED_ITEM* aItem )
{
    return aItem->GetNetCode() > 0 ? str( aItem->GetNetname() ) : std::string( "no net" );
}


std::string mm( long long aNm )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.4g", aNm / 1e6 );
    return buf;
}


/// The router's view of the board, handing new copper to us instead of to a commit.
class IFACE : public PNS_KICAD_IFACE_BASE
{
public:
    void AddItem( PNS::ITEM* aItem ) override
    {
        PCB_TRACK* t = create( aItem );

        if( !t )
            return;

        aItem->SetParent( t );
        current.push_back( t );
        added.emplace_back( t );
    }

    void RemoveItem( PNS::ITEM* aItem ) override
    {
        BOARD_ITEM* parent = aItem->Parent();

        if( !parent )
            return;

        auto mine = std::find_if( added.begin(), added.end(),
                                  [&]( const std::unique_ptr<PCB_TRACK>& t ) { return t.get() == parent; } );

        if( mine != added.end() )
        {
            std::erase( current, mine->get() );
            added.erase( mine );
            return;
        }

        if( parent->Type() == PCB_TRACE_T || parent->Type() == PCB_ARC_T || parent->Type() == PCB_VIA_T )
            replaced.push_back( static_cast<PCB_TRACK*>( parent ) );
    }

    void UpdateItem( PNS::ITEM* aItem ) override {}

    int GetNetCode( PNS::NET_HANDLE aNet ) const override
    {
        return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetCode() : -1;
    }

    wxString GetNetName( PNS::NET_HANDLE aNet ) const override
    {
        return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetname() : wxString();
    }

    std::vector<std::unique_ptr<PCB_TRACK>> added;     ///< every route of the session
    std::vector<PCB_TRACK*>                 current;   ///< the route being committed
    std::vector<PCB_TRACK*>                 replaced;  ///< board tracks the router took out

private:
    PCB_TRACK* create( PNS::ITEM* aItem )
    {
        NETINFO_ITEM* net = static_cast<NETINFO_ITEM*>( aItem->Net() );

        if( !net )
            net = NETINFO_LIST::OrphanedItem();

        switch( aItem->Kind() )
        {
        case PNS::ITEM::SEGMENT_T:
        {
            PNS::SEGMENT* seg = static_cast<PNS::SEGMENT*>( aItem );
            PCB_TRACK*    t = new PCB_TRACK( m_board );
            t->SetStart( seg->Seg().A );
            t->SetEnd( seg->Seg().B );
            t->SetWidth( seg->Width() );
            t->SetLayer( GetBoardLayerFromPNSLayer( seg->Layers().Start() ) );
            t->SetNet( net );
            return t;
        }

        case PNS::ITEM::ARC_T:
        {
            PNS::ARC* arc = static_cast<PNS::ARC*>( aItem );
            PCB_ARC*  a = new PCB_ARC( m_board, static_cast<const SHAPE_ARC*>( arc->Shape( -1 ) ) );
            a->SetWidth( arc->Width() );
            a->SetLayer( GetBoardLayerFromPNSLayer( arc->Layers().Start() ) );
            a->SetNet( net );
            return a;
        }

        case PNS::ITEM::VIA_T:
        {
            PNS::VIA* via = static_cast<PNS::VIA*>( aItem );
            PCB_VIA*  v = new PCB_VIA( m_board );
            v->SetPosition( via->Pos() );
            v->SetWidth( PADSTACK::TEMP_ALL_LAYERS, via->Diameter( 0 ) );
            v->SetDrill( via->Drill() );
            v->SetNet( net );
            v->SetViaType( via->ViaType() );   // before SetLayerPair
            v->Padstack().SetUnconnectedLayerMode( via->UnconnectedLayerMode() );
            v->SetIsFree( via->IsFree() );
            v->SetLayerPair( GetBoardLayerFromPNSLayer( via->Layers().Start() ),
                             GetBoardLayerFromPNSLayer( via->Layers().End() ) );
            return v;
        }

        default:
            return nullptr;
        }
    }
};


std::string handle( const BOARD_ITEM* aItem )
{
    if( !aItem )
        return "?";

    switch( aItem->Type() )
    {
    case PCB_PAD_T:
    {
        const PAD* pad = static_cast<const PAD*>( aItem );
        return str( pad->GetParentFootprint()->GetReference() ) + "." + str( pad->GetNumber() );
    }

    case PCB_TRACE_T:
    case PCB_ARC_T:
        return "track " + netOf( static_cast<const BOARD_CONNECTED_ITEM*>( aItem ) ) + " ("
               + str( LSET::Name( aItem->GetLayer() ) ) + ")";

    case PCB_VIA_T:
        return "via " + netOf( static_cast<const BOARD_CONNECTED_ITEM*>( aItem ) );

    case PCB_ZONE_T:
    {
        const ZONE* zone = static_cast<const ZONE*>( aItem );

        if( zone->GetIsRuleArea() )
            return "keepout" + ( zone->GetZoneName().IsEmpty() ? std::string() : " " + str( zone->GetZoneName() ) );

        return "zone " + netOf( zone );
    }

    case PCB_SHAPE_T:
        if( aItem->GetLayer() == Edge_Cuts )
            return "board edge";

        return "graphic on " + str( LSET::Name( aItem->GetLayer() ) );

    case PCB_FOOTPRINT_T:
        return str( static_cast<const FOOTPRINT*>( aItem )->GetReference() );

    default:
        if( const FOOTPRINT* fp = aItem->GetParentFootprint() )
            return str( fp->GetReference() ) + " " + str( aItem->GetTypeDesc() );

        return str( aItem->GetTypeDesc() );
    }
}

} // namespace


std::string Describe( const BOARD_CONNECTED_ITEM* aItem )
{
    if( aItem && aItem->Type() == PCB_PAD_T )
        return "pad " + handle( aItem ) + " [" + netOf( aItem ) + "]";

    return handle( aItem );
}


struct SESSION::IMPL
{
    BOARD*                                  board;
    IFACE                                   iface;
    std::unique_ptr<PNS::ROUTING_SETTINGS>  settings;
    std::unique_ptr<PNS::ROUTER>            router;

    explicit IMPL( BOARD* aBoard ) :
            board( aBoard )
    {
        iface.SetBoard( aBoard );
        iface.SetDebugDecorator( new PNS::DEBUG_DECORATOR() );   // the interface deletes it
        router = std::make_unique<PNS::ROUTER>();
        router->SetInterface( &iface );
        router->ClearWorld();
        router->SyncWorld();

        settings = std::make_unique<PNS::ROUTING_SETTINGS>( nullptr, "" );
        settings->SetMode( PNS::RM_Walkaround );
        settings->SetFixAllSegments( true );
        settings->SetAllowDRCViolations( false );
        settings->SetRemoveLoops( true );
        router->LoadSettings( settings.get() );
        router->SetMode( PNS::PNS_MODE_ROUTE_SINGLE );
    }

    ~IMPL()
    {
        router->StopRouting();
        router.reset();   // gives the router instance back to the editor's
    }

    /// The router's head after a Move: where it ends, and whether it ends in a via.
    std::pair<VECTOR2I, bool> head( const VECTOR2I& aStart )
    {
        PNS::ITEM_SET traces = router->Placer()->Traces();

        for( PNS::ITEM* item : traces.CItems() )
        {
            if( item->Kind() == PNS::ITEM::LINE_T )
            {
                PNS::LINE* l = static_cast<PNS::LINE*>( item );

                if( l->PointCount() )
                    return { l->CLine().CLastPoint(), l->EndsWithVia() };
            }
        }

        return { aStart, false };
    }

    /// What stops a straight track from aFrom towards aTo on aLayer.
    void blocker( const VECTOR2I& aFrom, const VECTOR2I& aTo, int aPnsLayer, int aWidth, PNS::NET_HANDLE aNet,
                  RESULT& aOut )
    {
        if( aFrom == aTo )
            return;

        PNS::LINE line;
        line.SetShape( SHAPE_LINE_CHAIN( { aFrom, aTo } ) );
        line.SetWidth( std::max( aWidth, 1 ) );
        line.SetLayer( aPnsLayer );
        line.SetNet( aNet );

        if( std::optional<PNS::OBSTACLE> obs = router->GetWorld()->NearestObstacle( &line ) )
        {
            if( obs->m_item && obs->m_item->Parent() )
            {
                BOARD_ITEM* p = obs->m_item->Parent();
                aOut.blockedBy = p->IsConnected() ? Describe( static_cast<BOARD_CONNECTED_ITEM*>( p ) ) : handle( p );
            }
            else if( obs->m_item && obs->m_item->Kind() == PNS::ITEM::SOLID_T )
            {
                aOut.blockedBy = "board edge or keepout";
            }

            aOut.blockedAt = obs->m_ipFirst;
        }
    }

    /// The head collides (FixRoute refused it): with what
    void collision( RESULT& aOut )
    {
        PNS::ITEM_SET traces = router->Placer()->Traces();

        for( PNS::ITEM* item : traces.CItems() )
        {
            if( item->Kind() != PNS::ITEM::LINE_T )
                continue;

            if( std::optional<PNS::OBSTACLE> obs = router->GetWorld()->CheckColliding( item ) )
            {
                BOARD_ITEM* p = obs->m_item ? obs->m_item->Parent() : nullptr;
                aOut.blockedBy = !p ? std::string( "board edge or keepout" )
                                    : p->IsConnected() ? Describe( static_cast<BOARD_CONNECTED_ITEM*>( p ) )
                                                       : handle( p );

                if( obs->m_item )
                    aOut.blockedAt = obs->m_item->Anchor( 0 );
            }
        }
    }

    void measure( const std::vector<PCB_TRACK*>& aNew, RESULT& aOut );
};


SESSION::SESSION( BOARD* aBoard ) :
        m_impl( std::make_unique<IMPL>( aBoard ) )
{
}


SESSION::~SESSION() = default;


std::vector<std::unique_ptr<PCB_TRACK>> SESSION::TakeAdded()
{
    m_impl->iface.current.clear();
    return std::move( m_impl->iface.added );
}


std::vector<PCB_TRACK*> SESSION::Replaced() const
{
    return m_impl->iface.replaced;
}


RESULT SESSION::Route( const REQUEST& aReq )
{
    IMPL&        m = *m_impl;
    RESULT       out;
    PNS::ROUTER& router = *m.router;
    BOARD*       board = m.board;

    if( !aReq.fromItem )
    {
        out.error = "no start item";
        return out;
    }

    NETINFO_ITEM* net = aReq.fromItem->GetNet();
    out.net = netOf( aReq.fromItem );

    if( aReq.toItem && aReq.toItem->GetNetCode() != aReq.fromItem->GetNetCode() )
    {
        out.error = "the ends are on different nets (" + out.net + ", " + netOf( aReq.toItem ) + ")";
        return out;
    }

    // The layer: the one asked for, else the start's (SMD pad, track), else the end's, else F.Cu
    auto singleLayer = []( const BOARD_CONNECTED_ITEM* aItem ) -> PCB_LAYER_ID
    {
        if( !aItem )
            return UNDEFINED_LAYER;

        LSET cu = LSET( aItem->GetLayerSet() & LSET::AllCuMask() );
        return cu.count() == 1 ? cu.Seq().front() : UNDEFINED_LAYER;
    };

    PCB_LAYER_ID layer = aReq.layer;

    if( layer == UNDEFINED_LAYER )
        layer = singleLayer( aReq.fromItem );

    if( layer == UNDEFINED_LAYER && aReq.stops.empty() )
        layer = singleLayer( aReq.toItem );

    if( layer == UNDEFINED_LAYER )
        layer = F_Cu;

    if( !aReq.fromItem->IsOnLayer( layer ) )
    {
        out.error = "the start (" + Describe( aReq.fromItem ) + ") is not on " + str( LSET::Name( layer ) );
        return out;
    }

    PCB_LAYER_ID      last = layer;
    std::vector<STOP> stops = aReq.stops;

    for( STOP& s : stops )
    {
        if( s.otherSide )
        {
            // the end's layer if it has one, else the other outer layer
            PCB_LAYER_ID endLayer = singleLayer( aReq.toItem );
            s.viaTo = endLayer != UNDEFINED_LAYER && endLayer != last ? endLayer : last == F_Cu ? B_Cu : F_Cu;
        }

        if( s.viaTo != UNDEFINED_LAYER )
        {
            if( s.viaTo == last )
            {
                out.error = "a via at " + mm( s.pos.x ) + "," + mm( s.pos.y ) + " to " + str( LSET::Name( last ) )
                            + ", the layer the route is already on";
                return out;
            }

            last = s.viaTo;
        }
    }

    if( aReq.toItem && !aReq.toItem->IsOnLayer( last ) )
    {
        out.error = "the end (" + Describe( aReq.toItem ) + ") is not on " + str( LSET::Name( last ) )
                    + ", where the route arrives: put a via on the way (waypoint [x, y, layer])";
        return out;
    }

    out.layer = str( LSET::Name( layer ) );

    // straight-line length through the stops
    VECTOR2I prev = aReq.from;

    for( const STOP& s : stops )
    {
        out.direct += ( s.pos - prev ).EuclideanNorm();
        prev = s.pos;
    }

    out.direct += ( aReq.to - prev ).EuclideanNorm();

    PNS::NODE* world = router.GetWorld();
    PNS::ITEM* startItem = world->FindItemByParent( aReq.fromItem );
    PNS::ITEM* endItem = aReq.toItem ? world->FindItemByParent( aReq.toItem ) : nullptr;

    if( !startItem )
    {
        out.error = "the router does not know the start (" + Describe( aReq.fromItem ) + ")";
        return out;
    }

    const int pnsLayer = m.iface.GetPNSLayerFromBoardLayer( layer );
    m.iface.SetStartLayerFromPCBNew( layer );

    PNS::SIZES_SETTINGS sizes;
    m.iface.ImportSizes( sizes, startItem, net, aReq.from );

    if( aReq.width > 0 )
    {
        sizes.SetTrackWidth( aReq.width );
        sizes.SetTrackWidthIsExplicit( true );
    }

    sizes.SetViaType( VIATYPE::THROUGH );
    router.UpdateSizes( sizes );
    out.width = sizes.TrackWidth();

    auto fail = [&]( const VECTOR2I& aReached, const VECTOR2I& aTarget, int aLayer, const std::string& aWhy )
    {
        if( aReached == aTarget )
            m.collision( out );

        router.StopRouting();
        out.ok = false;
        out.error = aWhy;
        out.reached = aReached;

        if( out.blockedBy.empty() )
            m.blocker( aReached, aTarget, aLayer, out.width, net, out );

        return out;
    };

    // A stub ending in a via (fanout tries many spots): the via and the straight stub must fit
    // before the router is asked, which is far slower to say no.
    if( aReq.endViaTo != UNDEFINED_LAYER && stops.empty() )
    {
        PNS::VIA via( aReq.to,
                      PNS_LAYER_RANGE( m.iface.GetPNSLayerFromBoardLayer( F_Cu ), m.iface.GetPNSLayerFromBoardLayer( B_Cu ) ),
                      sizes.ViaDiameter(), sizes.ViaDrill(), net, VIATYPE::THROUGH );

        if( world->CheckColliding( &via ) )
        {
            out.error = "no room for a via at " + mm( aReq.to.x ) + "," + mm( aReq.to.y );
            return out;
        }

        PNS::SEGMENT stub( SEG( aReq.from, aReq.to ), net );
        stub.SetWidth( sizes.TrackWidth() );
        stub.SetLayer( pnsLayer );

        if( world->CheckColliding( &stub ) )
        {
            out.error = "no straight way to " + mm( aReq.to.x ) + "," + mm( aReq.to.y );
            return out;
        }
    }

    if( !router.StartRouting( aReq.from, startItem, pnsLayer ) )
    {
        router.StopRouting();
        out.error = "the router cannot start at " + Describe( aReq.fromItem ) + " on " + out.layer;
        return out;
    }

    int curLayer = pnsLayer;

    for( const STOP& s : stops )
    {
        router.Move( s.pos, nullptr );
        auto [end, via] = m.head( aReq.from );

        if( end != s.pos )
            return fail( end, s.pos, curLayer, "blocked on the way to " + mm( s.pos.x ) + "," + mm( s.pos.y ) );

        if( s.viaTo != UNDEFINED_LAYER )
        {
            const int target = m.iface.GetPNSLayerFromBoardLayer( s.viaTo );

            sizes.ClearLayerPairs();
            sizes.AddLayerPair( curLayer, target );
            router.UpdateSizes( sizes );

            if( !router.IsPlacingVia() )
                router.ToggleViaPlacement();

            router.Move( s.pos, nullptr );
            auto [vEnd, vOk] = m.head( aReq.from );

            if( vEnd != s.pos || !vOk )
                return fail( vEnd, s.pos, curLayer, "no room for a via at " + mm( s.pos.x ) + "," + mm( s.pos.y ) );

            // FixRoute answers true only at the end: fixed means the route now starts here
            router.FixRoute( s.pos, nullptr, false, false );

            if( router.Placer()->CurrentStart() != s.pos )
                return fail( s.pos, s.pos, curLayer, "the via at " + mm( s.pos.x ) + "," + mm( s.pos.y ) + " collides" );

            if( !router.SwitchLayer( target ) )
                return fail( s.pos, s.pos, curLayer, "cannot continue on " + str( LSET::Name( s.viaTo ) ) );

            curLayer = target;
        }
        else
        {
            router.FixRoute( s.pos, nullptr, false, false );

            if( router.Placer()->CurrentStart() != s.pos )
                return fail( s.pos, s.pos, curLayer, "the route to " + mm( s.pos.x ) + "," + mm( s.pos.y ) + " collides" );
        }
    }

    router.Move( aReq.to, endItem );
    auto [end, via] = m.head( aReq.from );

    if( end != aReq.to )
        return fail( end, aReq.to, curLayer,
                     "blocked on the way to " + ( aReq.toItem ? Describe( aReq.toItem ) : mm( aReq.to.x ) + "," + mm( aReq.to.y ) ) );

    if( aReq.endViaTo != UNDEFINED_LAYER )
    {
        sizes.ClearLayerPairs();
        sizes.AddLayerPair( curLayer, m.iface.GetPNSLayerFromBoardLayer( aReq.endViaTo ) );
        router.UpdateSizes( sizes );

        if( !router.IsPlacingVia() )
            router.ToggleViaPlacement();

        router.Move( aReq.to, nullptr );
        auto [vEnd, vOk] = m.head( aReq.from );

        if( vEnd != aReq.to || !vOk )
            return fail( vEnd, aReq.to, curLayer, "no room for a via at " + mm( aReq.to.x ) + "," + mm( aReq.to.y ) );
    }

    m.iface.current.clear();

    if( !router.FixRoute( aReq.to, endItem, true, false ) )
        return fail( end, aReq.to, curLayer, "the last segment collides" );

    router.CommitRouting();

    std::vector<PCB_TRACK*> mine = m.iface.current;
    m.iface.current.clear();

    for( PCB_TRACK* t : mine )
    {
        if( t->Type() == PCB_VIA_T )
        {
            out.vias++;
        }
        else
        {
            out.segments++;
            out.length += t->GetLength();
        }
    }

    m.measure( mine, out );

    // A rule the router does not keep (hole to hole, copper on a cut-out): take the route out of
    // the session again, so it is neither drawn nor in the way of the next ones
    if( !out.drc.empty() )
    {
        for( PCB_TRACK* t : mine )
        {
            if( PNS::ITEM* item = router.GetWorld()->FindItemByParent( t ) )
                router.GetWorld()->Remove( item );

            std::erase_if( m.iface.added, [&]( const std::unique_ptr<PCB_TRACK>& a ) { return a.get() == t; } );
        }

        out.ok = false;
        out.error = "the route would break " + std::to_string( out.drc.size() ) + " rule(s) the router does not keep "
                    "(e.g. hole to hole); not kept";
        return out;
    }

    out.ok = true;
    return out;
}


void SESSION::IMPL::measure( const std::vector<PCB_TRACK*>& aNew, RESULT& aOut )
{
    std::shared_ptr<DRC_ENGINE> engine = board->GetDesignSettings().m_DRCEngine;
    std::set<const BOARD_ITEM*> gone( iface.replaced.begin(), iface.replaced.end() );
    std::set<const BOARD_ITEM*> mine( aNew.begin(), aNew.end() );
    const LSET                  copper = LSET::AllCuMask();
    const int                   epsilon = board->GetDesignSettings().GetDRCEpsilon();

    std::set<std::pair<const BOARD_ITEM*, const BOARD_ITEM*>> reported;

    auto test = [&]( PCB_TRACK* aTrack, BOARD_ITEM* aOther, bool aEdge )
    {
        LSET layers = aEdge ? ( aTrack->GetLayerSet() & copper ) : ( aTrack->GetLayerSet() & aOther->GetLayerSet() & copper );

        for( PCB_LAYER_ID layer : layers.Seq() )
        {
            std::shared_ptr<SHAPE> a = aTrack->GetEffectiveShape( layer );
            int      actual = 0;
            VECTOR2I pos;

            if( !a )
                continue;

            if( aEdge )
            {
                // the outline's lines, not a filled board shape's inside
                bool near = false;

                for( SHAPE* edge : static_cast<PCB_SHAPE*>( aOther )->MakeEffectiveShapes( true ) )
                {
                    int      d = 0;
                    VECTOR2I p;

                    if( a->Collide( edge, GAP_RANGE, &d, &p ) && ( !near || d < actual ) )
                    {
                        near = true;
                        actual = d;
                        pos = p;
                    }

                    delete edge;
                }

                if( !near )
                    continue;
            }
            else
            {
                std::shared_ptr<SHAPE> b = aOther->GetEffectiveShape( layer );

                if( !b || !a->Collide( b.get(), GAP_RANGE, &actual, &pos ) )
                    continue;
            }

            if( aOut.minGap < 0 || actual < aOut.minGap )
            {
                aOut.minGap = actual;
                aOut.gapTo = aOther->IsConnected() ? Describe( static_cast<BOARD_CONNECTED_ITEM*>( aOther ) )
                                                   : handle( aOther );
            }

            if( !engine )
                continue;

            DRC_CONSTRAINT c = engine->EvalRules( aEdge ? EDGE_CLEARANCE_CONSTRAINT : CLEARANCE_CONSTRAINT, aTrack,
                                                  aOther, layer );
            int required = c.GetValue().Min();

            // the same epsilon as full DRC (lengths in nm round; the router keeps exactly the
            // clearance). Copper on the edge itself is wrong whatever the rule says.
            if( aEdge && actual == 0 )
                required = std::max( required, 1 );
            else if( c.GetSeverity() == RPT_SEVERITY_IGNORE || required <= 0 || actual >= required - epsilon )
                continue;

            if( !reported.insert( { aTrack, aOther } ).second )
                continue;

            board::DrcViolation v;
            v.set_type( aEdge ? board::DT_EDGE_CLEARANCE : actual == 0 ? board::DT_SHORT : board::DT_CLEARANCE );
            v.set_a( "new " + handle( aTrack ) );
            v.set_b( aOther->IsConnected() ? Describe( static_cast<BOARD_CONNECTED_ITEM*>( aOther ) ) : handle( aOther ) );
            v.set_net_a( netOf( aTrack ) );

            if( aOther->IsConnected() )
                v.set_net_b( netOf( static_cast<BOARD_CONNECTED_ITEM*>( aOther ) ) );

            v.set_layer( str( LSET::Name( layer ) ) );
            v.set_required( required );
            v.set_actual( actual );
            v.mutable_pos()->set_x( pos.x );
            v.mutable_pos()->set_y( pos.y );
            aOut.drc.push_back( v );
        }
    };

    auto otherNet = [&]( const BOARD_CONNECTED_ITEM* aA, const BOARD_CONNECTED_ITEM* aB )
    {
        return aA->GetNetCode() <= 0 || aB->GetNetCode() <= 0 || aA->GetNetCode() != aB->GetNetCode();
    };

    for( PCB_TRACK* t : aNew )
    {
        BOX2I area = t->GetBoundingBox();
        area.Inflate( GAP_RANGE );

        for( FOOTPRINT* fp : board->Footprints() )
        {
            if( !area.Intersects( fp->GetBoundingBox( false ) ) )
                continue;

            for( PAD* pad : fp->Pads() )
            {
                if( otherNet( t, pad ) && area.Intersects( pad->GetBoundingBox() ) )
                    test( t, pad, false );
            }
        }

        for( PCB_TRACK* other : board->Tracks() )
        {
            if( !gone.count( other ) && otherNet( t, other ) && area.Intersects( other->GetBoundingBox() ) )
                test( t, other, false );
        }

        for( const std::unique_ptr<PCB_TRACK>& other : iface.added )   // earlier routes of the session
        {
            if( !mine.count( other.get() ) && otherNet( t, other.get() ) && area.Intersects( other->GetBoundingBox() ) )
                test( t, other.get(), false );
        }

        // a via's drill against every other hole, of any net (hole to hole); the router only
        // keeps copper clearances
        if( t->Type() == PCB_VIA_T )
        {
            std::shared_ptr<SHAPE_SEGMENT> hole = t->GetEffectiveHoleShape();

            auto testHole = [&]( BOARD_ITEM* aOther )
            {
                std::shared_ptr<SHAPE_SEGMENT> other = aOther->GetEffectiveHoleShape();
                int                            d = 0;
                VECTOR2I                       p;

                if( !hole || !other || !hole->Collide( other.get(), GAP_RANGE, &d, &p ) )
                    return;

                int required = engine ? engine->EvalRules( HOLE_TO_HOLE_CONSTRAINT, t, aOther, UNDEFINED_LAYER )
                                                .GetValue().Min()
                                      : 0;

                required = std::max( required, board->GetDesignSettings().m_HoleToHoleMin );

                if( d >= required - epsilon || !reported.insert( { t, aOther } ).second )
                    return;

                board::DrcViolation v;
                v.set_type( board::DT_HOLE_TO_HOLE );
                v.set_a( "new " + handle( t ) );
                v.set_b( aOther->IsConnected() ? Describe( static_cast<BOARD_CONNECTED_ITEM*>( aOther ) ) : handle( aOther ) );
                v.set_net_a( netOf( t ) );
                v.set_required( required );
                v.set_actual( d );
                v.mutable_pos()->set_x( p.x );
                v.mutable_pos()->set_y( p.y );
                aOut.drc.push_back( v );
            };

            for( FOOTPRINT* fp : board->Footprints() )
            {
                if( !area.Intersects( fp->GetBoundingBox( false ) ) )
                    continue;

                for( PAD* pad : fp->Pads() )
                {
                    if( pad->HasHole() && area.Intersects( pad->GetBoundingBox() ) )
                        testHole( pad );
                }
            }

            for( PCB_TRACK* other : board->Tracks() )
            {
                if( other->Type() == PCB_VIA_T && !gone.count( other ) && area.Intersects( other->GetBoundingBox() ) )
                    testHole( other );
            }

            for( const std::unique_ptr<PCB_TRACK>& other : iface.added )
            {
                if( other.get() != t && other->Type() == PCB_VIA_T && area.Intersects( other->GetBoundingBox() ) )
                    testHole( other.get() );
            }
        }

        for( BOARD_ITEM* item : board->Drawings() )
        {
            if( item->Type() == PCB_SHAPE_T && item->GetLayer() == Edge_Cuts && area.Intersects( item->GetBoundingBox() ) )
                test( t, item, true );
        }

        // cut-outs drawn in footprints (mounting holes, slots)
        for( FOOTPRINT* fp : board->Footprints() )
        {
            if( !area.Intersects( fp->GetBoundingBox( false ) ) )
                continue;

            for( BOARD_ITEM* item : fp->GraphicalItems() )
            {
                if( item->Type() == PCB_SHAPE_T && item->GetLayer() == Edge_Cuts && area.Intersects( item->GetBoundingBox() ) )
                    test( t, item, true );
            }
        }
    }
}

} // namespace kicadd::route
