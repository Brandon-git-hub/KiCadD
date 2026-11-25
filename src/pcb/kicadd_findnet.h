/*
 * KiCadD: a board net by the name an agent gives (the rules in common/kicadd_names.h: exact, the
 * leading "/" added or taken off, a unique leaf, blind to case; an error names the candidates or
 * the closest nets).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include "common/kicadd_names.h"

#include <board.h>
#include <netinfo.h>

#include <string>
#include <vector>

namespace kicadd
{

/// the net aName means (nullptr: none; aErr, when given, says why with candidates)
inline NETINFO_ITEM* FindNetLoose( const BOARD& aBoard, const std::string& aName, std::string* aErr = nullptr )
{
    if( aName.empty() )
    {
        if( aErr )
            *aErr = "no net name given";

        return nullptr;
    }

    if( NETINFO_ITEM* net = aBoard.FindNet( wxString::FromUTF8( aName ) ) )
        return net;

    std::vector<std::string>   names;
    std::vector<NETINFO_ITEM*> nets;

    for( NETINFO_ITEM* n : aBoard.GetNetInfo() )
    {
        if( n->GetNetCode() > 0 )
        {
            names.push_back( n->GetNetname().ToStdString( wxConvUTF8 ) );
            nets.push_back( n );
        }
    }

    names::RESOLVED r = names::Resolve( names, aName );

    if( r.index < 0 )
    {
        if( aErr )
            *aErr = r.error;

        return nullptr;
    }

    return nets[r.index];
}

inline NETINFO_ITEM* FindNetLoose( const BOARD& aBoard, const wxString& aName, std::string* aErr = nullptr )
{
    return FindNetLoose( aBoard, aName.ToStdString( wxConvUTF8 ), aErr );
}

} // namespace kicadd
