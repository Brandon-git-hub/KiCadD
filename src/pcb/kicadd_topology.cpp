/*
 * KiCadD: a net's copper as a graph of transmission lines, for the reflection tool (roadmap phase
 * 3, item 4). Nodes: pads, vias, track ends and T junctions (a track end on another track's middle
 * splits that one); edges: the track pieces between them with Z0 and delay of their layer and
 * width (kicadd_impedance's cross-section: the planes by poured copper). Vias are joints (their
 * barrel's delay and capacitance are left out, said by the tool).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_pdn.h"
#include "pcb/kicadd_findnet.h"
#include "pcb/kicadd_impedance.h"

#include <board.h>
#include <connectivity/connectivity_data.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <ratsnest/ratsnest_data.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace kicadd
{

namespace
{

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}


struct DSU
{
    std::vector<int> p;

    int add()
    {
        p.push_back( (int) p.size() );
        return (int) p.size() - 1;
    }

    int find( int x )
    {
        while( p[x] != x )
        {
            p[x] = p[p[x]];
            x = p[x];
        }

        return x;
    }

    void join( int a, int b ) { p[find( a )] = find( b ); }
};

} // namespace


std::string NetTopology( BOARD& aBoard, const kicadd::board::NetTopology& aReq, kicadd::board::NetTopologyResponse& aOut )
{
    NETINFO_ITEM* net = kicadd::FindNetLoose( aBoard, aReq.net() );

    if( !net || net->GetNetCode() <= 0 )
        return "no net " + aReq.net();

    const int code = net->GetNetCode();
    aOut.set_net( str( net->GetNetname() ) );

    std::vector<PCB_TRACK*> tracks;
    std::vector<PCB_VIA*>   vias;
    std::vector<PAD*>       pads;

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->GetNetCode() != code )
            continue;

        if( t->Type() == PCB_VIA_T )
            vias.push_back( static_cast<PCB_VIA*>( t ) );
        else
            tracks.push_back( t );
    }

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( PAD* p : fp->Pads() )
        {
            if( p->GetNetCode() == code )
                pads.push_back( p );
        }
    }

    DSU              dsu;
    std::vector<int> padNode, viaNode, startNode, endNode;

    for( size_t i = 0; i < pads.size(); ++i )
        padNode.push_back( dsu.add() );

    for( size_t i = 0; i < vias.size(); ++i )
        viaNode.push_back( dsu.add() );

    for( size_t i = 0; i < tracks.size(); ++i )
    {
        startNode.push_back( dsu.add() );
        endNode.push_back( dsu.add() );
    }

    // where a point on a layer joins: pads and vias there, other tracks' ends there
    auto attach = [&]( const VECTOR2I& aPt, PCB_LAYER_ID aLayer, int aNode, size_t aSelf )
    {
        for( size_t i = 0; i < pads.size(); ++i )
        {
            if( pads[i]->IsOnLayer( aLayer ) && pads[i]->HitTest( aPt, 0, aLayer ) )
                dsu.join( aNode, padNode[i] );
        }

        for( size_t i = 0; i < vias.size(); ++i )
        {
            const PCB_VIA* v = vias[i];

            if( v->IsOnLayer( aLayer ) && ( VECTOR2D( aPt ) - VECTOR2D( v->GetPosition() ) ).EuclideanNorm() <= v->GetWidth( aLayer ) / 2.0 + 1000 )
                dsu.join( aNode, viaNode[i] );
        }

        for( size_t j = 0; j < tracks.size(); ++j )
        {
            if( j == aSelf || tracks[j]->GetLayer() != aLayer )
                continue;

            const double tol = std::max( 1000.0, tracks[j]->GetWidth() / 4.0 );

            if( ( VECTOR2D( aPt ) - VECTOR2D( tracks[j]->GetStart() ) ).EuclideanNorm() <= tol )
                dsu.join( aNode, startNode[j] );

            if( ( VECTOR2D( aPt ) - VECTOR2D( tracks[j]->GetEnd() ) ).EuclideanNorm() <= tol )
                dsu.join( aNode, endNode[j] );
        }
    };

    for( size_t i = 0; i < tracks.size(); ++i )
    {
        attach( tracks[i]->GetStart(), tracks[i]->GetLayer(), startNode[i], i );
        attach( tracks[i]->GetEnd(), tracks[i]->GetLayer(), endNode[i], i );
    }

    // T junctions: a track end (or a via) on another straight track's middle splits it there
    std::vector<std::vector<std::pair<double, int>>> splits( tracks.size() );

    auto splitAt = [&]( const VECTOR2I& aPt, PCB_LAYER_ID aLayer, int aNode, size_t aSelf )
    {
        for( size_t j = 0; j < tracks.size(); ++j )
        {
            const PCB_TRACK* t = tracks[j];

            if( j == aSelf || t->Type() != PCB_TRACE_T || t->GetLayer() != aLayer )
                continue;

            const VECTOR2D a( t->GetStart() ), b( t->GetEnd() ), p( aPt );
            const VECTOR2D d = b - a;
            const double   l2 = d.SquaredEuclideanNorm();

            if( l2 <= 0 )
                continue;

            const double u = ( ( p - a ).x * d.x + ( p - a ).y * d.y ) / l2;
            const double len = std::sqrt( l2 );

            if( u * len <= t->GetWidth() / 2.0 || ( 1 - u ) * len <= t->GetWidth() / 2.0 )
                continue;

            if( ( a + d * u - p ).EuclideanNorm() <= t->GetWidth() / 2.0 )
                splits[j].emplace_back( u, aNode );
        }
    };

    for( size_t i = 0; i < tracks.size(); ++i )
    {
        splitAt( tracks[i]->GetStart(), tracks[i]->GetLayer(), startNode[i], i );
        splitAt( tracks[i]->GetEnd(), tracks[i]->GetLayer(), endNode[i], i );
    }

    for( size_t i = 0; i < vias.size(); ++i )
    {
        for( PCB_LAYER_ID l : vias[i]->GetLayerSet().CuStack() )
            splitAt( vias[i]->GetPosition(), l, viaNode[i], SIZE_MAX );
    }

    // nodes numbered from 0
    std::map<int, int> index;

    auto nodeOf = [&]( int aRaw )
    {
        int r = dsu.find( aRaw );
        auto it = index.find( r );

        if( it != index.end() )
            return it->second;

        int n = (int) index.size();
        index[r] = n;
        return n;
    };

    // Z0 and delay per layer and width
    std::map<std::pair<PCB_LAYER_ID, int>, std::pair<double, double>> zc;
    std::string                                                       zErr;

    auto zOf = [&]( PCB_LAYER_ID aLayer, int aWidth )
    {
        auto key = std::make_pair( aLayer, aWidth );

        if( !zc.count( key ) )
        {
            std::string err;
            auto        line = impedance::Line( aBoard, aLayer, err );
            auto        r = line ? zcalc::Single( line->line, aWidth * 1e-9 ) : zcalc::RESULT();

            if( !r.ok && zErr.empty() )
                zErr = str( LSET::Name( aLayer ) ) + ": " + ( line ? r.error : err );

            zc[key] = r.ok ? std::make_pair( r.z, r.delay ) : std::make_pair( 0.0, 0.0 );
        }

        return zc[key];
    };

    for( size_t i = 0; i < tracks.size(); ++i )
    {
        PCB_TRACK*      t = tracks[i];
        auto&           sp = splits[i];
        std::sort( sp.begin(), sp.end() );
        auto [z, delay] = zOf( t->GetLayer(), t->GetWidth() );
        const double total = t->GetLength();

        double prevU = 0;
        int    prev = nodeOf( startNode[i] );

        auto edge = [&]( int a, int b, double aLen )
        {
            if( a == b )
                return;

            kicadd::board::TopoEdge* e = aOut.add_edges();
            e->set_a( a );
            e->set_b( b );
            e->set_layer( str( LSET::Name( t->GetLayer() ) ) );
            e->set_length( aLen / 1e6 );
            e->set_width( t->GetWidth() / 1e6 );
            e->set_z0( z );
            e->set_delay( delay );
        };

        for( auto [u, n] : sp )
        {
            int node = nodeOf( n );
            edge( prev, node, total * ( u - prevU ) );
            prev = node;
            prevU = u;
        }

        edge( prev, nodeOf( endNode[i] ), total * ( 1 - prevU ) );
    }

    for( size_t i = 0; i < pads.size(); ++i )
    {
        PAD*                    p = pads[i];
        const FOOTPRINT*        fp = p->GetParentFootprint();
        kicadd::board::TopoPad* o = aOut.add_pads();
        o->set_node( nodeOf( padNode[i] ) );
        o->set_ref( fp ? str( fp->GetReference() ) : "" );
        o->set_pin( str( p->GetNumber() ) );
        o->set_pin_type( str( p->GetPinType() ) );
        o->set_pin_name( str( p->GetPinFunction() ) );
        o->set_x( p->GetPosition().x / 1e6 );
        o->set_y( p->GetPosition().y / 1e6 );

        if( fp )
        {
            int copper = 0;
            PAD* other = nullptr;

            for( PAD* q : fp->Pads() )
            {
                if( q->IsOnCopperLayer() )
                {
                    copper++;

                    if( q != p )
                        other = q;
                }
            }

            o->set_pads( copper );
            o->set_value( str( fp->GetValue() ) );

            if( copper == 2 && other )
                o->set_other_net( str( other->GetNetname() ) );

            if( const PCB_FIELD* f = fp->GetField( wxS( "LCSC" ) ) )
                o->set_lcsc( str( f->GetText() ) );
        }
    }

    for( size_t i = 0; i < vias.size(); ++i )
        nodeOf( viaNode[i] );

    aOut.set_nodes( (int) index.size() );
    aOut.set_vias( (int) vias.size() );

    if( RN_NET* rn = aBoard.GetConnectivity()->GetRatsnestForNet( code ) )
        aOut.set_unrouted( (int) rn->GetEdges().size() );

    if( !zErr.empty() )
        aOut.add_notes( "no Z0 on " + zErr );

    if( std::any_of( tracks.begin(), tracks.end(), []( PCB_TRACK* t ) { return t->Type() == PCB_ARC_T; } ) )
        aOut.add_notes( "arcs are not split at T junctions" );

    return {};
}

} // namespace kicadd
