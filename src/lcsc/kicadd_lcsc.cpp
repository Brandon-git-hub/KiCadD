/*
 * KiCadD M4a: LCSC part number -> KiCad footprint, on demand. See kicadd_lcsc.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "lcsc/kicadd_lcsc.h"
#include "common/kicadd_libtable.h"

#include <board.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_group.h>
#include <pcb_shape.h>
#include <zone.h>
#include <paths.h>
#include <pgm_base.h>
#include <project.h>
#include <project_pcb.h>
#include <footprint_library_adapter.h>
#include <kicad_curl/kicad_curl.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <lib_id.h>
#include <pcb_io/easyeda/pcb_io_easyeda_plugin.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <ki_exception.h>
#include <kiway.h>
#include <kiface_ids.h>
#include <kiface_services.h>

#include <json_common.h>   // not <nlohmann/json.hpp>: kicommon exports basic_json (LNK2005 without PCH)

#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <thread>
#include <regex>

using json = nlohmann::json;

namespace kicadd::lcsc
{

namespace
{

const char* API_URL = "https://easyeda.com/api/products/%s/components?version=6.4.19.5";
const char* USER_AGENT = "KiCadD/0.1 (+https://github.com/Brandon-git-hub/KiCadD)";


wxString cacheDir()
{
    wxString dir;

    if( !wxGetEnv( wxS( "KICADD_LCSC_CACHE" ), &dir ) || dir.IsEmpty() )
    {
        wxFileName fn( PATHS::GetUserCachePath(), wxEmptyString );
        fn.AppendDir( wxS( "kicadd" ) );
        fn.AppendDir( wxS( "lcsc" ) );
        dir = fn.GetPath();
    }

    if( !wxFileName::DirExists( dir ) )
        wxFileName::Mkdir( dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    return dir;
}


bool readFile( const wxString& aPath, std::string& aOut )
{
    wxFFile f( aPath, wxS( "rb" ) );

    if( !f.IsOpened() )
        return false;

    aOut.resize( static_cast<size_t>( f.Length() ) );
    return f.Read( aOut.data(), aOut.size() ) == aOut.size();
}


bool writeFile( const wxString& aPath, const std::string& aData )
{
    // write next to it and rename, so a reader never sees half a file
    wxString tmp = aPath + wxS( ".part" );

    {
        wxFFile f( tmp, wxS( "wb" ) );

        if( !f.IsOpened() || !f.Write( aData.data(), aData.size() ) )
            return false;
    }

    return wxRenameFile( tmp, aPath, true );
}


bool httpGet( const std::string& aUrl, std::string& aBody, std::string& aError )
{
    // easyeda.com's CDN starts refusing (HTTP 403) after a burst of requests: keep at least
    // MIN_GAP between them, so an agent fetching a whole BOM stays under it.
    using clock = std::chrono::steady_clock;
    constexpr auto            MIN_GAP = std::chrono::milliseconds( 1500 );
    static clock::time_point  last;

    if( auto wait = last + MIN_GAP - clock::now(); last != clock::time_point() && wait.count() > 0 )
        std::this_thread::sleep_for( wait );

    last = clock::now();

    KICAD_CURL_EASY curl;
    curl.SetUserAgent( USER_AGENT );
    curl.SetFollowRedirects( true );
    curl.SetConnectTimeout( 10 );
    curl.SetTimeout( 30 );
    curl.SetHeader( "Accept", "application/json" );

    if( !curl.SetURL( aUrl ) )
    {
        aError = "bad URL " + aUrl;
        return false;
    }

    if( int rc = curl.Perform(); rc != CURLE_OK )
    {
        aError = curl.GetErrorText( rc );
        return false;
    }

    if( int status = curl.GetResponseStatusCode(); status != 200 )
    {
        aError = "HTTP " + std::to_string( status );

        if( status == 403 || status == 429 )
            aError += " (easyeda.com refused the request; it rate-limits bursts of requests, try again "
                      "in a few minutes. Parts already fetched keep working from the cache)";

        return false;
    }

    aBody = curl.GetBuffer();
    return true;
}


std::string str( const json& aObj, const char* aKey )
{
    auto it = aObj.find( aKey );
    return it != aObj.end() && it->is_string() ? it->get<std::string>() : std::string();
}


/// Make the importer's output fit a KiCad library footprint:
///  * solid regions come in as footprint zones; on paste / mask (EasyEDA's own stencil and
///    mask openings) they become filled shapes, which every fab output plots
///  * the component-shape (99) layer becomes the F.Fab body outline, polarity (101) a filled
///    F.Fab mark; lead shapes (100) and unmapped layers go
///  * the courtyard covers pads and body; no STEP model is referenced (no OpenCASCADE here)
void tidy( FOOTPRINT& aFp )
{
    std::vector<ZONE*> zones( aFp.Zones().begin(), aFp.Zones().end() );

    for( ZONE* zone : zones )
    {
        PCB_LAYER_ID layer = zone->GetFirstLayer();
        bool         keep = layer == F_Paste || layer == B_Paste || layer == F_Mask || layer == B_Mask
                    || layer == User_3 || layer == User_5;

        if( keep && zone->Outline() && zone->Outline()->OutlineCount() > 0 )
        {
            PCB_SHAPE* shape = new PCB_SHAPE( &aFp, SHAPE_T::POLY );
            shape->SetPolyShape( *zone->Outline() );
            shape->SetLayer( layer == User_3 || layer == User_5 ? F_Fab : layer );
            shape->SetFilled( layer != User_3 );
            shape->SetWidth( layer == User_3 ? pcbIUScale.mmToIU( 0.1 ) : 0 );
            aFp.Add( shape, ADD_MODE::APPEND );
        }

        if( EDA_GROUP* group = zone->GetParentGroup() )
            group->RemoveItem( zone );

        aFp.Remove( zone );
        delete zone;
    }

    std::vector<BOARD_ITEM*> drop;

    for( BOARD_ITEM* item : aFp.GraphicalItems() )
    {
        PCB_LAYER_ID layer = item->GetLayer();

        if( layer == User_3 || layer == User_5 )
            item->SetLayer( F_Fab );
        else if( layer == User_4 || layer == User_1 || layer == F_CrtYd )
            drop.push_back( item );
    }

    for( BOARD_ITEM* item : drop )
    {
        if( EDA_GROUP* group = item->GetParentGroup() )
            group->RemoveItem( item );

        aFp.Remove( item );
        delete item;
    }

    // Groups the importer made for SVG nodes, now empty or not needed in a library part.
    std::vector<PCB_GROUP*> groups( aFp.Groups().begin(), aFp.Groups().end() );

    for( PCB_GROUP* group : groups )
    {
        for( EDA_ITEM* member : group->GetItems() )
            static_cast<BOARD_ITEM*>( member )->SetParentGroup( nullptr );

        group->RemoveAll();
        aFp.Remove( group );
        delete group;
    }

    // EasyEDA marks pads whose stencil opening is drawn separately (the paste regions above)
    // with a -100 mm paste margin; KiCad DRC calls that questionable. Same result, cleanly:
    // those pads have no paste layer, the paste shapes are the stencil.
    for( PAD* pad : aFp.Pads() )
    {
        std::optional<int> margin = pad->GetLocalSolderPasteMargin();

        if( margin && *margin < -pcbIUScale.mmToIU( 10 ) )
        {
            pad->SetLocalSolderPasteMargin( std::nullopt );
            LSET layers = pad->GetLayerSet();
            layers.reset( F_Paste );
            layers.reset( B_Paste );
            pad->SetLayerSet( layers );
        }
    }

    aFp.Models().clear();

    BOX2I box = aFp.GetLayerBoundingBox( LSET( { F_Cu, B_Cu, F_Paste, B_Paste, F_Mask, B_Mask, F_Fab } ) );

    for( PAD* pad : aFp.Pads() )
        box.Merge( pad->GetBoundingBox() );

    if( box.GetWidth() > 0 && box.GetHeight() > 0 )
    {
        const int grid = pcbIUScale.mmToIU( 0.01 );
        box.Inflate( pcbIUScale.mmToIU( 0.25 ) );

        auto down = [&]( int v ) { return static_cast<int>( std::floor( double( v ) / grid ) * grid ); };
        auto up = [&]( int v ) { return static_cast<int>( std::ceil( double( v ) / grid ) * grid ); };

        PCB_SHAPE* crtyd = new PCB_SHAPE( &aFp, SHAPE_T::RECTANGLE );
        crtyd->SetLayer( F_CrtYd );
        crtyd->SetWidth( pcbIUScale.mmToIU( 0.05 ) );
        crtyd->SetStart( VECTOR2I( down( box.GetLeft() ), down( box.GetTop() ) ) );
        crtyd->SetEnd( VECTOR2I( up( box.GetRight() ), up( box.GetBottom() ) ) );
        aFp.Add( crtyd, ADD_MODE::APPEND );
    }

    aFp.AutoPositionFields();
}


void setField( FOOTPRINT& aFp, const wxString& aName, const wxString& aValue )
{
    PCB_FIELD* field = aFp.GetField( aName );

    if( !field )
    {
        field = new PCB_FIELD( &aFp, FIELD_T::USER, aName );
        field->SetLayer( F_Fab );
        field->SetVisible( false );
        field->SetFPRelativePosition( VECTOR2I( 0, 0 ) );
        aFp.Add( field );
    }

    field->SetText( aValue );
}


bool convert( const json& aResult, const std::string& aLcsc, PART& aPart, std::string& aError )
{
    auto pkg = aResult.find( "packageDetail" );

    if( pkg == aResult.end() || !pkg->is_object() || !pkg->contains( "dataStr" ) )
    {
        aError = aLcsc + " has no footprint in the EasyEDA library";
        return false;
    }

    const json& dataStr = ( *pkg )["dataStr"];
    const json  head = dataStr.value( "head", json::object() );
    const json  cpara = head.value( "c_para", json::object() );

    aPart.title = str( aResult, "title" );
    aPart.package = str( cpara, "package" );

    if( aPart.package.empty() )
        aPart.package = str( *pkg, "title" );

    if( aPart.package.empty() )
    {
        aError = aLcsc + ": the footprint has no package name";
        return false;
    }

    aPart.datasheet = str( cpara, "link" );

    if( auto sym = aResult.find( "dataStr" ); sym != aResult.end() && sym->is_object() )
    {
        json symPara = sym->value( "head", json::object() ).value( "c_para", json::object() );

        if( aPart.datasheet.empty() )
            aPart.datasheet = str( symPara, "link" );
    }

    aPart.description = str( aResult, "description" );

    if( auto l = aResult.find( "lcsc" ); l != aResult.end() && l->is_object() )
    {
        if( auto v = l->find( "stock" ); v != l->end() && v->is_number() )
            aPart.stock = v->get<long long>();

        if( auto v = l->find( "price" ); v != l->end() && v->is_number() )
            aPart.priceUsd = v->get<double>();

        if( auto v = l->find( "min" ); v != l->end() && v->is_number() )
            aPart.minQty = v->get<int>();

        aPart.url = str( *l, "url" );
    }

    aPart.jlcSmt = aResult.value( "SMT", false ) && aResult.value( "jlcOnSale", 0 ) != 0;

    if( auto sym = aResult.find( "dataStr" ); sym != aResult.end() && sym->is_object() )
        aPart.jlcClass = str( sym->value( "head", json::object() ).value( "c_para", json::object() ),
                              "JLCPCB Part Class" );

    // The importer reads EasyEDA Standard documents from a file.
    wxFileName file( cacheDir(), wxString::FromUTF8( aLcsc ) + wxS( ".footprint" ), wxS( "json" ) );

    if( !writeFile( file.GetFullPath(), dataStr.dump() ) )
    {
        aError = "cannot write " + file.GetFullPath().ToStdString();
        return false;
    }

    try
    {
        PCB_IO_EASYEDA io;
        aPart.footprint = io.FootprintLoad( file.GetFullPath(), wxString::FromUTF8( aPart.package ) );
    }
    catch( const IO_ERROR& e )
    {
        aError = aLcsc + ": EasyEDA import failed: " + e.What().ToStdString();
        return false;
    }
    catch( const std::exception& e )
    {
        aError = aLcsc + ": EasyEDA import failed: " + e.what();
        return false;
    }

    if( !aPart.footprint )
    {
        aError = aLcsc + ": EasyEDA import produced no footprint";
        return false;
    }

    FOOTPRINT& fp = *aPart.footprint;
    tidy( fp );

    UTF8 name = LIB_ID::FixIllegalChars( UTF8( aPart.package ), false );
    aPart.package = name.c_str();
    fp.SetFPID( LIB_ID( wxString::FromUTF8( LIB_NICKNAME ), wxString::FromUTF8( aPart.package ) ) );

    wxString desc = wxString::FromUTF8( aPart.package );

    if( !aPart.title.empty() )
        desc << wxS( ", used by " ) << wxString::FromUTF8( aPart.title ) << wxS( " (LCSC " )
             << wxString::FromUTF8( aLcsc ) << wxS( ")" );

    if( !aPart.datasheet.empty() )
        desc << wxS( ", " ) << wxString::FromUTF8( aPart.datasheet );

    desc << wxS( ". " ) << wxString::FromUTF8( ATTRIBUTION );
    fp.SetLibDescription( desc );
    fp.SetKeywords( wxString::FromUTF8( aLcsc + " " + aPart.title + " LCSC EasyEDA" ) );
    fp.SetReference( wxS( "REF**" ) );
    fp.SetValue( wxString::FromUTF8( aPart.title.empty() ? aPart.package : aPart.title ) );

    if( !aPart.datasheet.empty() )
        fp.GetField( FIELD_T::DATASHEET )->SetText( wxString::FromUTF8( aPart.datasheet ) );

    setField( fp, wxS( "LCSC" ), wxString::FromUTF8( aLcsc ) );
    return true;
}


/// The folder of the board's project (or of the board file); empty when there is none.
wxString projectDir( BOARD* aBoard )
{
    wxString dir;

    if( PROJECT* prj = aBoard->GetProject(); prj && !prj->GetProjectPath().IsEmpty() )
        dir = prj->GetProjectPath();
    else if( !aBoard->GetFileName().IsEmpty() )
        dir = wxFileName( aBoard->GetFileName() ).GetPath();

    return wxFileName::DirExists( dir ) ? dir : wxString();
}


/// Make an open editor see what was just written: reload the LCSC library (both kinds of
/// libraries are cached by the library manager).
void refreshLibrary( bool aSymbols, std::string& aNote )
{
    LIBRARY_TABLE_TYPE type = aSymbols ? LIBRARY_TABLE_TYPE::SYMBOL : LIBRARY_TABLE_TYPE::FOOTPRINT;

    try
    {
        LIBRARY_MANAGER& mgr = Pgm().GetLibraryManager();
        mgr.ReloadLibraryEntry( type, wxString::FromUTF8( LIB_NICKNAME ), LIBRARY_TABLE_SCOPE::PROJECT );
        mgr.LoadLibraryEntry( type, wxString::FromUTF8( LIB_NICKNAME ) );
    }
    catch( ... )
    {
        aNote = "saved; reopen the project if the library browser doesn't show it yet";
    }
}


/**
 * Add the LCSC row to the project fp-lib-table (or sym-lib-table) once, then reload that table
 * and the library itself.
 * @return true when the row was added now.
 */
