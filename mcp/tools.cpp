/*
 * KiCadD MCP - tool definitions and compact text rendering.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "drc.h"
#include "ipc_client.h"

#include <algorithm>
#include <set>

#include <cmath>
#include <cstdio>
#include <map>

#include <kicadd/board.pb.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/board/board_commands.pb.h>
#include <chrono>
#include <api/common/types/base_types.pb.h>
#include <google/protobuf/empty.pb.h>

using json = nlohmann::json;
namespace kb = kicadd::board;

namespace kicadd::mcp
{

namespace
{

constexpr double NM_PER_MM = 1e6;

int64_t toNm( double aMm ) { return static_cast<int64_t>( std::llround( aMm * NM_PER_MM ) ); }


/// Millimetres with at most 3 decimals, trailing zeros trimmed ("152.5", "0.1", "-3").
std::string mm( int64_t aNm )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.3f", aNm / NM_PER_MM );
    std::string s( buf );

    while( !s.empty() && s.back() == '0' )
        s.pop_back();

    if( !s.empty() && s.back() == '.' )
        s.pop_back();

    return s == "-0" ? "0" : s;
}


std::string pt( const kb::Point& aP ) { return mm( aP.x() ) + "," + mm( aP.y() ); }


std::string deg( int aDecideg )
{
    char buf[16];
    std::snprintf( buf, sizeof( buf ), "%g", aDecideg / 10.0 );
    return buf;
}


TOOL_RESULT error( const std::string& aMsg ) { return { aMsg, true }; }


// ---- argument helpers ----------------------------------------------------------------------

bool getNumber( const json& aArgs, const char* aKey, double& aOut )
{
    auto it = aArgs.find( aKey );

    if( it == aArgs.end() || !it->is_number() )
        return false;

    aOut = it->get<double>();
    return true;
}


std::vector<std::string> getStrings( const json& aArgs, const char* aKey )
{
    std::vector<std::string> out;
    auto                     it = aArgs.find( aKey );

    if( it == aArgs.end() )
        return out;

    if( it->is_string() )
        out.push_back( it->get<std::string>() );
    else if( it->is_array() )
        for( const json& v : *it )
            if( v.is_string() )
                out.push_back( v.get<std::string>() );

    return out;
}


const std::map<std::string, kb::ItemKind>& kindNames()
{
    static const std::map<std::string, kb::ItemKind> names = {
        { "footprint", kb::IK_FOOTPRINT }, { "pad", kb::IK_PAD },   { "track", kb::IK_TRACK },
        { "arc", kb::IK_ARC },             { "via", kb::IK_VIA },   { "zone", kb::IK_ZONE },
    };
    return names;
}


// ---- rendering -----------------------------------------------------------------------------

/// aRadius: query radius (nm), 0 if not a radius query.
std::string renderItem( const kb::CompactItem& aItem, int64_t aRadius = 0 )
{
    std::string s;

    switch( aItem.kind() )
    {
    case kb::IK_FOOTPRINT: s = "FP "; break;
    case kb::IK_PAD:       s = "PAD "; break;
    case kb::IK_TRACK:     s = "TRK "; break;
    case kb::IK_ARC:       s = "ARC "; break;
    case kb::IK_VIA:       s = "VIA "; break;
    case kb::IK_ZONE:      s = "ZONE "; break;
    default:               s = "? "; break;
    }

    s += aItem.handle();

    if( !aItem.net().empty() )
        s += " [" + aItem.net() + "]";

    if( !aItem.layer().empty() )
        s += " " + aItem.layer();

    if( aItem.kind() == kb::IK_TRACK || aItem.kind() == kb::IK_ARC )
        s += " " + pt( aItem.pos() ) + ">" + pt( aItem.end() ) + " w" + mm( aItem.width() );
    else
        s += " @" + pt( aItem.pos() );

    if( aItem.kind() == kb::IK_VIA )
        s += " d" + mm( aItem.width() );

    if( aItem.kind() == kb::IK_FOOTPRINT && aItem.rot_decideg() != 0 )
        s += " r" + deg( aItem.rot_decideg() );

    if( aItem.dist() > 0 )
        s += " ~" + mm( aItem.dist() );

    // A footprint far larger than the query whose body covers the centre (modules, heatsinks):
    // say so, or its distant anchor position reads like a wrong result.
    if( aItem.kind() == kb::IK_FOOTPRINT && aItem.dist() == 0 && aRadius > 0
        && ( aItem.size().x() > 2 * aRadius || aItem.size().y() > 2 * aRadius ) )
    {
        s += " (body " + mm( aItem.size().x() ) + "x" + mm( aItem.size().y() ) + " covers query point)";
    }

    return s;
}


std::string renderQuery( const kb::QueryResponse& aResp, int64_t aRadius = 0 )
{
    std::string s = std::to_string( aResp.items_size() );

    if( aResp.truncated() )
        s += " of " + std::to_string( aResp.total() ) + " (truncated; raise limit or narrow query)";

    s += " items\n";

    for( const kb::CompactItem& it : aResp.items() )
        s += renderItem( it, aRadius ) + "\n";

    return s;
}


std::string renderDrc( const google::protobuf::RepeatedPtrField<kb::DrcViolation>& aDrc )
{
    if( aDrc.empty() )
        return "drc: clean\n";

    std::string s = "drc: " + std::to_string( aDrc.size() ) + "\n";

    for( const kb::DrcViolation& v : aDrc )
    {
        switch( v.type() )
        {
        case kb::DT_CLEARANCE:         s += "CLEARANCE "; break;
        case kb::DT_SHORT:             s += "SHORT "; break;
        case kb::DT_HOLE_CLEARANCE:    s += "HOLE "; break;
        case kb::DT_COURTYARD_OVERLAP: s += "COURTYARD "; break;
        case kb::DT_EDGE_CLEARANCE:    s += "EDGE "; break;
        case kb::DT_COPPER_OUTSIDE_BOARD:
            s += "OUTSIDE_BOARD " + v.a() + ( v.net_a().empty() ? "" : "[" + v.net_a() + "]" ) + " " + v.layer()
                 + " @" + pt( v.pos() ) + "\n";
            continue;
        default:                       s += "DRC "; break;
        }

        s += v.a();

        if( !v.net_a().empty() )
            s += "[" + v.net_a() + "]";

        s += " x " + v.b();

        if( !v.net_b().empty() )
            s += "[" + v.net_b() + "]";

        s += " " + v.layer();

        if( v.type() != kb::DT_COURTYARD_OVERLAP )
            s += " need" + mm( v.required() ) + " have" + mm( v.actual() );

        s += " @" + pt( v.pos() ) + "\n";
    }

    return s;
}


std::string renderPending( const google::protobuf::RepeatedPtrField<std::string>& aPending )
{
    if( aPending.empty() )
        return "";

    std::string s = " (excl. deferred nets:";

    for( const std::string& n : aPending )
        s += " " + n;

    return s + "; call flush_ratsnest for the full count)";
}


std::string renderMoveResult( const kb::MoveResponse& r )
{
    std::string s;

    if( !r.source().empty() )
        s += "from " + r.source() + "\n";

    if( !r.created_nets().empty() )
    {
        s += "new nets:";

        for( const std::string& n : r.created_nets() )
            s += " " + n;

        s += "\n";
    }

    for( const std::string& ref : r.removed() )
        s += ref + " deleted\n";

    for( const kb::MoveResult& m : r.moved() )
    {
        bool added = std::find( r.added().begin(), r.added().end(), m.ref() ) != r.added().end();
        s += m.ref() + ( added ? " added" : "" ) + " @" + pt( m.pos() )
             + ( m.rot_decideg() ? " r" + deg( m.rot_decideg() ) : "" ) + ( m.back() ? " B.Cu" : "" ) + "\n";
    }

    if( !r.missing().empty() )
    {
        s += "not found:";

        for( const std::string& m : r.missing() )
            s += " " + m;

        s += "\n";
    }

    s += "unconnected=" + std::to_string( r.unconnected() ) + renderPending( r.ratsnest_pending() ) + "\n";
    s += renderDrc( r.drc() );
    return s;
}


std::string renderHistory( const kb::HistoryResponse& r )
{
    std::string s = "history: " + std::to_string( r.undo_size() ) + " undo, " + std::to_string( r.redo_size() )
                    + " redo" + ( r.modified() ? ", unsaved changes" : ", saved" );

    if( r.undo_size() )
        s += "; next undo: " + r.undo( r.undo_size() - 1 );

    return s + "\n";
}


// ---- tool bodies ---------------------------------------------------------------------------

TOOL_RESULT boardSummary( IPC_CLIENT& aIpc )
{
    kb::BoardSummaryResponse r;
    std::string              err;

    if( !aIpc.Call( kb::GetBoardSummary(), r, err ) )
        return error( err );

    std::string file = r.file();

    if( size_t slash = file.find_last_of( '/' ); slash != std::string::npos )
        file = file.substr( slash + 1 );

    return { "file=" + file + " footprints=" + std::to_string( r.footprints() )
             + " tracks=" + std::to_string( r.tracks() ) + " vias=" + std::to_string( r.vias() )
             + " zones=" + std::to_string( r.zones() ) + " nets=" + std::to_string( r.nets() )
             + " cu_layers=" + std::to_string( r.copper_layers() )
             + " outline=" + pt( r.bbox_min() ) + ".." + pt( r.bbox_max() ) + "mm\n" };
}


std::string boardFileName( IPC_CLIENT& aIpc, std::string& aErr )
{
    kb::BoardSummaryResponse r;

    if( !aIpc.Call( kb::GetBoardSummary(), r, aErr ) )
        return {};

    std::string file = r.file();

    if( size_t slash = file.find_last_of( '/' ); slash != std::string::npos )
        file = file.substr( slash + 1 );

    return file;
}


TOOL_RESULT queryArea( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::QueryRadius req;
    double          radius = 0, x = 0, y = 0, limit = 0;

    if( !getNumber( aArgs, "radius", radius ) || radius <= 0 )
        return error( "radius (mm, > 0) is required" );

    req.set_radius( toNm( radius ) );

    if( aArgs.contains( "ref" ) && aArgs["ref"].is_string() )
        req.set_reference( aArgs["ref"].get<std::string>() );
    else if( getNumber( aArgs, "x", x ) && getNumber( aArgs, "y", y ) )
    {
        req.mutable_point()->set_x( toNm( x ) );
        req.mutable_point()->set_y( toNm( y ) );
    }
    else
        return error( "give either ref, or x and y (mm)" );

    for( const std::string& k : getStrings( aArgs, "kinds" ) )
    {
        auto it = kindNames().find( k );

        if( it == kindNames().end() )
            return error( "unknown kind '" + k + "' (footprint, pad, track, arc, via, zone)" );

        req.add_kinds( it->second );
    }

    if( getNumber( aArgs, "limit", limit ) )
        req.set_limit( static_cast<int>( limit ) );

    kb::QueryResponse r;
    std::string       err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    return { renderQuery( r, req.radius() ) };
}


TOOL_RESULT queryNet( IPC_CLIENT& aIpc, const json& aArgs )
{
    if( !aArgs.contains( "net" ) || !aArgs["net"].is_string() )
        return error( "net (name) is required" );

    kb::QueryNet req;
    req.set_net( aArgs["net"].get<std::string>() );

    if( double limit = 0; getNumber( aArgs, "limit", limit ) )
        req.set_limit( static_cast<int>( limit ) );

    kb::QueryResponse r;
    std::string       err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    return { renderQuery( r ) };
}


TOOL_RESULT moveFootprints( IPC_CLIENT& aIpc, const json& aArgs )
{
    std::vector<std::string> refs = getStrings( aArgs, "refs" );

    if( refs.empty() )
        return error( "refs (list of reference designators) is required" );

    double dx = 0, dy = 0, rot = 0;
    getNumber( aArgs, "dx", dx );
    getNumber( aArgs, "dy", dy );
    getNumber( aArgs, "rotate", rot );
    bool flip = aArgs.contains( "flip" ) && aArgs["flip"].is_boolean() && aArgs["flip"].get<bool>();

    if( dx == 0 && dy == 0 && rot == 0 && !flip )
        return error( "nothing to do: give dx, dy (mm), rotate (deg) and/or flip" );

    kb::MoveDelta req;

    for( const std::string& r : refs )
        req.add_refs( r );

    req.set_dx( toNm( dx ) );
    req.set_dy( toNm( dy ) );
    req.set_drot_decideg( static_cast<int>( std::lround( rot * 10 ) ) );
    req.set_flip( flip );
    req.set_group_pivot( aArgs.contains( "pivot" ) && aArgs["pivot"].is_string()
                         && aArgs["pivot"].get<std::string>() == "group" );

    kb::MoveResponse r;
    std::string      err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    return { renderMoveResult( r ), r.moved().empty() };
}


TOOL_RESULT deleteFootprints( IPC_CLIENT& aIpc, const json& aArgs )
{
    std::vector<std::string> refs = getStrings( aArgs, "refs" );

    if( refs.empty() )
        return error( "refs (list of reference designators) is required" );

    kb::DeleteFootprints req;

    for( const std::string& r : refs )
        req.add_refs( r );

    kb::MoveResponse r;
    std::string      err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    return { renderMoveResult( r ), r.removed().empty() };
}


TOOL_RESULT addFootprint( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::AddFootprint req;

    auto str = [&]( const char* aKey ) -> std::string
    {
        return aArgs.contains( aKey ) && aArgs[aKey].is_string() ? aArgs[aKey].get<std::string>() : "";
    };

    req.set_lib_id( str( "footprint" ) );
    req.set_lcsc( str( "lcsc" ) );
    req.set_like( str( "like" ) );
    req.set_ref( str( "ref" ) );
    req.set_value( str( "value" ) );

    if( req.lib_id().empty() && req.like().empty() && req.lcsc().empty() )
        return error( "give footprint (\"Library:Name\"), lcsc (\"C8734\") or like (an existing ref to copy)" );

    double x = 0, y = 0, rot = 0;

    if( !getNumber( aArgs, "x", x ) || !getNumber( aArgs, "y", y ) )
        return error( "x and y (mm) are required" );

    getNumber( aArgs, "rotate", rot );
    req.mutable_pos()->set_x( toNm( x ) );
    req.mutable_pos()->set_y( toNm( y ) );
    req.set_rot_decideg( static_cast<int>( std::lround( rot * 10 ) ) );
    req.set_back( aArgs.contains( "back" ) && aArgs["back"].is_boolean() && aArgs["back"].get<bool>() );

    if( aArgs.contains( "nets" ) && aArgs["nets"].is_object() )
    {
        for( const auto& [pad, net] : aArgs["nets"].items() )
        {
            if( !net.is_string() )
                return error( "nets: pad number -> net name, e.g. {\"1\": \"GND\", \"2\": \"+3V3\"}" );

            ( *req.mutable_nets() )[pad] = net.get<std::string>();
        }
    }

    kb::MoveResponse r;
    std::string      err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    return { renderMoveResult( r ), r.added().empty() };
}


TOOL_RESULT lcscFetch( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::FetchLcsc req;

    if( !aArgs.contains( "lcsc" ) || !aArgs["lcsc"].is_string() )
        return error( "lcsc (an LCSC part number, e.g. \"C8734\") is required" );

    req.set_lcsc( aArgs["lcsc"].get<std::string>() );
    req.set_refresh( aArgs.contains( "refresh" ) && aArgs["refresh"].is_boolean() && aArgs["refresh"].get<bool>() );

    kb::FetchLcscResponse r;
    std::string           err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    int tht = 0;

    for( const kb::LcscPad& p : r.pads() )
        tht += p.tht() ? 1 : 0;

    std::string out = r.lcsc() + " " + r.title() + ": footprint " + r.package();

    if( !r.lib_id().empty() )
        out += "\n  saved as " + r.lib_id() + " (" + r.file() + ")"
               + ( r.registered() ? "; LCSC library added to the project fp-lib-table" : "" );

    out += std::string( "\n  " ) + ( r.from_cache() ? "from the cache" : "fetched" ) + ", courtyard "
           + mm( r.courtyard().x() ) + "x" + mm( r.courtyard().y() ) + " mm, " + std::to_string( r.pads_size() )
           + " pads" + ( tht ? " (" + std::to_string( tht ) + " through-hole)" : " (SMD)" );

    if( r.has_symbol() )
    {
        const kb::LcscSymbol& s = r.symbol();
        out += "\n  symbol " + s.lib_id() + " (" + s.file() + "), reference " + s.reference() + ", "
               + std::to_string( s.pins_size() ) + " pins"
               + ( s.units() > 1 ? ", " + std::to_string( s.units() ) + " units" : "" )
               + ( s.registered() ? "; LCSC library added to the project sym-lib-table" : "" );

        auto list = []( const auto& aItems )
        {
            std::string t;

            for( const std::string& i : aItems )
                t += ( t.empty() ? "" : " " ) + i;

            return t;
        };

        if( s.pins_without_pad_size() )
            out += "\n  WARNING: symbol pins with no pad on the footprint: " + list( s.pins_without_pad() );

        if( s.pads_without_pin_size() )
            out += "\n  note: footprint pads no symbol pin uses: " + list( s.pads_without_pin() );
    }

    if( !r.description().empty() )
        out += "\n  " + r.description();

    out += "\n  pads (number@x,y mm):";
    int shown = 0;

    for( const kb::LcscPad& p : r.pads() )
    {
        if( ++shown > 120 )
        {
            out += " ... " + std::to_string( r.pads_size() - 120 ) + " more";
            break;
        }

        out += " " + p.number() + "@" + pt( p.pos() );
    }

    if( !r.datasheet().empty() )
        out += "\n  datasheet: " + r.datasheet();

    if( !r.note().empty() )
        out += "\n  note: " + r.note();

    out += "\n  " + r.attribution();
    return { out };
}


TOOL_RESULT history( IPC_CLIENT& aIpc, bool aUndo )
{
    kb::HistoryResponse r;
    std::string         err;
    bool                ok = aUndo ? aIpc.Call( kb::Undo(), r, err ) : aIpc.Call( kb::Redo(), r, err );

    if( !ok )
        return error( err );

    const char* verb = aUndo ? "undo" : "redo";

    if( !r.ok() && r.label().empty() )
        return { std::string( "nothing to " ) + verb + "\n" + renderHistory( r ) };

    if( !r.ok() )   // the entry was there, but everything in it had been changed by someone else
    {
        std::string why;

        for( const std::string& m : r.result().missing() )
            why += ( why.empty() ? "" : "; " ) + m;

        return { std::string( "did not " ) + verb + " \"" + r.label() + "\": " + why + "\n"
                 + renderHistory( r ) };
    }

    return { std::string( aUndo ? "undid: " : "redid: " ) + r.label() + "\n" + renderMoveResult( r.result() )
             + renderHistory( r ) };
}


TOOL_RESULT checkDrc( IPC_CLIENT& aIpc, const json& aArgs )
{
    std::vector<std::string> refs = getStrings( aArgs, "refs" );

    if( refs.empty() )
        return error( "refs (list of reference designators) is required" );

    kb::CheckLocalDrc req;

    for( const std::string& r : refs )
        req.add_refs( r );

    kb::LocalDrcResponse r;
    std::string          err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s;

    if( !r.missing().empty() )
    {
        s += "not found:";

        for( const std::string& m : r.missing() )
            s += " " + m;

        s += "\n";
    }

    return { s + renderDrc( r.drc() ) };
}


TOOL_RESULT flushRatsnest( IPC_CLIENT& aIpc )
{
    kb::FlushRatsnestResponse r;
    std::string               err;

    if( !aIpc.Call( kb::FlushRatsnest(), r, err ) )
        return error( err );

    return { "rebuilt " + std::to_string( r.nets_flushed() ) + " deferred nets; unconnected="
             + std::to_string( r.unconnected() ) + "\n" };
}


TOOL_RESULT saveBoard( IPC_CLIENT& aIpc )
{
    std::string err;
    std::string file = boardFileName( aIpc, err );

    if( file.empty() )
        return error( err.empty() ? "no board loaded" : err );

    kiapi::common::commands::SaveDocument req;
    req.mutable_document()->set_type( kiapi::common::types::DOCTYPE_PCB );
    req.mutable_document()->set_board_filename( file );

    google::protobuf::Empty r;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    return { "saved " + file + "\n" };
}


TOOL_RESULT runDrc( IPC_CLIENT& aIpc, const json& aArgs )
{
    double limit = 40;
    getNumber( aArgs, "limit", limit );
    const bool warnings = !aArgs.contains( "include_warnings" ) || aArgs["include_warnings"].get<bool>();

    std::vector<std::string> typeList = getStrings( aArgs, "types" );
    std::vector<std::string> refList = getStrings( aArgs, "refs" );
    std::set<std::string>    types( typeList.begin(), typeList.end() ), refs( refList.begin(), refList.end() );

    DRC_REPORT  report;
    std::string err;

    if( !RunFullDrc( aIpc, report, err ) )
        return error( err );

    auto involves = [&]( const DRC_ENTRY& e )
    {
        if( refs.empty() )
            return true;

        for( const std::string* h : { &e.a, &e.b } )
        {
            std::string ref = h->substr( 0, h->find_first_of( ".:" ) );

            if( refs.count( ref ) )
                return true;
        }

        return false;
    };

    auto counts = []( const std::map<std::string, int>& m )
    {
        std::string s;

        for( const auto& [k, v] : m )
            s += ( s.empty() ? "" : ", " ) + k + " " + std::to_string( v );

        return s.empty() ? std::string( "none" ) : s;
    };

    int nErr = 0, nWarn = 0;

    for( const auto& [k, v] : report.errors )
        nErr += v;

    for( const auto& [k, v] : report.warnings )
        nWarn += v;

    char head[160];
    std::snprintf( head, sizeof( head ), "full DRC %.1f s: %d errors, %d warnings (%d excluded, %d ignored)\n",
                   report.seconds, nErr, nWarn, report.excluded, report.ignored );

    std::string s = head;
    s += "errors: " + counts( report.errors ) + "\n";
    s += "warnings: " + counts( report.warnings ) + "\n";

    int shown = 0, matching = 0;

    for( const DRC_ENTRY& e : report.entries )
    {
        if( ( e.warning && !warnings ) || ( !types.empty() && !types.count( e.type ) ) || !involves( e ) )
            continue;

        if( matching++ >= static_cast<int>( limit ) )
            continue;

        s += std::string( e.warning ? "W " : "E " ) + e.type + " " + e.a;

        if( !e.netA.empty() )
            s += "[" + e.netA + "]";

        if( !e.b.empty() )
        {
            s += " x " + e.b;

            if( !e.netB.empty() )
                s += "[" + e.netB + "]";
        }

        if( !e.layer.empty() )
            s += " " + e.layer;

        s += " @" + mm( e.x ) + "," + mm( e.y );

        if( !e.description.empty() )
            s += " " + e.description;

        s += "\n";
        ++shown;
    }

    if( matching > shown )
        s += "... " + std::to_string( matching - shown ) + " more (raise limit or filter by types/refs)\n";

    return { s };
}


TOOL_RESULT refillZones( IPC_CLIENT& aIpc )
{
    std::string err;
    std::string file = boardFileName( aIpc, err );

    if( file.empty() )
        return error( err.empty() ? "no board loaded" : err );

    kiapi::board::commands::RefillZones req;
    req.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    req.mutable_board()->set_board_filename( file );

    google::protobuf::Empty r;
    auto                    t0 = std::chrono::steady_clock::now();

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    char buf[64];
    std::snprintf( buf, sizeof( buf ), "all zones refilled in %.1f s\n",
                   std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
    return { buf };
}


TOOL_RESULT openBoard( IPC_CLIENT& aIpc, const json& aArgs )
{
    if( !aArgs.contains( "path" ) || !aArgs["path"].is_string() )
        return error( "path (absolute path to .kicad_pcb or .kicad_pro) is required" );

    std::string path = aArgs["path"].get<std::string>();

    kiapi::common::commands::OpenDocument req;
    req.set_type( path.ends_with( ".kicad_pro" ) ? kiapi::common::types::DOCTYPE_PROJECT
                                                : kiapi::common::types::DOCTYPE_PCB );
    req.set_path( path );

    kiapi::common::commands::OpenDocumentResponse r;
    std::string                                   err;

    if( !aIpc.Call( req, r, err ) )
    {
        if( err.starts_with( "AS_UNIMPLEMENTED" ) )
            err += " (in the desktop editor, open the file in pcbnew; the agent works on the "
                   "board that is open there)";

        return error( err );
    }

    return boardSummary( aIpc );
}


json schema( json aProps, std::vector<std::string> aRequired = {} )
{
    json s = { { "type", "object" }, { "properties", std::move( aProps ) } };

    if( !aRequired.empty() )
        s["required"] = aRequired;

    return s;
}

} // namespace


std::vector<TOOL> MakeTools( IPC_CLIENT& aIpc )
{
    const json kinds = { { "type", "array" },
                         { "items", { { "type", "string" },
                                      { "enum", { "footprint", "pad", "track", "arc", "via", "zone" } } } },
                         { "description", "Item kinds to include (default: footprint, pad, track, arc, via)" } };

    const json refs = { { "type", "array" }, { "items", { { "type", "string" } } },
                        { "description", "Reference designators, e.g. [\"U5\", \"C12\"]" } };

    std::vector<TOOL> tools;

    tools.push_back( { "board_summary",
                       "Counts, layer count and board outline of the open PCB. Units: mm. KiCad's Y axis "
                       "points down.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return boardSummary( aIpc ); } } );

    tools.push_back( { "query_area",
                       "Items near a footprint (ref) or a point (x,y), nearest first. Lines: KIND handle [net] "
                       "layer @x,y (pads are REF.PADNUM; tracks start>end wWIDTH; ~DIST from the centre). "
                       "Use this instead of dumping the board.",
                       schema( { { "ref", { { "type", "string" }, { "description", "Centre on this footprint" } } },
                                 { "x", { { "type", "number" }, { "description", "Centre X, mm" } } },
                                 { "y", { { "type", "number" }, { "description", "Centre Y, mm" } } },
                                 { "radius", { { "type", "number" }, { "description", "Radius, mm" } } },
                                 { "kinds", kinds },
                                 { "limit", { { "type", "integer" }, { "description", "Max items (default 64)" } } } },
                               { "radius" } ),
                       [&aIpc]( const json& a ) { return queryArea( aIpc, a ); } } );

    tools.push_back( { "query_net",
                       "Pads, tracks, vias and zones on one net (topology of a single net).",
                       schema( { { "net", { { "type", "string" }, { "description", "Net name, e.g. GND or /USB/D+" } } },
                                 { "limit", { { "type", "integer" }, { "description", "Max items (default 256)" } } } },
                               { "net" } ),
                       [&aIpc]( const json& a ) { return queryNet( aIpc, a ); } } );

    tools.push_back( { "move_footprints",
                       "Move/rotate/flip footprints as one undoable edit (move, then rotate, then flip; each "
                       "around its own anchor, or as a group with pivot=group). Returns new positions (B.Cu = bottom side), the "
                       "unrouted-connection count and a local DRC of the moved parts (clearance, shorts, "
                       "hole clearance, courtyard overlap, board-edge clearance, copper outside the board). "
                       "Large nets (GND, power) skip ratsnest updates while you iterate; the count says which.",
                       schema( { { "refs", refs },
                                 { "dx", { { "type", "number" }, { "description", "Delta X, mm" } } },
                                 { "dy", { { "type", "number" }, { "description", "Delta Y, mm (down is +)" } } },
                                 { "rotate", { { "type", "number" }, { "description", "Rotation delta, degrees CCW" } } },
                                 { "flip", { { "type", "boolean" },
                                             { "description", "Move to the other side (F.Cu <-> B.Cu), mirrored left-right" } } },
                                 { "pivot", { { "type", "string" }, { "enum", { "each", "group" } },
                                              { "description", "Rotate/flip each part around its own anchor (default) or "
                                                               "the parts as one group around their common centre" } } } },
                               { "refs" } ),
                       [&aIpc]( const json& a ) { return moveFootprints( aIpc, a ); } } );

    tools.push_back( { "lcsc_fetch",
                       "Get the footprint and schematic symbol of an LCSC part (e.g. C8734) from the "
                       "JLCEDA/EasyEDA Official Library: converted to KiCad, saved to the project "
                       "library LCSC (lcsc.pretty + lcsc.kicad_sym, added to fp-lib-table and "
                       "sym-lib-table; the symbol's Footprint field points at the footprint), cached for "
                       "offline use. Returns both lib_ids, size, pad numbers/positions and any pin/pad "
                       "mismatch. add_footprint with lcsc does this by itself.",
                       schema( { { "lcsc", { { "type", "string" }, { "description", "LCSC part number, e.g. C8734" } } },
                                 { "refresh", { { "type", "boolean" }, { "description", "Fetch again even if cached" } } } },
                               { "lcsc" } ),
                       [&aIpc]( const json& a ) { return lcscFetch( aIpc, a ); } } );

    tools.push_back( { "add_footprint",
                       "Place a new footprint as one undoable edit: from a library (footprint = "
                       "\"Library:Name\", e.g. \"Capacitor_SMD:C_0603_1608Metric\"), from an LCSC part "
                       "number (lcsc = \"C8734\", fetched and saved to the project library), or as a copy "
                       "of a part already on the board (like = \"C12\"). ref \"C?\" takes the next free number. "
                       "nets maps pad numbers to nets; unknown nets are created and reported. Returns the "
                       "placement, unrouted count and local DRC.",
                       schema( { { "footprint", { { "type", "string" }, { "description", "Library:Name" } } },
                                 { "lcsc", { { "type", "string" }, { "description", "LCSC part number, e.g. C8734 (instead of footprint)" } } },
                                 { "like", { { "type", "string" }, { "description", "Copy the footprint of this ref instead" } } },
                                 { "ref", { { "type", "string" }, { "description", "Reference, e.g. C301 or C? (default: prefix of like)" } } },
                                 { "value", { { "type", "string" }, { "description", "Value, e.g. 100nF" } } },
                                 { "x", { { "type", "number" }, { "description", "Position X, mm" } } },
                                 { "y", { { "type", "number" }, { "description", "Position Y, mm" } } },
                                 { "rotate", { { "type", "number" }, { "description", "Orientation, degrees CCW" } } },
                                 { "back", { { "type", "boolean" }, { "description", "Place on the bottom side" } } },
                                 { "nets", { { "type", "object" }, { "additionalProperties", { { "type", "string" } } },
                                             { "description", "Pad number -> net name, e.g. {\"1\": \"+3V3\", \"2\": \"GND\"}" } } } },
                               { "x", "y" } ),
                       [&aIpc]( const json& a ) { return addFootprint( aIpc, a ); } } );

    tools.push_back( { "delete_footprints",
                       "Delete footprints as one undoable edit (undo restores them with their nets). "
                       "Tracks and vias stay. Returns the new unrouted count.",
                       schema( { { "refs", refs } }, { "refs" } ),
                       [&aIpc]( const json& a ) { return deleteFootprints( aIpc, a ); } } );

    tools.push_back( { "undo",
                       "Undo the last KiCadD edit (move/rotate/flip/add/delete), restoring the exact previous "
                       "state. Returns what was undone, new positions and local DRC.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return history( aIpc, true ); } } );

    tools.push_back( { "redo",
                       "Redo the last undone edit.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return history( aIpc, false ); } } );

    tools.push_back( { "check_drc",
                       "Local DRC for the given footprints against their surroundings, without moving them.",
                       schema( { { "refs", refs } }, { "refs" } ),
                       [&aIpc]( const json& a ) { return checkDrc( aIpc, a ); } } );

    tools.push_back( { "flush_ratsnest",
                       "Rebuild the ratsnest of large nets deferred during moves; returns the complete "
                       "unrouted-connection count.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return flushRatsnest( aIpc ); } } );

    tools.push_back( { "run_drc",
                       "Full-board DRC (all KiCad checks: clearance, shorts, unconnected, courtyards, "
                       "silk, edge, holes, ...). Takes seconds on large boards. Output: counts by type, "
                       "then violations (errors first) as 'E|W type itemA[net] x itemB[net] layer @x,y "
                       "detail'. Refill zones first if parts moved.",
                       schema( { { "limit", { { "type", "integer" }, { "description", "Max violations listed (default 40)" } } },
                                 { "include_warnings", { { "type", "boolean" }, { "description", "Default true" } } },
                                 { "types", { { "type", "array" }, { "items", { { "type", "string" } } },
                                              { "description", "Only these types, e.g. [\"clearance\", \"shorting_items\"]" } } },
                                 { "refs", { { "type", "array" }, { "items", { { "type", "string" } } },
                                             { "description", "Only violations involving these footprints" } } } } ),
                       [&aIpc]( const json& a ) { return runDrc( aIpc, a ); } } );

    tools.push_back( { "refill_zones",
                       "Refill all copper zones. Fills go stale when parts move; refill before judging "
                       "copper connectivity, running full DRC or saving for fabrication.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return refillZones( aIpc ); } } );

    tools.push_back( { "open_board",
                       "Open a .kicad_pcb (or .kicad_pro) on the server (kicad-cli api-server), replacing the current board. "
                       "Not available with the desktop editor: there the user opens the file in pcbnew.",
                       schema( { { "path", { { "type", "string" }, { "description", "Absolute path on the server" } } } },
                               { "path" } ),
                       [&aIpc]( const json& a ) { return openBoard( aIpc, a ); } } );

    tools.push_back( { "save_board",
                       "Write the in-memory board back to its .kicad_pcb file.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return saveBoard( aIpc ); } } );

    return tools;
}

} // namespace kicadd::mcp
