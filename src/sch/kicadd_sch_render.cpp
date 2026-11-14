/*
 * KiCadD: a picture (PNG) of one schematic sheet in memory, for the agent to look at.
 *
 * KiCad's plotter draws the sheet (PNG_PLOTTER, cairo; SCH_SCREEN::Plot as File > Plot does).
 * On top of it: the parts asked about framed and labelled, nets drawn bright, a millimetre
 * ruler on the top and left in the coordinates the other schematic tools use.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_handler_sch.h"

#include <api/sch_context.h>
#include <base_units.h>
#include <drawing_sheet/ds_draw_item.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <font/font.h>
#include <font/text_attributes.h>
#include <pgm_base.h>
#include <plotters/plotter_png.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_painter.h>
#include <sch_pin.h>
#include <sch_plotter.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_sheet_pin.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <connectivity/conn_facade.h>

#include <wx/ffile.h>
#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <set>

using namespace kicadd::schematic;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;

namespace
{

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

ApiResponseStatus makeError( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}

COLOR4D rgba( int r, int g, int b, double a = 1.0 )
{
    return COLOR4D( r / 255.0, g / 255.0, b / 255.0, a );
}

/// Back to the sheet the editor was on (references of shared screens follow the current sheet)
struct SHEET_GUARD
{
    SCHEMATIC*     sch;
    SCH_SHEET_PATH old;

    ~SHEET_GUARD()
    {
        sch->SetCurrentSheet( old );
        sch->CurrentSheet().UpdateAllScreenReferences();
        sch->SetSheetNumberAndCount();
    }
};

} // namespace


HANDLER_RESULT<SchRenderResponse> KICADD_HANDLER_SCH::handleRender( const HANDLER_CONTEXT<SchRender>& aCtx )
{
    SCHEMATIC* sch = schematic();

    if( !sch )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    const SchRender&  req = aCtx.Request;
    SchRenderResponse out;

    rebuildConnectivity();

    // --- the sheet: asked for, else the one with the first ref, else the root
    std::optional<SCH_SHEET_PATH> sheet;
    std::string                   err;

    if( !req.sheet().empty() )
    {
        sheet = findSheet( req.sheet(), err );

        if( !sheet )
            return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );
    }

    // where each ref is
    std::map<std::string, std::vector<SCH_SHEET_PATH>> refSheets;

    for( const SCH_SHEET_PATH& path : sch->Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            std::string ref = str( static_cast<SCH_SYMBOL*>( item )->GetRef( &path ) );

            for( const std::string& r : req.refs() )
            {
                if( r == ref )
                    refSheets[r].push_back( path );
            }
        }
    }

    if( !sheet )
    {
        for( const std::string& r : req.refs() )
        {
            if( refSheets.count( r ) )
            {
                sheet = refSheets[r].front();
                break;
            }
        }
    }

    if( !sheet )
        sheet = findSheet( "/", err );

    if( !sheet )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "the schematic has no sheet" ) );

    out.set_sheet( str( sheet->PathHumanReadable() ) );

    SHEET_GUARD guard{ sch, sch->CurrentSheet() };
    sch->SetCurrentSheet( *sheet );
    sch->CurrentSheet().UpdateAllScreenReferences();
    sch->SetSheetNumberAndCount();

    SCH_SCREEN* screen = sheet->LastScreen();

    // --- the parts and nets to mark, on this sheet
    std::vector<SCH_SYMBOL*> marked;

    for( const std::string& r : req.refs() )
    {
        bool here = false;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );

            if( str( symbol->GetRef( &*sheet ) ) == r )
            {
                marked.push_back( symbol );
                here = true;
            }
        }

        if( here )
            continue;

        if( refSheets.count( r ) )
            out.add_elsewhere( r + " on " + str( refSheets[r].front().PathHumanReadable() ) );
        else
            out.add_missing( r );
    }

    auto netOf = [&]( SCH_ITEM* aItem ) -> std::string
    {
        std::optional<SCH_CONNECTIVITY::ITEM_VIEW> view = sch->Connectivity().Connection( aItem->m_Uuid, sheet->Path() );
        return view && !view->IsUnconnected() ? str( view->Name() ) : std::string();
    };

    std::set<std::string> nets;

    for( const std::string& n : req.nets() )
        nets.insert( n );

    auto wanted = [&]( const std::string& aNet )
    {
        if( aNet.empty() )
            return std::string();

        for( const std::string& n : nets )
        {
            // "/VPP" or a hierarchical "/Power/VOUT" answers to "VPP" / "VOUT" too
            if( aNet == n || aNet == "/" + n || ( aNet.size() > n.size() && aNet.compare( aNet.size() - n.size() - 1, n.size() + 1, "/" + n ) == 0 ) )
                return n;
        }

        return std::string();
    };

    // the items of the nets: wires, labels, pins, sheet pins
    std::vector<SCH_LINE*>        netWires;
    std::vector<SCH_ITEM*>        netLabels;
    std::vector<VECTOR2I>         netPins;
    std::set<std::string>         netsHere;

    if( !nets.empty() )
    {
        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->Type() == SCH_LINE_T && static_cast<SCH_LINE*>( item )->IsWire() )
            {
                std::string n = wanted( netOf( item ) );

                if( !n.empty() )
                {
                    netWires.push_back( static_cast<SCH_LINE*>( item ) );
                    netsHere.insert( n );
                }
            }
            else if( item->IsType( { SCH_LABEL_LOCATE_ANY_T } ) )
            {
                std::string n = wanted( netOf( item ) );

                if( !n.empty() )
                {
                    netLabels.push_back( item );
                    netsHere.insert( n );
                }
            }
            else if( item->Type() == SCH_SYMBOL_T )
            {
                for( SCH_PIN* pin : static_cast<SCH_SYMBOL*>( item )->GetPins( &*sheet ) )
                {
                    std::string n = wanted( netOf( pin ) );

                    if( !n.empty() )
                    {
                        netPins.push_back( pin->GetPosition() );
                        netsHere.insert( n );
                    }
                }
            }
            else if( item->Type() == SCH_SHEET_T )
            {
                for( SCH_SHEET_PIN* pin : static_cast<SCH_SHEET*>( item )->GetPins() )
                {
                    std::string n = wanted( netOf( pin ) );

                    if( !n.empty() )
                    {
                        netLabels.push_back( pin );
                        netsHere.insert( n );
                    }
                }
            }
        }

        for( const std::string& n : nets )
        {
            if( !netsHere.count( n ) )
                out.add_elsewhere( "net " + n + " not on this sheet" );
        }
    }

    // --- the area
    const PAGE_INFO& page = screen->GetPageSettings();
    const BOX2I      pageBox( VECTOR2I( 0, 0 ), VECTOR2I( page.GetWidthIU( schIUScale.IU_PER_MILS ),
                                                          page.GetHeightIU( schIUScale.IU_PER_MILS ) ) );
    BOX2I area;

    auto merge = [&]( BOX2I& aBox, bool& aFirst, const BOX2I& aAdd )
    {
        if( aFirst )
            aBox = aAdd;
        else
            aBox.Merge( aAdd );

        aFirst = false;
    };

    if( req.area_size() == 4 )
    {
        VECTOR2I a( schIUScale.mmToIU( req.area( 0 ) ), schIUScale.mmToIU( req.area( 1 ) ) );
        VECTOR2I b( schIUScale.mmToIU( req.area( 2 ) ), schIUScale.mmToIU( req.area( 3 ) ) );
        area = BOX2I( a, b - a );
        area.Normalize();
    }
    else if( req.area_size() )
    {
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "area is [x0, y0, x1, y1] in mm" ) );
    }
    else if( req.zoom() && ( !marked.empty() || !netWires.empty() || !netLabels.empty() || !netPins.empty() ) )
    {
        bool first = true;

        for( SCH_SYMBOL* s : marked )
            merge( area, first, s->GetBoundingBox() );

        for( SCH_LINE* w : netWires )
            merge( area, first, w->GetBoundingBox() );

        for( SCH_ITEM* l : netLabels )
            merge( area, first, l->GetBoundingBox() );

        for( const VECTOR2I& p : netPins )
            merge( area, first, BOX2I( p, VECTOR2I( 1, 1 ) ) );

        area.Inflate( schIUScale.mmToIU( req.margin() > 0 ? req.margin() : 5.0 ) );
    }
    else if( req.frame() )
    {
        area = pageBox;
    }
    else
    {
        // what is drawn, with a margin (the page's frame left out)
        bool first = true;

        for( SCH_ITEM* item : screen->Items() )
            merge( area, first, item->GetBoundingBox() );

        if( first )
            area = pageBox;
        else
            area.Inflate( schIUScale.mmToIU( 5 ) );
    }

    if( area.GetWidth() <= 0 || area.GetHeight() <= 0 )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "nothing to draw: the area is empty" ) );

    // --- the scale: an integer dpi, the long side about `size` pixels (the rulers included)
    const int    size = req.size() > 0 ? std::clamp( req.size(), 200, 4000 ) : 1600;
    const double wMm = schIUScale.IUTomm( area.GetWidth() ), hMm = schIUScale.IUTomm( area.GetHeight() );
    const int    bandX = 34, bandY = 18;
    const int    fit = size - ( wMm >= hMm ? bandX : bandY );
    const int    dpi = std::max( 1, (int) std::floor( fit / std::max( wMm, hMm ) * 25.4 ) );
    const double pxPerMm = dpi / 25.4;
    const double iuPerPx = schIUScale.mmToIU( 1.0 ) / pxPerMm;
    const int    W = bandX + (int) std::ceil( wMm * pxPerMm );
    const int    H = bandY + (int) std::ceil( hMm * pxPerMm );

    if( W > 8000 || H > 8000 )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "the picture would be too big" ) );

    const int bandXIu = KiROUND( bandX * iuPerPx ), bandYIu = KiROUND( bandY * iuPerPx );
    const int px = std::max( 1, KiROUND( iuPerPx ) );

    out.set_width( W );
    out.set_height( H );
    out.set_mm_per_px( 1.0 / pxPerMm );

    for( int v : { area.GetX(), area.GetY(), area.GetRight(), area.GetBottom() } )
        out.add_area( schIUScale.IUTomm( v ) );

    // --- the plotter, set up as kicad-cli sch export does
    SCH_RENDER_SETTINGS render;
    render.LoadColors( ::GetColorSettings( DEFAULT_THEME ) );
    render.m_ShowHiddenPins = false;
    render.m_ShowHiddenFields = false;
    render.m_ShowPinAltIcons = false;
    render.SetDefaultPenWidth( sch->Settings().m_DefaultLineWidth );
    render.m_LabelSizeRatio = sch->Settings().m_LabelSizeRatio;
    render.m_TextOffsetRatio = sch->Settings().m_TextOffsetRatio;
    render.m_PinSymbolSize = sch->Settings().m_PinSymbolSize;
    render.m_ShowDNPMarkers = sch->Settings().m_ShowDNPMarkers;
    render.SetDashLengthRatio( sch->Settings().m_DashedLineDashRatio );
    render.SetGapLengthRatio( sch->Settings().m_DashedLineGapRatio );

    const COLOR4D background = render.GetLayerColor( LAYER_SCHEMATIC_BACKGROUND );

    PNG_PLOTTER plotter;
    plotter.SetPixelSize( W, H );
    plotter.SetResolution( dpi );
    plotter.SetAntialias( true );
    plotter.SetBackgroundColor( background );
    plotter.SetRenderSettings( &render );
    plotter.SetPageSettings( page );
    plotter.SetColorMode( true );
    plotter.SetViewport( VECTOR2I( area.GetX() - bandXIu, area.GetY() - bandYIu ), schIUScale.IU_PER_MILS / 10, 1.0,
                         false );
    plotter.SetCreator( wxT( "KiCadD" ) );

    wxString file = req.path().empty() ? wxFileName::CreateTempFileName( wxS( "kicadd-sch" ) )
                                       : wxString::FromUTF8( req.path() );

    if( !req.path().empty() && !wxFileName( file ).IsAbsolute() )
    {
        wxFileName f( file );
        f.MakeAbsolute( wxFileName( sch->Project().GetProjectFullName() ).GetPath() );
        file = f.GetFullPath();
    }

    if( !plotter.OpenFile( file ) || !plotter.StartPlot( sheet->GetPageNumber() ) )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "the picture could not be started" ) );

    SCH_PLOT_OPTS opts;
    opts.m_plotDrawingSheet = req.frame();

    if( req.frame() )
    {
        PlotDrawingSheet( &plotter, &sch->Project(), screen->GetTitleBlock(), page, sch->GetProperties(),
                          screen->GetPageNumber(), screen->GetPageCount(), sheet->Last()->GetName(),
                          sheet->PathHumanReadable(), screen->GetFileName(),
                          render.GetLayerColor( LAYER_SCHEMATIC_DRAWINGSHEET ), screen->GetVirtualPageNumber() == 1,
                          sch->GetCurrentVariant(), sch->GetVariantDescription( sch->GetCurrentVariant() ) );
    }

    screen->Plot( &plotter, opts );

    auto text = [&]( const VECTOR2I& aPos, const wxString& aText, int aPx, const COLOR4D& aColor,
                     GR_TEXT_H_ALIGN_T aH = GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_T aV = GR_TEXT_V_ALIGN_TOP )
    {
        TEXT_ATTRIBUTES attrs;
        attrs.m_Size = VECTOR2I( KiROUND( aPx * iuPerPx * 0.8 ), KiROUND( aPx * iuPerPx ) );
        attrs.m_StrokeWidth = std::max( 1, KiROUND( aPx * iuPerPx / 7 ) );
        attrs.m_Halign = aH;
        attrs.m_Valign = aV;
        attrs.m_Multiline = false;
        plotter.PlotText( aPos, aColor, aText, attrs, KIFONT::FONT::GetFont() );
    };

    // --- nets, bright
    const COLOR4D bright = rgba( 0, 180, 230, 0.9 );

    if( !netWires.empty() || !netLabels.empty() || !netPins.empty() )
    {
        plotter.SetColor( bright );

        for( SCH_LINE* w : netWires )
            plotter.ThickSegment( w->GetStartPoint(), w->GetEndPoint(), 4 * px, nullptr );

        for( SCH_ITEM* l : netLabels )
        {
            BOX2I box = l->GetBoundingBox();
            box.Inflate( px );
            plotter.ThickRect( box.GetOrigin(), box.GetEnd(), 2 * px, nullptr );
        }

        for( const VECTOR2I& p : netPins )
            plotter.FilledCircle( p, 8 * px, nullptr );
    }

    // --- the parts asked about
    const COLOR4D mark = rgba( 230, 0, 200 );

    for( SCH_SYMBOL* s : marked )
    {
        BOX2I box = s->GetBodyAndPinsBoundingBox();
        box.Inflate( 3 * px );
        plotter.SetColor( mark );
        plotter.ThickRect( box.GetOrigin(), box.GetEnd(), 3 * px, nullptr );
        text( VECTOR2I( box.GetLeft(), box.GetTop() - 3 * px ), s->GetRef( &*sheet ), 16, mark, GR_TEXT_H_ALIGN_LEFT,
              GR_TEXT_V_ALIGN_BOTTOM );
    }

    // --- the rulers: top (x) and left (y), mm
    {
        const VECTOR2I topLeft( area.GetX() - bandXIu, area.GetY() - bandYIu );
        const COLOR4D  ink = rgba( 70, 70, 80 );

        plotter.SetColor( rgba( 228, 230, 235 ) );
        plotter.Rect( topLeft, VECTOR2I( area.GetRight(), area.GetY() ), FILL_T::FILLED_SHAPE, 0 );
        plotter.Rect( topLeft, VECTOR2I( area.GetX(), area.GetBottom() ), FILL_T::FILLED_SHAPE, 0 );

        double step = 1000;

        for( double s : { 1.27, 2.54, 5.08, 10.16, 25.4, 50.8, 101.6, 254.0 } )
        {
            if( s * pxPerMm >= 60 )
            {
                step = s;
                break;
            }
        }

        auto fmt = []( double v ) { return wxString::Format( wxS( "%g" ), std::fabs( v ) < 0.005 ? 0.0 : std::round( v * 100 ) / 100 ); };

        for( double x = std::ceil( schIUScale.IUTomm( area.GetX() ) / step - 1e-9 ) * step;
             schIUScale.mmToIU( x ) <= area.GetRight(); x += step )
        {
            VECTOR2I p( schIUScale.mmToIU( x ), area.GetY() );
            plotter.SetColor( ink );
            plotter.ThickSegment( p, p - VECTOR2I( 0, 5 * px ), px, nullptr );
            plotter.SetColor( rgba( 0, 0, 0, 0.06 ) );
            plotter.ThickSegment( p, VECTOR2I( p.x, area.GetBottom() ), px, nullptr );
            text( p + VECTOR2I( 2 * px, -6 * px ), fmt( x ), 10, ink, GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_BOTTOM );
        }

        for( double y = std::ceil( schIUScale.IUTomm( area.GetY() ) / step - 1e-9 ) * step;
             schIUScale.mmToIU( y ) <= area.GetBottom(); y += step )
        {
            VECTOR2I p( area.GetX(), schIUScale.mmToIU( y ) );
            plotter.SetColor( ink );
            plotter.ThickSegment( p, p - VECTOR2I( 5 * px, 0 ), px, nullptr );
            plotter.SetColor( rgba( 0, 0, 0, 0.06 ) );
            plotter.ThickSegment( p, VECTOR2I( area.GetRight(), p.y ), px, nullptr );
            text( VECTOR2I( p.x - 32 * px, p.y + 2 * px ), fmt( y ), 10, ink, GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_TOP );
        }

        text( topLeft + VECTOR2I( 2 * px, 2 * px ), wxS( "mm" ), 10, ink );
    }

    if( !plotter.EndPlot() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "the picture could not be written to " + str( file ) ) );

    wxFFile     in( file, wxS( "rb" ) );
    std::string bytes;

    if( in.IsOpened() )
    {
        bytes.resize( (size_t) in.Length() );

        if( in.Read( bytes.data(), bytes.size() ) != bytes.size() )
            bytes.clear();

        in.Close();
    }

    if( req.path().empty() )
        wxRemoveFile( file );
    else
        out.set_path( str( file ) );

    if( bytes.empty() )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, "the picture could not be read back" ) );

    out.set_png( bytes );
    return out;
}
