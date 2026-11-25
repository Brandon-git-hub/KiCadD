/*
 * KiCadD MCP - tool parameters declared once (beta.14 2b).
 *
 * Each parameter of a tool is one PARAM: its name, type (number, SI value, integer, string, enum,
 * boolean, array, object with known fields, map with free keys), unit, range, default and a short
 * description. From the list come:
 *   - the tool's inputSchema (tools/list),
 *   - the check before the tool runs: types, ranges, enums, unknown keys in nested objects, one
 *     error format for every tool ("simulate: tran.stop: a time in s, e.g. \"2m\"; got \"\\\"4n\\\"\" (extra quotes)"),
 *     SI strings turned into numbers in place (the tool reads plain numbers),
 *   - the Arguments section of tool_help / TOOLS.md (unit, range, default).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_PARAMS_H
#define KICADD_MCP_PARAMS_H

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace kicadd::mcp::params
{

enum class TYPE
{
    NUMBER,    ///< a JSON number (mm, degrees ...)
    SI,        ///< a number or an SI string ("4k7", "10u", "2.2Meg", "100nF"): made a number
    INTEGER,
    STRING,
    ENUM,      ///< one of choices
    BOOLEAN,
    ARRAY,     ///< of item (any when there is none)
    OBJECT,    ///< fields known; another key is an error
    MAP,       ///< free keys (refs, nets), each value of item (any when there is none)
    ANY        ///< checked by the tool (several shapes: "U1.5" or [x, y])
};

/// a tool's own check on a value (after the type check): "" when fine, else what is wrong
using CHECK = std::function<std::string( nlohmann::json& aValue )>;

struct PARAM
{
    std::string                         name;
    TYPE                                type = TYPE::ANY;
    std::string                         brief;       ///< the schema's description (short; tools/list)
    std::string                         doc;         ///< tool_help / TOOLS.md only (tools/list stays small)
    std::string                         unit;        ///< "mm", "s", "Ω" ... (said in errors and docs)
    double                              lo = -std::numeric_limits<double>::infinity();
    double                              hi = std::numeric_limits<double>::infinity();
    bool                                loOpen = false;   ///< lo itself not allowed (> 0)
    nlohmann::json                      def;              ///< the default (docs only; null: none)
    std::vector<std::string>            choices;          ///< ENUM
    std::vector<double>                 allowed;          ///< INTEGER / NUMBER: only these values (the schema's enum)
    std::vector<PARAM>                  fields;           ///< OBJECT
    std::shared_ptr<PARAM>              item;             ///< ARRAY / MAP: each element
    bool                                required = false;
    bool                                nested = true;    ///< OBJECT: its fields in the schema too (false: the brief says them)
    std::vector<std::string>            types;            ///< ANY: the JSON types the schema allows ("string", "array" ...)
    bool                                itemInSchema = false;   ///< MAP: its value's schema as additionalProperties
    std::string                         shorthand;        ///< OBJECT: a bare value means {shorthand: value} (tran: "2m")
    CHECK                               check;

    // --- building (each returns *this for chaining)
    PARAM& Brief( const std::string& aText ) { brief = aText; return *this; }
    PARAM& Doc( const std::string& aText ) { doc = aText; return *this; }
    PARAM& Unit( const std::string& aUnit ) { unit = aUnit; return *this; }
    PARAM& Range( double aLo, double aHi = std::numeric_limits<double>::infinity() ) { lo = aLo; hi = aHi; return *this; }
    PARAM& Positive() { lo = 0; loOpen = true; return *this; }
    PARAM& Default( const nlohmann::json& aValue ) { def = aValue; return *this; }
    PARAM& Required() { required = true; return *this; }
    PARAM& Flat() { nested = false; return *this; }
    PARAM& Check( CHECK aCheck ) { check = std::move( aCheck ); return *this; }
    PARAM& Shorthand( const std::string& aField ) { shorthand = aField; return *this; }
    PARAM& ItemSchema() { itemInSchema = true; return *this; }
    PARAM& Allowed( std::vector<double> aValues ) { allowed = std::move( aValues ); return *this; }
};

PARAM Number( const std::string& aName, const std::string& aBrief = "", const std::string& aUnit = "" );
PARAM Si( const std::string& aName, const std::string& aBrief = "", const std::string& aUnit = "" );
PARAM Integer( const std::string& aName, const std::string& aBrief = "" );
PARAM String( const std::string& aName, const std::string& aBrief = "" );
PARAM Enum( const std::string& aName, std::vector<std::string> aChoices, const std::string& aBrief = "" );
PARAM Boolean( const std::string& aName, const std::string& aBrief = "" );
PARAM Array( const std::string& aName, const PARAM& aItem, const std::string& aBrief = "" );
PARAM Strings( const std::string& aName, const std::string& aBrief = "" );   ///< an array of strings
PARAM Object( const std::string& aName, std::vector<PARAM> aFields, const std::string& aBrief = "" );
PARAM Map( const std::string& aName, const PARAM& aValue, const std::string& aBrief = "" );
PARAM Any( const std::string& aName, std::vector<std::string> aTypes, const std::string& aBrief = "" );

/// the inputSchema of a parameter list
nlohmann::json Schema( const std::vector<PARAM>& aParams );

/// check aArgs against aParams (SI strings become numbers in place); "" when fine, else the
/// first problem as "path: what was wanted; got ..."
std::string Validate( const std::vector<PARAM>& aParams, nlohmann::json& aArgs );

/// the Arguments section for tool_help / TOOLS.md: one line per parameter (nested fields indented)
std::string Docs( const std::vector<PARAM>& aParams );

} // namespace kicadd::mcp::params

#endif
