/*
 * KiCadD MCP - new_project (roadmap phase 4, item 3): a new KiCad project on the server, opened.
 *
 * Writes <name>.kicad_pro, an empty <name>.kicad_pcb and <name>.kicad_sch (never over a file
 * that is there), opens them as open_board does (refused while the open board has unsaved
 * changes, unless discard), sets the layers / stackup (board_setup) and the outline
 * (board_outline) when asked, and saves, so the files on disk are KiCad's own from then on.
 * Only the server's own files: nothing is started (the KiCad programs are not launched here).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace kicadd::mcp
{

namespace
{

std::string uuid4()
{
    static std::mt19937_64 rng( (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count() ^ (uint64_t) std::random_device{}() );
    uint64_t               a = rng(), b = rng();
    a = ( a & 0xFFFFFFFFFFFF0FFFULL ) | 0x0000000000004000ULL;
    b = ( b & 0x3FFFFFFFFFFFFFFFULL ) | 0x8000000000000000ULL;
    char buf[40];
    std::snprintf( buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", (unsigned) ( a >> 32 ), (unsigned) ( ( a >> 16 ) & 0xFFFF ),
                   (unsigned) ( a & 0xFFFF ), (unsigned) ( b >> 48 ), (unsigned long long) ( b & 0xFFFFFFFFFFFFULL ) );
    return buf;
}


fs::path toPath( const std::string& aUtf8 )
{
#ifdef _WIN32
    return fs::path( std::u8string( aUtf8.begin(), aUtf8.end() ) );
#else
    return fs::path( aUtf8 );
#endif
}


std::string fromPath( const fs::path& aPath )
{
    std::u8string s = aPath.u8string();
    return std::string( s.begin(), s.end() );
}


bool goodName( const std::string& aName )
{
    if( aName.empty() || aName.size() > 100 || aName == "." || aName == ".." )
        return false;

    for( char c : aName )
    {
        if( c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char) c < 32 )
            return false;
    }

    return true;
}


TOOL_RESULT newProject( std::shared_ptr<std::vector<TOOL>> aTable, const json& a )
{
    auto fail = []( const std::string& m ) { return TOOL_RESULT{ m, true }; };

    auto call = [&]( const std::string& aTool, const json& aArgs ) -> TOOL_RESULT
    {
        for( const TOOL& t : *aTable )
        {
            if( t.name == aTool )
                return t.run( aArgs );
        }

        return { aTool + ": not available", true };
    };

    if( !a.contains( "path" ) || !a["path"].is_string() )
        return fail( "path: the project's folder on the server (absolute), or its .kicad_pro" );

    fs::path    path = toPath( a["path"].get<std::string>() );
    std::string name = a.value( "name", std::string() );

    if( !path.is_absolute() )
        return fail( "path must be absolute (on the server): " + fromPath( path ) );

    if( path.extension() == ".kicad_pro" )
    {
        if( name.empty() )
            name = fromPath( path.stem() );

        path = path.parent_path();
    }
    else if( path.has_extension() && !path.extension().empty() && path.extension() != "." )
    {
        std::error_code ec;

        if( !fs::is_directory( path, ec ) )
            return fail( "path: a folder or a .kicad_pro, not " + fromPath( path.filename() ) );
    }

    if( name.empty() )
        name = fromPath( path.filename().empty() ? path.parent_path().filename() : path.filename() );

    if( !goodName( name ) )
        return fail( "name: a file name without / \\ : * ? \" < > | (" + name + ")" );

    std::error_code ec;

    if( fs::exists( path, ec ) && !fs::is_directory( path, ec ) )
        return fail( fromPath( path ) + " is a file, not a folder" );

    const fs::path pro = path / ( name + ".kicad_pro" );
    const fs::path pcb = path / ( name + ".kicad_pcb" );
    const fs::path sch = path / ( name + ".kicad_sch" );

    for( const fs::path& f : { pro, pcb, sch } )
    {
        if( fs::exists( f, ec ) )
            return fail( fromPath( f ) + " is there already: new_project writes over nothing (open_board opens it)" );
    }

    // the layers asked for, checked before anything is written
    const int layers = a.value( "copper_layers", 2 );

    if( layers < 2 || layers > 32 || layers % 2 )
        return fail( "copper_layers: 2, 4, 6 ... 32" );

    std::vector<double> outline;

    if( a.contains( "outline" ) )
    {
        if( !a["outline"].is_array() || a["outline"].size() != 2 || !a["outline"][0].is_number() || !a["outline"][1].is_number() )
            return fail( "outline: [width, height] mm (a rectangle; board_outline does any other)" );

        outline = { a["outline"][0].get<double>(), a["outline"][1].get<double>() };

        if( outline[0] <= 0 || outline[1] <= 0 || outline[0] > 1000 || outline[1] > 1000 )
            return fail( "outline: [width, height] mm, 0 < each <= 1000" );
    }

    const bool madeDir = !fs::exists( path, ec );
    fs::create_directories( path, ec );

    if( ec )
        return fail( "cannot make " + fromPath( path ) + ": " + ec.message() );

    // the files: the bare minimum KiCad reads; saved by KiCad right after
    std::vector<fs::path> written;

    auto write = [&]( const fs::path& aFile, const std::string& aText ) -> bool
    {
        std::ofstream f( aFile, std::ios::binary | std::ios::trunc );
        f << aText;
        f.close();

        if( !f )
            return false;

        written.push_back( aFile );
        return true;
    };

    auto undo = [&]()
    {
        std::error_code e2;

        for( const fs::path& f : written )
            fs::remove( f, e2 );

        for( const char* side : { ".kicad_prl", "-backups" } )
        {
            fs::path extra = path / ( name + side );

            if( fs::exists( extra, e2 ) && !fs::is_directory( extra, e2 ) )
                fs::remove( extra, e2 );
        }

        if( madeDir && fs::is_empty( path, e2 ) )
            fs::remove( path, e2 );
    };

    const std::string fileName = fromPath( pro.filename() );
    json proJson = { { "meta", { { "filename", fileName }, { "version", 3 } } } };

    if( !write( pro, proJson.dump( 2 ) + "\n" )
        || !write( pcb, "(kicad_pcb (version 20241229) (generator \"kicadd\") (generator_version \"10.99\") (paper \"A4\"))\n" )
        || !write( sch, "(kicad_sch (version 20231120) (generator \"kicadd\") (uuid \"" + uuid4() + "\") (paper \"A4\") (lib_symbols) "
                        "(sheet_instances (path \"/\" (page \"1\"))))\n" ) )
    {
        undo();
        return fail( "cannot write the project's files in " + fromPath( path ) );
    }

    TOOL_RESULT opened = call( "open_board", { { "path", fromPath( pcb ) }, { "discard", a.value( "discard", false ) } } );

    if( opened.isError )
    {
        undo();
        return fail( "not opened, nothing left on disk: " + opened.text );
    }

    std::string out = "new project " + fromPath( pro ) + ": .kicad_pro, .kicad_pcb, .kicad_sch written and opened";
    std::string notes;

    // the layers and stackup: a stackup always (impedance, IR drop and the PDN read it)
    json setup = { { "copper_layers", layers }, { "thickness", a.value( "thickness", 1.6 ) } };

    if( a.contains( "stackup" ) && a["stackup"].is_string() )
        setup = { { "stackup", a["stackup"] } };

    TOOL_RESULT s = call( "board_setup", setup );
    notes += ( s.isError ? "\nboard_setup failed: " : "\n" ) + s.text.substr( 0, s.text.find( '\n' ) );

    if( !outline.empty() )
    {
        // at (100, 100) like KiCad's own new boards sit on the sheet
        TOOL_RESULT o = call( "board_outline", { { "rect", { 100.0, 100.0, 100.0 + outline[0], 100.0 + outline[1] } },
                                                 { "corner_radius", a.value( "corner_radius", 0.0 ) } } );
        std::string first = o.text.substr( 0, o.text.find( '\n' ) );
        notes += ( o.isError ? "\nboard_outline failed: " : "\noutline: " ) + first;
    }

    TOOL_RESULT saved = call( "save_board", json::object() );
    TOOL_RESULT schSaved = call( "sch_save", json::object() );

    if( saved.isError || schSaved.isError )
        notes += "\nnote: not saved yet (" + ( saved.isError ? saved.text : schSaved.text ) + "): save_board / sch_save";

    while( !notes.empty() && notes.back() == '\n' )
        notes.pop_back();

    // what is open now (opened.text is from before the setup)
    TOOL_RESULT summary = call( "board_summary", json::object() );
    return { out + notes + "\n" + ( summary.isError ? opened.text : summary.text ), false };
}

} // namespace


std::string NewProjectDescription()
{
    return "Make a new KiCad project on the server and open it (replacing the open project, as open_board): path (the "
           "project's folder, absolute; made when missing) or its .kicad_pro, name (default the folder's name). Writes "
           "<name>.kicad_pro, an empty .kicad_pcb and .kicad_sch, refused when any of them is there (never written "
           "over). copper_layers (default 2), thickness (mm, 1.6) or stackup (a JLCPCB one, as board_setup), outline "
           "[width, height] mm (a rectangle at (100, 100), corner_radius), then saved by KiCad. Refused while the open "
           "board has unsaved changes, unless discard=true. Not in the desktop editor (the user makes projects there).";
}


TOOL NewProjectTool( std::shared_ptr<std::vector<TOOL>> aTable )
{
    json schema = { { "type", "object" },
                    { "properties",
                      { { "path", { { "type", "string" }, { "description", "Absolute: the folder, or the .kicad_pro" } } },
                        { "name", { { "type", "string" } } },
                        { "copper_layers", { { "type", "integer" } } },
                        { "thickness", { { "type", "number" } } },
                        { "stackup", { { "type", "string" } } },
                        { "outline", { { "type", "array" }, { "items", { { "type", "number" } } }, { "description", "[width, height] mm" } } },
                        { "corner_radius", { { "type", "number" } } },
                        { "discard", { { "type", "boolean" } } } } },
                    { "required", { "path" } } };

    return { "new_project", NewProjectDescription(), schema, [aTable]( const json& a ) { return newProject( aTable, a ); } };
}

} // namespace kicadd::mcp
