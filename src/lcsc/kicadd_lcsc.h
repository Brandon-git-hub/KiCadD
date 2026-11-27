/*
 * KiCadD M4a/M4b: LCSC part number -> KiCad footprint and symbol, on demand.
 *
 * The part comes from the JLCEDA/EasyEDA component API (the data behind the LCSC catalogue),
 * is cached per user, converted with KiCad's own EasyEDA Standard importer
 * (pcbnew/pcb_io/easyeda) and written to the project's footprint library LCSC
 * (${KIPRJMOD}/lcsc.pretty), which is added to the project fp-lib-table on first use.
 *
 * Source data: JLCEDA/EasyEDA Official Library, https://lceda.cn/ , https://easyeda.com .
 * Every converted footprint names it in its description.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lcsc/kicadd_obj2wrl.h"

class BOARD;
class FOOTPRINT;
class KIWAY;
class wxString;

namespace kicadd::lcsc
{

/// Nickname of the project library the parts go to.
inline constexpr const char* LIB_NICKNAME = "LCSC";

/// Attribution required by the data source, put in each footprint's description.
inline constexpr const char* ATTRIBUTION =
        "Source: JLCEDA/EasyEDA Official Library (https://lceda.cn/, https://easyeda.com)";

struct PART
{
    std::map<std::string, std::string> pinTypeSet;   ///< the agent's pin types for the symbol (number or name -> type)
    std::string                lcsc;          ///< "C8734"
    std::string                title;         ///< part name, e.g. "STM32F103C8T6"
    std::string                value;         ///< a passive's value ("10k", "100nF"), as the symbol has it
    std::string                package;       ///< footprint name, e.g. "LQFP-48_L7.0-W7.0-P0.50-LS9.0-BL"
    std::string                description;
    std::string                datasheet;
    bool                       fromCache = false;
    std::string                note;          ///< e.g. "offline: used the cache from <date>"

    // M4c: catalogue data that comes with the same answer (as of asOf, not live)
    long long                  stock = -1;    ///< LCSC stock, -1 unknown
    double                     priceUsd = -1; ///< LCSC unit price at the minimum quantity, USD
    int                        minQty = 0;
    std::string                url;           ///< LCSC product page
    bool                       jlcSmt = false;       ///< JLCPCB can assemble it
    std::string                jlcClass;      ///< "Basic Part" / "Extended Part" / ""
    std::string                asOf;          ///< date of that data (fetch or cache date)
    std::unique_ptr<FOOTPRINT> footprint;     ///< no board, no nets, at the origin

    // 3D: EasyEDA's model of the footprint (its OBJ), as VRML for <project>/lcsc.3dshapes
    std::string                modelUuid;     ///< "": the footprint has none in EasyEDA
    kicadd::obj2wrl::PLACE     modelPlace;    ///< where EasyEDA puts it, from the footprint's 3D node
    std::string                wrl;           ///< converted (saved with the footprint); "" none
    std::string                modelNote;     ///< what became of it, one line
};

/// "C8734", "c8734" and "8734" -> "C8734"; empty when it isn't an LCSC number.
std::string Normalize( const std::string& aLcsc );

/**
 * Fetch (or read from the cache) and convert one part.
 *
 * Cache: KICADD_LCSC_CACHE or <KiCad user cache>/kicadd/lcsc/<C>.json, the raw API answer.
 * A cached part is used without asking the network unless @a aRefresh; when the network
 * fails a cached copy is used anyway (with a note). KICADD_LCSC_OFFLINE=1: cache only.
 * @a aModel: also its 3D model (EasyEDA's OBJ, cached as 3d-<uuid>.obj, one more request the
 * first time), converted to VRML (PART::wrl) and referenced by the footprint; a model that
 * can't be had only leaves a note (PART::modelNote).
 */
bool FetchPart( const std::string& aLcsc, bool aRefresh, PART& aPart, std::string& aError, bool aModel = true );

/// Where the converted 3D models go, next to lcsc.pretty; the footprints point at
/// ${KIPRJMOD}/lcsc.3dshapes/<package>.wrl
inline constexpr const char* MODEL_DIR = "lcsc.3dshapes";

struct SAVED
{
    std::string libId;        ///< "LCSC:<package>"
    std::string file;         ///< .kicad_mod written
    bool        registered = false;   ///< the LCSC row was added to the project fp-lib-table now
    bool        kept = false;         ///< the library's file differs (edited): kept, PART::footprint is now it
    std::string note;
};

