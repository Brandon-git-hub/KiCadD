/*
 * KiCadD: what changed between the board in memory and a board file (the last save by default).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

#include <string>

class BOARD;

namespace kicadd
{

/// Compare @a aNow (in memory) with the board in @a aFile. False with @a aError when the file
/// cannot be read.
bool DiffBoard( const BOARD& aNow, const std::string& aFile, int aLimit, kicadd::board::BoardDiffResponse& aOut,
                std::string& aError );

} // namespace kicadd
