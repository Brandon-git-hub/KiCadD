/*
 * KiCadD: project library tables. See kicadd_libtable.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "common/kicadd_libtable.h"

#include <libraries/library_manager.h>
#include <pgm_base.h>

#include <wx/ffile.h>
#include <wx/filename.h>

#include <regex>

namespace kicadd
{

namespace
{

bool readText( const wxString& aPath, std::string& aOut )
{
    wxFFile f( aPath, wxS( "rb" ) );

    if( !f.IsOpened() )
        return false;

    aOut.resize( f.Length() );
    return f.Read( aOut.data(), aOut.size() ) == aOut.size();
}


bool writeText( const wxString& aPath, const std::string& aData )
{
    wxFFile f( aPath, wxS( "wb" ) );
    return f.IsOpened() && f.Write( aData.data(), aData.size() ) == aData.size() && f.Close();
}


std::string quoted( const std::string& aText )
{
    std::string s = "\"";

    for( char c : aText )
    {
        if( c == '"' || c == '\\' )
            s += '\\';

        s += c;
    }

    return s + "\"";
}

} // namespace


LIBTABLE_RESULT AddProjectLibrary( const wxString& aProjectDir, bool aSymbols, const std::string& aNickname,
                                   const std::string& aUri, const std::string& aDescr, std::string& aNote,
                                   std::string* aExistingUri )
{
    const char* tableName = aSymbols ? "sym-lib-table" : "fp-lib-table";
    const char* root = aSymbols ? "sym_lib_table" : "fp_lib_table";

    wxFileName  table( aProjectDir, wxString::FromUTF8( tableName ) );
    std::string text;
    bool        exists = table.FileExists();

    if( exists && !readText( table.GetFullPath(), text ) )
    {
        aNote = "cannot read " + table.GetFullPath().ToStdString();
        return LIBTABLE_RESULT::FAILED;
    }

    // (lib (name "X")(type "KiCad")(uri "...") ...): names and uris quoted or bare
    static const std::regex row( R"re(\(lib\s*\(name\s+"?([^"\)]*)"?\)[^\n]*?\(uri\s+"?([^"\)]*)"?\))re" );

    for( std::sregex_iterator it( text.begin(), text.end(), row ), end; it != end; ++it )
    {
        if( ( *it )[1].str() != aNickname )
            continue;

        if( aExistingUri )
            *aExistingUri = ( *it )[2].str();

        return ( *it )[2].str() == aUri ? LIBTABLE_RESULT::PRESENT : LIBTABLE_RESULT::CONFLICT;
    }

    const std::string line = "  (lib (name " + quoted( aNickname ) + ")(type \"KiCad\")(uri " + quoted( aUri )
                             + ")(options \"\")(descr " + quoted( aDescr ) + "))\n";

    if( !exists )
    {
        text = "(" + std::string( root ) + "\n  (version 7)\n" + line + ")\n";
    }
    else
    {
        size_t close = text.find_last_of( ')' );

        if( close == std::string::npos )
        {
            aNote = std::string( tableName ) + " not understood; add " + aNickname + " by hand";
            return LIBTABLE_RESULT::FAILED;
        }

        if( close > 0 && text[close - 1] != '\n' )
            text.insert( close, "\n" );

        text.insert( text.find_last_of( ')' ), line );
    }

    if( !writeText( table.GetFullPath(), text ) )
    {
        aNote = "cannot write " + table.GetFullPath().ToStdString();
        return LIBTABLE_RESULT::FAILED;
    }

    try
    {
        Pgm().GetLibraryManager().ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT,
                                                { aSymbols ? LIBRARY_TABLE_TYPE::SYMBOL
                                                           : LIBRARY_TABLE_TYPE::FOOTPRINT } );
    }
    catch( ... )
    {
        aNote = std::string( "added to " ) + tableName + "; reopen the project to see it in the library browser";
    }

    return LIBTABLE_RESULT::ADDED;
}


bool ReloadProjectLibrary( bool aSymbols, const std::string& aNickname )
{
    LIBRARY_TABLE_TYPE type = aSymbols ? LIBRARY_TABLE_TYPE::SYMBOL : LIBRARY_TABLE_TYPE::FOOTPRINT;

    try
    {
        LIBRARY_MANAGER& mgr = Pgm().GetLibraryManager();
        mgr.ReloadLibraryEntry( type, wxString::FromUTF8( aNickname ), LIBRARY_TABLE_SCOPE::PROJECT );
        mgr.LoadLibraryEntry( type, wxString::FromUTF8( aNickname ) );
        return true;
    }
    catch( ... )
    {
        return false;
    }
}

} // namespace kicadd
