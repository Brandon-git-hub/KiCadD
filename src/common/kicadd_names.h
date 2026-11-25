/*
 * KiCadD: a net (or any named item) by the name an agent gives, and net name globs: one set of
 * rules for the board handlers, the schematic handlers and kicadd-mcp (pure C++, header only).
 *
 * A name resolves by the first rule that gives exactly one:
 *   1. the same name;
 *   2. the name with its leading "/" added or taken off ("/USB_D_P" for "USB_D_P", and back);
 *   3. the leaf (after the last "/") when one name has it ("VOUT" for "/Power/VOUT");
 *   4. 1 - 3 again in that order, blind to case ("usb_d_p"; "cc1" is "/CC1" before "/USB/CC1").
 * Two or more at a rule: an error naming them (the agent picks). None: an error with the three
 * closest names (edit distance on the leaves, blind to case).
 *
 * A glob ("*PCIE*TX*", "USB_D?") matches the whole name or the part after a sheet path
 * ("USB_D+" matches "/USB/USB_D+"), blind to case.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace kicadd::names
{

/// the part after the last "/" (the name itself without one)
inline std::string Leaf( const std::string& aName )
{
    const size_t s = aName.rfind( '/' );
    return s == std::string::npos ? aName : aName.substr( s + 1 );
}

/// without a leading "/" (a root-sheet local net "/X" -> "X"; "/A/X" stays)
inline std::string Bare( const std::string& aName )
{
    return aName.size() > 1 && aName[0] == '/' && aName.find( '/', 1 ) == std::string::npos ? aName.substr( 1 ) : aName;
}

inline std::string Upper( std::string s )
{
    for( char& c : s )
        c = (char) std::toupper( (unsigned char) c );

    return s;
}

/// Levenshtein distance (for the closest names)
inline size_t Distance( const std::string& a, const std::string& b )
{
    std::vector<size_t> row( b.size() + 1 );

    for( size_t j = 0; j <= b.size(); ++j )
        row[j] = j;

    for( size_t i = 1; i <= a.size(); ++i )
    {
        size_t diag = row[0];
        row[0] = i;

        for( size_t j = 1; j <= b.size(); ++j )
        {
            const size_t up = row[j];
            row[j] = std::min( { row[j] + 1, row[j - 1] + 1, diag + ( a[i - 1] == b[j - 1] ? 0 : 1 ) } );
            diag = up;
        }
    }

    return row[b.size()];
}

struct RESOLVED
{
    int         index = -1;   ///< into the names; -1 none
    int         rule = 0;     ///< 1 - 4 (0 none)
    std::string error;        ///< when index is -1: why, with the candidates or the closest names
};

/// aWant among aNames (aWhat names the kind in the error: "net", "part")
inline RESOLVED Resolve( const std::vector<std::string>& aNames, const std::string& aWant, const std::string& aWhat = "net" )
{
    RESOLVED r;

    if( aWant.empty() )
    {
        r.error = "no " + aWhat + " name given";
        return r;
    }

    const std::string toggled = aWant[0] == '/' ? aWant.substr( 1 ) : "/" + aWant;
    const bool        leafRule = aWant.find( '/', aWant[0] == '/' ? 1 : 0 ) == std::string::npos;
    const std::string leafWant = Leaf( aWant );

    auto list = [&]( const std::vector<int>& aHits )
    {
        std::string l;

        for( size_t i = 0; i < aHits.size() && i < 6; ++i )
            l += ( i ? ", " : "" ) + aNames[aHits[i]];

        return l + ( aHits.size() > 6 ? ", ... (" + std::to_string( aHits.size() ) + ")" : "" );
    };

    // rules 1 - 3 as they are, then the same three blind to case (all rule 4)
    for( int step = 1; step <= 6; ++step )
    {
        const bool       blind = step > 3;
        const int        rule = blind ? step - 3 : step;
        std::vector<int> hits;

        auto same = [&]( const std::string& x, const std::string& y ) { return blind ? Upper( x ) == Upper( y ) : x == y; };

        for( size_t i = 0; i < aNames.size(); ++i )
        {
            const std::string& n = aNames[i];
            bool               hit = false;

            if( rule == 1 )
                hit = same( n, aWant );
            else if( rule == 2 )
                hit = same( n, toggled );
            else
                hit = leafRule && same( Leaf( n ), leafWant ) && !same( n, leafWant ) && !same( n, toggled );

            if( hit )
                hits.push_back( (int) i );
        }

        if( hits.size() == 1 )
        {
            r.index = hits[0];
            r.rule = blind ? 4 : rule;
            return r;
        }

        if( hits.size() > 1 )
        {
            r.error = aWant + ": which " + aWhat + ", " + list( hits ) + "?";
            return r;
        }
    }

    // none: the closest three leaves
    std::vector<std::pair<size_t, int>> closest;
    const std::string                   uw = Upper( leafWant );

    for( size_t i = 0; i < aNames.size(); ++i )
        closest.emplace_back( Distance( uw, Upper( Leaf( aNames[i] ) ) ), (int) i );

    std::stable_sort( closest.begin(), closest.end(), []( const auto& x, const auto& y ) { return x.first < y.first; } );
    r.error = "no " + aWhat + " " + aWant;

    if( !closest.empty() )
    {
        r.error += " (closest: ";

        for( size_t i = 0; i < closest.size() && i < 3; ++i )
            r.error += ( i ? ", " : "" ) + aNames[closest[i].second];

        r.error += ")";
    }

    return r;
}

/// whether aName is one aWant may mean by rules 1 - 4 (no uniqueness: for picking every net a
/// name covers, e.g. what to draw)
inline bool Means( const std::string& aWant, const std::string& aName )
{
    if( aWant.empty() || aName.empty() )
        return false;

    const std::string toggled = aWant[0] == '/' ? aWant.substr( 1 ) : "/" + aWant;
    const bool        leafRule = aWant.find( '/', aWant[0] == '/' ? 1 : 0 ) == std::string::npos;
    const std::string un = Upper( aName );

    return un == Upper( aWant ) || un == Upper( toggled ) || ( leafRule && Upper( Leaf( aName ) ) == Upper( Leaf( aWant ) ) );
}

/// a glob ("*" any run, "?" one character) against a whole string, blind to case
inline bool WildMatch( const std::string& aPattern, const std::string& aText )
{
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;

    while( t < aText.size() )
    {
        if( p < aPattern.size()
            && ( aPattern[p] == '?' || std::toupper( (unsigned char) aPattern[p] ) == std::toupper( (unsigned char) aText[t] ) ) )
        {
            ++p;
            ++t;
        }
        else if( p < aPattern.size() && aPattern[p] == '*' )
        {
            star = p++;
            mark = t;
        }
        else if( star != std::string::npos )
        {
            p = star + 1;
            t = ++mark;
        }
        else
        {
            return false;
        }
    }

    while( p < aPattern.size() && aPattern[p] == '*' )
        ++p;

    return p == aPattern.size();
}

/// a net glob: the whole name, or the part after any sheet path ("USB_D+" matches "/USB/USB_D+";
/// "/USB/*" the sheet's nets), blind to case
inline bool GlobMatch( const std::string& aPattern, const std::string& aName )
{
    if( WildMatch( aPattern, aName ) )
        return true;

    for( size_t s = aName.find( '/' ); s != std::string::npos; s = aName.find( '/', s + 1 ) )
    {
        if( WildMatch( aPattern, aName.substr( s + 1 ) ) )
            return true;
    }

    return false;
}

} // namespace kicadd::names
