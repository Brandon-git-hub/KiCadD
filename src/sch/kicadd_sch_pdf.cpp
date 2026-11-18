/*
 * KiCadD: the whole schematic as one PDF, from the schematic in memory (unsaved edits included).
 *
 * KiCad's plotter (SCH_PLOTTER, PDF) set up as kicad-cli sch export pdf does, but on the open
 * schematic: kicad-cli's job (and the API's RunSchematicJobExportPdf) loads the file from disk
 * headless, so it would miss what the agent changed since the last save.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_handler_sch.h"

#include <api/sch_context.h>
#include <drawing_sheet/ds_data_model.h>
#include <reporter.h>
#include <sch_painter.h>
#include <sch_plotter.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <schematic.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>

#include <wx/filename.h>

using namespace kicadd::schematic;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;

namespace
{

ApiResponseStatus pdfError( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}

} // namespace


HANDLER_RESULT<SchExportPdfResponse> KICADD_HANDLER_SCH::handleExportPdf( const HANDLER_CONTEXT<SchExportPdf>& aCtx )
{
    SCHEMATIC* sch = schematic();

    if( !sch )
        return tl::unexpected( pdfError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    const SchExportPdf& req = aCtx.Request;
    wxFileName          project( sch->Project().GetProjectFullName() );
    wxFileName          file;

    if( req.path().empty() )
    {
        file = wxFileName( project.GetPath(), wxFileName( sch->GetFileName() ).GetName(), wxS( "pdf" ) );
    }
    else
    {
        file = wxFileName( wxString::FromUTF8( req.path() ) );

        if( !file.IsAbsolute() )
            file.MakeAbsolute( project.GetPath() );

        if( file.GetExt().IsEmpty() )
            file.SetExt( wxS( "pdf" ) );
    }

    if( !wxFileName::DirExists( file.GetPath() ) && !wxFileName::Mkdir( file.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return tl::unexpected( pdfError( ApiStatusCode::AS_BAD_REQUEST, "cannot make the folder " + file.GetPath().ToStdString( wxConvUTF8 ) ) );

    // the sheet the editor (or the other tools) are on comes back after the plot walks the pages
    SCH_SHEET_PATH old = sch->CurrentSheet();

    // the project's drawing sheet (headless nothing has loaded it; the editor has, the same one)
    if( !editor() )
    {
        if( sch->Settings().m_SchDrawingSheetFileName == wxS( "empty.kicad_wks" ) )
        {
            DS_DATA_MODEL::GetTheInstance().SetEmptyLayout();
        }
        else
        {
            wxString msg;
            DS_DATA_MODEL::GetTheInstance().LoadFromName( sch->Settings().m_SchDrawingSheetFileName, project.GetPath(),
                                                          &sch->Project(), { sch->GetEmbeddedFiles() }, &msg );
        }
    }

    SCH_RENDER_SETTINGS render;
    kicadd::InitSchRenderSettings( render, *sch );

    SCH_PLOT_OPTS opts;
    opts.m_plotAll = true;
    opts.m_plotDrawingSheet = true;
    opts.m_blackAndWhite = req.black_and_white();
    opts.m_PDFHierarchicalLinks = true;
    opts.m_PDFPropertyPopups = true;
    opts.m_PDFMetadata = true;
    opts.m_pageSizeSelect = PageFormatReq::PAGE_SIZE_AUTO;
    opts.m_outputFile = file.GetFullPath();

    if( file.FileExists() && !wxRemoveFile( file.GetFullPath() ) )
        return tl::unexpected( pdfError( ApiStatusCode::AS_BAD_REQUEST, "cannot replace " + file.GetFullPath().ToStdString( wxConvUTF8 )
                                                                            + " (open in a viewer?)" ) );

    WX_STRING_REPORTER reporter;
    SCH_PLOTTER        plotter( sch );
    plotter.Plot( PLOT_FORMAT::PDF, opts, &render, &reporter );

    sch->SetCurrentSheet( old );
    sch->CurrentSheet().UpdateAllScreenReferences();
    sch->SetSheetNumberAndCount();

    if( reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR ) || !file.FileExists() )
    {
        std::string why = reporter.GetMessages().ToStdString( wxConvUTF8 );
        return tl::unexpected( pdfError( ApiStatusCode::AS_BAD_REQUEST,
                                         "the PDF could not be written" + ( why.empty() ? "" : ": " + why ) ) );
    }

    SchExportPdfResponse out;
    out.set_path( file.GetFullPath().ToStdString( wxConvUTF8 ) );

    SCH_SHEET_LIST pages;
    pages.BuildSheetList( &sch->Root(), true );
    pages.SortByHierarchicalPageNumbers();

    for( const SCH_SHEET_PATH& p : pages )
        out.add_pages( ( p.GetPageNumber() + wxS( " " ) + p.PathHumanReadable() ).ToStdString( wxConvUTF8 ) );

    return out;
}
