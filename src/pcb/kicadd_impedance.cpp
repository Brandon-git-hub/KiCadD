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

#include <board.h>
#include <board_design_settings.h>
#include <zone.h>
#include <board_stackup_manager/board_stackup.h>
#include <transline_calculations/dielectric_djordjevic_sarkar.h>

namespace kicadd::impedance
{

namespace
{

struct DIELECTRIC
{
    double h = 0, er = 0, tand = 0;   // m
};


// the dielectrics strictly between stackup items aFrom and aTo, averaged by thickness
DIELECTRIC between( const std::vector<BOARD_STACKUP_ITEM*>& aList, int aFrom, int aTo )
{
    DIELECTRIC d;

    for( int i = std::min( aFrom, aTo ) + 1; i < std::max( aFrom, aTo ); ++i )
    {
        const BOARD_STACKUP_ITEM* item = aList[i];

        if( item->GetType() != BS_ITEM_TYPE_DIELECTRIC || !item->HasEpsilonRValue() )
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

    if( at == 0 || at == (int) copper.size() - 1 )
    {
        const int  plane = copper[at == 0 ? 1 : at - 1];
        DIELECTRIC d = between( list, self, plane );

        out.planes = { list[plane]->GetBrdLayerId() };
        out.line.microstrip = true;
        out.line.h = d.h;
        out.line.er = d.er;
        out.line.tand = d.tand;

        // the solder mask over it
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
        const int  above = copper[at - 1], below = copper[at + 1];
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
        s = "microstrip over " + LSET::Name( aLine.planes[0] ).ToStdString() + ", " + num( l.h * 1e3 ) + " mm Er "
             + num( l.er, "%.3g" ) + ", copper " + num( l.t * 1e3 ) + " mm";
        s += l.maskT > 0 ? ", mask " + num( l.maskT * 1e3 ) + " mm Er " + num( l.maskEr, "%.3g" ) : ", no mask";
    }
    else
    {
        s = "stripline between " + LSET::Name( aLine.planes[0] ).ToStdString() + " (" + num( l.h * 1e3 )
             + " mm) and " + LSET::Name( aLine.planes[1] ).ToStdString() + " (" + num( l.hBelow * 1e3 )
             + " mm), Er " + num( l.er, "%.3g" ) + ", copper " + num( l.t * 1e3 ) + " mm";
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
