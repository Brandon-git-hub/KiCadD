/*
 * KiCadD: one schematic sheet as SVG for the web view's Schematic tab (roadmap phase 4 item 2).
 *
 * KiCad's SVG plotter draws the sheet in memory as File > Plot does (frame and title block,
 * the schematic's colours); its user units are mm with the page's top left at 0, the same
 * coordinates as the other schematic tools. With it: each part's body box (to click and to find
 * its footprint), each net's wires and connection points (to highlight a net here and on the
 * board), the sheet symbols on the page (to go down into them) and every sheet (a picker).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_handler_sch.h"

#include <api/sch_context.h>
#include <base_units.h>
#include <connectivity/conn_facade.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <plotters/plotters_pslike.h>
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

#include <wx/ffile.h>
#include <wx/filename.h>

#include <map>

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


double mm( int aIu )
{
    return std::round( schIUScale.IUTomm( aIu ) * 1000 ) / 1000;
}


void box( google::protobuf::RepeatedField<double>* aOut, const BOX2I& aBox )
{
    for( int v : { aBox.GetLeft(), aBox.GetTop(), aBox.GetRight(), aBox.GetBottom() } )
        aOut->Add( mm( v ) );
}


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


HANDLER_RESULT<SchSvgResponse> KICADD_HANDLER_SCH::handleSheetSvg( const HANDLER_CONTEXT<SchSheetSvg>& aCtx )
{
    SCHEMATIC* sch = schematic();

    if( !sch )
        return tl::unexpected( makeError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    rebuildConnectivity();

    std::string                   err;
    std::optional<SCH_SHEET_PATH> sheet = findSheet( aCtx.Request.sheet().empty() ? "/" : aCtx.Request.sheet(), err );

    if( !sheet )
        return tl::unexpected( makeError( ApiStatusCode::AS_BAD_REQUEST, err ) );

    SchSvgResponse out;
    out.set_sheet( str( sheet->PathHumanReadable() ) );

    for( const SCH_SHEET_PATH& path : sch->Hierarchy() )
    {
        SchSvgSheetRef* r = out.add_sheets();
        r->set_name( path.size() > 1 ? str( path.Last()->GetName() ) : std::string( "root" ) );
        r->set_path( str( path.PathHumanReadable() ) );
        r->set_page( str( path.GetPageNumber() ) );
    }

    SHEET_GUARD guard{ sch, sch->CurrentSheet() };
    sch->SetCurrentSheet( *sheet );
    sch->CurrentSheet().UpdateAllScreenReferences();
    sch->SetSheetNumberAndCount();

    SCH_SCREEN*      screen = sheet->LastScreen();
    const PAGE_INFO& page = screen->GetPageSettings();
    out.set_width( mm( page.GetWidthIU( schIUScale.IU_PER_MILS ) ) );
    out.set_height( mm( page.GetHeightIU( schIUScale.IU_PER_MILS ) ) );

    // --- the plot, as eeschema's SVG export (sch_plotter.cpp)
    SCH_RENDER_SETTINGS render;
    kicadd::InitSchRenderSettings( render, *sch );

    wxString file = wxFileName::CreateTempFileName( wxS( "kicadd-svg" ) );

    {
        SVG_PLOTTER plotter;
        plotter.SetRenderSettings( &render );
        plotter.SetPageSettings( page );
        plotter.SetColorMode( true );
        plotter.SetViewport( VECTOR2I( 0, 0 ), schIUScale.IU_PER_MILS / 10, 1.0, false );
        plotter.SetCreator( wxT( "KiCadD" ) );

        if( !plotter.OpenFile( file ) || !plotter.StartPlot( sheet->GetPageNumber() ) )
        {
            wxRemoveFile( file );
            return tl::unexpected( makeError( ApiStatusCode::AS_INTERNAL_ERROR, "the SVG could not be started" ) );
        }

        plotter.SetColor( render.GetLayerColor( LAYER_SCHEMATIC_BACKGROUND ) );
        plotter.Rect( VECTOR2I( 0, 0 ), VECTOR2I( page.GetWidthIU( schIUScale.IU_PER_MILS ), page.GetHeightIU( schIUScale.IU_PER_MILS ) ),
                      FILL_T::FILLED_SHAPE, 1.0, 0 );

        PlotDrawingSheet( &plotter, &sch->Project(), screen->GetTitleBlock(), page, sch->GetProperties(),
                          screen->GetPageNumber(), screen->GetPageCount(), sheet->Last()->GetName(),
                          sheet->PathHumanReadable(), screen->GetFileName(),
                          render.GetLayerColor( LAYER_SCHEMATIC_DRAWINGSHEET ), screen->GetVirtualPageNumber() == 1,
                          sch->GetCurrentVariant(), sch->GetVariantDescription( sch->GetCurrentVariant() ) );

        SCH_PLOT_OPTS opts;
        opts.m_plotDrawingSheet = true;
        screen->Plot( &plotter, opts );
        plotter.EndPlot();
    }

    wxFFile f( file, wxS( "rb" ) );
    wxString text;

    if( !f.IsOpened() || !f.ReadAll( &text, wxConvUTF8 ) )
    {
        wxRemoveFile( file );
        return tl::unexpected( makeError( ApiStatusCode::AS_INTERNAL_ERROR, "the SVG could not be read" ) );
    }

    f.Close();
    wxRemoveFile( file );
    out.set_svg( str( text ) );

    // --- parts, nets, sheet symbols
    auto netOf = [&]( SCH_ITEM* aItem ) -> std::string
    {
        std::optional<SCH_CONNECTIVITY::ITEM_VIEW> view = sch->Connectivity().Connection( aItem->m_Uuid, sheet->Path() );
        return view && !view->IsUnconnected() ? str( view->Name() ) : std::string();
    };

    std::map<std::string, SchSvgNet*> nets;

    auto net = [&]( const std::string& aName ) -> SchSvgNet*
    {
        auto it = nets.find( aName );

        if( it != nets.end() )
            return it->second;

        SchSvgNet* n = out.add_nets();
        n->set_name( aName );
        return nets[aName] = n;
    };

    auto point = [&]( const std::string& aNet, const VECTOR2I& aPos )
    {
        if( aNet.empty() )
            return;

        SchSvgNet* n = net( aNet );
        n->add_pins( mm( aPos.x ) );
        n->add_pins( mm( aPos.y ) );
    };

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() == SCH_LINE_T && static_cast<SCH_LINE*>( item )->IsWire() )
        {
            auto*       line = static_cast<SCH_LINE*>( item );
            std::string n = netOf( item );

            if( n.empty() )
                continue;

            SchSvgNet* sn = net( n );

            for( int v : { line->GetStartPoint().x, line->GetStartPoint().y, line->GetEndPoint().x, line->GetEndPoint().y } )
                sn->add_segs( mm( v ) );
        }
        else if( item->IsType( { SCH_LABEL_LOCATE_ANY_T } ) )
        {
            point( netOf( item ), item->GetPosition() );
        }
        else if( item->Type() == SCH_SYMBOL_T )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );

            for( SCH_PIN* pin : symbol->GetPins( &*sheet ) )
                point( netOf( pin ), pin->GetPosition() );

            if( symbol->IsPower() )
                continue;

            SchSvgSymbol* s = out.add_symbols();
            s->set_ref( str( symbol->GetRef( &*sheet ) ) );
            s->set_value( str( symbol->GetValue( &*sheet, FOR_GUI ) ) );
            s->set_footprint( str( symbol->GetFootprintFieldText( &*sheet, FOR_GUI ) ) );
            box( s->mutable_box(), symbol->GetBodyBoundingBox() );
        }
        else if( item->Type() == SCH_SHEET_T )
        {
            auto* child = static_cast<SCH_SHEET*>( item );

            for( SCH_SHEET_PIN* pin : child->GetPins() )
                point( netOf( pin ), pin->GetPosition() );

            SCH_SHEET_PATH path = *sheet;
            path.push_back( child );

            SchSvgSheetRef* r = out.add_children();
            r->set_name( str( child->GetName() ) );
            r->set_path( str( path.PathHumanReadable() ) );
            r->set_page( str( path.GetPageNumber() ) );
            box( r->mutable_box(), child->GetBodyBoundingBox() );
        }
    }

    return out;
}
