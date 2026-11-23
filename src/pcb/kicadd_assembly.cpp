/*
 * KiCadD roadmap phase 5 item 1a: the assembly drawing (export_assembly).
 *
 * One PDF from the board in memory: a page for the top side and, when parts are on the bottom,
 * a page for it seen from below (mirrored), each with the drawing sheet and title block, scaled to
 * the page. KiCad's own plot of the side's Fab layer (part outlines, their reference text, the
 * pads sketched) and the outline; then, for what the Fab layer leaves out (LCSC / EasyEDA
 * footprints have none), each part's outline, pin 1 and reference drawn from its courtyard; DNP
 * parts crossed out. Footprints the assembler does not place (no pads, or left out of the
 * position file: logos, graphics) get nothing added.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "kicadd_handler_pcb.h"

#include <board.h>
#include <board_design_settings.h>
#include <font/font.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_plot_params.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcbplot.h>
#include <plotters/plotters_pslike.h>

#include <wx/filename.h>

#include <algorithm>

using namespace kicadd::board;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;

namespace
{

ApiResponseStatus errorA( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}


struct FAB_INFO
{
    bool outline = false;   ///< the footprint draws on its side's Fab layer
    bool ref = false;       ///< its reference shows there (a ${REFERENCE} text or the field)
};


FAB_INFO fabOf( FOOTPRINT* aFp, PCB_LAYER_ID aFab )
{
    FAB_INFO info;

    for( BOARD_ITEM* item : aFp->GraphicalItems() )
    {
        if( item->GetLayer() != aFab )
            continue;

        if( item->Type() == PCB_SHAPE_T )
            info.outline = true;
        else if( item->Type() == PCB_TEXT_T && static_cast<PCB_TEXT*>( item )->GetText().Contains( wxS( "REFERENCE" ) )
                 && static_cast<PCB_TEXT*>( item )->IsVisible() )
            info.ref = true;
    }

    for( PCB_FIELD* field : aFp->GetFields() )
    {
        if( field && field->GetLayer() == aFab && field->IsVisible() && field->GetId() == FIELD_T::REFERENCE )
            info.ref = true;
    }

    return info;
}

/// a part the assembler places (or would: DNP parts are left out of the position file too, and
/// are shown crossed out); not logos, graphics, holes without a part
bool assembled( FOOTPRINT* aFp )
{
    return !aFp->Pads().empty() && ( aFp->IsDNP() || !( aFp->GetAttributes() & FP_EXCLUDE_FROM_POS_FILES ) );
}

} // namespace


HANDLER_RESULT<ExportAssemblyResponse>
KICADD_HANDLER_PCB::handleExportAssembly( const HANDLER_CONTEXT<ExportAssembly>& aCtx )
{
    BOARD* brd = board();

    if( !brd )
        return tl::unexpected( errorA( ApiStatusCode::AS_BAD_REQUEST, "no board loaded" ) );

    const ExportAssembly& req = aCtx.Request;
    ExportAssemblyResponse out;

    // the file: path, else <dir or <project>/fab>/<board>-assembly.pdf
    wxFileName board( brd->GetFileName() );
    wxFileName file;

    if( !req.path().empty() )
    {
        file.Assign( wxString::FromUTF8( req.path() ) );

        if( !file.IsAbsolute() )
            file.MakeAbsolute( board.GetPath() );
    }
    else
    {
        wxString dir = req.dir().empty() ? wxString( wxS( "fab" ) ) : wxString::FromUTF8( req.dir() );
        wxFileName d = wxFileName::DirName( dir );

        if( !d.IsAbsolute() )
            d.MakeAbsolute( board.GetPath() );

        file.Assign( d.GetPath(), board.GetName().IsEmpty() ? wxString( wxS( "board" ) ) : board.GetName() );
        file.SetName( file.GetName() + wxS( "-assembly" ) );
        file.SetExt( wxS( "pdf" ) );
    }

    if( !wxFileName::DirExists( file.GetPath() ) && !wxFileName::Mkdir( file.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return tl::unexpected( errorA( ApiStatusCode::AS_BAD_REQUEST, "cannot make " + file.GetPath().ToStdString( wxConvUTF8 ) ) );

    // the sides with parts
    int front = 0, back = 0;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        if( assembled( fp ) )
            ( fp->IsFlipped() ? back : front )++;
    }

    std::vector<bool> sides = { false };   // flipped?

    if( back > 0 )
        sides.push_back( true );

    PCB_PLOT_PARAMS opts;
    opts.SetFormat( PLOT_FORMAT::PDF );
    opts.m_PDFSingle = true;
    opts.SetPlotFrameRef( true );
    opts.SetAutoScale( true );
    opts.SetBlackAndWhite( true );
    opts.SetSketchPadsOnFabLayers( true );
    opts.SetPlotReference( true );
    opts.SetPlotValue( req.values() );
    opts.SetUseAuxOrigin( false );
    opts.SetDrillMarksType( DRILL_MARKS::NO_DRILL_SHAPE );
    opts.SetMirror( false );

    const int  pages = (int) sides.size();
    PLOTTER*   plotter = StartPlotBoard( brd, &opts, F_Fab, wxS( "Top" ), file.GetFullPath(), wxS( "Assembly, top" ),
                                         wxS( "/" ), wxS( "Top" ), wxS( "1" ), pages );

    if( !plotter )
        return tl::unexpected( errorA( ApiStatusCode::AS_BAD_REQUEST, "cannot write " + file.GetFullPath().ToStdString( wxConvUTF8 ) ) );

    const int pen = pcbIUScale.mmToIU( 0.12 );
    int       drawn = 0, dnp = 0;

    for( size_t si = 0; si < sides.size(); ++si )
    {
        const bool         bottom = sides[si];
        const PCB_LAYER_ID fab = bottom ? B_Fab : F_Fab;

        if( si > 0 )
        {
            // the bottom seen from below
            opts.SetMirror( true );
            static_cast<PDF_PLOTTER*>( plotter )->ClosePage();
            static_cast<PDF_PLOTTER*>( plotter )->StartPage( wxS( "2" ), wxS( "Bottom" ) );
            setupPlotterNewPDFPage( plotter, brd, &opts, wxS( "Bottom" ), wxS( "Assembly, bottom (seen from below)" ), wxS( "/" ),
                                    wxS( "2" ), pages );
        }

        plotter->SetLayer( fab );
        PlotBoardLayers( brd, plotter, LSEQ( { fab, Edge_Cuts } ), opts );

        // what the Fab layer leaves out, from the courtyard (else the pads)
        plotter->SetColor( COLOR4D::BLACK );

        for( FOOTPRINT* fp : brd->Footprints() )
        {
            if( fp->IsFlipped() != bottom )
                continue;

            // not placed by the assembler: logos, graphics, mounting holes without a part
            if( !assembled( fp ) )
                continue;

            const FAB_INFO info = fabOf( fp, fab );
            BOX2I          box;
            const SHAPE_POLY_SET& cy = fp->GetCourtyard( bottom ? B_CrtYd : F_CrtYd );

            if( cy.OutlineCount() > 0 )
                box = cy.BBox();
            else
                box = fp->GetBoundingBox( false );

            if( !info.outline )
            {
                plotter->Rect( box.GetOrigin(), box.GetEnd(), FILL_T::NO_FILL, pen );

                // pin 1
                for( PAD* pad : fp->Pads() )
                {
                    if( pad->GetNumber() == wxS( "1" ) || pad->GetNumber() == wxS( "A1" ) )
                    {
                        plotter->Circle( pad->GetPosition(), pcbIUScale.mmToIU( 0.35 ), FILL_T::FILLED_SHAPE, 0 );
                        break;
                    }
                }

                drawn++;
            }

            if( !info.ref )
            {
                const int       w = box.GetWidth(), h = box.GetHeight();
                const bool      tall = h > w * 1.3;
                const int       len = std::max<int>( 1, (int) fp->GetReference().length() );
                const int       along = tall ? h : w, across = tall ? w : h;
                int             size = std::min( (int) ( along * 0.8 / len / 0.9 ), (int) ( across * 0.5 ) );
                size = std::clamp( size, pcbIUScale.mmToIU( 0.4 ), pcbIUScale.mmToIU( 2.0 ) );

                TEXT_ATTRIBUTES attrs;
                attrs.m_Size = VECTOR2I( size, size );
                attrs.m_StrokeWidth = std::max( pen, size / 8 );
                attrs.m_Halign = GR_TEXT_H_ALIGN_CENTER;
                attrs.m_Valign = GR_TEXT_V_ALIGN_CENTER;
                attrs.m_Angle = tall ? ANGLE_90 : ANGLE_0;
                attrs.m_Mirrored = bottom;   // the page is mirrored: the text reads right
                plotter->PlotText( box.GetCenter(), COLOR4D::BLACK, fp->GetReference(), attrs, KIFONT::FONT::GetFont() );
            }

            if( fp->IsDNP() )
            {
                plotter->ThickSegment( box.GetOrigin(), box.GetEnd(), pen, nullptr );
                plotter->ThickSegment( VECTOR2I( box.GetLeft(), box.GetBottom() ), VECTOR2I( box.GetRight(), box.GetTop() ), pen,
                                       nullptr );
                dnp++;
            }
        }
    }

    plotter->EndPlot();
    delete plotter->RenderSettings();
    delete plotter;

    out.set_file( file.GetFullPath().ToStdString( wxConvUTF8 ) );
    out.set_pages( pages );
    out.set_front( front );
    out.set_back( back );
    out.set_dnp( dnp );
    out.set_drawn( drawn );
    return out;
}
