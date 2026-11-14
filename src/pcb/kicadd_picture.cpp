/*
 * KiCadD: a picture (PNG) of the board in memory, for the agent to look at.
 *
 * KiCad's plotter does the layers (PNG_PLOTTER, cairo): the same pads, tracks, zone fills and
 * text the Gerbers have. On top of them, what the agent asked to look at: parts framed and
 * labelled, nets drawn bright, the unrouted connections, a millimetre ruler on the top and left.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "pcb/kicadd_picture.h"

#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>
#include <pcb_plot_params.h>
#include <pcbplot.h>
#include <pcb_painter.h>
#include <plotters/plotter_png.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <pgm_base.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_algo.h>
#include <ratsnest/ratsnest_data.h>
#include <geometry/shape_segment.h>
#include <font/text_attributes.h>

#include <wx/ffile.h>
#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace kicadd
{

namespace
{

COLOR4D rgba( int r, int g, int b, double a = 1.0 )
{
    return COLOR4D( r / 255.0, g / 255.0, b / 255.0, a );
}

const COLOR4D BACKGROUND = rgba( 20, 22, 28 );

std::string str( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

/// Stacking order seen from the front, bottom first
int frontRank( PCB_LAYER_ID aLayer, int aCopper )
{
    switch( aLayer )
    {
    case B_CrtYd:   return 0;
    case B_Fab:     return 1;
    case B_Paste:   return 2;
    case B_SilkS:   return 3;
    case B_Mask:    return 4;
    case B_Cu:      return 10;
    case F_Cu:      return 100;
    case F_Mask:    return 101;
    case F_Paste:   return 102;
    case F_SilkS:   return 103;
    case F_Fab:     return 104;
    case F_CrtYd:   return 105;
    case Edge_Cuts: return 200;
    case Margin:    return 199;
    default:        break;
    }

    if( IsInnerCopperLayer( aLayer ) )
    {
        // In1 is next to F.Cu
        int k = ( (int) aLayer - (int) In1_Cu ) / 2 + 1;
        return 11 + ( aCopper - k );
    }

    return 150;   // user layers
}

/// The palette: copper a little transparent, so a layer under another shows through
void setPalette( COLOR_SETTINGS& aColors )
{
    aColors.SetColor( F_Cu, rgba( 200, 52, 52, 0.85 ) );
    aColors.SetColor( B_Cu, rgba( 70, 120, 210, 0.80 ) );

    const COLOR4D inner[] = { rgba( 194, 194, 0, 0.65 ), rgba( 194, 0, 194, 0.65 ), rgba( 0, 194, 120, 0.65 ),
                              rgba( 230, 140, 40, 0.65 ), rgba( 120, 200, 230, 0.65 ), rgba( 160, 120, 230, 0.65 ) };
    int k = 0;

    for( PCB_LAYER_ID l : LSET::InternalCuMask().Seq() )
        aColors.SetColor( l, inner[k++ % 6] );

    aColors.SetColor( F_SilkS, rgba( 242, 237, 161 ) );
    aColors.SetColor( B_SilkS, rgba( 232, 178, 167 ) );
    aColors.SetColor( F_Fab, rgba( 175, 175, 175 ) );
    aColors.SetColor( B_Fab, rgba( 140, 145, 190 ) );
    aColors.SetColor( F_CrtYd, rgba( 255, 38, 226, 0.8 ) );
    aColors.SetColor( B_CrtYd, rgba( 38, 233, 255, 0.8 ) );
    aColors.SetColor( F_Mask, rgba( 216, 100, 255, 0.35 ) );
    aColors.SetColor( B_Mask, rgba( 2, 255, 238, 0.35 ) );
    aColors.SetColor( F_Paste, rgba( 180, 160, 154, 0.6 ) );
    aColors.SetColor( B_Paste, rgba( 0, 194, 194, 0.6 ) );
    aColors.SetColor( Edge_Cuts, rgba( 230, 230, 220 ) );
    aColors.SetColor( Margin, rgba( 255, 38, 226 ) );
    aColors.SetColor( Dwgs_User, rgba( 194, 194, 194 ) );
    aColors.SetColor( Cmts_User, rgba( 89, 148, 220 ) );

    for( int t = 0; t < 4; ++t )
        aColors.SetColor( LAYER_VIAS + t, rgba( 220, 220, 220, 0.9 ) );
}

/// A ruler step (mm) with at least aMinPx pixels between ticks
double rulerStep( double aPxPerMm, double aMinPx )
{
    for( double s : { 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0 } )
    {
        if( s * aPxPerMm >= aMinPx )
            return s;
    }

    return 1000.0;
}

} // namespace


bool RenderBoardPng( BOARD* aBoard, const kicadd::board::RenderBoard& aReq,
                     kicadd::board::RenderBoardResponse& aOut, std::string& aError )
{
    const bool back = aReq.back();
    const int  copper = aBoard->GetCopperLayerCount();
    const LSET enabled = aBoard->GetEnabledLayers();

    // --- the layers
    LSET layers;
    const LSET cu = LSET::AllCuMask( copper ) & enabled;
    const PCB_LAYER_ID nearCu = back ? B_Cu : F_Cu;

    std::vector<std::string> names( aReq.layers().begin(), aReq.layers().end() );

    if( names.empty() )
        names.push_back( back ? "back" : "front" );

    for( const std::string& n : names )
    {
        std::string low = n;
        std::transform( low.begin(), low.end(), low.begin(), ::tolower );

        if( low == "front" || low == "back" || low == "all" )
            layers |= cu | LSET( { back ? B_SilkS : F_SilkS, Edge_Cuts } );
        else if( low == "copper" )
            layers |= cu | LSET( { Edge_Cuts } );
        else if( low == "silk" || low == "silkscreen" )
            layers |= LSET( { back ? B_Mask : F_Mask, back ? B_SilkS : F_SilkS, Edge_Cuts } );   // the mask openings show the pads
        else if( low == "fab" || low == "assembly" )
            layers |= LSET( { back ? B_Fab : F_Fab, back ? B_CrtYd : F_CrtYd, Edge_Cuts } );
        else
        {
            PCB_LAYER_ID id = aBoard->GetLayerID( wxString::FromUTF8( n ) );

            if( id == UNDEFINED_LAYER )
            {
                // the short names too ("F.SilkS" for "F.Silkscreen")
                for( PCB_LAYER_ID l : LSET::AllLayersMask().Seq() )
                {
                    if( str( LSET::Name( l ) ) == n || str( aBoard->GetLayerName( l ) ) == n )
                        id = l;
                }
            }

            if( id == UNDEFINED_LAYER || !enabled.Contains( id ) )
                aOut.add_missing( "layer " + n );
            else
                layers.set( id );
        }
    }

    if( layers.none() )
    {
        aError = "no layer to draw (front, back, copper, silk, fab, or names like F.Cu)";
        return false;
    }

    LSEQ seq = layers.Seq();
    std::sort( seq.begin(), seq.end(),
               [&]( PCB_LAYER_ID a, PCB_LAYER_ID b )
               {
                   int ra = frontRank( back ? FlipLayer( a, copper ) : a, copper );
                   int rb = frontRank( back ? FlipLayer( b, copper ) : b, copper );
                   return ra < rb;
               } );

    // the standard names ("F.Cu"), the board's own after it when it has one ("F.Cu (top_layer)")
    for( PCB_LAYER_ID l : seq )
    {
        std::string name = str( BOARD::GetStandardLayerName( l ) ), own = str( aBoard->GetLayerName( l ) );
        aOut.add_layers( own == name ? name : name + " (" + own + ")" );
    }

    // --- what to mark
    std::vector<FOOTPRINT*> marked;
    std::set<int>           nets;

    for( const std::string& r : aReq.refs() )
    {
        bool found = false;

        for( FOOTPRINT* fp : aBoard->Footprints() )
        {
            if( str( fp->GetReference() ) == r )
            {
                marked.push_back( fp );
                found = true;
            }
        }

        if( !found )
            aOut.add_missing( r );
    }

    for( const std::string& n : aReq.nets() )
    {
        NETINFO_ITEM* net = aBoard->FindNet( wxString::FromUTF8( n ) );

        if( net && net->GetNetCode() > 0 )
            nets.insert( net->GetNetCode() );
        else
            aOut.add_missing( "net " + n );
    }

    // --- the area
    BOX2I area;

    if( aReq.has_area_min() && aReq.has_area_max() )
    {
        VECTOR2I a( aReq.area_min().x(), aReq.area_min().y() );
        VECTOR2I c( aReq.area_max().x(), aReq.area_max().y() );
        area = BOX2I( a, c - a );
        area.Normalize();
    }
    else if( aReq.zoom() && ( !marked.empty() || !nets.empty() ) )
    {
        bool first = true;

        auto add = [&]( const BOX2I& aBox )
        {
            if( first )
                area = aBox;
            else
                area.Merge( aBox );

            first = false;
        };

        for( FOOTPRINT* fp : marked )
            add( fp->GetBoundingBox( false ) );

        for( PCB_TRACK* t : aBoard->Tracks() )
        {
            if( nets.count( t->GetNetCode() ) )
                add( t->GetBoundingBox() );
        }

        for( FOOTPRINT* fp : aBoard->Footprints() )
        {
            for( PAD* p : fp->Pads() )
            {
                if( nets.count( p->GetNetCode() ) )
                    add( p->GetBoundingBox() );
            }
        }

        const int margin = pcbIUScale.mmToIU( aReq.margin() > 0 ? aReq.margin() : 3.0 );
        area.Inflate( margin );
    }
    else
    {
        // the board and everything on it (parts left outside the outline too)
        area = aBoard->ComputeBoundingBox( false );

        for( FOOTPRINT* fp : aBoard->Footprints() )
            area.Merge( fp->GetBoundingBox( false ) );

        area.Inflate( pcbIUScale.mmToIU( 2 ) );
    }

    if( area.GetWidth() <= 0 || area.GetHeight() <= 0 )
    {
        aError = "nothing to draw: the area is empty";
        return false;
    }

    // --- the scale: an integer dpi (the plotter's), the long side about `size` pixels
    const int    size = aReq.size() > 0 ? std::clamp( aReq.size(), 200, 4000 ) : 1200;
    const double wMm = pcbIUScale.IUTomm( area.GetWidth() ), hMm = pcbIUScale.IUTomm( area.GetHeight() );
    const int    fit = size - ( wMm >= hMm ? 34 : 18 );   // the ruler strip is part of the size
    int          dpi = std::max( 1, (int) std::floor( fit / std::max( wMm, hMm ) * 25.4 ) );
    const double pxPerMm = dpi / 25.4;
    const double iuPerPx = pcbIUScale.mmToIU( 1.0 ) / pxPerMm;
    const int    bandX = 34, bandY = 18;   // the rulers' strips (left, top), pixels
    const int    W = bandX + (int) std::ceil( wMm * pxPerMm );
    const int    H = bandY + (int) std::ceil( hMm * pxPerMm );

    if( W > 8000 || H > 8000 )
    {
        aError = "the picture would be too big";
        return false;
    }

    const int bandXIu = KiROUND( bandX * iuPerPx ), bandYIu = KiROUND( bandY * iuPerPx );
    VECTOR2I  offset( back ? area.GetX() : area.GetX() - bandXIu, area.GetY() - bandYIu );

    aOut.set_width( W );
    aOut.set_height( H );
    aOut.set_mm_per_px( 1.0 / pxPerMm );
    aOut.mutable_origin()->set_x( back ? area.GetRight() : area.GetX() );
    aOut.mutable_origin()->set_y( area.GetY() );
    aOut.mutable_extent()->set_x( back ? area.GetX() : area.GetRight() );
    aOut.mutable_extent()->set_y( area.GetBottom() );

    // --- the plotter
    COLOR_SETTINGS colors( *::GetColorSettings( DEFAULT_THEME ) );
    setPalette( colors );

    PCB_PLOT_PARAMS opts;
    opts.SetFormat( PLOT_FORMAT::PNG );
    opts.SetColorSettings( &colors );
    opts.SetBlackAndWhite( false );
    opts.SetDrillMarksType( DRILL_MARKS::NO_DRILL_SHAPE );    // the holes: below, in the background colour
    opts.SetPlotReference( true );
    opts.SetPlotValue( false );
    opts.SetSketchPadsOnFabLayers( true );
    opts.SetPlotPadNumbers( true );
    opts.SetSubtractMaskFromSilk( false );
    opts.SetMirror( back );
    opts.SetSkipPlotNPTH_Pads( false );

    PNG_PLOTTER plotter;
    plotter.SetPixelSize( W, H );
    plotter.SetResolution( dpi );
    plotter.SetAntialias( true );
    plotter.SetBackgroundColor( BACKGROUND );

    KIGFX::PCB_RENDER_SETTINGS* render = new KIGFX::PCB_RENDER_SETTINGS();
    render->LoadColors( &colors );
    render->SetDefaultPenWidth( pcbIUScale.mmToIU( 0.0212 ) );
    plotter.SetRenderSettings( render );
    std::unique_ptr<KIGFX::PCB_RENDER_SETTINGS> ownRender( render );

    plotter.SetPageSettings( aBoard->GetPageSettings() );
    plotter.SetViewport( offset, pcbIUScale.IU_PER_MILS / 10, 1.0, back );
    plotter.SetCreator( wxT( "KiCadD" ) );
    plotter.SetColorMode( true );

    wxString file = aReq.path().empty() ? wxFileName::CreateTempFileName( wxS( "kicadd-pic" ) )
                                        : wxString::FromUTF8( aReq.path() );

    if( !plotter.OpenFile( file ) || !plotter.StartPlot( wxS( "1" ) ) )
    {
        aError = "the picture could not be started";
        return false;
    }

    PlotBoardLayers( aBoard, &plotter, seq, opts );

    const int px = std::max( 1, KiROUND( iuPerPx ) );   // one pixel, in IU

    // the holes, in the background colour: through every copper layer drawn
    if( ( layers & LSET::AllCuMask() ).any() )
    {
        plotter.SetColor( BACKGROUND );

        for( FOOTPRINT* fp : aBoard->Footprints() )
        {
            for( PAD* p : fp->Pads() )
            {
                if( !p->HasHole() )
                    continue;

                std::shared_ptr<SHAPE_SEGMENT> hole = p->GetEffectiveHoleShape();

                if( hole )
                    plotter.ThickSegment( hole->GetSeg().A, hole->GetSeg().B, hole->GetWidth(), nullptr );
            }
        }

        for( PCB_TRACK* t : aBoard->Tracks() )
        {
            if( t->Type() == PCB_VIA_T )
                plotter.FilledCircle( t->GetPosition(), static_cast<PCB_VIA*>( t )->GetDrillValue(), nullptr );
        }
    }

    // text that reads the right way round in a mirrored picture too
    auto text = [&]( const VECTOR2I& aPos, const wxString& aText, int aPx, const COLOR4D& aColor,
                     GR_TEXT_H_ALIGN_T aH = GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_T aV = GR_TEXT_V_ALIGN_TOP )
    {
        TEXT_ATTRIBUTES attrs;
        attrs.m_Size = VECTOR2I( KiROUND( aPx * iuPerPx * 0.8 ), KiROUND( aPx * iuPerPx ) );
        attrs.m_StrokeWidth = std::max( 1, KiROUND( aPx * iuPerPx / 7 ) );
        attrs.m_Halign = aH;
        attrs.m_Valign = aV;
        attrs.m_Mirrored = back;
        attrs.m_Multiline = false;
        plotter.PlotText( aPos, aColor, aText, attrs, KIFONT::FONT::GetFont() );
    };

    // --- nets drawn bright
    if( !nets.empty() )
    {
        const COLOR4D bright = rgba( 80, 255, 255, 0.95 );
        plotter.SetColor( bright );

        for( PCB_TRACK* t : aBoard->Tracks() )
        {
            if( !nets.count( t->GetNetCode() ) || ( !layers.Contains( t->GetLayer() ) && t->Type() != PCB_VIA_T ) )
                continue;

            if( t->Type() == PCB_VIA_T )
                plotter.FilledCircle( t->GetPosition(), static_cast<PCB_VIA*>( t )->GetWidth( F_Cu ), nullptr );
            else
                plotter.ThickSegment( t->GetStart(), t->GetEnd(), t->GetWidth(), nullptr );
        }

        for( FOOTPRINT* fp : aBoard->Footprints() )
        {
            for( PAD* p : fp->Pads() )
            {
                if( !nets.count( p->GetNetCode() ) )
                    continue;

                SHAPE_POLY_SET poly;
                PCB_LAYER_ID   side = p->IsOnLayer( nearCu ) ? nearCu : p->GetPrincipalLayer();
                p->TransformShapeToPolygon( poly, side, 0, aBoard->GetDesignSettings().m_MaxError, ERROR_INSIDE );

                for( int o = 0; o < poly.OutlineCount(); ++o )
                {
                    std::vector<VECTOR2I> pts;

                    for( const VECTOR2I& v : poly.COutline( o ).CPoints() )
                        pts.push_back( v );

                    if( !pts.empty() )
                    {
                        pts.push_back( pts.front() );
                        plotter.PlotPoly( pts, FILL_T::FILLED_SHAPE, 0, nullptr );
                    }
                }
            }
        }

        for( ZONE* z : aBoard->Zones() )
        {
            if( !nets.count( z->GetNetCode() ) || !( z->GetLayerSet() & layers ).any() )
                continue;

            const SHAPE_POLY_SET* outline = z->Outline();

            for( int o = 0; o < outline->OutlineCount(); ++o )
            {
                std::vector<VECTOR2I> pts;

                for( const VECTOR2I& v : outline->COutline( o ).CPoints() )
                    pts.push_back( v );

                if( !pts.empty() )
                {
                    pts.push_back( pts.front() );
                    plotter.PlotPoly( pts, FILL_T::NO_FILL, 2 * px, nullptr );
                }
            }
        }
    }

    // --- the unrouted connections
    if( !aReq.no_ratsnest() )
    {
        std::shared_ptr<CONNECTIVITY_DATA> conn = aBoard->GetConnectivity();
        int                                n = 0;

        for( NETINFO_ITEM* net : aBoard->GetNetInfo() )
        {
            if( net->GetNetCode() <= 0 )
                continue;

            RN_NET* rn = conn->GetRatsnestForNet( net->GetNetCode() );

            if( !rn )
                continue;

            const bool hi = nets.count( net->GetNetCode() ) > 0;
            plotter.SetColor( hi ? rgba( 80, 255, 255 ) : rgba( 255, 255, 255, 0.75 ) );

            for( const CN_EDGE& e : rn->GetEdges() )
            {
                if( !e.IsVisible() || !e.GetSourceNode() || !e.GetTargetNode() )
                    continue;

                plotter.ThickSegment( e.GetSourceNode()->Pos(), e.GetTargetNode()->Pos(), hi ? 2 * px : px, nullptr );
                n++;
            }
        }

        aOut.set_unrouted( n );
    }

    // --- every part's reference
    if( aReq.labels() )
    {
        std::set<const FOOTPRINT*> skip( marked.begin(), marked.end() );

        for( FOOTPRINT* fp : aBoard->Footprints() )
        {
            if( skip.count( fp ) || ( fp->IsFlipped() != back && !layers.Contains( back ? F_Cu : B_Cu ) ) )
                continue;

            BOX2I    box = fp->GetBoundingBox( false );
            int      h = std::clamp( (int) ( std::min( box.GetWidth(), box.GetHeight() ) / iuPerPx / 2.5 ), 9, 16 );
            text( box.Centre(), fp->GetReference(), h, rgba( 150, 255, 150 ), GR_TEXT_H_ALIGN_CENTER,
                  GR_TEXT_V_ALIGN_CENTER );
        }
    }

    // --- the parts asked about: a frame and their reference
    for( FOOTPRINT* fp : marked )
    {
        const COLOR4D mark = rgba( 255, 60, 255 );
        PCB_LAYER_ID  side = fp->IsFlipped() ? B_CrtYd : F_CrtYd;
        BOX2I         box = fp->GetCourtyard( side ).OutlineCount() ? fp->GetCourtyard( side ).BBox()
                                                                    : fp->GetBoundingBox( false );
        box.Inflate( 2 * px );
        plotter.SetColor( mark );
        plotter.ThickRect( box.GetOrigin(), box.GetEnd(), 3 * px, nullptr );

        VECTOR2I at( back ? box.GetRight() : box.GetLeft(), box.GetTop() - 3 * px );
        text( at, fp->GetReference(), 14, mark, GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_BOTTOM );
    }

    // --- the rulers: top (x) and left (y), mm
    {
        const VECTOR2I topLeft( back ? area.GetRight() + bandXIu : area.GetX() - bandXIu, area.GetY() - bandYIu );
        const VECTOR2I farX( back ? area.GetX() : area.GetRight(), area.GetY() );
        const VECTOR2I farY( back ? area.GetRight() : area.GetX(), area.GetBottom() );

        plotter.SetColor( rgba( 40, 44, 54 ) );
        plotter.Rect( topLeft, VECTOR2I( farX.x, area.GetY() ), FILL_T::FILLED_SHAPE, 0 );
        plotter.Rect( topLeft, VECTOR2I( farY.x, farY.y ), FILL_T::FILLED_SHAPE, 0 );

        const double  step = rulerStep( pxPerMm, 70 );
        const COLOR4D ink = rgba( 200, 205, 215 );
        auto          fmt = []( double v )
        {
            return std::fabs( v - std::round( v ) ) < 1e-6 ? wxString::Format( wxS( "%.0f" ), v )
                                                          : wxString::Format( wxS( "%.1f" ), v );
        };

        double x0 = std::ceil( pcbIUScale.IUTomm( area.GetX() ) / step ) * step;

        for( double x = x0; pcbIUScale.mmToIU( x ) <= area.GetRight(); x += step )
        {
            VECTOR2I p( pcbIUScale.mmToIU( x ), area.GetY() );
            plotter.SetColor( ink );
            plotter.ThickSegment( p, p - VECTOR2I( 0, 5 * px ), px, nullptr );
            plotter.SetColor( rgba( 255, 255, 255, 0.10 ) );
            plotter.ThickSegment( p, VECTOR2I( p.x, area.GetBottom() ), px, nullptr );   // a faint grid
            text( p + VECTOR2I( back ? -2 * px : 2 * px, -6 * px ), fmt( x ), 10, ink, GR_TEXT_H_ALIGN_LEFT,
                  GR_TEXT_V_ALIGN_BOTTOM );
        }

        double y0 = std::ceil( pcbIUScale.IUTomm( area.GetY() ) / step ) * step;

        for( double y = y0; pcbIUScale.mmToIU( y ) <= area.GetBottom(); y += step )
        {
            VECTOR2I p( back ? area.GetRight() : area.GetX(), pcbIUScale.mmToIU( y ) );
            VECTOR2I in( back ? 5 * px : -5 * px, 0 );
            plotter.SetColor( ink );
            plotter.ThickSegment( p, p + in, px, nullptr );
            plotter.SetColor( rgba( 255, 255, 255, 0.10 ) );
            plotter.ThickSegment( p, VECTOR2I( back ? area.GetX() : area.GetRight(), p.y ), px, nullptr );

            text( VECTOR2I( p.x + ( back ? 32 * px : -32 * px ), p.y + 2 * px ), fmt( y ), 10, ink,
                  GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_TOP );
        }

        text( topLeft + VECTOR2I( back ? -2 * px : 2 * px, 2 * px ), wxS( "mm" ), 10, ink );
    }

    if( !plotter.EndPlot() )
    {
        aError = "the picture could not be written to " + str( file );
        return false;
    }

    wxFFile in( file, wxS( "rb" ) );
    std::string bytes;

    if( in.IsOpened() )
    {
        bytes.resize( (size_t) in.Length() );

        if( in.Read( bytes.data(), bytes.size() ) != bytes.size() )
            bytes.clear();

        in.Close();
    }

    if( aReq.path().empty() )
        wxRemoveFile( file );
    else
        aOut.set_path( str( file ) );

    if( bytes.empty() )
    {
        aError = "the picture could not be read back from " + str( file );
        return false;
    }

    aOut.set_png( bytes );
    return true;
}

} // namespace kicadd
