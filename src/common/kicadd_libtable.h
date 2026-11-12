/*
 * KiCadD: add a library to the project's sym-lib-table / fp-lib-table and reload it.
 * Compiled into both kifaces (LCSC libraries in pcbnew and eeschema, lib_register).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <string>

class wxString;

namespace kicadd
{

enum class LIBTABLE_RESULT
{
    ADDED,      ///< the row was added, the table reloaded
    PRESENT,    ///< a row of that name with that uri was there already
    CONFLICT,   ///< a row of that name points somewhere else (left alone)
    FAILED      ///< the table could not be read or written (aNote says why)
};

/**
 * Add (aNickname, aUri) to <aProjectDir>/sym-lib-table (aSymbols) or fp-lib-table, creating the
 * table if needed, then reload the project's table of that kind. A row that is already there is
 * left as it is.
 * @param aExistingUri  set to the uri of a row of that name found in the table
 */
LIBTABLE_RESULT AddProjectLibrary( const wxString& aProjectDir, bool aSymbols, const std::string& aNickname,
                                   const std::string& aUri, const std::string& aDescr, std::string& aNote,
                                   std::string* aExistingUri = nullptr );

/**
 * Why the footprint aLibId ("Lib:Name") cannot be found, or "" when it can (or cannot be told:
 * a library that is not a .pretty directory). Checks the footprint library table and the
 * .kicad_mod file in it; for symbols' Footprint fields, which nothing checks until sync_pcb.
 */
std::string FootprintProblem( const std::string& aLibId );

/// Reload one project library (after its file changed); false if the library manager refused.
bool ReloadProjectLibrary( bool aSymbols, const std::string& aNickname );

} // namespace kicadd
