/*
 * KiCadD: a rail's decoupling as the board has it (see kicadd_pdn.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_pdn.h"
#include "pcb/kicadd_findnet.h"
#include "pcb/kicadd_impedance.h"

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_poly_set.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <regex>

namespace kicadd
{

namespace
{

constexpr double MU0 = 4e-7 * M_PI;

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}


std::string num( double v, const char* aFmt = "%.3g" )
{
    char buf[32];
    std::snprintf( buf, sizeof buf, aFmt, v );
    return buf;
}


std::string prefix( const std::string& aRef )
{
    size_t i = 0;

    while( i < aRef.size() && std::isalpha( (unsigned char) aRef[i] ) )
        i++;

    return aRef.substr( 0, i );
}


bool groundName( const std::string& aNet )
{
    static const std::regex g( R"((^|[/_.])(A|D|P|S|DIG|ANA)?(GND|VSS|GROUND|0V|COM)([_./\d]|$))", std::regex::icase );
    return std::regex_search( aNet, g );
}


double mm( double aNm )
{
    return aNm / 1e6;
}


/// the zone fills of a net per copper layer (unfractured, for Contains)
struct NET_FILLS
{
    std::map<PCB_LAYER_ID, std::vector<SHAPE_POLY_SET>> layers;
    std::map<PCB_LAYER_ID, double>                      area;   // m2

    bool covers( PCB_LAYER_ID l, const VECTOR2I& p ) const
    {
        auto it = layers.find( l );

        if( it == layers.end() )
            return false;

        for( const SHAPE_POLY_SET& s : it->second )
        {
            if( s.Contains( p ) )
                return true;
        }

        return false;
    }

    /// the layer with the most of it (UNDEFINED_LAYER: no fill)
    PCB_LAYER_ID main() const
    {
        PCB_LAYER_ID best = UNDEFINED_LAYER;
        double       a = 0;

        for( auto [l, v] : area )
        {
            if( v > a )
            {
                a = v;
                best = l;
            }
        }

        return best;
    }
};


NET_FILLS fillsOf( const BOARD& aBoard, int aNet )
{
    NET_FILLS out;

    for( ZONE* z : aBoard.Zones() )
    {
        if( z->GetIsRuleArea() || z->GetNetCode() != aNet )
            continue;

        for( PCB_LAYER_ID l : z->GetLayerSet().CuStack() )
        {
            if( !z->HasFilledPolysForLayer( l ) )
                continue;

            std::shared_ptr<SHAPE_POLY_SET> poly = z->GetFilledPolysList( l );

            if( !poly || poly->OutlineCount() == 0 )
                continue;

            SHAPE_POLY_SET s = poly->CloneDropTriangulation();
            s.Unfracture();
            s.BuildBBoxCaches();
            out.area[l] += poly->Area() / 1e18;
            out.layers[l].push_back( std::move( s ) );
        }
    }

    return out;
}

} // namespace


std::string PdnGeometry( BOARD& aBoard, const kicadd::board::PdnGeometry& aReq, kicadd::board::PdnGeometryResponse& aOut )
{
    std::string   why;
    NETINFO_ITEM* rail = kicadd::FindNetLoose( aBoard, aReq.net(), &why );

    if( !rail || rail->GetNetCode() <= 0 )
        return why;

    const int railCode = rail->GetNetCode();
    aOut.set_net( str( rail->GetNetname() ) );

    // the two-pad parts on the rail, by the net of their other pad
    struct TWO
    {
        FOOTPRINT* fp;
        PAD*       railPad;
        PAD*       otherPad;
    };

    std::vector<TWO>   two;
    std::map<int, int> otherCount;

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        std::vector<PAD*> pads;

        for( PAD* p : fp->Pads() )
        {
            if( p->GetNetCode() > 0 || p->IsOnCopperLayer() )
                pads.push_back( p );
        }

        if( pads.size() != 2 )
            continue;

        PAD* r = pads[0]->GetNetCode() == railCode ? pads[0] : pads[1]->GetNetCode() == railCode ? pads[1] : nullptr;

        if( !r )
            continue;

        PAD* o = r == pads[0] ? pads[1] : pads[0];

        if( o->GetNetCode() <= 0 || o->GetNetCode() == railCode )
            continue;

        two.push_back( { fp, r, o } );

        if( prefix( str( fp->GetReference() ) ) == "C" )
            otherCount[o->GetNetCode()]++;
    }

    int gndCode = -1;

    if( !aReq.ground().empty() )
    {
        NETINFO_ITEM* g = kicadd::FindNetLoose( aBoard, aReq.ground(), &why );

        if( !g || g->GetNetCode() <= 0 )
            return why + " (ground)";

        gndCode = g->GetNetCode();
    }
    else
    {
        // the ground most capacitors go to (a ground name first)
        int best = 0;

        for( auto [code, n] : otherCount )
        {
            int score = n * 2 + ( groundName( str( aBoard.FindNet( code )->GetNetname() ) ) ? 1000 : 0 );

            if( score > best )
            {
                best = score;
                gndCode = code;
            }
        }

        if( gndCode < 0 )
            return "no capacitors (C references) between " + aReq.net() + " and another net: nothing decouples it (ground: name one)";
    }

    aOut.set_ground( str( aBoard.FindNet( gndCode )->GetNetname() ) );

    // the load: the part named, else the one with most pads on the rail (3 pads or more)
    FOOTPRINT* load = nullptr;

    if( !aReq.at().empty() )
    {
        for( FOOTPRINT* fp : aBoard.Footprints() )
        {
            if( str( fp->GetReference() ) == aReq.at() )
                load = fp;
        }

        if( !load )
            return "at: no part " + aReq.at();
    }
    else
    {
        int best = 0;

        for( FOOTPRINT* fp : aBoard.Footprints() )
        {
            if( fp->Pads().size() < 3 )
                continue;

            int n = (int) std::count_if( fp->Pads().begin(), fp->Pads().end(), [&]( PAD* p ) { return p->GetNetCode() == railCode; } );

            if( n > best )
            {
                best = n;
                load = fp;
            }
        }

        if( !load )
            return "no part with 3 or more pads on " + aReq.net() + ": name the load with at";
    }

    std::vector<PAD*> loadRail, loadGnd;

    for( PAD* p : load->Pads() )
    {
        if( p->GetNetCode() == railCode )
            loadRail.push_back( p );
        else if( p->GetNetCode() == gndCode )
            loadGnd.push_back( p );
    }

    if( loadRail.empty() )
        return "at: " + str( load->GetReference() ) + " has no pad on " + aReq.net();

    VECTOR2D at( 0, 0 );

    for( PAD* p : loadRail )
        at += VECTOR2D( p->GetPosition() );

    at = at / (double) loadRail.size();
    aOut.set_at( str( load->GetReference() ) );
    aOut.set_at_x( mm( at.x ) );
    aOut.set_at_y( mm( at.y ) );

    // the plane pair: the rail's main fill layer and the ground fill layer nearest to it
    const NET_FILLS railFill = fillsOf( aBoard, railCode );
    const NET_FILLS gndFill = fillsOf( aBoard, gndCode );
    const PCB_LAYER_ID railPlane = railFill.main();
    PCB_LAYER_ID gndPlane = UNDEFINED_LAYER;
    double       gndMax = 0;

    for( auto [l, a] : gndFill.area )
        gndMax = std::max( gndMax, a );

    const BOARD_STACKUP stackup = aBoard.GetStackupOrDefault();
    const LSET          cuSet = LSET::AllCuMask( aBoard.GetCopperLayerCount() ) & aBoard.GetEnabledLayers();
    std::vector<PCB_LAYER_ID> cu;

    for( PCB_LAYER_ID l : cuSet.CuStack() )
        cu.push_back( l );

    auto depth = [&]( PCB_LAYER_ID a, PCB_LAYER_ID b ) -> double   // m
    {
        if( a == b )
            return 0;

        double h = 0, er = 0;

        if( impedance::Between( aBoard, a, b, h, er ) && h > 0 )
            return h;

        const auto ia = std::find( cu.begin(), cu.end(), a ) - cu.begin();
        const auto ib = std::find( cu.begin(), cu.end(), b ) - cu.begin();
        return aBoard.GetDesignSettings().GetBoardThickness() * 1e-9 / std::max<size_t>( 1, cu.size() - 1 ) * std::abs( ia - ib );
    };

    if( gndMax > 0 )
    {
        double bestD = 1e9;

        for( auto [l, a] : gndFill.area )
        {
            if( a < 0.2 * gndMax || l == railPlane )
                continue;

            double d = railPlane == UNDEFINED_LAYER ? 0 : depth( l, railPlane );

            if( d < bestD )
            {
                bestD = d;
                gndPlane = l;
            }
        }

        if( gndPlane == UNDEFINED_LAYER )
            gndPlane = gndFill.main();
    }

    double pairH = 0, pairEr = 4.5;

    if( railPlane != UNDEFINED_LAYER && gndPlane != UNDEFINED_LAYER && railPlane != gndPlane )
    {
        impedance::Between( aBoard, railPlane, gndPlane, pairH, pairEr );

        if( pairH <= 0 )
            pairH = depth( railPlane, gndPlane );

        const double area = railFill.area.at( railPlane );
        aOut.set_plane_area( area * 1e6 );
        aOut.set_plane_h( pairH * 1e3 );
        aOut.set_plane_er( pairEr );
        aOut.set_plane_c( 8.854e-12 * pairEr * area / pairH );
        aOut.set_plane_layers( str( LSET::Name( railPlane ) ) + " over " + str( LSET::Name( gndPlane ) ) );
    }
    else if( railPlane == UNDEFINED_LAYER )
    {
        aOut.add_notes( aOut.net() + " has no pour (zone fill): no plane capacitance, and the path from the capacitors to the load "
                        "is tracks, not modelled here (their mounting only); refill_zones if it has unfilled zones" );
    }
    else
    {
        aOut.add_notes( "no " + aOut.ground() + " pour on another layer than " + aOut.net()
                        + "'s: no plane pair (no plane capacitance or spreading inductance)" );
    }

    // the per-layer track model, for the stubs from the pads to their vias
    std::map<std::pair<PCB_LAYER_ID, int>, std::pair<double, double>> zCache;   // (Z0, s/m)

    auto trackL = [&]( PCB_LAYER_ID aLayer, int aWidth, double aLenM ) -> double
    {
        auto key = std::make_pair( aLayer, aWidth );

        if( !zCache.count( key ) )
        {
            std::string err;
            auto        line = impedance::Line( aBoard, aLayer, err );
            auto        r = line ? zcalc::Single( line->line, aWidth * 1e-9 ) : zcalc::RESULT();
            zCache[key] = r.ok ? std::make_pair( r.z, r.delay * 1e-12 * 100 ) : std::make_pair( 50.0, 6e-9 );
        }

        auto [z, d] = zCache[key];
        return z * d * aLenM;
    };

    // the vias and tracks of the two nets
    std::vector<PCB_VIA*>   vias;
    std::vector<PCB_TRACK*> tracks;

    for( PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->GetNetCode() != railCode && t->GetNetCode() != gndCode )
            continue;

        if( t->Type() == PCB_VIA_T )
            vias.push_back( static_cast<PCB_VIA*>( t ) );
        else if( t->Type() == PCB_TRACE_T || t->Type() == PCB_ARC_T )
            tracks.push_back( t );
    }

    // how one pad gets to its net's plane: (track m, vias, via position, depth m, via radius m, how)
    struct REACH
    {
        double      track = 0;
        int         vias = 0;
        VECTOR2I    pos;
        double      depth = 0;
        double      radius = 0.15e-3;
        double      l = 0;          // the track's inductance
        std::string how;
        bool        open = false;
    };

    auto reach = [&]( PAD* aPad, PCB_LAYER_ID aSide, int aNet, PCB_LAYER_ID aPlane, const NET_FILLS& aFill ) -> REACH
    {
        REACH r;
        r.pos = aPad->GetPosition();

        if( aPlane == UNDEFINED_LAYER )
        {
            r.how = "no plane";
            return r;
        }

        // a plated hole is its own via
        if( aPad->GetAttribute() == PAD_ATTRIB::PTH )
        {
            r.vias = 1;
            r.depth = depth( aSide, aPlane );
            r.radius = std::max( 0.1e-3, aPad->GetDrillSize().x * 0.5e-9 );
            r.how = "plated hole";
            return r;
        }

        // the plane on the part's own side, under the pad
        if( aPlane == aSide && aFill.covers( aSide, aPad->GetPosition() ) )
        {
            r.how = "pour";
            return r;
        }

        // the nearest via of the net: in the pad, or at the end of a track from it
        PCB_VIA* best = nullptr;
        double   bestD = 1e18;

        for( PCB_VIA* v : vias )
        {
            if( v->GetNetCode() != aNet )
                continue;

            double d = ( VECTOR2D( v->GetPosition() ) - VECTOR2D( aPad->GetPosition() ) ).EuclideanNorm();

            if( d < bestD && d < 5e6 )
            {
                bestD = d;
                best = v;
            }
        }

        // a pour on the pad's side of the net reaches it too (the via further on, through it)
        if( !best )
        {
            if( aFill.covers( aSide, aPad->GetPosition() ) )
            {
                r.how = "pour on " + str( LSET::Name( aSide ) ) + " (no via within 5 mm)";
                r.depth = depth( aSide, aPlane );
                r.vias = 1;
                return r;
            }

            r.open = true;
            r.how = "no via within 5 mm";
            return r;
        }

        r.pos = best->GetPosition();
        r.radius = std::max( 0.05e-3, best->GetDrillValue() * 0.5e-9 );
        r.depth = depth( aSide, aPlane );

        // vias in parallel: the net's vias as near as the nearest + 0.5 mm
        for( PCB_VIA* v : vias )
        {
            if( v->GetNetCode() == aNet && ( VECTOR2D( v->GetPosition() ) - VECTOR2D( aPad->GetPosition() ) ).EuclideanNorm() <= bestD + 0.5e6 )
                r.vias++;
        }

        if( aPad->HitTest( best->GetPosition(), 0 ) )
        {
            r.how = std::to_string( r.vias ) + " via(s), in the pad";
            return r;
        }

        // in a pour of its net on its side: the pour, not a track, takes it to the via(s)
        if( aFill.covers( aSide, aPad->GetPosition() ) )
        {
            r.how = "pour to " + std::to_string( r.vias ) + " via(s) " + num( bestD / 1e6, "%.2g" ) + " mm away";
            return r;
        }

        // the track from the pad's edge to the via: straight, the width of the widest track there
        int width = 0;

        for( PCB_TRACK* t : tracks )
        {
            if( t->GetNetCode() == aNet && t->GetLayer() == aSide
                && ( aPad->HitTest( t->GetStart(), t->GetWidth() / 2 ) || aPad->HitTest( t->GetEnd(), t->GetWidth() / 2 ) ) )
                width = std::max( width, t->GetWidth() );
        }

        const double edge = std::min( aPad->GetBoundingBox().GetWidth(), aPad->GetBoundingBox().GetHeight() ) / 2.0;
        r.track = std::max( 0.0, bestD - edge ) * 1e-9;

        if( width == 0 )
        {
            width = 250000;
            r.how = "no track seen to its via(s) " + num( bestD / 1e6, "%.2g" ) + " mm away (taken as a 0.25 mm one)";
        }
        else
        {
            r.how = num( r.track * 1e3, "%.2g" ) + " mm of " + num( width / 1e6, "%.2g" ) + " mm track to " + std::to_string( r.vias ) + " via(s)";
        }

        r.l = trackL( aSide, width, r.track );
        return r;
    };

    // the via pair's loop: (mu0 / 2 pi) (h1 + h2) ln(s / r), over the pairs in parallel
    auto viaLoop = [&]( const REACH& a, const REACH& b ) -> double
    {
        const double s = std::max( ( VECTOR2D( a.pos ) - VECTOR2D( b.pos ) ).EuclideanNorm() * 1e-9, 2.5 * std::max( a.radius, b.radius ) );
        const double h = a.depth + b.depth;
        const int    n = std::max( 1, a.vias == 0 ? b.vias : b.vias == 0 ? a.vias : std::min( a.vias, b.vias ) );
        return h > 0 ? MU0 / ( 2 * M_PI ) * h * std::log( s / std::max( a.radius, b.radius ) ) / n : 0;
    };

    auto spread = [&]( const VECTOR2I& aFrom, double aRadius ) -> double
    {
        if( pairH <= 0 )
            return 0;

        const double d = ( VECTOR2D( aFrom ) - at ).EuclideanNorm() * 1e-9;
        const double r = std::max( aRadius, 0.15e-3 );
        return d > r ? MU0 * pairH / ( 2 * M_PI ) * std::log( d / r ) : 0;
    };

    int skipped = 0;

    for( const TWO& t : two )
    {
        if( t.otherPad->GetNetCode() != gndCode )
        {
            if( aOut.others_size() < 8 )
                aOut.add_others( str( t.fp->GetReference() ) + " " + str( t.fp->GetValue() ) + " to "
                                 + str( t.otherPad->GetNetname() ) );

            continue;
        }

        const std::string ref = str( t.fp->GetReference() );

        if( prefix( ref ) != "C" )
        {
            if( aOut.others_size() < 8 )
                aOut.add_others( ref + " " + str( t.fp->GetValue() ) + " to " + aOut.ground() + " (not a C)" );

            skipped++;
            continue;
        }

        const PCB_LAYER_ID side = t.fp->GetLayer() == B_Cu ? B_Cu : F_Cu;
        REACH              rr = reach( t.railPad, side, railCode, railPlane, railFill );
        REACH              rg = reach( t.otherPad, side, gndCode, gndPlane, gndFill );

        kicadd::board::PdnCap* c = aOut.add_caps();
        c->set_ref( ref );
        c->set_value( str( t.fp->GetValue() ) );
        c->set_footprint( str( t.fp->GetFPID().GetLibItemName() ) );

        if( const PCB_FIELD* f = t.fp->GetField( wxS( "LCSC" ) ) )
            c->set_lcsc( str( f->GetText() ) );

        c->set_layer( str( LSET::Name( side ) ) );
        c->set_x( mm( t.fp->GetPosition().x ) );
        c->set_y( mm( t.fp->GetPosition().y ) );
        c->set_distance( ( VECTOR2D( t.fp->GetPosition() ) - at ).EuclideanNorm() / 1e6 );
        c->set_trace( ( rr.track + rg.track ) * 1e3 );
        c->set_trace_l( rr.l + rg.l );
        c->set_vias( rr.vias + rg.vias );
        c->set_via_l( viaLoop( rr, rg ) );
        c->set_spread_l( spread( rr.vias ? rr.pos : t.railPad->GetPosition(), rr.radius ) );

        std::string how = str( LSET::Name( side ) ) + "; " + aOut.net() + ": " + rr.how
                          + ( rr.depth > 0 ? ", " + num( rr.depth * 1e3, "%.2g" ) + " mm down to " + str( LSET::Name( railPlane ) ) : "" )
                          + "; " + aOut.ground() + ": " + rg.how
                          + ( rg.depth > 0 ? ", " + num( rg.depth * 1e3, "%.2g" ) + " mm down to " + str( LSET::Name( gndPlane ) ) : "" );

        if( rr.open || rg.open )
            how += " [not connected to the plane, as far as seen]";

        c->set_how( how );
    }

    if( aOut.caps_size() == 0 )
        return "no capacitors between " + aOut.net() + " and " + aOut.ground();

    // the load's own vias: its rail and ground pads' vias, pairs in parallel
    {
        const PCB_LAYER_ID side = load->GetLayer() == B_Cu ? B_Cu : F_Cu;
        double             lsum = 0;
        int                nv = 0;

        for( PAD* p : loadRail )
        {
            REACH rr = reach( p, side, railCode, railPlane, railFill );
            REACH rg;
            double nearest = 1e18;

            for( PAD* g : loadGnd )
            {
                double d = ( VECTOR2D( g->GetPosition() ) - VECTOR2D( p->GetPosition() ) ).EuclideanNorm();

                if( d < nearest )
                {
                    nearest = d;
                    rg = reach( g, side, gndCode, gndPlane, gndFill );
                }
            }

            if( rr.open )
                continue;

            double l = viaLoop( rr, rg ) * std::max( 1, std::min( rr.vias ? rr.vias : 1, rg.vias ? rg.vias : 1 ) ) + rr.l + rg.l;

            if( l > 0 )
                lsum += 1 / l;

            nv += rr.vias;
        }

        aOut.set_load_l( lsum > 0 ? 1 / lsum : 0 );
        aOut.set_load_vias( nv );

        if( loadGnd.empty() )
            aOut.add_notes( aOut.at() + " has no pad on " + aOut.ground() + ": its own via loop not counted" );
    }

    if( skipped )
        aOut.add_notes( std::to_string( skipped ) + " two-pad part(s) to " + aOut.ground() + " that are not C references left out" );

    return {};
}

} // namespace kicadd
