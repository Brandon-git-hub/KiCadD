/*
 * KiCadD: a board net by the name an agent gives.
 *
 * The schematic tools name a root-sheet local net "/USB_D_P"; a board may hold it as "USB_D_P" (a
 * global label, an older or imported board) or the other way round. Board tools that look a net
 * up by name accept either: the exact name first, then the same name with the leading "/" added
 * or taken off.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <board.h>
#include <netinfo.h>

#include <string>

namespace kicadd
{

inline NETINFO_ITEM* FindNetLoose( const BOARD& aBoard, const wxString& aName )
{
    if( aName.IsEmpty() )
        return nullptr;

    if( NETINFO_ITEM* net = aBoard.FindNet( aName ) )
        return net;

    if( aName.StartsWith( wxS( "/" ) ) && aName.Find( '/', true ) == 0 )
        return aBoard.FindNet( aName.Mid( 1 ) );   // "/X" asked, the board has "X"

    if( !aName.StartsWith( wxS( "/" ) ) )
        return aBoard.FindNet( wxS( "/" ) + aName );   // "X" asked, the board has "/X"

    return nullptr;
}

inline NETINFO_ITEM* FindNetLoose( const BOARD& aBoard, const std::string& aName )
{
    return FindNetLoose( aBoard, wxString::FromUTF8( aName ) );
}

} // namespace kicadd
