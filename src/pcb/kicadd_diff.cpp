/*
 * KiCadD: what changed between the board in memory and a board file (the last save by default).
 *
 * The file is read into a board of its own (the KiCad s-expression reader, no project: nothing of
 * the open project is touched). Footprints, zones and drawings are paired by their UUID (stable
 * across saves), footprints without a partner then by reference (another revision of the
 * design). Tracks are compared per net: segments, length, vias. Net classes and rules live in
 * the project file and are not compared.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_diff.h"

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_track.h>
#include <zone.h>
#include <io/io_mgr.h>
#include <pcb_io/pcb_io.h>
#include <pcb_io/pcb_io_mgr.h>
#include <ki_exception.h>
#include <string_utils.h>

#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <map>
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
    std::snprintf( buf, sizeof( buf ), "%.*f", aDigits, std::fabs( aMm ) < 0.5 * std::pow( 10, -aDigits ) ? 0.0 : aMm );
    std::string s( buf );

    if( s.find( '.' ) != std::string::npos )
    {
        while( s.back() == '0' )
            s.pop_back();

        if( s.back() == '.' )
            s.pop_back();
    }

    return s;
}

std::string at( const VECTOR2I& p )
{
    return "(" + mm( p.x / 1e6 ) + ", " + mm( p.y / 1e6 ) + ")";
}

std::string signedMm( double aMm )
{
    const std::string s = mm( aMm );   // rounded first: -0.001 reads "+0", not "0"
    return ( s[0] == '-' ? "" : "+" ) + s;
}

/// A section of the answer: a summary and at most aLimit lines
struct SECTION
{
    BoardDiffSection* s;
    int               limit;

    void line( const std::string& aLine )
    {
        if( s->lines_size() < limit )
            s->add_lines( aLine );
        else
            s->set_more( s->more() + 1 );
    }
};

std::string describeFp( const FOOTPRINT* aFp )
{
    return str( aFp->GetReference() ) + " " + str( aFp->GetValue() ) + " (" + str( aFp->GetFPID().Format().wx_str() )
           + ") at " + at( aFp->GetPosition() ) + ( aFp->IsFlipped() ? " bottom" : "" );
}

std::map<std::string, std::string> userFields( const FOOTPRINT* aFp )
{
    std::map<std::string, std::string> out;

    for( const PCB_FIELD* f : aFp->GetFields() )
    {
        if( f && !f->IsMandatory() )
            out[str( f->GetName() )] = str( f->GetText() );
    }

    return out;
}

/// A footprint's texts (its fields and its own text items) where they sit on the part: position
/// and angle relative to the footprint (so moving or turning the part changes nothing here)
struct FP_TEXT
{
    VECTOR2I     rel;
    double       angle = 0;      // on the part
    double       shown = 0;      // on the board (how it reads)
    bool         visible = true;
    PCB_LAYER_ID layer = UNDEFINED_LAYER;
};

std::map<std::string, FP_TEXT> footprintTexts( const FOOTPRINT* aFp )
{
    std::map<std::string, FP_TEXT> out;

    auto add = [&]( const std::string& aKey, const PCB_TEXT* aText )
    {
        FP_TEXT t;
        t.rel = aText->GetFPRelativePosition();
        t.angle = ( aText->GetTextAngle() - aFp->GetOrientation() ).Normalized().AsDegrees();
        t.shown = aText->GetTextAngle().Normalized().AsDegrees();
        t.visible = aText->IsVisible();
        t.layer = aText->GetLayer();
        out[aKey] = t;
    };

    for( const PCB_FIELD* f : aFp->GetFields() )
    {
        if( f )
            add( f->IsReference() ? "reference" : f->IsValue() ? "value" : str( f->GetName() ), f );
    }

    for( const BOARD_ITEM* item : aFp->GraphicalItems() )
    {
        if( item->Type() == PCB_TEXT_T )
            add( "text '" + str( static_cast<const PCB_TEXT*>( item )->GetText() ) + "'",
                 static_cast<const PCB_TEXT*>( item ) );
    }

    return out;
}

// ---------------------------------------------------------------------------- footprints

void diffFootprints( const BOARD& aOld, const BOARD& aNow, int aLimit, BoardDiffResponse& aOut, int& aChanges )
{
    SECTION sec{ aOut.add_sections(), aLimit };
    sec.s->set_name( "footprints" );

    std::map<KIID, const FOOTPRINT*>        oldById;
    std::map<std::string, const FOOTPRINT*> oldByRef;

    for( const FOOTPRINT* fp : aOld.Footprints() )
    {
        oldById[fp->m_Uuid] = fp;
        oldByRef[str( fp->GetReference() )] = fp;
    }

    std::set<KIID> nowIds;

    for( const FOOTPRINT* fp : aNow.Footprints() )
        nowIds.insert( fp->m_Uuid );

    std::set<const FOOTPRINT*> paired;
    std::vector<std::pair<const FOOTPRINT*, const FOOTPRINT*>> pairs;   // old, now
    std::vector<const FOOTPRINT*> added;

    for( const FOOTPRINT* fp : aNow.Footprints() )
    {
        auto it = oldById.find( fp->m_Uuid );
        const FOOTPRINT* o = it != oldById.end() && !paired.count( it->second ) ? it->second : nullptr;

        if( !o )
        {
            auto r = oldByRef.find( str( fp->GetReference() ) );

            if( r != oldByRef.end() && !paired.count( r->second ) && !nowIds.count( r->second->m_Uuid ) )
                o = r->second;
        }

        if( o )
        {
            paired.insert( o );
            pairs.push_back( { o, fp } );
        }
        else
        {
            added.push_back( fp );
        }
    }

    std::vector<const FOOTPRINT*> removed;

    for( const FOOTPRINT* fp : aOld.Footprints() )
    {
        if( !paired.count( fp ) )
            removed.push_back( fp );
    }

    auto byRef = []( const FOOTPRINT* a, const FOOTPRINT* b )
    {
        return StrNumCmp( a->GetReference(), b->GetReference(), CMP_T::IGNORE_CASE ) < 0;
    };

    std::sort( added.begin(), added.end(), byRef );
    std::sort( removed.begin(), removed.end(), byRef );
    std::sort( pairs.begin(), pairs.end(), [&]( const auto& a, const auto& b ) { return byRef( a.second, b.second ); } );

    for( const FOOTPRINT* fp : added )
        sec.line( "added " + describeFp( fp ) );

    for( const FOOTPRINT* fp : removed )
        sec.line( "removed " + describeFp( fp ) );

    std::map<std::string, int> kinds;
    int                        changed = 0;

    for( const auto& [o, n] : pairs )
    {
        std::vector<std::string> what;
        auto                     kind = [&]( const char* aKind, const std::string& aText )
        {
            kinds[aKind]++;
            what.push_back( aText );
        };

        if( o->GetReference() != n->GetReference() )
            kind( "renumbered", "was " + str( o->GetReference() ) );

        if( o->IsFlipped() != n->IsFlipped() )
            kind( "flipped", std::string( "to the " ) + ( n->IsFlipped() ? "bottom" : "top" ) );

        const VECTOR2I d = n->GetPosition() - o->GetPosition();

        if( d.x != 0 || d.y != 0 )
            kind( "moved", "moved " + signedMm( d.x / 1e6 ) + ", " + signedMm( d.y / 1e6 ) + " mm to " + at( n->GetPosition() ) );

        if( o->GetOrientation() != n->GetOrientation() )
            kind( "rotated", "rotated " + mm( o->GetOrientation().Normalized().AsDegrees(), 1 ) + " -> " + mm( n->GetOrientation().Normalized().AsDegrees(), 1 ) + " deg" );

        if( o->GetValue() != n->GetValue() )
            kind( "value", "value " + str( o->GetValue() ) + " -> " + str( n->GetValue() ) );

        if( o->GetFPID() != n->GetFPID() )
            kind( "footprint", "footprint " + str( o->GetFPID().Format().wx_str() ) + " -> " + str( n->GetFPID().Format().wx_str() ) );

        std::map<std::string, std::string> of = userFields( o ), nf = userFields( n );

        for( const auto& [k, v] : nf )
        {
            if( !of.count( k ) )
                kind( "fields", k + " added (" + v + ")" );
            else if( of[k] != v )
                kind( "fields", k + " " + ( of[k].empty() ? "\"\"" : of[k] ) + " -> " + ( v.empty() ? "\"\"" : v ) );
        }

        for( const auto& [k, v] : of )
        {
            if( !nf.count( k ) )
                kind( "fields", k + " removed" );
        }

        // texts on the part (silk_tidy, a hand-placed reference): moved, turned, shown / hidden
        {
            std::map<std::string, FP_TEXT> ot = footprintTexts( o ), nt = footprintTexts( n );

            for( const auto& [k, b] : nt )
            {
                auto it = ot.find( k );

                if( it == ot.end() )
                    continue;   // a new field: listed under fields

                const FP_TEXT& a = it->second;
                std::string    t;

                if( a.rel != b.rel )
                    t += " moved " + signedMm( ( b.rel.x - a.rel.x ) / 1e6 ) + ", " + signedMm( ( b.rel.y - a.rel.y ) / 1e6 )
                         + " mm on the part";

                if( std::fabs( a.angle - b.angle ) > 0.01 )
                    t += std::string( t.empty() ? "" : "," ) + " turned " + mm( a.shown, 1 ) + " -> " + mm( b.shown, 1 ) + " deg";

                if( a.visible != b.visible )
                    t += std::string( t.empty() ? "" : "," ) + ( b.visible ? " shown" : " hidden" );

                if( a.layer != b.layer )
                    t += std::string( t.empty() ? "" : "," ) + " " + str( LSET::Name( a.layer ) ) + " -> "
                         + str( LSET::Name( b.layer ) );

                if( !t.empty() )
                    kind( "texts", k + t );
            }
        }

        if( o->IsDNP() != n->IsDNP() )
            kind( "attributes", n->IsDNP() ? "now DNP" : "no longer DNP" );

        if( o->IsExcludedFromBOM() != n->IsExcludedFromBOM() )
            kind( "attributes", n->IsExcludedFromBOM() ? "now out of the BOM" : "back in the BOM" );

        if( o->IsLocked() != n->IsLocked() )
            kind( "attributes", n->IsLocked() ? "locked" : "unlocked" );

        // pads changing net
        std::map<std::string, std::string> oldPadNets;

        for( const PAD* p : o->Pads() )
            oldPadNets[str( p->GetNumber() )] = str( p->GetNetname() );

        int padNets = 0;
        std::string padText;

        for( const PAD* p : n->Pads() )
        {
            auto it = oldPadNets.find( str( p->GetNumber() ) );

            if( it == oldPadNets.end() || it->second == str( p->GetNetname() ) )
                continue;

            if( padNets++ < 4 )
                padText += std::string( padText.empty() ? "" : ", " ) + "pad " + str( p->GetNumber() ) + " "
                           + ( it->second.empty() ? "no net" : it->second ) + " -> "
                           + ( p->GetNetname().IsEmpty() ? "no net" : str( p->GetNetname() ) );
        }

        if( padNets )
            kind( "pad nets", padText + ( padNets > 4 ? ", ... (" + std::to_string( padNets ) + " pads)" : "" ) );

        if( what.empty() )
            continue;

        changed++;
        std::string text = str( n->GetReference() ) + ": ";

        for( size_t i = 0; i < what.size(); i++ )
            text += ( i ? "; " : "" ) + what[i];

        sec.line( text );
    }

    std::string summary = std::to_string( aOld.Footprints().size() ) + " -> " + std::to_string( aNow.Footprints().size() )
                          + "; " + std::to_string( added.size() ) + " added, " + std::to_string( removed.size() )
                          + " removed, " + std::to_string( changed ) + " changed";

    if( !kinds.empty() )
    {
        summary += " (";
        bool first = true;

        for( const auto& [k, n] : kinds )
        {
            summary += ( first ? "" : ", " ) + std::to_string( n ) + " " + k;
            first = false;
        }

        summary += ")";
    }

    sec.s->set_summary( summary );
    aChanges += (int) added.size() + (int) removed.size() + changed;
}

// ---------------------------------------------------------------------------- nets and tracks

struct NET_COPPER
{
    int    segments = 0;
    double length = 0;   // mm
    int    vias = 0;
};

std::map<std::string, NET_COPPER> copperByNet( const BOARD& aBoard )
{
    std::map<std::string, NET_COPPER> out;

    for( const PCB_TRACK* t : aBoard.Tracks() )
    {
        NET_COPPER& c = out[str( t->GetNetname() )];

        if( t->Type() == PCB_VIA_T )
        {
            c.vias++;
        }
        else
        {
            c.segments++;
            c.length += t->GetLength() / 1e6;
        }
    }

    return out;
}

std::set<std::string> netNames( const BOARD& aBoard )
{
    std::set<std::string> out;

    for( const NETINFO_ITEM* net : aBoard.GetNetInfo() )
    {
        if( net->GetNetCode() > 0 )
            out.insert( str( net->GetNetname() ) );
    }

    return out;
}

void diffNets( const BOARD& aOld, const BOARD& aNow, int aLimit, BoardDiffResponse& aOut, int& aChanges )
{
    SECTION                     sec{ aOut.add_sections(), aLimit };
    const std::set<std::string> o = netNames( aOld ), n = netNames( aNow );
    std::vector<std::string>    added, removed;

    std::set_difference( n.begin(), n.end(), o.begin(), o.end(), std::back_inserter( added ) );
    std::set_difference( o.begin(), o.end(), n.begin(), n.end(), std::back_inserter( removed ) );
    sec.s->set_name( "nets" );
    sec.s->set_summary( std::to_string( o.size() ) + " -> " + std::to_string( n.size() ) + "; " + std::to_string( added.size() )
                        + " added, " + std::to_string( removed.size() ) + " removed" );

    for( const std::string& name : added )
        sec.line( "added " + name );

    for( const std::string& name : removed )
        sec.line( "removed " + name );

    aChanges += (int) ( added.size() + removed.size() );
}

void diffTracks( const BOARD& aOld, const BOARD& aNow, int aLimit, BoardDiffResponse& aOut, int& aChanges )
{
    SECTION sec{ aOut.add_sections(), aLimit };
    sec.s->set_name( "tracks" );

    std::map<std::string, NET_COPPER> o = copperByNet( aOld ), n = copperByNet( aNow );
    NET_COPPER                        to, tn;
    std::set<std::string>             names;

    for( const auto& [k, c] : o )
    {
        names.insert( k );
        to.segments += c.segments;
        to.length += c.length;
        to.vias += c.vias;
    }

    for( const auto& [k, c] : n )
    {
        names.insert( k );
        tn.segments += c.segments;
        tn.length += c.length;
        tn.vias += c.vias;
    }

    // tracks paired by UUID: how many are new, gone, or changed in place
    std::map<KIID, const PCB_TRACK*> oldById;

    for( const PCB_TRACK* t : aOld.Tracks() )
        oldById[t->m_Uuid] = t;

    // without a partner by UUID (another revision of the file): the same segment / via in place
    auto sig = []( const PCB_TRACK* t )
    {
        VECTOR2I a = t->GetStart(), b = t->GetEnd();

        if( b.x < a.x || ( b.x == a.x && b.y < a.y ) )
            std::swap( a, b );

        return std::make_tuple( (int) t->Type(), a.x, a.y, b.x, b.y, t->GetWidth(), (int) t->GetLayer(),
                                str( t->GetNetname() ) );
    };

    std::set<KIID> nowIds;

    for( const PCB_TRACK* t : aNow.Tracks() )
        nowIds.insert( t->m_Uuid );

    std::multiset<decltype( sig( nullptr ) )> unpaired;

    for( const PCB_TRACK* t : aOld.Tracks() )
    {
        if( !nowIds.count( t->m_Uuid ) )
            unpaired.insert( sig( t ) );
    }

    int added = 0, edited = 0, same = 0;
    std::set<KIID> seen;

    for( const PCB_TRACK* t : aNow.Tracks() )
    {
        auto it = oldById.find( t->m_Uuid );

        if( it == oldById.end() )
        {
            auto u = unpaired.find( sig( t ) );

            if( u != unpaired.end() )
            {
                unpaired.erase( u );
                same++;
            }
            else
            {
                added++;
            }

            continue;
        }

        seen.insert( t->m_Uuid );
        const PCB_TRACK* p = it->second;

        if( p->GetStart() != t->GetStart() || p->GetEnd() != t->GetEnd() || p->GetWidth() != t->GetWidth()
            || p->GetLayer() != t->GetLayer() || p->GetNetname() != t->GetNetname() )
        {
            edited++;
        }
    }

    const int gone = (int) aOld.Tracks().size() - (int) seen.size() - same;

    sec.s->set_summary( std::to_string( to.segments ) + " -> " + std::to_string( tn.segments ) + " segments, "
                        + mm( to.length, 1 ) + " -> " + mm( tn.length, 1 ) + " mm, " + std::to_string( to.vias ) + " -> "
                        + std::to_string( tn.vias ) + " vias; " + std::to_string( added ) + " new, " + std::to_string( gone )
                        + " removed, " + std::to_string( edited ) + " changed in place (segments and vias)" );

    // the nets whose copper changed, the largest change in length first
    std::vector<std::pair<double, std::string>> lines;

    for( const std::string& k : names )
    {
        const NET_COPPER a = o.count( k ) ? o[k] : NET_COPPER(), b = n.count( k ) ? n[k] : NET_COPPER();

        if( a.segments == b.segments && a.vias == b.vias && std::fabs( a.length - b.length ) < 0.005 )
            continue;

        std::string text = ( k.empty() ? std::string( "(no net)" ) : k ) + ": " + std::to_string( a.segments ) + " -> "
                           + std::to_string( b.segments ) + " segments, " + mm( a.length ) + " -> " + mm( b.length ) + " mm";

        if( a.vias != b.vias )
            text += ", " + std::to_string( a.vias ) + " -> " + std::to_string( b.vias ) + " vias";

        if( a.segments == 0 && b.segments > 0 )
            text += " (newly routed)";
        else if( b.segments == 0 && a.segments > 0 )
            text += " (all tracks removed)";

        lines.push_back( { std::fabs( b.length - a.length ) + std::abs( b.vias - a.vias ), text } );
    }

    std::sort( lines.begin(), lines.end(), []( const auto& x, const auto& y ) { return x.first > y.first; } );

    for( const auto& [w, text] : lines )
        sec.line( text );

    aChanges += added + gone + edited;
}

// ---------------------------------------------------------------------------- zones, outline, the rest

std::string zoneName( const ZONE* aZone )
{
    std::string layers;

    for( PCB_LAYER_ID l : aZone->GetLayerSet().Seq() )
        layers += ( layers.empty() ? "" : "+" ) + str( LSET::Name( l ) );

    std::string name = aZone->GetIsRuleArea() ? "rule area" : "zone " + str( aZone->GetNetname() );

    if( !aZone->GetZoneName().IsEmpty() )
        name += " '" + str( aZone->GetZoneName() ) + "'";

    return name + " on " + layers;
}

void diffZones( const BOARD& aOld, const BOARD& aNow, int aLimit, BoardDiffResponse& aOut, int& aChanges )
{
    SECTION sec{ aOut.add_sections(), aLimit };
    sec.s->set_name( "zones" );

    std::map<KIID, const ZONE*> oldById;

    for( const ZONE* z : aOld.Zones() )
        oldById[z->m_Uuid] = z;

    int added = 0, removed = 0, changed = 0;
    std::set<KIID> seen;

    // without a partner by UUID: the zone of the same net on the same layers (the closest outline)
    std::set<KIID> nowIds;

    for( const ZONE* z : aNow.Zones() )
        nowIds.insert( z->m_Uuid );

    for( const ZONE* z : aNow.Zones() )
    {
        if( oldById.count( z->m_Uuid ) )
            continue;

        const ZONE* best = nullptr;
        double      bestD = 0;

        for( const ZONE* o : aOld.Zones() )
        {
            if( nowIds.count( o->m_Uuid ) || seen.count( o->m_Uuid ) || o->GetNetname() != z->GetNetname()
                || o->GetLayerSet() != z->GetLayerSet() || o->GetIsRuleArea() != z->GetIsRuleArea() )
                continue;

            const double d = ( o->Outline()->BBox().Centre() - z->Outline()->BBox().Centre() ).EuclideanNorm();

            if( !best || d < bestD )
            {
                best = o;
                bestD = d;
            }
        }

        if( best )
        {
            oldById.erase( best->m_Uuid );
            oldById[z->m_Uuid] = best;
            seen.insert( best->m_Uuid );   // taken
        }
    }

    seen.clear();

    for( const ZONE* z : aNow.Zones() )
    {
        auto it = oldById.find( z->m_Uuid );

        if( it == oldById.end() )
        {
            added++;
            sec.line( "added " + zoneName( z ) );
            continue;
        }

        seen.insert( it->second->m_Uuid );
        const ZONE*              o = it->second;
        std::vector<std::string> what;

        if( o->GetNetname() != z->GetNetname() )
            what.push_back( "net " + str( o->GetNetname() ) + " -> " + str( z->GetNetname() ) );

        if( o->GetLayerSet() != z->GetLayerSet() )
            what.push_back( "layers changed" );

        SHAPE_POLY_SET a = *o->Outline(), b = *z->Outline();

        if( a.BBox() != b.BBox() || a.FullPointCount() != b.FullPointCount() )
        {
            BOX2I ob = a.BBox(), nb = b.BBox();
            what.push_back( "outline " + mm( ob.GetWidth() / 1e6 ) + " x " + mm( ob.GetHeight() / 1e6 ) + " -> "
                            + mm( nb.GetWidth() / 1e6 ) + " x " + mm( nb.GetHeight() / 1e6 ) + " mm" );
        }

        if( o->GetAssignedPriority() != z->GetAssignedPriority() )
            what.push_back( "priority " + std::to_string( o->GetAssignedPriority() ) + " -> "
                            + std::to_string( z->GetAssignedPriority() ) );

        if( o->GetLocalClearance().value_or( 0 ) != z->GetLocalClearance().value_or( 0 ) )
            what.push_back( "clearance " + mm( o->GetLocalClearance().value_or( 0 ) / 1e6 ) + " -> "
                            + mm( z->GetLocalClearance().value_or( 0 ) / 1e6 ) + " mm" );

        if( o->GetMinThickness() != z->GetMinThickness() )
            what.push_back( "min width " + mm( o->GetMinThickness() / 1e6 ) + " -> " + mm( z->GetMinThickness() / 1e6 ) + " mm" );

        if( o->GetPadConnection() != z->GetPadConnection() )
            what.push_back( "pad connection changed" );

        if( what.empty() )
            continue;

        changed++;
        std::string text = zoneName( z ) + ": ";

        for( size_t i = 0; i < what.size(); i++ )
            text += ( i ? "; " : "" ) + what[i];

        sec.line( text );
    }

    for( const ZONE* z : aOld.Zones() )
    {
        if( !seen.count( z->m_Uuid ) )
        {
            removed++;
            sec.line( "removed " + zoneName( z ) );
        }
    }

    sec.s->set_summary( std::to_string( aOld.Zones().size() ) + " -> " + std::to_string( aNow.Zones().size() ) + "; "
                        + std::to_string( added ) + " added, " + std::to_string( removed ) + " removed, "
                        + std::to_string( changed ) + " changed (fills not compared)" );
    aChanges += added + removed + changed;
}

void diffBoard( const BOARD& aOld, const BOARD& aNow, int aLimit, BoardDiffResponse& aOut, int& aChanges )
{
    SECTION sec{ aOut.add_sections(), aLimit };
    sec.s->set_name( "board" );
    int n = 0;

    const BOX2I ob = aOld.GetBoardEdgesBoundingBox(), nb = aNow.GetBoardEdgesBoundingBox();

    if( ob != nb )
    {
        n++;
        sec.line( "outline " + mm( ob.GetWidth() / 1e6 ) + " x " + mm( ob.GetHeight() / 1e6 ) + " mm at " + at( ob.GetOrigin() )
                  + " -> " + mm( nb.GetWidth() / 1e6 ) + " x " + mm( nb.GetHeight() / 1e6 ) + " mm at " + at( nb.GetOrigin() ) );
    }

    if( aOld.GetCopperLayerCount() != aNow.GetCopperLayerCount() )
    {
        n++;
        sec.line( "copper layers " + std::to_string( aOld.GetCopperLayerCount() ) + " -> "
                  + std::to_string( aNow.GetCopperLayerCount() ) );
    }

    const int ot = aOld.GetDesignSettings().GetBoardThickness(), nt = aNow.GetDesignSettings().GetBoardThickness();

    if( ot != nt )
    {
        n++;
        sec.line( "thickness " + mm( ot / 1e6, 3 ) + " -> " + mm( nt / 1e6, 3 ) + " mm" );
    }

    // drawings (outline, silkscreen, text) by UUID
    std::map<KIID, const BOARD_ITEM*> oldById;

    for( const BOARD_ITEM* d : aOld.Drawings() )
        oldById[d->m_Uuid] = d;

    std::map<std::string, int> addedOn, removedOn, changedOn;
    std::set<KIID>             seen;

    auto layerOf = []( const BOARD_ITEM* aItem ) { return str( LSET::Name( aItem->GetLayer() ) ); };

    // without a partner by UUID: the same kind of item with the same box on the same layer
    auto sig = []( const BOARD_ITEM* d )
    {
        const BOX2I b = d->GetBoundingBox();
        return std::make_tuple( (int) d->Type(), (int) d->GetLayer(), b.GetX(), b.GetY(), b.GetWidth(), b.GetHeight() );
    };

    std::set<KIID> nowIds;

    for( const BOARD_ITEM* d : aNow.Drawings() )
        nowIds.insert( d->m_Uuid );

    std::multimap<decltype( sig( nullptr ) ), KIID> unpaired;

    for( const BOARD_ITEM* d : aOld.Drawings() )
    {
        if( !nowIds.count( d->m_Uuid ) )
            unpaired.insert( { sig( d ), d->m_Uuid } );
    }

    for( const BOARD_ITEM* d : aNow.Drawings() )
    {
        auto it = oldById.find( d->m_Uuid );

        if( it == oldById.end() )
        {
            auto u = unpaired.find( sig( d ) );

            if( u != unpaired.end() )
            {
                seen.insert( u->second );
                unpaired.erase( u );
            }
            else
            {
                addedOn[layerOf( d )]++;
            }

            continue;
        }

        seen.insert( d->m_Uuid );

        if( it->second->GetBoundingBox() != d->GetBoundingBox() || it->second->GetLayer() != d->GetLayer() )
            changedOn[layerOf( d )]++;
    }

    for( const BOARD_ITEM* d : aOld.Drawings() )
    {
        if( !seen.count( d->m_Uuid ) )
            removedOn[layerOf( d )]++;
    }

    for( auto [map, verb] : { std::pair{ &addedOn, "added" }, std::pair{ &removedOn, "removed" },
                              std::pair{ &changedOn, "changed" } } )
    {
        for( const auto& [layer, count] : *map )
        {
            n += count;
            sec.line( "graphics on " + layer + ": " + std::to_string( count ) + " " + verb );
        }
    }

    sec.s->set_summary( n ? std::to_string( n ) + " change(s) to the outline, layers and graphics"
                          : "outline, layers and graphics unchanged" );
    aChanges += n;
}

} // namespace


bool DiffBoard( const BOARD& aNow, const std::string& aFile, int aLimit, BoardDiffResponse& aOut, std::string& aError )
{
    wxString file = aFile.empty() ? aNow.GetFileName() : wxString::FromUTF8( aFile );

    if( !aFile.empty() && !wxFileName( file ).IsAbsolute() )
    {
        wxFileName f( file );
        f.MakeAbsolute( wxFileName( aNow.GetFileName() ).GetPath() );
        file = f.GetFullPath();
    }

    if( file.IsEmpty() || !wxFileName::FileExists( file ) )
    {
        aError = aFile.empty() ? "the board has not been saved yet: nothing to compare with (give file)"
                               : "no file " + str( file );
        return false;
    }

    std::unique_ptr<BOARD> old;

    try
    {
        IO_RELEASER<PCB_IO> pi( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::KICAD_SEXP ) );
        old = pi->LoadBoard( file, nullptr, nullptr );
    }
    catch( const IO_ERROR& e )
    {
        aError = "could not read " + str( file ) + ": " + str( e.What() );
        return false;
    }

    if( !old )
    {
        aError = "could not read " + str( file );
        return false;
    }

    aOut.set_file( str( file ) );

    if( wxFileName( file ).GetModificationTime().IsValid() )
        aOut.set_file_time( str( wxFileName( file ).GetModificationTime().FormatISOCombined( ' ' ) ) );

    const int limit = aLimit > 0 ? aLimit : 20;
    int       changes = 0;

    diffFootprints( *old, aNow, limit, aOut, changes );
    diffNets( *old, aNow, limit, aOut, changes );
    diffTracks( *old, aNow, limit, aOut, changes );
    diffZones( *old, aNow, limit, aOut, changes );
    diffBoard( *old, aNow, limit, aOut, changes );

    aOut.set_changes( changes );
    return true;
}

} // namespace kicadd