bool registerLibrary( const wxString& aDir, bool aSymbols, std::string& aNote )
{
    const char* uri = aSymbols ? "${KIPRJMOD}/lcsc.kicad_sym" : "${KIPRJMOD}/lcsc.pretty";

    switch( kicadd::AddProjectLibrary( aDir, aSymbols, LIB_NICKNAME, uri,
                                       std::string( "LCSC parts fetched by KiCadD; " ) + ATTRIBUTION, aNote ) )
    {
    case kicadd::LIBTABLE_RESULT::ADDED:
        refreshLibrary( aSymbols, aNote );
        return true;

    case kicadd::LIBTABLE_RESULT::PRESENT:
    case kicadd::LIBTABLE_RESULT::CONFLICT:     // the user's own LCSC row: theirs to keep
        refreshLibrary( aSymbols, aNote );
        return false;

    default:
        return false;
    }
}

} // namespace


std::string Normalize( const std::string& aLcsc )
{
    std::string s;

    for( char c : aLcsc )
    {
        if( !std::isspace( static_cast<unsigned char>( c ) ) )
            s += static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) );
    }

    if( !s.empty() && s[0] != 'C' )
        s = "C" + s;

    static const std::regex re( "^C[0-9]{1,12}$" );
    return std::regex_match( s, re ) ? s : std::string();
}


