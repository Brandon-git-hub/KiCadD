/*
 * KiCadD: controlled-impedance track widths from the board's stackup (see kicadd_impedance.h).
 * The cross-section is read as Board Setup's tuning profiles read it
 * (pcbnew/dialogs/panel_setup_tuning_profile_info.cpp): the dielectrics between the layer and
 * its plane averaged by thickness, a Djordjevic-Sarkar dielectric taken at the frequency used.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_impedance.h"

#include <cmath>
#include <cstdio>
#include <map>

#include <board.h>
#include <board_design_settings.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape_poly_set.h>
#include <board_stackup_manager/board_stackup.h>
#include <transline_calculations/dielectric_djordjevic_sarkar.h>

namespace kicadd::impedance
{

namespace
{

struct DIELECTRIC
{
    double h = 0, er = 0, tand = 0;   // m; h takes in the copper of layers passed over
};


// the dielectrics strictly between stackup items aFrom and aTo (either may be -1 or the list's
// size: up to the surface), averaged by thickness; a copper layer between adds its thickness to h
// (its Er: the resin around its tracks, so the dielectrics' average); aMask: a solder mask too
DIELECTRIC between( const std::vector<BOARD_STACKUP_ITEM*>& aList, int aFrom, int aTo, bool aMask = false )
{
    DIELECTRIC d;
    double     cu = 0;

    for( int i = std::min( aFrom, aTo ) + 1; i < std::max( aFrom, aTo ); ++i )
    {
        const BOARD_STACKUP_ITEM* item = aList[i];

        if( item->GetType() == BS_ITEM_TYPE_COPPER )
        {
            cu += item->GetThickness() * 1e-9;
            continue;
        }

        const bool mask = item->GetType() == BS_ITEM_TYPE_SOLDERMASK;

        if( ( item->GetType() != BS_ITEM_TYPE_DIELECTRIC && !( aMask && mask && item->IsEnabled() ) )
            || !item->HasEpsilonRValue() )
            continue;

        for( int k = 0; k < item->GetSublayersCount(); ++k )
        {
            const double h = item->GetThickness( k ) * 1e-9;
            double       er = item->GetEpsilonR( k );
            double       tand = item->GetLossTangent( k );
            const double spec = item->GetSpecFreq( k );

            if( item->GetDielectricModel( k ) == DIELECTRIC_MODEL::DJORDJEVIC_SARKAR && std::isfinite( spec )
                && spec > 0 )
            {
                try
                {
                    DIELECTRIC_DJORDJEVIC_SARKAR ds;
                    ds.Fit( er, tand, spec );
                    er = ds.EpsilonRealAt( kicadd::zcalc::FREQUENCY );
                    tand = ds.TanDeltaAt( kicadd::zcalc::FREQUENCY );
                }
                catch( const std::invalid_argument& )
                {
                }
            }

            d.h += h;
            d.er += er * h;
            d.tand += tand * h;
        }
    }

    if( d.h > 0 )
    {
        d.er /= d.h;
        d.tand /= d.h;
    }

    d.h += cu;
    return d;
}


std::string num( double aV, const char* aFmt = "%.4g" )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), aFmt, aV );
    return buf;
}

} // namespace


double Mm( double aMetres )
{
    return std::round( aMetres * 1e6 ) / 1e3;
}


std::set<PCB_LAYER_ID> Planes( const BOARD& aBoard, std::string* aWhy )
{
    const LSET             copper = LSET::AllCuMask( aBoard.GetCopperLayerCount() ) & aBoard.GetEnabledLayers();
    std::set<PCB_LAYER_ID> out;
    SHAPE_POLY_SET         outline;
    double                 board = 0;

    if( const_cast<BOARD&>( aBoard ).GetBoardPolygonOutlines( outline, false ) && outline.OutlineCount() > 0 )
        board = outline.Area();
    else
        board = (double) aBoard.GetBoardEdgesBoundingBox().GetArea();

    // copper in zones per layer: a zone's fill, its outline while not filled
    std::map<PCB_LAYER_ID, double> area;

    for( const ZONE* z : aBoard.Zones() )
    {
        if( z->GetIsRuleArea() || z->GetNetCode() <= 0 )
            continue;

        for( PCB_LAYER_ID l : LSET( z->GetLayerSet() & copper ).Seq() )
        {
            area[l] += z->IsFilled() && z->HasFilledPolysForLayer( l ) ? z->GetFilledPolysList( l )->Area()
                                                                      : SHAPE_POLY_SET( *z->Outline() ).Area();
        }
    }

    // the layers with tracks (an empty layer is a plane not poured yet)
    LSET routed;

    for( const PCB_TRACK* t : aBoard.Tracks() )
    {
        if( t->Type() != PCB_VIA_T )
            routed.set( t->GetLayer() );
    }

    // a layer with zones by their copper; one without (not poured yet) by its Board Setup type,
    // or as a plane to be when nothing is on it
    std::string yes, no;

    for( PCB_LAYER_ID l : aBoard.GetEnabledLayers().CuStack() )
    {
        auto              a = area.find( l );
        const bool        typed = aBoard.GetLayerType( l ) == LT_POWER || aBoard.GetLayerType( l ) == LT_MIXED;
        const double      cover = a != area.end() && board > 0 ? std::min( 1.0, a->second / board ) : 0;
        const bool        plane = a != area.end() ? cover >= PLANE_COVER : typed || !routed.Contains( l );
        const std::string why = a != area.end() ? num( cover * 100, "%.0f" ) + "%"
                                : typed         ? "(typed power, no zone)"
                                : plane         ? "(empty)"
                                                : "(no zone)";
        std::string&      to = plane ? yes : no;

        if( plane )
            out.insert( l );

        to += ( to.empty() ? "" : ", " ) + LSET::Name( l ).ToStdString() + " " + why;
    }

    if( aWhy && area.empty() && no.empty() )
    {
        *aWhy = "no copper zones or tracks yet: each layer's nearest copper layers are its planes";
    }
    else if( aWhy )
    {
        *aWhy = out.empty() ? "no layer poured over " + num( PLANE_COVER * 100, "%.0f" )
                                      + "% of the board: each layer's nearest copper layers taken as its planes"
                            : "planes (zones over " + num( PLANE_COVER * 100, "%.0f" ) + "% of the board): " + yes;

        if( !out.empty() && !no.empty() )
            *aWhy += "; signal layers: " + no;
    }

    return out;
}


std::optional<LAYER_LINE> Line( const BOARD& aBoard, PCB_LAYER_ID aLayer, std::string& aError )
{
    if( !IsCopperLayer( aLayer ) || !aBoard.GetEnabledLayers().Contains( aLayer ) )
    {
        aError = "not a copper layer of this board";
        return std::nullopt;
    }

    const BOARD_STACKUP                     stackup = aBoard.GetStackupOrDefault();
    const std::vector<BOARD_STACKUP_ITEM*>& list = stackup.GetList();

    // the copper layers in stackup order, and where aLayer is among them
    std::vector<int> copper;
    int              at = -1;

    for( int i = 0; i < (int) list.size(); ++i )
    {
        if( list[i]->GetType() != BS_ITEM_TYPE_COPPER )
            continue;

        if( list[i]->GetBrdLayerId() == aLayer )
            at = (int) copper.size();

        copper.push_back( i );
    }

    if( at < 0 || copper.size() < 2 )
    {
        aError = "the stackup has no plane for it (a board with one copper layer?)";
        return std::nullopt;
    }

    LAYER_LINE out;
    out.layer = aLayer;

    const int self = copper[at];
    out.line.t = list[self]->GetThickness() * 1e-9;

    if( out.line.t <= 0 )
    {
        aError = "the layer's copper has no thickness in the stackup";
        return std::nullopt;
    }

    // its planes: the nearest plane layer each way, the signal layers between passed over; with
    // no planes on the board (nothing poured, no layer typed power) or none but itself, the
    // nearest copper layers
    std::set<PCB_LAYER_ID> planes = Planes( aBoard );
    planes.erase( aLayer );

    if( planes.empty() )
    {
        out.guessed = true;

        for( int i : copper )
            planes.insert( list[i]->GetBrdLayerId() );

        planes.erase( aLayer );
    }

    auto find = [&]( int aStep, std::vector<PCB_LAYER_ID>& aCrossed ) -> int
    {
        std::vector<PCB_LAYER_ID> crossed;

        for( int k = at + aStep; k >= 0 && k < (int) copper.size(); k += aStep )
        {
            const PCB_LAYER_ID l = list[copper[k]]->GetBrdLayerId();

            if( planes.count( l ) )
            {
                aCrossed.insert( aCrossed.end(), crossed.begin(), crossed.end() );
                return copper[k];
            }

            crossed.push_back( l );
        }

        return -1;
    };

    const int above = find( -1, out.crossed ), below = find( +1, out.crossed );

    if( above >= 0 && below >= 0 )
    {
        DIELECTRIC up = between( list, self, above ), down = between( list, self, below );

        out.planes = { list[above]->GetBrdLayerId(), list[below]->GetBrdLayerId() };
        out.line.microstrip = false;
        out.line.h = up.h;
        out.line.hBelow = down.h;

        if( up.h + down.h > 0 )
        {
            out.line.er = ( up.er * up.h + down.er * down.h ) / ( up.h + down.h );
            out.line.tand = ( up.tand * up.h + down.tand * down.h ) / ( up.h + down.h );
        }
    }
    else
    {
        const int  plane = above >= 0 ? above : below;
        DIELECTRIC d = between( list, self, plane );

        out.planes = { list[plane]->GetBrdLayerId() };
        out.line.microstrip = true;
        out.line.h = d.h;
        out.line.er = d.er;
        out.line.tand = d.tand;

        // what covers it: on an outer layer the solder mask; on an inner one with a plane on one
        // side only (an embedded microstrip) all of the board on the other side, the mask with it
        const bool outer = at == 0 || at == (int) copper.size() - 1;

        if( outer )
        {
            const PCB_LAYER_ID mask = aLayer == F_Cu ? F_Mask : B_Mask;

            for( const BOARD_STACKUP_ITEM* item : list )
            {
                if( item->GetType() == BS_ITEM_TYPE_SOLDERMASK && item->GetBrdLayerId() == mask && item->IsEnabled() )
                {
                    out.line.maskT = item->GetThickness() * 1e-9;
                    out.line.maskEr = item->GetEpsilonR();
                    out.line.maskTand = item->GetLossTangent();
                }
            }
        }
        else
        {
            DIELECTRIC cover = between( list, self, plane == above ? (int) list.size() : -1, true );
            out.embedded = true;
            out.line.maskT = cover.h;
            out.line.maskEr = cover.er;
            out.line.maskTand = cover.tand;
        }
    }

    if( out.line.h <= 0 && out.line.hBelow <= 0 )
    {
        aError = "the stackup has no dielectric between it and its plane";
        return std::nullopt;
    }

    if( out.line.er < 1 )
    {
        aError = "the stackup's dielectric has no dielectric constant";
        return std::nullopt;
    }

    return out;
}


std::string Describe( const LAYER_LINE& aLine )
{
    const kicadd::zcalc::LINE& l = aLine.line;
    std::string                s;

    if( l.microstrip )
    {
        s = std::string( aLine.embedded ? "embedded microstrip over " : "microstrip over " )
             + LSET::Name( aLine.planes[0] ).ToStdString() + ", " + num( l.h * 1e3 ) + " mm Er "
             + num( l.er, "%.3g" ) + ", copper " + num( l.t * 1e3 ) + " mm";

        if( aLine.embedded )
            s += ", no plane on the other side: the " + num( l.maskT * 1e3 ) + " mm of board over it (Er "
                 + num( l.maskEr, "%.3g" ) + ") its cover";
        else
            s += l.maskT > 0 ? ", mask " + num( l.maskT * 1e3 ) + " mm Er " + num( l.maskEr, "%.3g" ) : ", no mask";
    }
    else
    {
        s = "stripline between " + LSET::Name( aLine.planes[0] ).ToStdString() + " (" + num( l.h * 1e3 )
             + " mm) and " + LSET::Name( aLine.planes[1] ).ToStdString() + " (" + num( l.hBelow * 1e3 )
             + " mm), Er " + num( l.er, "%.3g" ) + ", copper " + num( l.t * 1e3 ) + " mm";
    }

    if( !aLine.crossed.empty() )
    {
        s += "; across ";

        for( size_t i = 0; i < aLine.crossed.size(); ++i )
            s += ( i ? ", " : "" ) + LSET::Name( aLine.crossed[i] ).ToStdString();

        s += " (" + std::string( aLine.crossed.size() > 1 ? "signal layers" : "a signal layer" ) + ", under "
             + num( PLANE_COVER * 100, "%.0f" ) + "% poured: no plane)";
    }

    return s;
}


std::vector<PCB_LAYER_ID> WithoutZone( const BOARD& aBoard, const std::vector<PCB_LAYER_ID>& aPlanes )
{
    std::vector<PCB_LAYER_ID> out;

    for( PCB_LAYER_ID p : aPlanes )
    {
        bool found = false;

        for( const ZONE* z : aBoard.Zones() )
        {
            if( !z->GetIsRuleArea() && z->GetNetCode() > 0 && z->GetLayerSet().Contains( p ) )
            {
                found = true;
                break;
            }
        }

        if( !found )
            out.push_back( p );
    }

    return out;
}


std::string Source( const BOARD& aBoard )
{
    const BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();

    if( bds.GetStackupDescriptor().GetList().empty() )
        return "the board has no stackup: KiCad's default one (FR4, Er 4.5) was used; board_setup stackup=... "
               "sets a fab's";

    if( bds.GetStackupDescriptor().m_HasDielectricConstrains )
        return "the stackup's own dielectrics (a fab's impedance stackup); field solvers and the fab's calculator "
               "differ by a few percent: check with the fab before ordering";

    return "the stackup's dielectrics are generic (not an impedance-controlled stackup): board_setup "
           "stackup=JLC04161H-7628 (or another, stackup=\"list\") sets a fab's own numbers";
}

} // namespace kicadd::impedance
