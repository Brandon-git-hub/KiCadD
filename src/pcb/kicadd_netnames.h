/*
 * KiCadD: net names for the audit's layout checks — the agent's net globs, supply names and diff
 * pair partners (shared by kicadd_refplane and kicadd_lengths).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include "common/kicadd_names.h"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <regex>
#include <string>
#include <vector>

namespace kicadd::netnames
{

/// Named like a supply or ground: +5V, 3V3, GND, AGND, VCC_IO, VDD, VBUS, VBAT ...
inline bool SupplyName( const std::string& aNet )
{
    static const std::regex re( R"(^([+-][A-Za-z].*|[+-]?\d+(\.\d+)?V\d*|[+-]?\d+(\.\d+)?V\d*_.*|[PN]\d+V\d*(_.*)?|.*GND.*|VCC.*|VDD.*|VSS.*|VEE.*|VBUS.*|VBAT.*|VIN|VSYS.*|.*PWR.*)$)",
                                std::regex::icase );
    const size_t      slash = aNet.rfind( '/' );
    return std::regex_match( slash == std::string::npos ? aNet : aNet.substr( slash + 1 ), re );
}


/// The name of a diff pair partner: X+ / X-, X_P / X_N, XDP / XDN ("" when the name is no pair
/// member). P / N only after a separator or a D (USB_DP / USB_DN), so SRP / SRN are not a pair.
inline std::string PartnerName( const std::string& aName )
{
    if( aName.size() < 2 )
        return "";

    const bool pn = aName.size() > 2 && std::strchr( "PpNn", aName.back() )
                    && ( !std::isalpha( (unsigned char) aName[aName.size() - 2] ) || aName[aName.size() - 2] == 'D' );

    if( !pn && aName.back() != '+' && aName.back() != '-' )
        return "";

    std::string partner = aName;
    char&       last = partner.back();
    last = last == '+' ? '-' : last == '-' ? '+' : last == 'P' ? 'N' : last == 'N' ? 'P' : last == 'p' ? 'n' : 'p';
    return partner;
}


/// The negative member of a pair (X- / X_N).
inline bool NegativeMember( const std::string& aName )
{
    return !aName.empty() && std::strchr( "Nn-", aName.back() );
}


/// Net name globs with a number each ({"*PCIE*TX*": 1.0, "*": 3}). For a net, an exact name wins,
/// then the glob with the most literal characters. A glob matches as names::GlobMatch: the whole
/// name or the part after a sheet path ("USB_D+" matches "/USB/USB_D+"), case blind.
struct GLOBS
{
    struct GLOB
    {
        std::string pattern;
        int         rank;
        double      value;
        double      value2 = 0;
    };

    std::vector<GLOB> list;

    void add( const std::string& aPattern, double aValue, double aValue2 = 0 )
    {
        const int  literal = (int) std::count_if( aPattern.begin(), aPattern.end(), []( char ch ) { return ch != '*' && ch != '?'; } );
        const bool exact = aPattern.find_first_of( "*?" ) == std::string::npos;
        list.push_back( { aPattern, literal + ( exact ? 100000 : 0 ), aValue, aValue2 } );
    }

    bool empty() const { return list.empty(); }

    /// The best glob for a name (nullptr: none).
    const GLOB* match( const std::string& aName ) const
    {
        const GLOB* best = nullptr;

        for( const GLOB& g : list )
        {
            if( ( !best || g.rank > best->rank ) && names::GlobMatch( g.pattern, aName ) )
                best = &g;
        }

        return best;
    }

    /// Whether this one glob matches the name (for groups: each glob on its own).
    static bool matches( const GLOB& aGlob, const std::string& aName ) { return names::GlobMatch( aGlob.pattern, aName ); }
};

} // namespace kicadd::netnames
