/*
 * KiCadD: parts search by specification. See kicadd_parts.h.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "lcsc/kicadd_parts.h"
#include "lcsc/kicadd_lcsc.h"

#include <footprint.h>     // PART owns one (MSVC wants the type complete for its destructor)

#include <kicad_curl/kicad_curl.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <paths.h>

#include <wx/filename.h>
#include <wx/utils.h>

#include <sqlite3.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace kicadd::lcsc
{

namespace
{

const char* DEFAULT_URL = "https://github.com/Brandon-git-hub/KiCadD/releases/download/parts-index/parts.db.gz";
const char* AGENT = "KiCadD/0.1 (+https://github.com/Brandon-git-hub/KiCadD)";
const int   MAX_AGE_DAYS = 7;

std::mutex g_lock;     // one download at a time

std::string env( const char* aName )
{
    wxString v;
    return wxGetEnv( wxString::FromUTF8( aName ), &v ) ? v.ToStdString( wxConvUTF8 ) : std::string();
}

wxString indexPath()
{
    wxFileName fn( PATHS::GetUserCachePath(), wxS( "parts.db" ) );
    fn.AppendDir( wxS( "kicadd" ) );

    if( !wxFileName::DirExists( fn.GetPath() ) )
        wxFileName::Mkdir( fn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    return fn.GetFullPath();
}

/// gzip in memory -> file
bool gunzip( const std::string& aData, const wxString& aPath, std::string& aError )
{
    std::ofstream out( aPath.fn_str(), std::ios::binary | std::ios::trunc );

    if( !out )
    {
        aError = "cannot write " + aPath.ToStdString( wxConvUTF8 );
        return false;
    }

    z_stream z{};

    if( inflateInit2( &z, 16 + MAX_WBITS ) != Z_OK )
    {
        aError = "zlib";
        return false;
    }

    z.next_in = reinterpret_cast<Bytef*>( const_cast<char*>( aData.data() ) );
    z.avail_in = (uInt) aData.size();
    std::vector<char> buf( 1 << 20 );
    int               rc = Z_OK;

    while( rc != Z_STREAM_END )
    {
        z.next_out = reinterpret_cast<Bytef*>( buf.data() );
        z.avail_out = (uInt) buf.size();
        rc = inflate( &z, Z_NO_FLUSH );

        if( rc != Z_OK && rc != Z_STREAM_END )
        {
            inflateEnd( &z );
            aError = "the parts index download is damaged (" + std::to_string( rc ) + ")";
            return false;
        }

        out.write( buf.data(), buf.size() - z.avail_out );
    }

    inflateEnd( &z );
    return (bool) out;
}

bool download( const wxString& aPath, std::string& aError )
{
    std::string url = env( "KICADD_PARTS_URL" );

    if( url.empty() )
        url = DEFAULT_URL;

    KICAD_CURL_EASY curl;
    curl.SetUserAgent( AGENT );
    curl.SetFollowRedirects( true );
    curl.SetConnectTimeout( 20 );
    curl.SetTimeout( 600 );

    if( !curl.SetURL( url ) )
    {
        aError = "bad parts index URL " + url;
        return false;
    }

    if( int rc = curl.Perform(); rc != CURLE_OK )
    {
        aError = "downloading the parts index failed: " + curl.GetErrorText( rc );
        return false;
    }

    if( int status = curl.GetResponseStatusCode(); status != 200 )
    {
        aError = "downloading the parts index: HTTP " + std::to_string( status ) + " from " + url;
        return false;
    }

    wxString tmp = aPath + wxS( ".part" );

    if( !gunzip( curl.GetBuffer(), tmp, aError ) )
        return false;

    wxRemoveFile( aPath );
    return wxRenameFile( tmp, aPath );
}

struct DB
{
    sqlite3* db = nullptr;
    ~DB() { if( db ) sqlite3_close( db ); }
};

struct STMT
{
    sqlite3_stmt* st = nullptr;
    ~STMT() { if( st ) sqlite3_finalize( st ); }
};

std::string text( sqlite3_stmt* aSt, int aCol )
{
    const unsigned char* t = sqlite3_column_text( aSt, aCol );
    return t ? reinterpret_cast<const char*>( t ) : std::string();
}

std::string meta( sqlite3* aDb, const char* aKey )
{
    STMT s;

    if( sqlite3_prepare_v2( aDb, "SELECT value FROM meta WHERE key = ?", -1, &s.st, nullptr ) != SQLITE_OK )
        return {};

    sqlite3_bind_text( s.st, 1, aKey, -1, SQLITE_TRANSIENT );
    return sqlite3_step( s.st ) == SQLITE_ROW ? text( s.st, 0 ) : std::string();
}

/// days since "2026-11-11", large when unknown
int ageDays( const std::string& aDate )
{
    int y, m, d;

    if( std::sscanf( aDate.c_str(), "%d-%d-%d", &y, &m, &d ) != 3 )
        return 9999;

    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    std::time_t then = std::mktime( &t );
    return (int) ( std::difftime( std::time( nullptr ), then ) / 86400 );
}

std::string lower( std::string s )
{
    std::transform( s.begin(), s.end(), s.begin(), []( unsigned char c ) { return (char) std::tolower( c ); } );
    return s;
}

} // namespace


bool ParseValue( const std::string& aText, double& aValue, std::string& aUnit )
{
    std::string s;

    for( char c : aText )
    {
        if( !std::isspace( static_cast<unsigned char>( c ) ) )
            s += c;
    }

    // unit at the end
    std::string unit;
    auto        ends = [&]( const char* suffix )
    {
        std::string x = suffix;
        return s.size() >= x.size() && lower( s.substr( s.size() - x.size() ) ) == lower( x );
    };

    if( ends( "ohm" ) ) { unit = "Ω"; s.resize( s.size() - 3 ); }
    else if( s.size() >= 2 && s.compare( s.size() - 2, 2, "Ω" ) == 0 ) { unit = "Ω"; s.resize( s.size() - 2 ); }
    else if( ends( "F" ) ) { unit = "F"; s.pop_back(); }
    else if( ends( "H" ) ) { unit = "H"; s.pop_back(); }

    if( s.empty() )
        return false;

    // a multiplier letter, at the end or as the decimal point ("4k7", "2R2", "4n7")
    static const std::map<std::string, double> mult = { { "p", 1e-12 }, { "n", 1e-9 }, { "u", 1e-6 },
                                                         { "µ", 1e-6 },  { "m", 1e-3 }, { "R", 1.0 },
                                                         { "r", 1.0 },   { "k", 1e3 },  { "K", 1e3 },
                                                         { "M", 1e6 },   { "G", 1e9 } };
    double      factor = 1.0;
    std::string number = s;

    for( const auto& [key, f] : mult )
    {
        size_t at = s.find( key );

        if( at == std::string::npos || at == 0 )
            continue;

        number = s.substr( 0, at ) + ( at + key.size() < s.size() ? "." + s.substr( at + key.size() ) : "" );
        factor = f;

        if( key == "R" || key == "r" )
            unit = "Ω";
        else if( unit.empty() && ( key == "k" || key == "K" || key == "M" || key == "G" ) )
            unit = "Ω";     // 10k, 1M: resistors

        break;
    }

    char*  end = nullptr;
    double v = std::strtod( number.c_str(), &end );

    if( !end || *end != '\0' )
        return false;

    aValue = v * factor;

    if( !unit.empty() )
        aUnit = unit;

    return true;
}


bool SearchParts( const kicadd::board::LcscSearch& aReq, kicadd::board::LcscSearchResponse& aOut, std::string& aError )
{
    std::lock_guard<std::mutex> lock( g_lock );

    // --- the index
    wxString path = wxString::FromUTF8( env( "KICADD_PARTS_DB" ) );
    bool     fixed = !path.IsEmpty();

    if( !fixed )
        path = indexPath();

    std::string note;

    auto open = [&]( DB& aDb ) -> bool
    {
        return sqlite3_open_v2( path.ToStdString( wxConvUTF8 ).c_str(), &aDb.db, SQLITE_OPEN_READONLY, nullptr ) == SQLITE_OK
               && !meta( aDb.db, "date" ).empty();
    };

    std::unique_ptr<DB> db = std::make_unique<DB>();
    bool                have = wxFileName::FileExists( path ) && open( *db );

    if( !fixed && ( !have || aReq.update() || ageDays( meta( db->db, "date" ) ) > MAX_AGE_DAYS ) )
    {
        db.reset();
        std::string err;

        if( download( path, err ) )
        {
            note = have ? "parts index updated" : "parts index downloaded";
        }
        else if( have )
        {
            note = "could not update the parts index (" + err + "); using the old one";
        }
        else
        {
            aError = err + " (the parts index is needed once, about 45 MB)";
            return false;
        }

        db = std::make_unique<DB>();

        if( !open( *db ) )
        {
            aError = "the parts index could not be opened";
            return false;
        }
    }
    else if( !have )
    {
        aError = "no parts index at " + path.ToStdString( wxConvUTF8 );
        return false;
    }

    aOut.set_index_date( meta( db->db, "date" ) );

    // --- the query
    std::vector<std::string> where;
    std::string              sql;

    struct BIND
    {
        bool        isText;
        std::string s;
        double      d;
    };

    std::vector<BIND> binds;
    auto bindText = [&]( const std::string& s ) { binds.push_back( { true, s, 0 } ); return std::string( "?" ); };
    auto bindNum = [&]( double d ) { binds.push_back( { false, {}, d } ); return std::string( "?" ); };

    std::string unit;
    std::string cat = lower( aReq.category() );

    if( cat.find( "capacitor" ) != std::string::npos || cat == "cap" || cat == "c" )
        unit = "F";
    else if( cat.find( "resistor" ) != std::string::npos || cat == "res" || cat == "r" )
        unit = "Ω";
    else if( cat.find( "inductor" ) != std::string::npos || cat == "l" )
        unit = "H";
    else if( !cat.empty() )
    {
        // (one bind per statement: the operands of + are evaluated in no fixed order)
        std::string a = bindText( "%" + cat + "%" );
        std::string b = bindText( "%" + cat + "%" );
        where.push_back( "(lower(p.category) LIKE " + a + " OR lower(p.subcategory) LIKE " + b + ")" );
    }

    if( !aReq.value().empty() )
    {
        double v = 0;

        if( !ParseValue( aReq.value(), v, unit ) )
        {
            aError = "value \"" + aReq.value() + "\" not understood (e.g. 100nF, 10k, 4k7, 2.2uH)";
            return false;
        }

        if( unit.empty() )
        {
            aError = "value \"" + aReq.value() + "\": give a unit (F, H, Ω) or a category";
            return false;
        }

        where.push_back( "p.unit = " + bindText( unit ) );
        std::string lo = bindNum( v * 0.999 );
        std::string hi = bindNum( v * 1.001 );
        where.push_back( "p.value BETWEEN " + lo + " AND " + hi );
    }
    else if( !unit.empty() )
    {
        where.push_back( "p.unit = " + bindText( unit ) );
    }

    if( !aReq.package().empty() )
    {
        std::string a = bindText( aReq.package() );
        std::string b = bindText( aReq.package() + "-%" );
        std::string c = bindText( aReq.package() + "\\_%" );
        where.push_back( "(p.package = " + a + " OR p.package LIKE " + b + " OR p.package LIKE " + c + " ESCAPE '\\')" );
    }

    if( aReq.tolerance() > 0 )
        where.push_back( "p.tolerance <= " + bindNum( aReq.tolerance() + 1e-9 ) );

    if( aReq.voltage_min() > 0 )
        where.push_back( "p.voltage >= " + bindNum( aReq.voltage_min() - 1e-9 ) );

    if( !aReq.dielectric().empty() )
    {
        std::string d = aReq.dielectric();
        std::transform( d.begin(), d.end(), d.begin(), ::toupper );

        if( d == "NP0" || d == "COG" )
            d = "C0G";

        where.push_back( "p.dielectric = " + bindText( d ) );
    }

    if( aReq.basic_only() )
        where.push_back( "p.basic = 1" );

    where.push_back( "p.stock >= " + bindNum( (double) std::max<int64_t>( 1, aReq.min_stock() ) ) );

    std::string from = "parts p";

    if( !aReq.keyword().empty() )
    {
        // every word, as a prefix ("AMS1117" finds "AMS1117-3.3")
        std::string match, word;
        std::string kw = aReq.keyword() + " ";

        for( char c : kw )
        {
            if( std::isspace( static_cast<unsigned char>( c ) ) || c == ',' )
            {
                if( !word.empty() )
                {
                    std::string q;

                    for( char w : word )
                        q += w == '"' ? std::string( "\"\"" ) : std::string( 1, w );

                    match += ( match.empty() ? "" : " " ) + std::string( "\"" ) + q + "\"*";
                    word.clear();
                }
            }
            else
            {
                word += c;
            }
        }

        if( !match.empty() )
        {
            from = "fts JOIN parts p ON p.lcsc = fts.rowid";
            where.push_back( "fts MATCH " + bindText( match ) );
        }
    }

    if( where.size() <= 1 )
    {
        aError = "say what to look for: value, keyword, category, package...";
        return false;
    }

    std::string cond;

    for( const std::string& w : where )
        cond += ( cond.empty() ? " WHERE " : " AND " ) + w;

    auto bindAll = [&]( sqlite3_stmt* aSt )
    {
        for( size_t i = 0; i < binds.size(); ++i )
        {
            if( binds[i].isText )
                sqlite3_bind_text( aSt, (int) i + 1, binds[i].s.c_str(), -1, SQLITE_TRANSIENT );
            else
                sqlite3_bind_double( aSt, (int) i + 1, binds[i].d );
        }
    };

    {
        STMT s;

        if( sqlite3_prepare_v2( db->db, ( "SELECT count(*) FROM " + from + cond ).c_str(), -1, &s.st, nullptr ) != SQLITE_OK )
        {
            aError = std::string( "parts index query: " ) + sqlite3_errmsg( db->db );
            return false;
        }

        bindAll( s.st );

        if( sqlite3_step( s.st ) == SQLITE_ROW )
            aOut.set_total( sqlite3_column_int64( s.st, 0 ) );
    }

    const int limit = aReq.limit() > 0 ? std::min( aReq.limit(), 50 ) : 8;
    STMT      s;
    sql = "SELECT p.lcsc, p.mfr, p.manufacturer, p.package, p.description, p.basic, p.preferred, p.stock, p.price FROM "
          + from + cond + " ORDER BY p.basic DESC, p.preferred DESC, p.stock DESC LIMIT " + std::to_string( limit );

    if( sqlite3_prepare_v2( db->db, sql.c_str(), -1, &s.st, nullptr ) != SQLITE_OK )
    {
        aError = std::string( "parts index query: " ) + sqlite3_errmsg( db->db );
        return false;
    }

    bindAll( s.st );

    while( sqlite3_step( s.st ) == SQLITE_ROW )
    {
        kicadd::board::LcscHit* h = aOut.add_hits();
        h->set_lcsc( "C" + std::to_string( sqlite3_column_int64( s.st, 0 ) ) );
        h->set_mfr( text( s.st, 1 ) );
        h->set_manufacturer( text( s.st, 2 ) );
        h->set_package( text( s.st, 3 ) );
        h->set_description( text( s.st, 4 ) );
        h->set_basic( sqlite3_column_int( s.st, 5 ) != 0 );
        h->set_preferred( sqlite3_column_int( s.st, 6 ) != 0 );
        h->set_stock( sqlite3_column_int64( s.st, 7 ) );
        h->set_price( sqlite3_column_type( s.st, 8 ) == SQLITE_NULL ? -1 : sqlite3_column_double( s.st, 8 ) );
        h->set_live_stock( -1 );
    }

    // --- stage 2: today's stock and assembly for the first few
    const int live = aReq.live() == 0 ? 3 : std::max( 0, aReq.live() );

    for( int i = 0; i < aOut.hits_size() && i < live; ++i )
    {
        kicadd::board::LcscHit* h = aOut.mutable_hits( i );
        PART                    part;
        std::string             err;

        if( FetchPart( h->lcsc(), true, part, err ) )
        {
            h->set_live_stock( part.stock );
            h->set_live_class( part.jlcClass );
            h->set_live_smt( part.jlcSmt );

            if( !part.note.empty() )
                h->set_live_note( part.note );
        }
        else
        {
            h->set_live_note( err );
        }
    }

    aOut.set_note( note );
    return true;
}

} // namespace kicadd::lcsc