bool FetchPart( const std::string& aLcsc, bool aRefresh, PART& aPart, std::string& aError )
{
    aPart = PART();
    aPart.lcsc = Normalize( aLcsc );

    if( aPart.lcsc.empty() )
    {
        aError = "not an LCSC part number: \"" + aLcsc + "\" (expected e.g. C8734)";
        return false;
    }

    wxFileName cached( cacheDir(), wxString::FromUTF8( aPart.lcsc ), wxS( "json" ) );
    bool       offline = wxGetEnv( wxS( "KICADD_LCSC_OFFLINE" ), nullptr );
    std::string body;

    if( cached.FileExists() && ( !aRefresh || offline ) && readFile( cached.GetFullPath(), body ) )
    {
        aPart.fromCache = true;
    }
    else if( offline )
    {
        aError = aPart.lcsc + " is not in the cache (" + cached.GetPath().ToStdString()
                 + ") and KICADD_LCSC_OFFLINE is set";
        return false;
    }
    else
    {
        char url[256];
        std::snprintf( url, sizeof( url ), API_URL, aPart.lcsc.c_str() );
        std::string netError;

        if( httpGet( url, body, netError ) )
        {
            // only keep answers that are a part; "not found" is not worth caching
            json probe = json::parse( body, nullptr, false );

            if( !probe.is_discarded() && probe.value( "success", false ) && probe.contains( "result" )
                && probe["result"].is_object() )
            {
                writeFile( cached.GetFullPath(), body );
            }
        }
        else if( cached.FileExists() && readFile( cached.GetFullPath(), body ) )
        {
            aPart.fromCache = true;
            aPart.note = "network failed (" + netError + "), used the cached copy from "
                         + cached.GetModificationTime().FormatISODate().ToStdString();
        }
        else
        {
            aError = "fetching " + aPart.lcsc + " from easyeda.com failed: " + netError;
            return false;
        }
    }

    aPart.asOf = ( aPart.fromCache && cached.FileExists() ? cached.GetModificationTime() : wxDateTime::Now() )
                         .FormatISODate()
                         .ToStdString();

    json doc = json::parse( body, nullptr, false );

    if( doc.is_discarded() )
    {
        aError = aPart.lcsc + ": the EasyEDA answer is not JSON";
        return false;
    }

    if( !doc.value( "success", false ) || !doc.contains( "result" ) || !doc["result"].is_object() )
    {
        std::string msg = str( doc, "message" );
        aError = aPart.lcsc + " not found in the EasyEDA/LCSC library" + ( msg.empty() ? "" : ": " + msg );
        return false;
    }

    try
    {
        return convert( doc["result"], aPart.lcsc, aPart, aError );
    }
    catch( const json::exception& e )
    {
        aError = aPart.lcsc + ": unexpected EasyEDA data: " + e.what();
        return false;
    }
}


