/*
 * KiCadD: parts search by specification (LcscSearch). The index is a SQLite file built by
 * ci/build_parts_index.py from jlcparts' data and published by KiCadD's CI; it is downloaded to
 * the user cache on first use and refreshed when a week old.
 *
 * KICADD_PARTS_DB: use this index file as it is (no download). KICADD_PARTS_URL: where
 * parts.db.gz comes from (default the KiCadD release "parts-index").
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <kicadd/board.pb.h>

#include <string>

namespace kicadd::lcsc
{

bool SearchParts( const kicadd::board::LcscSearch& aReq, kicadd::board::LcscSearchResponse& aOut, std::string& aError );

/// "100nF" -> 1e-7 with unit "F"; "4k7" -> 4700 "Ω"; aUnit is the default when none is given.
bool ParseValue( const std::string& aText, double& aValue, std::string& aUnit );

} // namespace kicadd::lcsc
