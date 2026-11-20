/*
 * KiCadD: the board half of the design review (audit).
 *
 * - current: the narrowest track of each net given a current, layer by layer, against the width
 *   IPC-2221 asks for in that layer's copper (outer / inner);
 * - voltage: the closest copper of each net given a voltage (or named like "+12V") to every other
 *   net on the same layer, against IPC-2221B table 6-1 for the voltage between them (nets not given
 *   are taken as 0 V);
 * - silk: silkscreen text over exposed pads (unreadable, and the fab clips it) and references
 *   overlapping each other; silkscreen lines over pads, per footprint;
 * - testpoint: nets with no test point (TP* footprints or TestPoint library parts);
 * - plane, return_via: reference planes under signal tracks and at their vias (kicadd_refplane);
 * - diff_pair, length: diff pair skew / coupling and net lengths (kicadd_lengths);
 * - ir_drop: DC IR drop per supply rail, via and track currents (kicadd_irdrop);
 * - crosstalk: signal tracks side by side within 3H (kicadd_xtalk).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_audit.h"
#include "common/kicadd_ipc2221.h"
#include "pcb/kicadd_irdrop.h"
#include "pcb/kicadd_lengths.h"
#include "pcb/kicadd_refplane.h"
#include "pcb/kicadd_xtalk.h"

#include <board.h>
#include <board_design_settings.h>
#include <board_stackup_manager/board_stackup.h>
#include <footprint.h>
#include <netclass.h>
#include <project/net_settings.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_textbox.h>
#include <pcb_track.h>
#include <zone.h>
#include <geometry/shape.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <optional>
#include <regex>
#include <set>

namespace kicadd
{

using namespace kicadd::board;

namespace
{

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

std::string mm( double aMm, int aDigits = 2 )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.*f", aDigits, aMm );
    std::string s( buf );

    while( s.find( '.' ) != std::string::npos && ( s.back() == '0' || s.back() == '.' ) )
    {
        bool dot = s.back() == '.';
        s.pop_back();

        if( dot )
            break;
    }

    return s;
}

std::string at( const VECTOR2I& p )
{
    return "(" + mm( p.x / 1e6 ) + ", " + mm( p.y / 1e6 ) + ")";
}

/// "VCC" answers to "VCC", "/VCC" and "/Sheet/VCC"
bool sameNet( const std::string& aNet, const std::string& aWanted )
{
    if( aNet == aWanted || aNet == "/" + aWanted )
        return true;

    return aNet.size() > aWanted.size()
           && aNet.compare( aNet.size() - aWanted.size() - 1, aWanted.size() + 1, "/" + aWanted ) == 0;
}

/// "+5V" 5, "3V3" 3.3, "-12V" -12, "/Power/+13V" 13; nullopt for other names
std::optional<double> voltsFromName( const std::string& aNet )
{
    static const std::regex re( R"(^([+-]?)(\d+)V(\d*)$)", std::regex::icase );
    std::string             last = aNet.substr( aNet.rfind( '/' ) == std::string::npos ? 0 : aNet.rfind( '/' ) + 1 );
    std::smatch             m;

    if( !std::regex_match( last, m, re ) )
        return std::nullopt;

    double v = std::stod( m[2].str() + ( m[3].length() ? "." + m[3].str() : "" ) );
    return m[1] == "-" ? -v : v;
}

void finding( BoardAuditResponse& aOut, const char* aCat, const char* aSeverity, const std::string& aMsg,
              const std::vector<std::string>& aRefs = {}, std::optional<VECTOR2I> aAt = std::nullopt )
{
    BoardAuditFinding* f = aOut.add_findings();
    f->set_category( aCat );
    f->set_severity( aSeverity );
    f->set_message( aMsg );

    for( const std::string& r : aRefs )
        f->add_refs( r );

    if( aAt )
    {
        f->mutable_at()->set_x( aAt->x );
        f->mutable_at()->set_y( aAt->y );
    }
}

std::string padName( const PAD* aPad )
{
    const FOOTPRINT* fp = aPad->GetParentFootprint();
    const std::string ref = fp ? str( fp->GetReference() ) : std::string( "?" );
    return aPad->GetNumber().IsEmpty() ? ref + " (unnumbered pad)" : ref + "." + str( aPad->GetNumber() );
}

// ---------------------------------------------------------------------------- current

void checkCurrents( BOARD& aBoard, const BoardAudit& aReq, double aOuterUm, double aInnerUm,
                    BoardAuditResponse& aOut )
{
    const double rise = aReq.temp_rise() > 0 ? aReq.temp_rise() : 10.0;

    std::map<PCB_LAYER_ID, double> copperUm;

    for( BOARD_STACKUP_ITEM* item : aBoard.GetDesignSettings().GetStackupDescriptor().GetList() )
    {
        if( item->GetType() == BS_ITEM_TYPE_COPPER && item->GetThickness() > 0 )
            copperUm[item->GetBrdLayerId()] = item->GetThickness() / 1000.0;
    }

    for( const auto& [wanted, amps] : aReq.currents() )
    {
        if( amps <= 0 )
            continue;

        std::vector<NETINFO_ITEM*> nets;

        for( NETINFO_ITEM* net : aBoard.GetNetInfo() )
        {
            if( net->GetNetCode() > 0 && sameNet( str( net->GetNetname() ), wanted ) )
                nets.push_back( net );
        }

        if( nets.empty() )
        {
            finding( aOut, "current", "warning", "no net " + wanted + " on the board" );
            continue;
        }

        for( NETINFO_ITEM* net : nets )
        {
            const std::string name = str( net->GetNetname() );

            struct WORST
            {
                PCB_TRACK* track = nullptr;
                int        narrow = 0;   // segments under the width asked for
            };

            std::map<PCB_LAYER_ID, WORST> worst;
            int                           tracks = 0;

            for( PCB_TRACK* t : aBoard.Tracks() )
            {
                if( t->GetNetCode() != net->GetNetCode() || t->Type() == PCB_VIA_T )
                    continue;

                tracks++;
                WORST& w = worst[t->GetLayer()];

                if( !w.track || t->GetWidth() < w.track->GetWidth() )
                    w.track = t;
            }

            if( tracks == 0 )
            {
                finding( aOut, "current", "info",
                         name + " (" + mm( amps ) + " A): no tracks yet (zones and pads only, or not routed)" );
                continue;
            }

            for( auto& [layer, w] : worst )
            {
                const bool   outer = IsExternalCopperLayer( layer );
                const double um = copperUm.count( layer ) ? copperUm[layer] : ( outer ? aOuterUm : aInnerUm > 0 ? aInnerUm : 17.5 );
                const double need = ipc2221::TrackWidthMm( amps, rise, um, outer );

                for( PCB_TRACK* t : aBoard.Tracks() )
                {
                    if( t->GetNetCode() == net->GetNetCode() && t->Type() != PCB_VIA_T && t->GetLayer() == layer
                        && t->GetWidth() / 1e6 < need - 0.0005 )
                    {
                        w.narrow++;
                    }
                }

                if( w.narrow == 0 )
                    continue;

                const double   width = w.track->GetWidth() / 1e6;
                const VECTOR2I mid = ( w.track->GetStart() + w.track->GetEnd() ) / 2;

                finding( aOut, "current", "warning",
                         name + " (" + mm( amps ) + " A): " + std::to_string( w.narrow ) + " " + str( LSET::Name( layer ) )
                                 + " track segment(s) narrower than " + mm( need ) + " mm (IPC-2221, " + mm( rise, 0 )
                                 + " C rise, " + mm( um, 1 ) + " um copper, " + ( outer ? "outer" : "inner" )
                                 + "); the narrowest is " + mm( width ) + " mm at " + at( mid ) + ", good for about "
                                 + mm( ipc2221::CurrentA( width, rise, um, outer ) ) + " A",
                         {}, mid );
            }

            // the class new tracks get
            const NETCLASS* nc = net->GetNetClass();
            const double    need = ipc2221::TrackWidthMm( amps, rise, aOuterUm, true );

            if( nc && nc->GetTrackWidth() > 0 && nc->GetTrackWidth() / 1e6 < need - 0.0005 )
            {
                finding( aOut, "current", "info",
                         name + ": its net class " + str( nc->GetHumanReadableName() ) + " routes "
                                 + mm( nc->GetTrackWidth() / 1e6 ) + " mm tracks; " + mm( amps ) + " A wants "
                                 + mm( need ) + " mm on the outer layers: set_rules {\"classes\": {\"<name>\": "
                                 "{\"current\": " + mm( amps ) + "}}, \"assign\": {\"" + wanted + "\": \"<name>\"}}" );
            }
        }
    }
}

// ---------------------------------------------------------------------------- voltage

struct COPPER
{
    BOARD_CONNECTED_ITEM*  item;
    std::shared_ptr<SHAPE> shape;
    BOX2I                  box;
    int                    net;
};

std::string describe( const BOARD_CONNECTED_ITEM* aItem )
{
    switch( aItem->Type() )
    {
    case PCB_PAD_T:  return "pad " + padName( static_cast<const PAD*>( aItem ) );
    case PCB_VIA_T:  return "via";
    case PCB_ZONE_T: return "zone";
    default:         return "track";
    }
}

void checkVoltages( BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    // the voltage of each net: given, else from its name
    std::map<int, double>      volts;
    std::map<int, bool>        fromName;
    std::set<std::string>      found;

    for( NETINFO_ITEM* net : aBoard.GetNetInfo() )
    {
        if( net->GetNetCode() <= 0 )
            continue;

        const std::string name = str( net->GetNetname() );
        bool              given = false;

        for( const auto& [wanted, v] : aReq.voltages() )
        {
            if( sameNet( name, wanted ) )
            {
                volts[net->GetNetCode()] = v;
                found.insert( wanted );
                given = true;
            }
        }

        if( !given )
        {
            if( std::optional<double> v = voltsFromName( name ) )
            {
                volts[net->GetNetCode()] = *v;
                fromName[net->GetNetCode()] = true;
            }
        }
    }

    for( const auto& [wanted, v] : aReq.voltages() )
    {
        if( !found.count( wanted ) )
            finding( aOut, "voltage", "warning", "no net " + wanted + " on the board" );
    }

    if( volts.empty() )
        return;

    auto voltsOf = [&]( int aNet ) { return volts.count( aNet ) ? volts[aNet] : 0.0; };

    // what DRC already holds every pair of nets to: the smallest clearance of any net class (or the
    // board minimum, if larger). A pair needing no more than that is DRC's job, not looked at here.
    const BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
    int                          floorNm = std::numeric_limits<int>::max();

    if( bds.m_NetSettings )
    {
        if( std::shared_ptr<NETCLASS> d = bds.m_NetSettings->GetDefaultNetclass(); d && d->GetClearance() >= 0 )
            floorNm = d->GetClearance();

        for( const auto& [name, nc] : bds.m_NetSettings->GetNetclasses() )
        {
            if( nc && nc->HasClearance() )
                floorNm = std::min( floorNm, nc->GetClearance() );
        }
    }

    floorNm = floorNm == std::numeric_limits<int>::max() ? bds.m_MinClearance : std::max( floorNm, bds.m_MinClearance );

    // the largest voltage difference any net has: nets that cannot need more than the floor are skipped
    double vMin = 0, vMax = 0;

    for( const auto& [net, v] : volts )
    {
        vMin = std::min( vMin, v );
        vMax = std::max( vMax, v );
    }

    std::set<int> hot;   // nets with a pair that may need more than DRC asks

    for( const auto& [net, v] : volts )
    {
        const double worst = std::max( std::fabs( v - vMin ), std::fabs( v - vMax ) );

        if( KiROUND( ipc2221::ClearanceMm( worst, true ) * 1e6 ) > floorNm )
            hot.insert( net );
    }

    char floorText[160];
    std::snprintf( floorText, sizeof( floorText ),
                   "pairs needing %s mm or less (the smallest net class clearance) are left to run_drc",
                   mm( floorNm / 1e6 ).c_str() );

    if( hot.empty() )
    {
        finding( aOut, "voltage", "info",
                 std::to_string( volts.size() ) + " net(s) with a voltage" + ( aReq.voltages().empty() ? " (from their names)" : "" )
                         + ": none is far enough from another to need more than DRC already checks; " + floorText );
        return;
    }

    // closest approach per (net, other net, layer): distance, place, what
    struct CLOSE
    {
        double      need = 0;
        int         actual = std::numeric_limits<int>::max();
        VECTOR2I    where;
        std::string what;
    };

    // per pair of nets, the layer where it falls shortest of what it needs (else where it is closest)
    struct PAIR
    {
        CLOSE        c;
        PCB_LAYER_ID layer = UNDEFINED_LAYER;
    };

    std::map<std::pair<int, int>, PAIR>                 pairs;
    std::map<std::tuple<int, int, PCB_LAYER_ID>, CLOSE> closest;
    bool                                                unfilled = false;

    for( PCB_LAYER_ID layer : aBoard.GetEnabledLayers().CuStack() )
    {
        const bool          outer = IsExternalCopperLayer( layer );
        std::vector<COPPER> items;

        auto add = [&]( BOARD_CONNECTED_ITEM* aItem, std::shared_ptr<SHAPE> aShape )
        {
            if( aShape && !aShape->IsNull() )
                items.push_back( { aItem, aShape, aShape->BBox(), aItem->GetNetCode() } );
        };

        for( PCB_TRACK* t : aBoard.Tracks() )
        {
            if( t->IsOnLayer( layer ) )
                add( t, t->GetEffectiveShape( layer ) );
        }

        for( FOOTPRINT* fp : aBoard.Footprints() )
        {
            for( PAD* pad : fp->Pads() )
            {
                if( pad->IsOnLayer( layer ) && pad->FlashLayer( layer ) )
                    add( pad, pad->GetEffectiveShape( layer ) );
            }
        }

        for( ZONE* z : aBoard.Zones() )
        {
            if( z->GetIsRuleArea() || !z->IsOnLayer( layer ) )
                continue;

            if( !z->IsFilled() )
                unfilled = true;

            if( z->HasFilledPolysForLayer( layer ) )
                add( z, z->GetEffectiveShape( layer ) );
        }

        // a grid of the items, so each hot item looks only at its neighbourhood
        const int                                  cell = 5000000;   // 5 mm
        std::map<std::pair<int, int>, std::vector<size_t>> grid;

        auto cellsOf = [&]( const BOX2I& aBox, auto&& aFn )
        {
            for( int cx = (int) std::floor( (double) aBox.GetLeft() / cell ); cx <= (int) std::floor( (double) aBox.GetRight() / cell ); cx++ )
            {
                for( int cy = (int) std::floor( (double) aBox.GetTop() / cell ); cy <= (int) std::floor( (double) aBox.GetBottom() / cell ); cy++ )
                    aFn( cx, cy );
            }
        };

        for( size_t i = 0; i < items.size(); i++ )
            cellsOf( items[i].box, [&]( int cx, int cy ) { grid[{ cx, cy }].push_back( i ); } );

        for( const COPPER& a : items )
        {
            if( !hot.count( a.net ) )
                continue;

            const int reach = KiROUND( ipc2221::ClearanceMm( std::max( std::fabs( voltsOf( a.net ) - vMin ),
                                                                       std::fabs( voltsOf( a.net ) - vMax ) ),
                                                             outer ) * 2e6 );
            BOX2I     around = a.box;
            around.Inflate( std::max( reach, 500000 ) );
            std::set<size_t> nearby;

            cellsOf( around, [&]( int cx, int cy )
                     {
                         auto it = grid.find( { cx, cy } );

                         if( it != grid.end() )
                             nearby.insert( it->second.begin(), it->second.end() );
                     } );

            for( size_t bi : nearby )
            {
                const COPPER& b = items[bi];

                if( b.net == a.net || ( hot.count( b.net ) && b.net < a.net ) )
                    continue;   // a pair of hot nets once

                const double need = ipc2221::ClearanceMm( voltsOf( a.net ) - voltsOf( b.net ), outer );
                const int    needNm = KiROUND( need * 1e6 );

                if( needNm <= floorNm )
                    continue;
                // look a little further than needed, to say how close the nearest copper is
                const int    look = std::max( needNm * 2, 500000 );

                BOX2I box = a.box;
                box.Inflate( look );

                if( !box.Intersects( b.box ) )
                    continue;

                int      actual = 0;
                VECTOR2I where;

                if( !a.shape->Collide( b.shape.get(), look, &actual, &where ) )
                    continue;

                CLOSE& c = closest[{ a.net, b.net, layer }];

                if( actual < c.actual )
                {
                    c.need = need;
                    c.actual = actual;
                    c.where = where;
                    c.what = describe( a.item ) + " to " + describe( b.item );
                }
            }
        }
    }

    for( const auto& [key, c] : closest )
    {
        const auto& [na, nb, layer] = key;
        PAIR&       p = pairs[{ na, nb }];
        auto        shortBy = []( const CLOSE& x ) { return x.need - x.actual / 1e6; };

        if( p.layer == UNDEFINED_LAYER || shortBy( c ) > shortBy( p.c ) )
            p = { c, layer };
    }

    // per net given a voltage: violations (the worst first, 15 a net), else its closest approach
    std::map<int, std::pair<double, std::string>> nearestOk;   // net -> mm, text

    struct SHORT
    {
        double      by;
        int         net;
        std::string text;
        VECTOR2I    where;
    };

    std::vector<SHORT> short_;

    for( const auto& [key, p] : pairs )
    {
        const auto& [na, nb] = key;
        const CLOSE&       c = p.c;
        const PCB_LAYER_ID layer = p.layer;
        NETINFO_ITEM* a = aBoard.FindNet( na );
        NETINFO_ITEM* b = aBoard.FindNet( nb );
        std::string   an = a ? str( a->GetNetname() ) : "?";
        std::string   bn = b && nb > 0 ? str( b->GetNetname() ) : "no net";
        const double  dv = std::fabs( voltsOf( na ) - voltsOf( nb ) );
        const double  actual = c.actual / 1e6;

        auto label = [&]( int aNet, const std::string& aName )
        {
            return aName + " (" + mm( voltsOf( aNet ), 1 ) + " V" + ( fromName.count( aNet ) ? ", from its name" : "" )
                   + ( volts.count( aNet ) ? "" : ", taken as 0" ) + ")";
        };

        if( actual < c.need - 0.0005 )
        {
            short_.push_back( { c.need - actual, na,
                                label( na, an ) + " to " + label( nb, bn ) + " on " + str( LSET::Name( layer ) ) + ": "
                                        + mm( actual ) + " mm (" + c.what + ") at " + at( c.where ) + "; IPC-2221B table 6-1 "
                                        + ( IsExternalCopperLayer( layer ) ? "B2 (outer, uncoated)" : "B1 (inner)" )
                                        + " wants " + mm( c.need ) + " mm for " + mm( dv, 1 ) + " V",
                                c.where } );
        }
        else
        {
            auto& n = nearestOk[na];

            if( n.second.empty() || actual < n.first )
                n = { actual, label( na, an ) + ": closest " + label( nb, bn ) + " copper " + mm( actual ) + " mm on "
                                      + str( LSET::Name( layer ) ) + " (wants " + mm( c.need ) + " mm for "
                                      + mm( dv, 1 ) + " V)" };
        }
    }

    std::sort( short_.begin(), short_.end(), []( const SHORT& x, const SHORT& y ) { return x.by > y.by; } );
    std::map<int, int> shown, more;

    for( const SHORT& v : short_ )
    {
        if( shown[v.net]++ < 15 )
            finding( aOut, "voltage", "warning", v.text, {}, v.where );
        else
            more[v.net]++;
    }

    for( const auto& [net, n] : more )
    {
        NETINFO_ITEM* ni = aBoard.FindNet( net );
        finding( aOut, "voltage", "warning",
                 std::to_string( n ) + " more net(s) too close to " + ( ni ? str( ni->GetNetname() ) : std::string( "?" ) )
                         + " (the 15 worst are above)" );
    }

    for( const auto& [net, n] : nearestOk )
    {
        if( !shown.count( net ) )
            finding( aOut, "voltage", "info", n.second );
    }

    finding( aOut, "voltage", "info", floorText );

    if( unfilled )
        finding( aOut, "voltage", "info", "some zones are not filled: their copper was not measured (refill_zones first)" );
}

// ---------------------------------------------------------------------------- silkscreen

struct SILK
{
    BOARD_ITEM*            item;
    PCB_LAYER_ID           layer;
    std::shared_ptr<SHAPE> shape;
    BOX2I                  box;
    bool                   text;
    std::string            owner;   // footprint reference, "" for the board's own
    std::string            what;    // "R5 reference", "R5 silkscreen line", "board text 'REV A'"
};

void checkSilk( BOARD& aBoard, BoardAuditResponse& aOut )
{
    std::vector<SILK> silk;

    auto add = [&]( BOARD_ITEM* aItem, const std::string& aOwner, const std::string& aWhat, bool aText )
    {
        PCB_LAYER_ID layer = aItem->GetLayer();

        if( layer != F_SilkS && layer != B_SilkS )
            return;

        std::shared_ptr<SHAPE> shape = aItem->GetEffectiveShape( layer );

        if( shape && !shape->IsNull() )
            silk.push_back( { aItem, layer, shape, aText ? aItem->GetBoundingBox() : shape->BBox(), aText, aOwner, aWhat } );
    };

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        const std::string ref = str( fp->GetReference() );

        for( PCB_FIELD* f : fp->GetFields() )
        {
            if( f && f->IsVisible() && !f->GetText().IsEmpty() )
            {
                std::string kind = f->IsReference() ? "reference" : f->IsValue() ? "value" : str( f->GetName() );
                add( f, ref, ref + " " + kind, true );
            }
        }

        for( BOARD_ITEM* item : fp->GraphicalItems() )
        {
            if( item->Type() == PCB_TEXT_T && static_cast<PCB_TEXT*>( item )->IsVisible() )
                add( item, ref, ref + " text '" + str( static_cast<PCB_TEXT*>( item )->GetText() ) + "'", true );
            else if( item->Type() == PCB_TEXTBOX_T )
                add( item, ref, ref + " text box", true );
            else if( item->Type() == PCB_SHAPE_T )
                add( item, ref, ref + " silkscreen line", false );
        }
    }

    for( BOARD_ITEM* item : aBoard.Drawings() )
    {
        if( item->Type() == PCB_TEXT_T )
            add( item, "", "board text '" + str( static_cast<PCB_TEXT*>( item )->GetText() ) + "'", true );
        else if( item->Type() == PCB_TEXTBOX_T )
            add( item, "", "board text box", true );
        else if( item->Type() == PCB_SHAPE_T )
            add( item, "", "board silkscreen shape", false );
    }

    aOut.set_silk_checked( (int) silk.size() );

    // exposed pads (in the mask opening) on each side
    struct EXPOSED
    {
        PAD*                   pad;
        std::shared_ptr<SHAPE> shape;
        BOX2I                  box;
    };

    std::map<PCB_LAYER_ID, std::vector<EXPOSED>> pads;   // by silk layer

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            for( auto [mask, cu, silkLayer] : { std::tuple{ F_Mask, F_Cu, F_SilkS }, std::tuple{ B_Mask, B_Cu, B_SilkS } } )
            {
                if( !pad->IsOnLayer( mask ) )
                    continue;

                std::shared_ptr<SHAPE> s = pad->GetEffectiveShape( pad->IsOnLayer( cu ) ? cu : mask );

                if( s && !s->IsNull() )
                    pads[silkLayer].push_back( { pad, s, s->BBox() } );
            }
        }
    }

    int                                 textOverPad = 0;
    std::map<std::string, int>          linesOverPads;   // footprint -> lines
    std::vector<std::string>            lineRefs;

    for( const SILK& s : silk )
    {
        std::vector<std::string> hit;
        VECTOR2I                 where;

        for( const EXPOSED& p : pads[s.layer] )
        {
            if( !s.box.Intersects( p.box ) )
                continue;

            VECTOR2I loc;

            if( s.shape->Collide( p.shape.get(), 0, nullptr, &loc ) )
            {
                if( hit.empty() )
                    where = loc;

                hit.push_back( padName( p.pad ) );
            }
        }

        if( hit.empty() )
            continue;

        if( s.text )
        {
            if( ++textOverPad <= 20 )
            {
                std::string list;

                for( size_t i = 0; i < hit.size() && i < 4; i++ )
                    list += ( i ? ", " : "" ) + hit[i];

                if( hit.size() > 4 )
                    list += ", ... (" + std::to_string( hit.size() ) + ")";

                finding( aOut, "silk", "warning", s.what + " over pad " + list + " at " + at( where ) + " (the fab clips it)",
                         s.owner.empty() ? std::vector<std::string>{} : std::vector<std::string>{ s.owner }, where );
            }
        }
        else
        {
            const std::string key = s.owner.empty() ? "board" : s.owner;

            if( !linesOverPads[key]++ )
                lineRefs.push_back( key );
        }
    }

    if( textOverPad > 20 )
        finding( aOut, "silk", "warning", std::to_string( textOverPad - 20 ) + " more silkscreen text(s) over pads" );

    if( !lineRefs.empty() )
    {
        std::string list;

        for( size_t i = 0; i < lineRefs.size() && i < 15; i++ )
            list += ( i ? ", " : "" ) + lineRefs[i] + " (" + std::to_string( linesOverPads[lineRefs[i]] ) + ")";

        if( lineRefs.size() > 15 )
            list += ", ... (" + std::to_string( lineRefs.size() ) + " footprints)";

        finding( aOut, "silk", "info", "silkscreen lines over exposed pads (clipped by the fab): " + list, lineRefs );
    }

    // references over each other
    int overlaps = 0;

    for( size_t i = 0; i < silk.size(); i++ )
    {
        if( !silk[i].text )
            continue;

        for( size_t j = i + 1; j < silk.size(); j++ )
        {
            // a part's own texts too (a reference on its value): silk_tidy judges them the same way
            if( !silk[j].text || silk[j].layer != silk[i].layer || silk[j].item == silk[i].item )
                continue;

            // the strokes, not the boxes (line spacing makes boxes of neighbouring rows touch)
            if( !silk[i].box.Intersects( silk[j].box ) || !silk[i].shape->Collide( silk[j].shape.get(), 0 ) )
                continue;

            if( ++overlaps <= 20 )
            {
                BOX2I both = silk[i].box;
                both.Intersect( silk[j].box );
                std::vector<std::string> refs;

                for( const SILK* s : { &silk[i], &silk[j] } )
                {
                    if( !s->owner.empty() )
                        refs.push_back( s->owner );
                }

                finding( aOut, "silk", "warning", silk[i].what + " overlaps " + silk[j].what + " at " + at( both.Centre() ),
                         refs, both.Centre() );
            }
        }
    }

    if( overlaps > 20 )
        finding( aOut, "silk", "warning", std::to_string( overlaps - 20 ) + " more overlapping silkscreen texts" );

    // texts reaching past the board edge (the fab clips them; silk_tidy moves such references)
    SHAPE_POLY_SET outline;

    if( !aBoard.GetBoardPolygonOutlines( outline, false ) || outline.OutlineCount() == 0 )
        return;

    int off = 0;

    for( const SILK& s : silk )
    {
        if( !s.text )
            continue;

        const BOX2I g = s.shape->BBox();   // the glyphs, not the text box
        bool        out = false;

        for( const VECTOR2I& c : { g.GetOrigin(), VECTOR2I( g.GetRight(), g.GetTop() ), g.GetEnd(),
                                   VECTOR2I( g.GetLeft(), g.GetBottom() ) } )
            out = out || !outline.Contains( c );

        if( out && ++off <= 20 )
            finding( aOut, "silk", "warning", s.what + " at " + at( g.Centre() ) + " reaches past the board edge (the fab clips it)",
                     s.owner.empty() ? std::vector<std::string>{} : std::vector<std::string>{ s.owner }, g.Centre() );
    }

    if( off > 20 )
        finding( aOut, "silk", "warning", std::to_string( off - 20 ) + " more silkscreen texts past the board edge" );
}

// ---------------------------------------------------------------------------- test points

void checkTestPoints( BOARD& aBoard, BoardAuditResponse& aOut )
{
    static const std::regex tpRef( R"(^TP\d+$)", std::regex::icase );
    std::set<int>           covered;
    std::map<int, int>      pads;   // net -> pads
    int                     tps = 0;

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        const bool tp = std::regex_match( str( fp->GetReference() ), tpRef )
                        || fp->GetFPID().GetLibItemName().wx_str().Contains( wxS( "TestPoint" ) );
        tps += tp;

        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() <= 0 )
                continue;

            pads[pad->GetNetCode()]++;

            if( tp )
                covered.insert( pad->GetNetCode() );
        }
    }

    aOut.set_test_points( tps );

    std::vector<std::string> without;
    int                      nets = 0;

    for( const auto& [code, n] : pads )
    {
        NETINFO_ITEM* net = aBoard.FindNet( code );
        std::string   name = net ? str( net->GetNetname() ) : "";

        if( n < 2 || name.empty() || name.rfind( "unconnected-", 0 ) == 0 )
            continue;

        nets++;

        if( !covered.count( code ) )
            without.push_back( name );
    }

    if( tps == 0 )
    {
        finding( aOut, "testpoint", "info", "no test points on the board (TP* footprints); " + std::to_string( nets )
                                                    + " nets could use one for bring-up or flying-probe testing" );
        return;
    }

    if( without.empty() )
        return;

    std::string list;

    for( size_t i = 0; i < without.size() && i < 15; i++ )
        list += ( i ? ", " : "" ) + without[i];

    if( without.size() > 15 )
        list += ", ...";

    finding( aOut, "testpoint", "info",
             std::to_string( without.size() ) + " of " + std::to_string( nets ) + " nets have no test point: " + list );
}

} // namespace


void BoardCopperUm( const BOARD& aBoard, double& aOuter, double& aInner )
{
    aOuter = 0;
    aInner = 0;

    for( BOARD_STACKUP_ITEM* item : aBoard.GetDesignSettings().GetStackupDescriptor().GetList() )
    {
        if( item->GetType() != BS_ITEM_TYPE_COPPER || item->GetThickness() <= 0 )
            continue;

        double& t = IsExternalCopperLayer( item->GetBrdLayerId() ) ? aOuter : aInner;
        t = t > 0 ? std::min( t, item->GetThickness() / 1000.0 ) : item->GetThickness() / 1000.0;
    }

    if( aOuter <= 0 )
        aOuter = 35;

    if( aInner <= 0 && aBoard.GetCopperLayerCount() > 2 )
        aInner = 17.5;
}


void AuditBoard( BOARD& aBoard, const BoardAudit& aReq, BoardAuditResponse& aOut )
{
    double outer, inner;
    BoardCopperUm( aBoard, outer, inner );
    aOut.set_outer_copper_um( outer );
    aOut.set_inner_copper_um( inner );

    checkCurrents( aBoard, aReq, outer, inner, aOut );
    checkVoltages( aBoard, aReq, aOut );
    checkSilk( aBoard, aOut );
    checkTestPoints( aBoard, aOut );
    CheckReferencePlanes( aBoard, aReq, aOut );
    CheckLengths( aBoard, aReq, aOut );
    CheckIrDrop( aBoard, aReq, aOut );
    CheckCrosstalk( aBoard, aReq, aOut );
}

} // namespace kicadd
