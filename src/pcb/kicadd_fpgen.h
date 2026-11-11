/*
 * KiCadD: parametric footprints for common packages (FpCreate).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

#include <string>

namespace kicadd
{

/// The .kicad_mod text for @a aReq (aName set to the footprint's name), or false with aError.
bool MakeFootprint( const kicadd::board::FpCreate& aReq, std::string& aName, std::string& aText, int& aPads,
                    std::string& aError );

} // namespace kicadd