bool SaveToProjectLibrary( BOARD* aBoard, const PART& aPart, SAVED& aSaved, std::string& aError )
{
    if( !aBoard )
    {
        aError = "nothing to save";
        return false;
    }

    wxString dir = projectDir( aBoard );

    if( dir.IsEmpty() )
    {
        aError = "the board is not saved in a folder yet, so there is no project library";
        return false;
    }

    return SaveToProjectLibrary( dir, aPart, aSaved, aError );
}


bool SaveToProjectLibrary( const wxString& dir, const PART& aPart, SAVED& aSaved, std::string& aError )
{
    aSaved = SAVED();

    if( !aPart.footprint )
    {
        aError = "nothing to save";
        return false;
    }

    wxFileName lib( dir, wxEmptyString );
    lib.AppendDir( wxS( "lcsc.pretty" ) );

    if( !lib.DirExists() && !wxFileName::Mkdir( lib.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        aError = "cannot create " + lib.GetPath().ToStdString();
        return false;
    }

    try
    {
        PCB_IO_KICAD_SEXPR io;
        io.FootprintSave( lib.GetPath(), aPart.footprint.get() );
    }
    catch( const IO_ERROR& e )
    {
        aError = "saving to " + lib.GetPath().ToStdString() + " failed: " + e.What().ToStdString();
        return false;
    }

    aSaved.libId = std::string( LIB_NICKNAME ) + ":" + aPart.package;
    aSaved.file = wxFileName( lib.GetPath(), wxString::FromUTF8( aPart.package ), wxS( "kicad_mod" ) )
                          .GetFullPath()
                          .ToStdString();

    aSaved.registered = registerLibrary( dir, false, aSaved.note );
    return true;
}


bool SaveSymbolToProjectLibrary( BOARD* aBoard, KIWAY* aKiway, const PART& aPart,
                                 SAVED_SYMBOL& aSaved, std::string& aError )
{
    wxString dir = aBoard ? projectDir( aBoard ) : wxString();

    if( dir.IsEmpty() )
    {
        aError = "the board is not saved in a folder yet, so there is no project library";
        return false;
    }

    return SaveSymbolToProjectLibrary( dir, aKiway, aPart, aSaved, aError );
}


bool SaveSymbolToProjectLibrary( const wxString& dir, KIWAY* aKiway, const PART& aPart,
                                 SAVED_SYMBOL& aSaved, std::string& aError )
{
    aSaved = SAVED_SYMBOL();

    KIFACE_KICADD_LCSC_SYMBOL_SERVICE::Type service = nullptr;

    try
    {
        // loads _eeschema.kiface on first use; throws when it can't
        if( aKiway )
            service = GetKifaceService<KIFACE_KICADD_LCSC_SYMBOL_SERVICE>( aKiway->KiFACE( KIWAY::FACE_SCH ) );
    }
    catch( const IO_ERROR& e )
    {
        aError = "the schematic module (eeschema) could not be loaded: " + e.What().ToStdString( wxConvUTF8 );
        return false;
    }

    if( !service )
    {
        aError = "the schematic module (eeschema) is not available";
        return false;
    }

    wxFileName source( cacheDir(), wxString::FromUTF8( aPart.lcsc ), wxS( "json" ) );
    wxFileName lib( dir, wxS( "lcsc" ), wxS( "kicad_sym" ) );

    std::string desc = aPart.title + " (LCSC " + aPart.lcsc + ")";

    if( !aPart.description.empty() )
        desc += ", " + aPart.description;

    if( !aPart.datasheet.empty() )
        desc += ", " + aPart.datasheet;

    desc += ". " + std::string( ATTRIBUTION );

    json req = { { "source", source.GetFullPath().ToStdString( wxConvUTF8 ) },
                 { "library", lib.GetFullPath().ToStdString( wxConvUTF8 ) },
                 { "lcsc", aPart.lcsc },
                 { "footprint", std::string( LIB_NICKNAME ) + ":" + aPart.package },
                 { "datasheet", aPart.datasheet },
                 { "description", desc },
                 { "keywords", aPart.lcsc + " " + aPart.title + " LCSC EasyEDA" },
                 { "attribution", ATTRIBUTION } };

    json ans = json::parse( ( *service )( wxString::FromUTF8( req.dump() ) ).ToStdString( wxConvUTF8 ),
                            nullptr, false );

    if( ans.is_discarded() || !ans.value( "ok", false ) )
    {
        aError = ans.is_discarded() ? "no answer from the symbol converter" : str( ans, "error" );
        return false;
    }

    aSaved.libId = std::string( LIB_NICKNAME ) + ":" + str( ans, "name" );
    aSaved.file = lib.GetFullPath().ToStdString( wxConvUTF8 );
    aSaved.reference = str( ans, "reference" );
    aSaved.units = ans.value( "units", 1 );

    for( const json& p : ans.value( "pins", json::array() ) )
        aSaved.pins.push_back( p.get<std::string>() );

    aSaved.registered = registerLibrary( dir, true, aSaved.note );
    return true;
}

bool FetchAndSave( const wxString& aProjectDir, KIWAY* aKiway, const std::string& aLcsc, bool aRefresh,
                   FETCHED& aOut, std::string& aError )
{
    aOut = FETCHED();

    if( !FetchPart( aLcsc, aRefresh, aOut.part, aError ) )
        return false;

    auto addNote = [&]( const std::string& aText )
    {
        if( !aText.empty() )
            aOut.note += ( aOut.note.empty() ? "" : "; " ) + aText;
    };

    addNote( aOut.part.note );

    if( aProjectDir.IsEmpty() || !wxFileName::DirExists( aProjectDir ) )
    {
        addNote( "not saved: the project is not saved in a folder yet" );
        return true;
    }

    std::string err;
    aOut.footprintSaved = SaveToProjectLibrary( aProjectDir, aOut.part, aOut.footprint, err );
    addNote( aOut.footprintSaved ? aOut.footprint.note : "footprint not saved: " + err );

    err.clear();
    aOut.symbolSaved = SaveSymbolToProjectLibrary( aProjectDir, aKiway, aOut.part, aOut.symbol, err );
    addNote( aOut.symbolSaved ? aOut.symbol.note : "symbol not saved: " + err );
    return true;
}


std::string Summary( const PART& aPart )
{
    std::string s = aPart.lcsc + " " + aPart.title + ":";

    if( aPart.stock >= 0 )
        s += " LCSC stock " + std::to_string( aPart.stock ) + ",";

    if( aPart.priceUsd >= 0 )
    {
        char price[48];
        std::snprintf( price, sizeof( price ), " US$%.4g,", aPart.priceUsd );
        s += price;
    }

    s += aPart.jlcSmt ? " JLC assembly" + ( aPart.jlcClass.empty() ? "" : " (" + aPart.jlcClass + ")" )
                      : std::string( " no JLC assembly" );
    s += " -- as of " + aPart.asOf;
    return s;
}

} // namespace kicadd::lcsc


wxString KicaddLcscFetchService( const wxString& aRequest, KIWAY* aKiway )
{
    using namespace kicadd::lcsc;

    json req = json::parse( aRequest.ToStdString( wxConvUTF8 ), nullptr, false );
    json out;

    if( req.is_discarded() || !req.is_object() )
        return wxS( "{\"ok\":false,\"error\":\"bad request\"}" );

    FETCHED     got;
    std::string err;

    if( !FetchAndSave( wxString::FromUTF8( str( req, "project_dir" ) ), aKiway, str( req, "lcsc" ),
                       req.value( "refresh", false ), got, err ) )
    {
        out = { { "ok", false }, { "error", err } };
    }
    else
    {
        out = { { "ok", true },
                { "lcsc", got.part.lcsc },
                { "title", got.part.title },
                { "summary", Summary( got.part ) },
                { "footprint", got.footprintSaved ? got.footprint.libId : std::string() },
                { "symbol", got.symbolSaved ? got.symbol.libId : std::string() },
                { "note", got.note } };
    }

    return wxString::FromUTF8( out.dump() );
}
