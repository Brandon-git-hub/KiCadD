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

#include <memory>
#include <string>
#include <vector>

class BOARD;
class FOOTPRINT;
class KIWAY;

namespace kicadd::lcsc
{

/// Nickname of the project library the parts go to.
inline constexpr const char* LIB_NICKNAME = "LCSC";

/// Attribution required by the data source, put in each footprint's description.
inline constexpr const char* ATTRIBUTION =
        "Source: JLCEDA/EasyEDA Official Library (https://lceda.cn/, https://easyeda.com)";

struct PART
{
    std::string                lcsc;          ///< "C8734"
    std::string                title;         ///< part name, e.g. "STM32F103C8T6"
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
};

/// "C8734", "c8734" and "8734" -> "C8734"; empty when it isn't an LCSC number.
std::string Normalize( const std::string& aLcsc );

/**
 * Fetch (or read from the cache) and convert one part.
 *
 * Cache: KICADD_LCSC_CACHE or <KiCad user cache>/kicadd/lcsc/<C>.json, the raw API answer.
 * A cached part is used without asking the network unless @a aRefresh; when the network
 * fails a cached copy is used anyway (with a note). KICADD_LCSC_OFFLINE=1: cache only.
 */
bool FetchPart( const std::string& aLcsc, bool aRefresh, PART& aPart, std::string& aError );

struct SAVED
{
    std::string libId;        ///< "LCSC:<package>"
    std::string file;         ///< .kicad_mod written
    bool        registered = false;   ///< the LCSC row was added to the project fp-lib-table now
    std::string note;
};

/**
 * Write the footprint to <project dir>/lcsc.pretty and make sure the project fp-lib-table
 * has the LCSC library. Needs a board that lives in a folder (a saved board or a project).
 */
bool SaveToProjectLibrary( BOARD* aBoard, const PART& aPart, SAVED& aSaved, std::string& aError );

struct SAVED_SYMBOL
{
    std::string              libId;        ///< "LCSC:<symbol>"
    std::string              file;         ///< the project's lcsc.kicad_sym
    std::string              reference;    ///< "U", "R", ...
    int                      units = 1;
    std::vector<std::string> pins;         ///< pin numbers, sorted
    bool                     registered = false;   ///< the LCSC row was added to sym-lib-table now
    std::string              note;
};

/**
 * M4b: convert the part's schematic symbol (eeschema, through KIFACE_KICADD_LCSC_SYMBOL) into
 * <project dir>/lcsc.kicad_sym, its Footprint field pointing at LCSC:<package>, and make sure
 * the project sym-lib-table has the LCSC library. Call after FetchPart (it reads the cache).
 */
bool SaveSymbolToProjectLibrary( BOARD* aBoard, KIWAY* aKiway, const PART& aPart,
                                 SAVED_SYMBOL& aSaved, std::string& aError );

} // namespace kicadd::lcsc
