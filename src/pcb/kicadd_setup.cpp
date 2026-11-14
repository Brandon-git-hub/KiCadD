/*
 * KiCadD: the board's physical setup and the fabs' process presets (see kicadd_setup.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_setup.h"

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <netclass.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>
#include <project/net_settings.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <board_stackup_manager/stackup_predefined_prms.h>

namespace kicadd::setup
{

namespace
{

constexpr double OZ_NM = 35000.0;   // 1 oz/ft^2 of copper: 35 um

std::string mm( int aNm )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.4g", aNm / 1e6 );
    return buf;
}

// ---------------------------------------------------------------------------------------------
// JLCPCB's impedance-controlled stackups, 1.6 mm, 1 oz outside (the "JLC04161H-..." /
// "JLC06161H-..." names), from https://jlcpcb.com/impedance as read on 2026-11-14: each
// dielectric between two copper layers as its prepreg sheets or core, mm. Inner copper is the
// 0.0152 mm the page gives (H/HOZ cores). Dk as the page states them (no frequency given):
// prepreg 7628 4.4, 3313 4.1, 2116 4.16, 1080 3.91; core 4.6; solder mask 3.8. JLCPCB's order
// page (cart.jlcpcb.com) gives other Dk values (7628 4.6, 3313 4.05, 2116 4.25): check with the
// fab's own impedance calculator before ordering.

struct SHEET
{
    const char* material;   // "7628", ...; "core"
    double      mm;
};

struct FAB_STACKUP
{
    const char*                     name;
    int                             layers;
    std::vector<std::vector<SHEET>> gaps;   // top to bottom, one per pair of copper layers
};

const std::vector<FAB_STACKUP>& fabStackups()
{
    using G = std::vector<SHEET>;
    auto four = []( const char* aName, G aPP, double aCore )
    {
        G down( aPP.rbegin(), aPP.rend() );
        return FAB_STACKUP{ aName, 4, { aPP, G{ { "core", aCore } }, down } };
    };
    auto six = []( const char* aName, G aOuter, double aCore, G aMiddle )
    {
        G down( aOuter.rbegin(), aOuter.rend() );
        return FAB_STACKUP{ aName, 6, { aOuter, G{ { "core", aCore } }, aMiddle, G{ { "core", aCore } }, down } };
    };

    // the prepregs from the top copper down to the first core
    static const std::vector<FAB_STACKUP> all = {
        four( "JLC04161H-7628", { { "7628", 0.2104 } }, 1.065 ),
        four( "JLC04161H-3313", { { "3313", 0.0994 } }, 1.265 ),
        four( "JLC04161H-1080", { { "1080", 0.0764 } }, 1.265 ),
        four( "JLC04161H-2116", { { "2116", 0.1164 } }, 1.265 ),
        four( "JLC04161H-7628A", { { "7628", 0.218 }, { "1080", 0.0764 } }, 0.865 ),
        four( "JLC04161H-3313A", { { "3313", 0.107 }, { "3313", 0.0994 } }, 1.065 ),
        four( "JLC04161H-1080A", { { "1080", 0.084 }, { "1080", 0.0764 } }, 1.065 ),
        four( "JLC04161H-7628B", { { "7628", 0.218 }, { "7628", 0.218 }, { "2116", 0.1164 } }, 0.4 ),
        four( "JLC04161H-2116A", { { "2116", 0.124 }, { "7628", 0.218 }, { "2116", 0.1164 } }, 0.6 ),
        four( "JLC04161H-2116B", { { "2116", 0.124 }, { "7628", 0.2104 } }, 0.865 ),
        four( "JLC04161H-2116C", { { "2116", 0.124 }, { "1080", 0.0764 } }, 1.065 ),
        four( "JLC04161H-1080B", { { "1080", 0.084 }, { "1080", 0.084 }, { "1080", 0.0764 } }, 1.065 ),
        four( "JLC04161H-7628G", { { "7628", 0.218 }, { "7628", 0.218 }, { "1080", 0.0764 } }, 0.5 ),
        four( "JLC04161H-7628E", { { "7628", 0.218 }, { "7628", 0.2104 } }, 0.6 ),
        four( "JLC04161H-2116D", { { "2116", 0.124 }, { "7628", 0.2104 } }, 0.7 ),
        four( "JLC04161H-7628F", { { "7628", 0.218 }, { "7628", 0.218 }, { "7628", 0.2104 } }, 0.25 ),
        four( "JLC04161H-2116E", { { "2116", 0.124 }, { "2116", 0.1164 } }, 0.865 ),
        four( "JLC04161H-7628C", { { "7628", 0.218 }, { "7628", 0.218 }, { "7628", 0.2104 } }, 0.15 ),
        six( "JLC06161H-3313", { { "3313", 0.0994 } }, 0.55, { { "2116", 0.1088 } } ),
        six( "JLC06161H-7628", { { "7628", 0.2104 } }, 0.4, { { "7628", 0.2028 } } ),
        six( "JLC06161H-7628D", { { "7628", 0.216 }, { "7628", 0.2084 } }, 0.2, { { "7628", 0.2008 } } ),
        six( "JLC06161H-1080", { { "1080", 0.0764 } }, 0.55, { { "7628", 0.2104 } } ),
        six( "JLC06161H-1080A", { { "1080", 0.0764 } }, 0.6, { { "3313", 0.0994 } } ),
        six( "JLC06161H-3313B", { { "3313", 0.0994 } }, 0.4, { { "7628", 0.2104 }, { "7628", 0.2104 } } ),
        six( "JLC06161H-2116B", { { "2116", 0.1164 } }, 0.5, { { "1080", 0.0764 }, { "1080", 0.0764 } } ),
        six( "JLC06161H-7628B", { { "7628", 0.218 }, { "1080", 0.0764 } }, 0.35, { { "1080", 0.0764 }, { "1080", 0.0764 } } ),
    };

    return all;
}

double dkOf( const std::string& aMaterial )
{
    static const std::map<std::string, double> dk = {
        { "7628", 4.4 }, { "3313", 4.1 }, { "2116", 4.16 }, { "1080", 3.91 }, { "core", 4.6 } };
    auto it = dk.find( aMaterial );
    return it == dk.end() ? 4.5 : it->second;
}

std::string upper( std::string s )
{
    std::transform( s.begin(), s.end(), s.begin(), ::toupper );
    return s;
}

} // namespace


std::vector<std::string> FabStackups()
{
    std::vector<std::string> out;

    for( const FAB_STACKUP& f : fabStackups() )
        out.push_back( f.name );

    return out;
}


std::string FabStackupSource()
{
    return "JLCPCB's impedance stackups from https://jlcpcb.com/impedance (read 2026-11-14): Dk 7628 4.4, 3313 4.1, "
           "2116 4.16, 1080 3.91, core 4.6, mask 3.8 (no frequency given; the order page says 7628 4.6, 3313 4.05, "
           "2116 4.25): check the widths with JLCPCB's impedance calculator before ordering";
}


std::vector<std::string> Describe( const STATE& aState )
{
    std::vector<std::string> out;

    for( const BOARD_STACKUP_ITEM* item : aState.stackup.GetList() )
    {
        switch( item->GetType() )
        {
        case BS_ITEM_TYPE_COPPER:
        {
            char oz[32];
            std::snprintf( oz, sizeof( oz ), "%.2g", item->GetThickness() / OZ_NM );
            out.push_back( LSET::Name( item->GetBrdLayerId() ).ToStdString() + " copper " + mm( item->GetThickness() )
                           + " mm (" + oz + " oz)" );
            break;
        }

        case BS_ITEM_TYPE_DIELECTRIC:
        {
            std::string line = item->GetTypeName().ToStdString();

            for( int k = 0; k < item->GetSublayersCount(); ++k )
            {
                char er[32];
                std::snprintf( er, sizeof( er ), "%.3g", item->GetEpsilonR( k ) );
                line += std::string( k ? " +" : "" ) + " " + item->GetMaterial( k ).ToStdString() + " "
                        + mm( item->GetThickness( k ) ) + " mm (er " + er + ")";
            }

            out.push_back( line );
            break;
        }

        case BS_ITEM_TYPE_SOLDERMASK:
            out.push_back( LSET::Name( item->GetBrdLayerId() ).ToStdString() + " mask " + mm( item->GetThickness() )
                           + " mm" );
            break;

        default:
            break;
        }
    }

    return out;
}


bool STATE::operator==( const STATE& aOther ) const
{
    return copperLayers == aOther.copperLayers && enabled == aOther.enabled && thickness == aOther.thickness
           && hasStackup == aOther.hasStackup && Describe( *this ) == Describe( aOther );
}


double CopperOz( const STATE& aState, PCB_LAYER_ID aLayer )
{
    for( const BOARD_STACKUP_ITEM* item : aState.stackup.GetList() )
    {
        if( item->GetType() == BS_ITEM_TYPE_COPPER && item->GetBrdLayerId() == aLayer )
            return item->GetThickness() / OZ_NM;
    }

    return 0;
}


STATE Capture( BOARD& aBoard )
{
    BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
    STATE                  st;
    st.copperLayers = aBoard.GetCopperLayerCount();
    st.enabled = aBoard.GetEnabledLayers();
    st.thickness = bds.GetBoardThickness();
    st.hasStackup = bds.m_HasStackup;
    st.stackup = bds.GetStackupDescriptor();
    return st;
}


void Apply( BOARD& aBoard, const STATE& aState )
{
    BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
    const LSET             before = aBoard.GetEnabledLayers();

    aBoard.SetCopperLayerCount( aState.copperLayers );
    aBoard.SetEnabledLayers( aState.enabled );
    aBoard.SetVisibleLayers( aBoard.GetVisibleLayers() | ( aState.enabled & ~before ) );
    bds.SetBoardThickness( aState.thickness );
    bds.GetStackupDescriptor() = aState.stackup;
    bds.m_HasStackup = aState.hasStackup;

    // as Board Setup does: plated holes reach every inner layer
    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->HasHole() && pad->IsOnCopperLayer() )
                pad->SetLayerSet( pad->GetLayerSet() | LSET::InternalCuMask() );
        }
    }
}


std::optional<STATE> Changed( BOARD& aBoard, const STATE& aFrom, const CHANGE& aChange, std::string& aError )
{
    STATE st = aFrom;
    const FAB_STACKUP* fab = nullptr;

    if( aChange.fab )
    {
        for( const FAB_STACKUP& f : fabStackups() )
        {
            if( upper( *aChange.fab ) == f.name )
                fab = &f;
        }

        if( !fab )
        {
            aError = "no stackup " + *aChange.fab + " (stackup=\"list\" for the names)";
            return std::nullopt;
        }

        if( ( aChange.copperLayers && *aChange.copperLayers != fab->layers ) || aChange.thickness || aChange.outerOz
            || aChange.innerOz )
        {
            aError = std::string( fab->name ) + " fixes the layers (" + std::to_string( fab->layers )
                     + "), the thickness and the copper: give it alone";
            return std::nullopt;
        }
    }

    const int n = fab ? fab->layers : aChange.copperLayers.value_or( aFrom.copperLayers );

    if( n < 1 || n > 32 || ( n > 1 && n % 2 ) )
    {
        aError = "copper layers: 1, 2, 4, 6, ... 32";
        return std::nullopt;
    }

    if( aChange.thickness && ( *aChange.thickness < 200000 || *aChange.thickness > 10000000 ) )
    {
        aError = "thickness: 0.2 to 10 mm";
        return std::nullopt;
    }

    for( const std::optional<double>& oz : { aChange.outerOz, aChange.innerOz } )
    {
        if( oz && ( *oz < 0.25 || *oz > 6 ) )
        {
            aError = "copper weight: 0.25 to 6 oz";
            return std::nullopt;
        }
    }

    // A copper layer that goes must be empty
    const LSET gone = LSET::AllCuMask( aFrom.copperLayers ) & ~LSET::AllCuMask( n );

    if( gone.any() )
    {
        std::map<std::string, int> on;

        auto count = [&]( const BOARD_ITEM* aItem, const char* aWhat )
        {
            LSET layers = aItem->GetLayerSet() & gone;

            // plated holes and through vias reach every layer: not "on" an inner one
            if( layers.any() && ( aItem->GetLayerSet() & LSET::AllCuMask() ) != LSET::AllCuMask() )
                on[std::string( aWhat ) + " on " + LSET::Name( layers.Seq().front() ).ToStdString()]++;
        };

        for( const PCB_TRACK* t : aBoard.Tracks() )
        {
            if( t->Type() == PCB_VIA_T )
            {
                const PCB_VIA* v = static_cast<const PCB_VIA*>( t );

                if( v->GetViaType() != VIATYPE::THROUGH && ( gone.Contains( v->TopLayer() ) || gone.Contains( v->BottomLayer() ) ) )
                    on["blind / buried via to " + LSET::Name( gone.Contains( v->TopLayer() ) ? v->TopLayer() : v->BottomLayer() ).ToStdString()]++;
            }
            else
            {
                count( t, "track" );
            }
        }

        for( const ZONE* z : aBoard.Zones() )
            count( z, z->GetIsRuleArea() ? "rule area" : "zone" );

        for( const BOARD_ITEM* d : aBoard.Drawings() )
            count( d, "graphic" );

        for( const FOOTPRINT* fp : aBoard.Footprints() )
        {
            for( const PAD* pad : fp->Pads() )
                count( pad, "pad" );
        }

        if( !on.empty() )
        {
            aError = "the layers that would go are not empty:";

            for( const auto& [what, k] : on )
                aError += " " + std::to_string( k ) + " " + what + ";";

            aError += " delete or move those first";
            return std::nullopt;
        }
    }

    st.copperLayers = n;
    st.enabled = ( aFrom.enabled & ~LSET::AllCuMask() ) | LSET::AllCuMask( n );
    st.thickness = aChange.thickness.value_or( aFrom.thickness );

    // A stackup made anew for the count and thickness (as Board Setup's default one), with the
    // copper weights asked for (else the ones there were; else 1 oz outside, 0.5 oz inside)
    double outer = aChange.outerOz.value_or( CopperOz( aFrom, F_Cu ) );
    double inner = aChange.innerOz.value_or( CopperOz( aFrom, In1_Cu ) );

    if( outer <= 0 )
        outer = 1;

    if( inner <= 0 )
        inner = 0.5;

    BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
    const int  oldCount = bds.GetCopperLayerCount();
    const LSET oldEnabled = bds.GetEnabledLayers();
    const int  oldThickness = bds.GetBoardThickness();

    bds.SetCopperLayerCount( n );
    bds.SetEnabledLayers( st.enabled );
    bds.SetBoardThickness( st.thickness );
    st.stackup.RemoveAll();
    st.stackup.BuildDefaultStackupList( &bds, n );
    bds.SetCopperLayerCount( oldCount );
    bds.SetEnabledLayers( oldEnabled );
    bds.SetBoardThickness( oldThickness );

    int fixed = 0, dielectrics = 0;

    for( BOARD_STACKUP_ITEM* item : st.stackup.GetList() )
    {
        if( item->GetType() == BS_ITEM_TYPE_COPPER )
        {
            bool outside = item->GetBrdLayerId() == F_Cu || item->GetBrdLayerId() == B_Cu;
            item->SetThickness( KiROUND( ( outside ? outer : inner ) * OZ_NM ) );
        }

        if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC )
            dielectrics++;
        else if( item->GetType() == BS_ITEM_TYPE_COPPER || item->GetType() == BS_ITEM_TYPE_SOLDERMASK )
            fixed += item->GetThickness();
    }

    // the dielectrics share what the copper and the mask leave of the board's thickness
    if( dielectrics > 0 )
    {
        int each = ( st.thickness - fixed ) / dielectrics;

        if( each < 50000 )
        {
            aError = "the board is too thin for " + std::to_string( n ) + " copper layers of that weight";
            return std::nullopt;
        }

        for( BOARD_STACKUP_ITEM* item : st.stackup.GetList() )
        {
            if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC )
                item->SetThickness( each );
        }
    }

    // a fab's stackup: its sheets, thicknesses and Dk in place of the shared-out dielectrics
    if( fab )
    {
        size_t gap = 0;
        int    total = 0;

        for( BOARD_STACKUP_ITEM* item : st.stackup.GetList() )
        {
            if( item->GetType() == BS_ITEM_TYPE_COPPER )
            {
                bool outside = item->GetBrdLayerId() == F_Cu || item->GetBrdLayerId() == B_Cu;
                item->SetThickness( outside ? 35000 : 15200 );
                total += item->GetThickness();
            }
            else if( item->GetType() == BS_ITEM_TYPE_SOLDERMASK )
            {
                item->SetEpsilonR( 3.8 );
            }
            else if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC && gap < fab->gaps.size() )
            {
                const std::vector<SHEET>& sheets = fab->gaps[gap++];
                const bool                core = std::string( sheets[0].material ) == "core";

                item->SetTypeName( core ? KEY_CORE : KEY_PREPREG );

                while( item->GetSublayersCount() < (int) sheets.size() )
                    item->AddDielectricPrms( item->GetSublayersCount() );

                for( int k = 0; k < (int) sheets.size(); ++k )
                {
                    item->SetMaterial( core ? wxString( wxS( "FR4" ) ) : wxString::FromUTF8( sheets[k].material ), k );
                    item->SetThickness( KiROUND( sheets[k].mm * 1e6 ), k );
                    item->SetEpsilonR( dkOf( sheets[k].material ), k );
                    total += item->GetThickness( k );
                }
            }
        }

        st.thickness = total;
        st.stackup.m_HasDielectricConstrains = true;
    }

    st.hasStackup = true;
    return st;
}


std::optional<kicadd::rules::STATE> Preset( const std::string& aName, const kicadd::rules::STATE& aFrom,
                                            std::string& aNote )
{
    // JLCPCB's standard process (no extra charge), 1 oz outer copper, from
    // https://jlcpcb.com/capabilities/pcb-capabilities (checked 2026-11). A little above the
    // absolute minimums: 2 layers 0.127 mm (5 mil) track and space (the page: 0.10 mm),
    // multilayer 0.09 mm (3.5 mil); 0.3 mm via drill (smaller costs extra), 0.15 mm annular ring;
    // copper 0.3 mm from the routed edge; silkscreen text 1 mm high, 0.15 mm stroke.
    struct NUMBERS
    {
        int track, clearance, drill, annular, viaDia, holeClearance, holeToHole, edge, silkH, silkW;
    };

    static const std::map<std::string, NUMBERS> presets = {
        { "jlcpcb_2layer", { 127000, 127000, 300000, 150000, 600000, 250000, 250000, 300000, 1000000, 150000 } },
        { "jlcpcb_4layer", { 90000, 90000, 300000, 150000, 600000, 250000, 250000, 300000, 1000000, 150000 } },
        { "jlcpcb_6layer", { 90000, 90000, 300000, 150000, 600000, 250000, 250000, 300000, 1000000, 150000 } },
    };

    auto it = presets.find( aName );

    if( it == presets.end() )
    {
        aNote = "presets: jlcpcb_2layer, jlcpcb_4layer, jlcpcb_6layer";
        return std::nullopt;
    }

    const NUMBERS& p = it->second;
    kicadd::rules::STATE st = aFrom;

    st.minTrackWidth = p.track;
    st.minClearance = p.clearance;
    st.minThroughDrill = p.drill;
    st.minAnnularWidth = p.annular;
    st.minViaDiameter = p.viaDia;
    st.holeClearance = p.holeClearance;
    st.holeToHole = p.holeToHole;
    st.copperEdgeClearance = p.edge;
    st.minSilkTextHeight = p.silkH;
    st.minSilkTextThickness = p.silkW;

    // the Default class may not ask for less than the fab can make
    kicadd::rules::CLASS& dflt = st.classes[NETCLASS::Default];

    auto raise = []( std::optional<int>& aValue, int aMin )
    {
        if( !aValue || *aValue < aMin )
            aValue = aMin;
    };

    raise( dflt.trackWidth, p.track );
    raise( dflt.clearance, p.clearance );
    raise( dflt.viaDrill, p.drill );
    raise( dflt.viaDiameter, std::max( p.viaDia, dflt.viaDrill.value_or( p.drill ) + 2 * p.annular ) );

    aNote = "JLCPCB standard process (no extra charge), 1 oz copper; numbers as of 2026-11 from "
            "jlcpcb.com/capabilities/pcb-capabilities, check there before ordering";
    return st;
}

} // namespace kicadd::setup
