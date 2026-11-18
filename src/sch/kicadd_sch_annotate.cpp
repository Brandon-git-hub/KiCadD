/*
 * KiCadD: number the schematic's parts (Annotate), headless or in the editor.
 *
 * The editor's Annotate (SCH_EDIT_FRAME::AnnotateSymbols) needs the frame and its selection;
 * the numbering itself is AnnotateReferences(), which does not. This collects the parts in
 * scope (all, a sheet and the sheets below it, or named parts) and their multi-unit groups as
 * the editor does, numbers them in one agent commit (one sch_undo step), and checks the whole
 * schematic afterwards (duplicates, units).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "sch/kicadd_handler_sch.h"

#include <api/sch_context.h>
#include <sch_reference_list.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>

#include <map>

using namespace kicadd::schematic;
using kiapi::common::ApiResponseStatus;
using kiapi::common::ApiStatusCode;

namespace
{

ApiResponseStatus annotateError( ApiStatusCode aCode, const std::string& aMsg )
{
    ApiResponseStatus e;
    e.set_status( aCode );
    e.set_error_message( aMsg );
    return e;
}

std::string utf8( const wxString& s )
{
    return s.ToStdString( wxConvUTF8 );
}

/// "R4", "U3A" for a unit of a multi-unit part
std::string refOf( SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aSheet )
{
    return utf8( aSymbol->GetRef( &aSheet, aSymbol->GetUnitCount() > 1 ) );
}

std::string instanceKey( SCH_SYMBOL* aSymbol, const SCH_SHEET_PATH& aSheet )
{
    KIID_PATH path = aSheet.Path();
    path.push_back( aSymbol->m_Uuid );
    return utf8( path.AsString() );
}

} // namespace


HANDLER_RESULT<SchAnnotateResponse> KICADD_HANDLER_SCH::handleAnnotate( const HANDLER_CONTEXT<SchAnnotate>& aCtx )
{
    SCHEMATIC* sch = schematic();

    if( !sch )
        return tl::unexpected( annotateError( ApiStatusCode::AS_UNHANDLED, "no schematic open" ) );

    if( auto err = busy() )
        return tl::unexpected( *err );

    const SchAnnotate&  req = aCtx.Request;
    SchAnnotateResponse out;
    out.set_dry_run( req.dry_run() );

    ANNOTATE_ORDER_T order = SORT_BY_X_POSITION;

    if( req.order() == "y" )
        order = SORT_BY_Y_POSITION;
    else if( !req.order().empty() && req.order() != "x" )
        return tl::unexpected( annotateError( ApiStatusCode::AS_BAD_REQUEST, "order is \"x\" or \"y\"" ) );

    ANNOTATE_ALGO_T algo = INCREMENTAL_BY_REF;

    if( req.per_sheet() == "100" )
        algo = SHEET_NUMBER_X_100;
    else if( req.per_sheet() == "1000" )
        algo = SHEET_NUMBER_X_1000;
    else if( !req.per_sheet().empty() )
        return tl::unexpected( annotateError( ApiStatusCode::AS_BAD_REQUEST, "per_sheet is \"100\" or \"1000\"" ) );

    if( !req.refs().empty() && !req.sheet().empty() )
        return tl::unexpected( annotateError( ApiStatusCode::AS_BAD_REQUEST, "give refs or sheet, not both" ) );

    // page numbers: the per-sheet series and the hierarchy order depend on them
    sch->SetSheetNumberAndCount();

    const SCH_SHEET_LIST         sheets = sch->Hierarchy();
    SCH_REFERENCE_LIST           references;
    SCH_MULTI_UNIT_REFERENCE_MAP locked;
    ANNOTATE_SCOPE_T             scope = ANNOTATE_ALL;

    if( !req.refs().empty() )
    {
        scope = ANNOTATE_SELECTION;

        for( const std::string& ref : req.refs() )
        {
            auto found = findSymbols( ref );

            if( found.empty() )
            {
                out.add_errors( "no part " + ref );
                continue;
            }

            for( auto& [symbol, sheet] : found )
            {
                if( symbol->IsPower() )
                {
                    out.add_errors( ref + " is a power symbol (numbered on its own, #PWR)" );
                    break;
                }

                sheet.AppendSymbol( references, symbol, SYMBOL_FILTER_NON_POWER );
                sheet.AppendMultiUnitSymbol( locked, symbol, SYMBOL_FILTER_NON_POWER );
            }
        }
    }
    else if( !req.sheet().empty() )
    {
        scope = ANNOTATE_CURRENT_SHEET;
        std::string err;
        std::optional<SCH_SHEET_PATH> top = findSheet( req.sheet(), err );

        if( !top )
            return tl::unexpected( annotateError( ApiStatusCode::AS_BAD_REQUEST, err ) );

        for( const SCH_SHEET_PATH& path : sheets )
        {
            if( path == *top || path.IsContainedWithin( *top ) )
            {
                path.GetSymbols( references, SYMBOL_FILTER_NON_POWER );
                path.GetMultiUnitSymbols( locked, SYMBOL_FILTER_NON_POWER );
            }
        }
    }
    else
    {
        sheets.GetSymbols( references, SYMBOL_FILTER_NON_POWER );
        sheets.GetMultiUnitSymbols( locked, SYMBOL_FILTER_NON_POWER );
    }

    out.set_checked( (int) references.GetCount() );

    if( references.GetCount() == 0 )
    {
        out.add_notes( "no parts to number" );
        return out;
    }

    if( auto collision = FindRefSuffixCollision( references, *sch ) )
    {
        const auto& [path, first] = *collision;
        return tl::unexpected( annotateError( ApiStatusCode::AS_BAD_REQUEST,
                                              "sheets " + utf8( first.PathHumanReadable( false, true ) ) + " and "
                                                      + utf8( path.PathHumanReadable( false, true ) )
                                                      + " use the same reference suffix: give each a distinct one first" ) );
    }

    // what each part was called (unnumbered ones as "R?")
    std::map<std::string, std::string> before;

    for( size_t i = 0; i < references.GetCount(); i++ )
    {
        SCH_REFERENCE& ref = references[i];
        before[instanceKey( ref.GetSymbol(), ref.GetSheetPath() )] = refOf( ref.GetSymbol(), ref.GetSheetPath() );
    }

    kicadd::KICADD_SCH_COMMIT commit( m_context->GetToolManager() );

    const std::set<const SCH_SCREEN*> shared =
            AnnotateReferences( references, std::move( locked ), *sch, scope, order, algo,
                                // KiCad takes "the first free number after": start 1 is after 0
                                req.start() > 0 ? req.start() - 1 : 0, req.reset(),
                                [&]( SCH_REFERENCE& aRef )
                                {
                                    commit.Modify( aRef.GetSymbol(), aRef.GetSheetPath().LastScreen() );
                                } );

    // the changes, in the order of the references (by page, then position)
    std::set<std::string> listed;

    for( size_t i = 0; i < references.GetCount(); i++ )
    {
        SCH_REFERENCE&        ref = references[i];
        SCH_SYMBOL*           symbol = ref.GetSymbol();
        const SCH_SHEET_PATH& sheet = ref.GetSheetPath();
        const std::string     key = instanceKey( symbol, sheet );
        const std::string     now = refOf( symbol, sheet );

        if( before[key] == now || !listed.insert( key ).second )
            continue;

        out.add_changes( before[key] + " -> " + now + " (" + utf8( symbol->GetValue( &sheet, FOR_GUI ) ) + ", "
                         + utf8( sheet.PathHumanReadable() ) + ")" );
    }

    for( const SCH_SCREEN* screen : shared )
        out.add_notes( "the references in " + utf8( screen->GetFileName() ) + " apply to every sheet that uses it" );

    // the whole schematic, afterwards
    SCH_REFERENCE_LIST all;
    sheets.GetSymbols( all, SYMBOL_FILTER_NON_POWER, true );

    all.CheckAnnotation(
            [&]( ERCE_T, const wxString& aMsg, SCH_REFERENCE*, SCH_REFERENCE* )
            {
                out.add_errors( utf8( aMsg ) );
            } );

    if( req.dry_run() || out.changes().empty() )
    {
        commit.Revert();
        sch->CurrentSheet().UpdateAllScreenReferences();

        if( out.changes().empty() )
            out.add_notes( req.reset() ? "every part already has the number it would get"
                                       : "every part in scope has a number already (reset=true numbers them again)" );

        return out;
    }

    out.set_history_label( "annotate " + std::to_string( out.changes_size() ) + " part(s)" );
    pushRecorded( commit, wxString::FromUTF8( out.history_label() ) );
    sch->CurrentSheet().UpdateAllScreenReferences();
    rebuildConnectivity();
    announce( wxS( "Agent: " ) + wxString::FromUTF8( out.history_label() ) );

    return out;
}
