/*
 * KiCadD: silkscreen tidying — references (and values) moved off pads, off each other and onto
 * the board, next to their part.
 *
 * A text has a problem when its box is over an exposed pad (in the mask opening, with a margin:
 * the fab clips silk there), over another silkscreen text, or not inside the board outline. For
 * each text to place (those with a problem, or all asked for), candidate spots around the part's
 * courtyard — above, below, left, right, the corners, then further out ring by ring — horizontal
 * or vertical (keeping the part's reading direction first), and inside the courtyard as a last
 * resort (common for large parts). The nearest free spot wins; texts placed earlier count as
 * obstacles for the later ones. Spots over another part's courtyard (the text would be under
 * that part once assembled) or its silkscreen lines cost extra.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_silk.h"

#include <board.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <geometry/shape_poly_set.h>
#include <geometry/shape_rect.h>
#include <string_utils.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <limits>
#include <optional>
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

std::string mm( double aMm )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.2f", std::fabs( aMm ) < 0.005 ? 0.0 : aMm );
    std::string s( buf );

    while( s.back() == '0' )
        s.pop_back();

    if( s.back() == '.' )
        s.pop_back();

    return s;
}

std::string at( const VECTOR2I& p )
{
    return "(" + mm( p.x / 1e6 ) + ", " + mm( p.y / 1e6 ) + ")";
}

struct PAD_OBS
{
    std::shared_ptr<SHAPE> shape;
    BOX2I                  box;
};

struct TEXT
{
    FOOTPRINT*   fp = nullptr;      // owner, or null for a board text
    PCB_FIELD*   field = nullptr;   // a footprint field we may move
    BOARD_ITEM*  item = nullptr;
    PCB_LAYER_ID layer = F_SilkS;
    BOX2I        box;
    std::string  what;
    bool         movable = false;
    std::shared_ptr<SHAPE> strokes;   // the drawn glyphs (problems are judged on these)
};

struct WORLD
{
    std::map<PCB_LAYER_ID, std::vector<PAD_OBS>> pads;         // by silk layer
    std::map<PCB_LAYER_ID, std::vector<std::pair<FOOTPRINT*, BOX2I>>> courtyards;
    std::map<PCB_LAYER_ID, std::vector<std::pair<FOOTPRINT*, BOX2I>>> lines;   // silk shapes
    SHAPE_POLY_SET outline;
    bool           haveOutline = false;
    int            padGap = 150000;   // nm, silk to an exposed pad
    int            gap = 100000;      // nm, between texts and around the part
};

/// Problems of a text box at that spot: over a pad, over another text, off the board
struct PROBLEMS
{
    bool pad = false, text = false, off = false;

    int count() const { return pad + text + off; }
};

PROBLEMS problemsAt( const WORLD& aW, const std::vector<TEXT>& aTexts, size_t aSelf, const BOX2I& aBox,
                     PCB_LAYER_ID aLayer, int aPadGap, int aTextGap )
{
    PROBLEMS p;

    auto padsIt = aW.pads.find( aLayer );

    if( padsIt != aW.pads.end() )
    {
        SHAPE_RECT rect( aBox );
        BOX2I      grown = aBox;
        grown.Inflate( aPadGap );

        for( const PAD_OBS& pad : padsIt->second )
        {
            if( grown.Intersects( pad.box ) && pad.shape->Collide( &rect, aPadGap ) )
            {
                p.pad = true;
                break;
            }
        }
    }

    BOX2I grown = aBox;
    grown.Inflate( aTextGap );

    for( size_t i = 0; i < aTexts.size(); i++ )
    {
        if( i != aSelf && aTexts[i].layer == aLayer && grown.Intersects( aTexts[i].box ) )
        {
            p.text = true;
            break;
        }
    }

    if( aW.haveOutline )
    {
        for( const VECTOR2I& c : { aBox.GetOrigin(), VECTOR2I( aBox.GetRight(), aBox.GetTop() ), aBox.GetEnd(),
                                   VECTOR2I( aBox.GetLeft(), aBox.GetBottom() ) } )
        {
            if( !aW.outline.Contains( c ) )
            {
                p.off = true;
                break;
            }
        }
    }

    return p;
}

/// Problems of a text where it is, judged on its strokes (as audit does): a stroke over an
/// exposed pad or another text's stroke, or its box off the board. Boxes are bigger than the
/// glyphs (line spacing): a text set close to a pad or between rows of others is fine.
PROBLEMS problemsNow( const WORLD& aW, const std::vector<TEXT>& aTexts, size_t aSelf )
{
    const TEXT& t = aTexts[aSelf];
    PROBLEMS    p;

    if( !t.strokes )
        return problemsAt( aW, aTexts, aSelf, t.box, t.layer, 0, 0 );

    auto padsIt = aW.pads.find( t.layer );

    if( padsIt != aW.pads.end() )
    {
        for( const PAD_OBS& pad : padsIt->second )
        {
            if( t.box.Intersects( pad.box ) && pad.shape->Collide( t.strokes.get(), 0 ) )
            {
                p.pad = true;
                break;
            }
        }
    }

    for( size_t i = 0; i < aTexts.size(); i++ )
    {
        if( i == aSelf || aTexts[i].layer != t.layer || !t.box.Intersects( aTexts[i].box ) )
            continue;

        if( !aTexts[i].strokes || aTexts[i].strokes->Collide( t.strokes.get(), 0 ) )
        {
            p.text = true;
            break;
        }
    }

    if( aW.haveOutline )
    {
        const BOX2I g = t.strokes->BBox();   // the glyphs (as audit judges it)

        for( const VECTOR2I& c : { g.GetOrigin(), VECTOR2I( g.GetRight(), g.GetTop() ), g.GetEnd(),
                                   VECTOR2I( g.GetLeft(), g.GetBottom() ) } )
        {
            if( !aW.outline.Contains( c ) )
            {
                p.off = true;
                break;
            }
        }
    }

    return p;
}

std::string describeProblems( const PROBLEMS& p )
{
    std::string s;

    if( p.pad )
        s += "over a pad";

    if( p.text )
        s += std::string( s.empty() ? "" : ", " ) + "over another text";

    if( p.off )
        s += std::string( s.empty() ? "" : ", " ) + "off the board";

    return s;
}

} // namespace


SILK_PLAN PlanSilk( BOARD& aBoard, const SilkTidy& aReq )
{
    SILK_PLAN plan;
    WORLD     w;

    if( aReq.gap() > 0 )
        w.gap = KiROUND( aReq.gap() * 1e6 );

    w.haveOutline = aBoard.GetBoardPolygonOutlines( w.outline, false ) && w.outline.OutlineCount() > 0;

    // the obstacles: exposed pads, courtyards, silkscreen lines
    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        fp->BuildCourtyardCaches();

        for( PCB_LAYER_ID silk : { F_SilkS, B_SilkS } )
        {
            const PCB_LAYER_ID crt = silk == F_SilkS ? F_CrtYd : B_CrtYd;
            const SHAPE_POLY_SET& c = fp->GetCourtyard( crt );

            if( c.OutlineCount() > 0 )
                w.courtyards[silk].push_back( { fp, c.BBox() } );
            else if( ( silk == B_SilkS ) == fp->IsFlipped() )
                w.courtyards[silk].push_back( { fp, fp->GetBoundingBox( false ) } );
        }

        for( PAD* pad : fp->Pads() )
        {
            for( auto [mask, cu, silk] : { std::tuple{ F_Mask, F_Cu, F_SilkS }, std::tuple{ B_Mask, B_Cu, B_SilkS } } )
            {
                if( !pad->IsOnLayer( mask ) )
                    continue;

                std::shared_ptr<SHAPE> s = pad->GetEffectiveShape( pad->IsOnLayer( cu ) ? cu : mask );

                if( s && !s->IsNull() )
                    w.pads[silk].push_back( { s, s->BBox() } );
            }
        }

        for( BOARD_ITEM* item : fp->GraphicalItems() )
        {
            if( item->Type() == PCB_SHAPE_T && ( item->GetLayer() == F_SilkS || item->GetLayer() == B_SilkS ) )
                w.lines[item->GetLayer()].push_back( { fp, item->GetBoundingBox() } );
        }
    }

    // the texts
    std::set<std::string> asked( aReq.refs().begin(), aReq.refs().end() );
    std::vector<TEXT>     texts;

    for( FOOTPRINT* fp : aBoard.Footprints() )
    {
        const std::string ref = str( fp->GetReference() );
        const bool        mine = asked.empty() || asked.count( ref );

        for( PCB_FIELD* f : fp->GetFields() )
        {
            if( !f || !f->IsVisible() || f->GetText().IsEmpty() )
                continue;

            if( f->GetLayer() != F_SilkS && f->GetLayer() != B_SilkS )
                continue;

            const bool movable = mine && ( f->IsReference() || ( aReq.values() && f->IsValue() ) );
            texts.push_back( { fp, f, f, f->GetLayer(), f->GetBoundingBox(),
                               ref + ( f->IsReference() ? " reference" : f->IsValue() ? " value" : " " + str( f->GetName() ) ),
                               movable, f->GetEffectiveShape( f->GetLayer() ) } );
        }

        for( BOARD_ITEM* item : fp->GraphicalItems() )
        {
            if( item->Type() == PCB_TEXT_T && static_cast<PCB_TEXT*>( item )->IsVisible()
                && ( item->GetLayer() == F_SilkS || item->GetLayer() == B_SilkS ) )
            {
                texts.push_back( { fp, nullptr, item, item->GetLayer(), item->GetBoundingBox(), ref + " text", false,
                                   item->GetEffectiveShape( item->GetLayer() ) } );
            }
        }
    }

    for( BOARD_ITEM* item : aBoard.Drawings() )
    {
        if( ( item->Type() == PCB_TEXT_T || item->Type() == PCB_TEXTBOX_T )
            && ( item->GetLayer() == F_SilkS || item->GetLayer() == B_SilkS ) )
        {
            texts.push_back( { nullptr, nullptr, item, item->GetLayer(), item->GetBoundingBox(), "board text", false,
                               item->GetEffectiveShape( item->GetLayer() ) } );
        }
    }

    for( const std::string& r : asked )
    {
        bool found = false;

        for( const TEXT& t : texts )
            found = found || ( t.fp && str( t.fp->GetReference() ) == r );

        if( !found )
            plan.stuck.push_back( r + ": no part, or no visible silkscreen reference" );
    }

    auto countProblems = [&]()
    {
        int n = 0;

        for( size_t i = 0; i < texts.size(); i++ )
        {
            if( texts[i].movable )
                n += problemsNow( w, texts, i ).count() > 0;
        }

        return n;
    };

    plan.problemsBefore = countProblems();

    // what to place, in reference order
    std::vector<size_t> todo;

    for( size_t i = 0; i < texts.size(); i++ )
    {
        if( !texts[i].movable )
            continue;

        plan.checked++;

        if( aReq.all() || problemsNow( w, texts, i ).count() > 0 )
            todo.push_back( i );
    }

    std::sort( todo.begin(), todo.end(),
               [&]( size_t a, size_t b )
               {
                   int c = StrNumCmp( texts[a].fp->GetReference(), texts[b].fp->GetReference(), CMP_T::IGNORE_CASE );
                   return c != 0 ? c < 0 : texts[a].field->IsReference() > texts[b].field->IsReference();
               } );

    // in turn; the ones waiting stay obstacles where they are, and one whose problem went away
    // (the text it was on has moved) stays
    for( size_t i : todo )
    {
        TEXT&      t = texts[i];
        FOOTPRINT* fp = t.fp;
        const bool back = t.layer == B_SilkS;

        const PROBLEMS was = problemsNow( w, texts, i );

        if( !aReq.all() && was.count() == 0 )
            continue;

        BOX2I body;
        bool  haveBody = false;

        for( const auto& [owner, box] : w.courtyards[t.layer] )
        {
            if( owner == fp )
            {
                body = box;
                haveBody = true;
            }
        }

        if( !haveBody )
            body = fp->GetBoundingBox( false );

        // the reading direction of the part first: vertical text for a part turned 90 / 270
        const double     rot = fp->GetOrientation().Normalized().AsDegrees();
        const bool       upright = std::fabs( std::fmod( rot, 180.0 ) - 90.0 ) < 1.0;
        const EDA_ANGLE  first = upright ? ANGLE_90 : ANGLE_0;
        const EDA_ANGLE  second = upright ? ANGLE_0 : ANGLE_90;

        struct SPOT
        {
            double      score;
            VECTOR2I    pos;
            EDA_ANGLE   angle;
            BOX2I       box;
            std::string where;
        };

        std::optional<SPOT> best;

        for( const EDA_ANGLE& angle : { first, second } )
        {
            // the text's box at that angle, and where its anchor sits in it
            std::unique_ptr<PCB_FIELD> probe( static_cast<PCB_FIELD*>( t.field->Clone() ) );
            probe->SetTextAngle( angle );
            const BOX2I    tb = probe->GetBoundingBox();
            const VECTOR2I anchorOff = probe->GetPosition() - tb.Centre();
            const int      tw = tb.GetWidth(), th = tb.GetHeight();

            auto consider = [&]( const VECTOR2I& aCentre, double aScore, const std::string& aWhere )
            {
                BOX2I box( aCentre - VECTOR2I( tw / 2, th / 2 ), VECTOR2I( tw, th ) );

                if( problemsAt( w, texts, i, box, t.layer, w.padGap, w.gap ).count() > 0 )
                    return;

                double score = aScore + ( aCentre - body.Centre() ).EuclideanNorm() / 1e6 + ( angle == first ? 0 : 0.6 );

                for( const auto& [owner, cbox] : w.courtyards[t.layer] )
                {
                    if( owner != fp && cbox.Intersects( box ) )
                    {
                        score += 6;
                        break;
                    }
                }

                for( const auto& [owner, lbox] : w.lines[t.layer] )
                {
                    if( owner != fp && lbox.Intersects( box ) )
                    {
                        score += 1.5;
                        break;
                    }
                }

                if( !best || score < best->score )
                    best = SPOT{ score, aCentre + anchorOff, angle, box, aWhere };
            };

            for( int ring = 0; ring < 4; ring++ )
            {
                BOX2I r = body;
                r.Inflate( w.gap + ring * ( std::min( tw, th ) + w.gap ) );
                const double cost = ring * 2.0;
                const int    cx = r.Centre().x, cy = r.Centre().y;
                const int    top = r.GetTop() - th / 2, bottom = r.GetBottom() + th / 2;
                const int    left = r.GetLeft() - tw / 2, right = r.GetRight() + tw / 2;
                const char*  above = back ? "below (seen from the front)" : "above";
                const char*  below = back ? "above (seen from the front)" : "below";

                consider( { cx, top }, cost, above );
                consider( { cx, bottom }, cost, below );
                consider( { left, cy }, cost, "left" );
                consider( { right, cy }, cost, "right" );
                consider( { r.GetLeft() + tw / 2, top }, cost + 0.3, above );
                consider( { r.GetRight() - tw / 2, top }, cost + 0.3, above );
                consider( { r.GetLeft() + tw / 2, bottom }, cost + 0.3, below );
                consider( { r.GetRight() - tw / 2, bottom }, cost + 0.3, below );
                consider( { left, top }, cost + 0.8, "corner" );
                consider( { right, top }, cost + 0.8, "corner" );
                consider( { left, bottom }, cost + 0.8, "corner" );
                consider( { right, bottom }, cost + 0.8, "corner" );
            }

            // inside the part (big parts: the text fits in the courtyard, under the part)
            if( tw < body.GetWidth() && th < body.GetHeight() )
                consider( body.Centre(), 4.0, "inside the part" );
        }

        if( !best )
        {
            if( problemsNow( w, texts, i ).count() > 0 )
                plan.stuck.push_back( t.what + " at " + at( t.field->GetPosition() ) + ": no free spot near the part ("
                                      + describeProblems( problemsNow( w, texts, i ) ) + ")" );

            continue;
        }

        {
            std::unique_ptr<PCB_FIELD> placed( static_cast<PCB_FIELD*>( t.field->Clone() ) );
            placed->SetTextAngle( best->angle );
            placed->SetPosition( best->pos );
            t.box = placed->GetBoundingBox();
            t.strokes = placed->GetEffectiveShape( t.layer );
        }

        if( best->pos == t.field->GetPosition() && best->angle == t.field->GetTextAngle() )
            continue;

        plan.moves.push_back( { fp, t.field, best->pos, best->angle,
                                t.what + " " + at( t.field->GetPosition() ) + " -> " + at( best->pos ) + ", " + best->where
                                        + ( best->angle != t.field->GetTextAngle().Normalized()
                                                    ? std::string( best->angle == ANGLE_90 ? ", vertical" : ", horizontal" )
                                                    : "" )
                                        + ( was.count() ? " (was " + describeProblems( was ) + ")" : "" ) } );
    }

    plan.problemsAfter = countProblems();
    return plan;
}

} // namespace kicadd
