/*
 * KiCadD MCP - tool parameters declared once (see params.h).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "params.h"
#include "sim.h"
#include "tools.h"   // sim::ParseSI, sim::FormatSI

#include <algorithm>
#include <cmath>
#include <cstdio>

using json = nlohmann::json;

namespace kicadd::mcp::params
{

namespace
{

PARAM make( const std::string& aName, TYPE aType, const std::string& aBrief, const std::string& aUnit = "" )
{
    PARAM p;
    p.name = aName;
    p.type = aType;
    p.brief = aBrief;
    p.unit = aUnit;
    return p;
}


/// a value as the agent wrote it, short
std::string shown( const json& aValue )
{
    std::string s = aValue.dump();

    if( s.size() > 48 )
        s = s.substr( 0, 45 ) + "...";

    return s;
}


std::string number( double aV )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%g", aV );
    return buf;
}


/// what a parameter takes, in words ("a number in mm, 0 - 90", "one of each, group")
std::string wanted( const PARAM& aP )
{
    std::string w;

    switch( aP.type )
    {
    case TYPE::NUMBER: w = "a number" + ( aP.unit.empty() ? "" : " in " + aP.unit ); break;
    case TYPE::SI: w = "a value" + ( aP.unit.empty() ? "" : " in " + aP.unit ) + " (a number, or SI as \"4k7\", \"10u\", \"2m\")"; break;
    case TYPE::INTEGER: w = "a whole number"; break;
    case TYPE::STRING: w = "a string"; break;
    case TYPE::BOOLEAN: w = "true or false"; break;
    case TYPE::ARRAY: w = "an array"; break;
    case TYPE::OBJECT:
    case TYPE::MAP: w = "an object"; break;
    case TYPE::ENUM:
    {
        w = "one of ";

        for( size_t i = 0; i < aP.choices.size(); ++i )
            w += ( i ? ", " : "" ) + aP.choices[i];

        break;
    }
    case TYPE::ANY: w = "a value"; break;
    }

    return w;
}


std::string range( const PARAM& aP )
{
    const bool lo = std::isfinite( aP.lo ), hi = std::isfinite( aP.hi );

    if( lo && hi )
        return number( aP.lo ) + " - " + number( aP.hi );

    if( lo )
        return ( aP.loOpen ? "> " : ">= " ) + number( aP.lo );

    if( hi )
        return "<= " + number( aP.hi );

    return {};
}


std::string checkOne( const PARAM& aP, json& aV, const std::string& aPath );


std::string checkFields( const std::vector<PARAM>& aFields, json& aObj, const std::string& aPrefix )
{
    for( auto& [key, value] : aObj.items() )
    {
        const PARAM* p = nullptr;

        for( const PARAM& f : aFields )
        {
            if( f.name == key )
                p = &f;
        }

        if( !p )
        {
            std::string keys;

            for( const PARAM& f : aFields )
                keys += ( keys.empty() ? "" : ", " ) + f.name;

            return aPrefix + key + ": unknown " + ( aPrefix.empty() ? "argument" : "key" ) + "; "
                   + ( aPrefix.empty() ? "the arguments" : "the keys of " + aPrefix.substr( 0, aPrefix.size() - 1 ) ) + " are: " + keys;
        }

        if( std::string e = checkOne( *p, value, aPrefix + key ); !e.empty() )
            return e;
    }

    for( const PARAM& f : aFields )
    {
        if( f.required && !aObj.contains( f.name ) )
            return aPrefix + f.name + ": missing (" + wanted( f ) + ( f.brief.empty() ? "" : ": " + f.brief ) + ")";
    }

    return {};
}


std::string checkOne( const PARAM& aP, json& aV, const std::string& aPath )
{
    auto bad = [&]( const std::string& aWhy = "" )
    {
        std::string got = shown( aV );

        // a string in a second pair of quotes ("\"4n\""): said, it is the usual slip
        if( aV.is_string() )
        {
            const std::string s = aV.get<std::string>();

            if( s.size() >= 2 && ( ( s.front() == '"' && s.back() == '"' ) || ( s.front() == '\'' && s.back() == '\'' ) ) )
                got += " (in extra quotes)";
        }

        return aPath + ": " + ( aWhy.empty() ? wanted( aP ) : aWhy ) + "; got " + got;
    };

    auto inRange = [&]( double v ) -> std::string
    {
        if( !aP.allowed.empty() && std::find( aP.allowed.begin(), aP.allowed.end(), v ) == aP.allowed.end() )
        {
            std::string l;

            for( double x : aP.allowed )
                l += ( l.empty() ? "" : ", " ) + number( x );

            return aPath + ": one of " + l + "; got " + number( v );
        }

        if( v < aP.lo || v > aP.hi || ( aP.loOpen && v <= aP.lo ) )
            return aPath + ": " + range( aP ) + ( aP.unit.empty() ? "" : " " + aP.unit ) + "; got " + number( v );

        return {};
    };

    switch( aP.type )
    {
    case TYPE::NUMBER:
        if( !aV.is_number() )
            return bad();

        if( std::string e = inRange( aV.get<double>() ); !e.empty() )
            return e;

        break;

    case TYPE::SI:
    {
        double v = 0;

        if( aV.is_number() )
            v = aV.get<double>();
        else if( !aV.is_string() || !sim::ParseSI( aV.get<std::string>(), v ) )
            return bad();
        else
            aV = v;   // the tool reads a number

        if( std::string e = inRange( v ); !e.empty() )
            return e;

        break;
    }

    case TYPE::INTEGER:
        if( !aV.is_number() || ( aV.is_number_float() && aV.get<double>() != std::floor( aV.get<double>() ) ) )
            return bad();

        if( aV.is_number_float() )
            aV = (long long) aV.get<double>();

        if( std::string e = inRange( aV.get<double>() ); !e.empty() )
            return e;

        break;

    case TYPE::STRING:
        if( !aV.is_string() )
            return bad();

        break;

    case TYPE::ENUM:
    {
        if( !aV.is_string() )
            return bad();

        const std::string s = aV.get<std::string>();

        if( std::find( aP.choices.begin(), aP.choices.end(), s ) == aP.choices.end() )
            return bad();

        break;
    }

    case TYPE::BOOLEAN:
        if( !aV.is_boolean() )
            return bad();

        break;

    case TYPE::ARRAY:
        if( !aV.is_array() )
            return bad();

        if( aP.item )
        {
            for( size_t i = 0; i < aV.size(); ++i )
            {
                if( std::string e = checkOne( *aP.item, aV[i], aPath + "[" + std::to_string( i ) + "]" ); !e.empty() )
                    return e;
            }
        }

        break;

    case TYPE::OBJECT:
        if( !aV.is_object() && !aP.shorthand.empty() && !aV.is_null() && !aV.is_array() )
            aV = json{ { aP.shorthand, aV } };

        if( !aV.is_object() )
            return bad();

        if( std::string e = checkFields( aP.fields, aV, aPath + "." ); !e.empty() )
            return e;

        break;

    case TYPE::MAP:
        if( !aV.is_object() )
            return bad();

        if( aP.item )
        {
            for( auto& [key, value] : aV.items() )
            {
                if( std::string e = checkOne( *aP.item, value, aPath + "." + key ); !e.empty() )
                    return e;
            }
        }

        break;

    case TYPE::ANY: break;
    }

    if( aP.check )
    {
        if( std::string e = aP.check( aV ); !e.empty() )
            return aPath + ": " + e;
    }

    return {};
}


json schemaOf( const PARAM& aP )
{
    json s = json::object();

    switch( aP.type )
    {
    case TYPE::NUMBER: s["type"] = "number"; break;
    case TYPE::SI: s["type"] = { "number", "string" }; break;
    case TYPE::INTEGER: s["type"] = "integer"; break;
    case TYPE::STRING: s["type"] = "string"; break;
    case TYPE::ENUM:
        s["type"] = "string";
        s["enum"] = aP.choices;
        break;
    case TYPE::BOOLEAN: s["type"] = "boolean"; break;
    case TYPE::ARRAY:
        s["type"] = "array";

        if( aP.item )
            s["items"] = schemaOf( *aP.item );

        break;
    case TYPE::OBJECT:
        s["type"] = "object";

        if( aP.nested && !aP.fields.empty() )
        {
            json props = json::object();
            json req = json::array();

            for( const PARAM& f : aP.fields )
            {
                props[f.name] = schemaOf( f );

                if( f.required )
                    req.push_back( f.name );
            }

            s["properties"] = props;

            if( !req.empty() )
                s["required"] = req;
        }

        break;
    case TYPE::MAP:
        s["type"] = "object";

        if( aP.itemInSchema && aP.item )
            s["additionalProperties"] = schemaOf( *aP.item );

        break;
    case TYPE::ANY:
        if( aP.types.size() == 1 )
            s["type"] = aP.types[0];
        else if( !aP.types.empty() )
            s["type"] = aP.types;

        break;
    }

    if( !aP.allowed.empty() )
    {
        json e = json::array();

        for( double x : aP.allowed )
            e.push_back( aP.type == TYPE::INTEGER ? json( (long long) x ) : json( x ) );

        s["enum"] = e;
    }

    if( !aP.brief.empty() )
        s["description"] = aP.brief;

    return s;
}


void docsOf( const PARAM& aP, const std::string& aPath, int aDepth, std::string& aOut )
{
    std::string what;

    switch( aP.type )
    {
    case TYPE::NUMBER: what = "number"; break;
    case TYPE::SI: what = "SI value"; break;
    case TYPE::INTEGER: what = "integer"; break;
    case TYPE::STRING: what = "string"; break;
    case TYPE::ENUM: what = wanted( aP ); break;
    case TYPE::BOOLEAN: what = "boolean"; break;
    case TYPE::ARRAY: what = aP.item && aP.item->type == TYPE::STRING ? "strings" : "array"; break;
    case TYPE::OBJECT: what = "object"; break;
    case TYPE::MAP: what = "map"; break;
    case TYPE::ANY: what = aP.types.empty() ? "any" : aP.types.front(); break;
    }

    if( !aP.unit.empty() )
        what += ", " + aP.unit;

    if( std::string r = range( aP ); !r.empty() )
        what += ", " + r;

    if( !aP.def.is_null() )
        what += ", default " + ( aP.def.is_string() ? aP.def.get<std::string>() : aP.def.dump() );

    if( aP.required )
        what += ", required";

    aOut += std::string( aDepth * 2, ' ' ) + "- `" + aPath + "` (" + what + ")" + ( aP.brief.empty() && aP.doc.empty() ? "" : ": " + ( aP.doc.empty() ? aP.brief : aP.brief.empty() ? aP.doc : aP.brief + "; " + aP.doc ) ) + "\n";

    for( const PARAM& f : aP.fields )
        docsOf( f, aPath + "." + f.name, aDepth + 1, aOut );
}

} // namespace


PARAM Number( const std::string& aName, const std::string& aBrief, const std::string& aUnit ) { return make( aName, TYPE::NUMBER, aBrief, aUnit ); }
PARAM Si( const std::string& aName, const std::string& aBrief, const std::string& aUnit ) { return make( aName, TYPE::SI, aBrief, aUnit ); }
PARAM Integer( const std::string& aName, const std::string& aBrief ) { return make( aName, TYPE::INTEGER, aBrief ); }
PARAM String( const std::string& aName, const std::string& aBrief ) { return make( aName, TYPE::STRING, aBrief ); }
PARAM Boolean( const std::string& aName, const std::string& aBrief ) { return make( aName, TYPE::BOOLEAN, aBrief ); }

PARAM Enum( const std::string& aName, std::vector<std::string> aChoices, const std::string& aBrief )
{
    PARAM p = make( aName, TYPE::ENUM, aBrief );
    p.choices = std::move( aChoices );
    return p;
}

PARAM Array( const std::string& aName, const PARAM& aItem, const std::string& aBrief )
{
    PARAM p = make( aName, TYPE::ARRAY, aBrief );

    if( aItem.type != TYPE::ANY || !aItem.types.empty() || aItem.check )   // (an Any item: no items in the schema)
        p.item = std::make_shared<PARAM>( aItem );

    return p;
}

PARAM Strings( const std::string& aName, const std::string& aBrief )
{
    return Array( aName, String( "" ), aBrief );
}

PARAM Object( const std::string& aName, std::vector<PARAM> aFields, const std::string& aBrief )
{
    PARAM p = make( aName, TYPE::OBJECT, aBrief );
    p.fields = std::move( aFields );
    return p;
}

PARAM Map( const std::string& aName, const PARAM& aValue, const std::string& aBrief )
{
    PARAM p = make( aName, TYPE::MAP, aBrief );

    if( aValue.type != TYPE::ANY || aValue.check )
        p.item = std::make_shared<PARAM>( aValue );

    return p;
}

PARAM Any( const std::string& aName, std::vector<std::string> aTypes, const std::string& aBrief )
{
    PARAM p = make( aName, TYPE::ANY, aBrief );
    p.types = std::move( aTypes );
    return p;
}


json Schema( const std::vector<PARAM>& aParams )
{
    json props = json::object();
    json req = json::array();

    for( const PARAM& p : aParams )
    {
        props[p.name] = schemaOf( p );

        if( p.required )
            req.push_back( p.name );
    }

    json s = { { "type", "object" }, { "properties", props } };

    if( !req.empty() )
        s["required"] = req;

    return s;
}


std::string Validate( const std::vector<PARAM>& aParams, json& aArgs )
{
    if( !aArgs.is_object() )
        return "the arguments: an object";

    return checkFields( aParams, aArgs, "" );
}


std::string Docs( const std::vector<PARAM>& aParams )
{
    std::string out;

    for( const PARAM& p : aParams )
        docsOf( p, p.name, 0, out );

    return out;
}

} // namespace kicadd::mcp::params


namespace kicadd::mcp
{

void ApplyParams( TOOL& aTool )
{
    aTool.inputSchema = params::Schema( aTool.params );

    auto run = aTool.run;
    auto list = aTool.params;
    auto name = aTool.name;

    aTool.run = [run, list, name]( const json& aArgs ) -> TOOL_RESULT
    {
        json        a = aArgs.is_object() ? aArgs : json::object();
        std::string e = params::Validate( list, a );

        if( !e.empty() )
            return { name + ": " + e, true };

        return run( a );
    };
}

} // namespace kicadd::mcp
