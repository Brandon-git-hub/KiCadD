/*
 * KiCadD: fabrication output (Gerbers, drill, BOM, CPL) from the board in memory.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

#include <string>

class BOARD;
class wxString;

namespace kicadd
{

/// Write the fabrication set for @a aBoard into @a aDir (created). False with @a aError when a
/// file could not be written.
bool ExportFab( BOARD* aBoard, const wxString& aDir, bool aZip, kicadd::board::ExportFabResponse& aOut,
                std::string& aError );

} // namespace kicadd