/**
 * Write the footprint to <project dir>/lcsc.pretty and make sure the project fp-lib-table
 * has the LCSC library. Needs a board that lives in a folder (a saved board or a project).
 */
///
/// A .kicad_mod (or 3D model) already there that differs from what the EasyEDA data gives is
/// the project's own (a pad fixed by hand): it is kept unless aReplace (refresh=true), said in
/// SAVED::note, and aPart.footprint becomes the library's one (with the part's fields), so what
/// is placed is what the library has.
bool SaveToProjectLibrary( BOARD* aBoard, PART& aPart, SAVED& aSaved, std::string& aError,
                           bool aReplace = false );

/// The same for a project folder (e.g. from the schematic editor, which has no BOARD).
bool SaveToProjectLibrary( const wxString& aProjectDir, PART& aPart, SAVED& aSaved,
                           std::string& aError, bool aReplace = false );

struct SAVED_SYMBOL
{
    std::string              libId;        ///< "LCSC:<symbol>"
    std::string              file;         ///< the project's lcsc.kicad_sym
    std::string              reference;    ///< "U", "R", ...
    int                      units = 1;
    std::vector<std::string> pins;         ///< pin numbers, sorted
    bool                     registered = false;   ///< the LCSC row was added to sym-lib-table now
    std::string              note;
    bool                     kept = false; ///< the project's own (edited) copy kept, not replaced
    std::string              pinTypes;     ///< "pin types inferred ...: 8 power_in, ..." ("" none)
    std::map<std::string, std::string> pinNames;   ///< pin number -> name
};

/**
 * M4b: convert the part's schematic symbol (eeschema, through KIFACE_KICADD_LCSC_SYMBOL) into
 * <project dir>/lcsc.kicad_sym, its Footprint field pointing at LCSC:<package>, and make sure
 * the project sym-lib-table has the LCSC library. Call after FetchPart (it reads the cache).
 */
bool SaveSymbolToProjectLibrary( BOARD* aBoard, KIWAY* aKiway, const PART& aPart,
                                 SAVED_SYMBOL& aSaved, std::string& aError, bool aReplace = false );
bool SaveSymbolToProjectLibrary( const wxString& aProjectDir, KIWAY* aKiway, const PART& aPart,
                                 SAVED_SYMBOL& aSaved, std::string& aError, bool aReplace = false );

/// M4d: everything the editors' "Add LCSC part" commands need, in one call.
struct FETCHED
{
    PART         part;
    SAVED        footprint;
    SAVED_SYMBOL symbol;
    bool         footprintSaved = false;
    bool         symbolSaved = false;
    std::string  note;         ///< why something was not saved, cache notes
};

/**
 * Fetch a part and save footprint and symbol to the project in @a aProjectDir. Fails only when
 * the part itself can't be had; save problems end up in aOut.note.
 */
bool FetchAndSave( const wxString& aProjectDir, KIWAY* aKiway, const std::string& aLcsc, bool aRefresh,
                   FETCHED& aOut, std::string& aError );

/// One line for a status bar: "C8734 STM32F103C8T6: LCSC stock 10693, US$2.398, JLC Extended Part (2026-11-09)"
std::string Summary( const PART& aPart );

/// Copper pads the data left with no number (a USB-C's shell, a tab): they would be on no net.
/// Numbered aBind (a pin number of the symbol), else the symbol's one pin that has no pad (its
/// shell / EH pin); with none or several such pins they stay as they are. The note ("" when none);
/// aChanged: pads were numbered (save the footprint again).
std::string NumberUnnumberedPads( PART& aPart, const std::vector<std::string>& aSymbolPins, const std::string& aBind,
                                  bool& aChanged );

/// "; agrees with the parts index (jlcparts <date>, ...): JLC stock N, extended" or "; differs from
/// ...": what lcsc_search's index holds for the part next to EasyEDA's answer; "" without one.
std::string IndexComparison( const PART& aPart );

} // namespace kicadd::lcsc

/**
 * pcbnew export KIFACE_KICADD_LCSC_FETCH, for the schematic editor:
 * request {"lcsc": "C8734", "project_dir": "...", "refresh": false}
 * answer  {"ok": true, "lcsc", "title", "summary", "footprint": "LCSC:...", "symbol": "LCSC:...",
 *          "note"}  or {"ok": false, "error": "..."}
 */
wxString KicaddLcscFetchService( const wxString& aRequest, KIWAY* aKiway );
