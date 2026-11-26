/*
 * KiCadD MCP - tool definitions and compact text rendering.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "drc.h"
#include "ipc_client.h"

#include <algorithm>
#include <fstream>
#include <cctype>
#include <set>

#include <cmath>
#include <filesystem>
#include <cstdio>
#include <map>
#include <memory>

#include <kicadd/board.pb.h>
#include <kicadd/render.pb.h>
#include <kicadd/schematic.pb.h>
#include <api/common/commands/editor_commands.pb.h>
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

    // pad copper as it lies on the board: where a via or track can go beside it
    if( aItem.kind() == kb::IK_PAD && aItem.size().x() > 0 )
        s += " " + mm( aItem.size().x() ) + "x" + mm( aItem.size().y() );

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


// Brief output (the default): long lists are cut to their first lines; detail=true lists all.
constexpr int BRIEF_PARTS = 8;    // more moved parts than this: the first BRIEF_SHOWN, then a count
constexpr int BRIEF_SHOWN = 5;
constexpr int BRIEF_DRC = 10;


bool detailOf( const json& aArgs )
{
    return aArgs.is_object() && aArgs.contains( "detail" ) && aArgs["detail"].is_boolean()
           && aArgs["detail"].get<bool>();
}


std::string renderDrc( const google::protobuf::RepeatedPtrField<kb::DrcViolation>& aDrc, bool aDetail = false )
{
    if( aDrc.empty() )
        return "drc: clean\n";

    std::string s = "drc: " + std::to_string( aDrc.size() );

    if( !aDetail && aDrc.size() > BRIEF_DRC )
    {
        std::map<std::string, int> byType;

        for( const kb::DrcViolation& v : aDrc )
        {
            switch( v.type() )
            {
            case kb::DT_CLEARANCE:             byType["clearance"]++; break;
            case kb::DT_SHORT:                 byType["short"]++; break;
            case kb::DT_HOLE_CLEARANCE:        byType["hole"]++; break;
            case kb::DT_COURTYARD_OVERLAP:     byType["courtyard"]++; break;
            case kb::DT_EDGE_CLEARANCE:        byType["edge"]++; break;
            case kb::DT_COPPER_OUTSIDE_BOARD:  byType["outside_board"]++; break;
            case kb::DT_HOLE_TO_HOLE:          byType["hole_to_hole"]++; break;
            default:                           byType["other"]++; break;
            }
        }

        std::string types;

        for( const auto& [k, n] : byType )
            types += ( types.empty() ? "" : ", " ) + k + " " + std::to_string( n );

        s += " (" + types + ")";
    }

    s += "\n";
    int shown = 0;

    for( const kb::DrcViolation& v : aDrc )
    {
        if( !aDetail && shown++ >= BRIEF_DRC )
        {
            s += "... " + std::to_string( aDrc.size() - BRIEF_DRC ) + " more (detail=true lists all)\n";
            break;
        }

        switch( v.type() )
        {
        case kb::DT_CLEARANCE:         s += "CLEARANCE "; break;
        case kb::DT_SHORT:             s += "SHORT "; break;
        case kb::DT_HOLE_CLEARANCE:    s += "HOLE "; break;
        case kb::DT_COURTYARD_OVERLAP: s += "COURTYARD "; break;
        case kb::DT_EDGE_CLEARANCE:    s += "EDGE "; break;
        case kb::DT_HOLE_TO_HOLE:      s += "HOLE_TO_HOLE "; break;
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


/// "unconnected=111 (incl. GND 24)" / "unconnected=87 (excl. deferred nets: GND; ...)": every
/// tool says the same thing the same way, so a count that moves because a large net was rebuilt
/// (route_short, fanout and stitch_vias read GND's ratsnest; moves defer it) reads as such.
std::string renderUnconnected( int aCount, const google::protobuf::RepeatedPtrField<std::string>& aPending,
                               const google::protobuf::RepeatedPtrField<std::string>& aLarge )
{
    std::string s = "unconnected=" + std::to_string( aCount );

    if( !aLarge.empty() )
    {
        s += " (incl.";

        for( const std::string& n : aLarge )
            s += " " + n;

        s += ")";
    }

    if( !aPending.empty() )
    {
        s += " (excl. deferred nets:";

        for( const std::string& n : aPending )
            s += " " + n;

        s += "; board_summary gives the full count)";
    }

    return s;
}


template <typename MSG>
std::string unconnectedOf( const MSG& r )
{
    return renderUnconnected( r.unconnected(), r.ratsnest_pending(), r.large_included() );
}


std::string renderMoveResult( const kb::MoveResponse& r, bool aDetail = false )
{
    std::string s;

    // The part's ref goes first (batch shows the first line): "C11 added @..., from copy of C10"
    const bool sourceInline = !r.source().empty() && r.added_size() == 1 && r.moved_size() == 1;

    if( !aDetail && r.removed_size() > BRIEF_PARTS )
    {
        std::string list;

        for( int i = 0; i < BRIEF_SHOWN; ++i )
            list += ( i ? ", " : "" ) + r.removed( i );

        s += std::to_string( r.removed_size() ) + " deleted: " + list + ", ... (detail=true lists all)\n";
    }
    else
    {
        for( const std::string& ref : r.removed() )
            s += ref + " deleted\n";
    }

    if( r.tracks_removed() > 0 )
        s += std::to_string( r.tracks_removed() ) + " track(s)/via(s) " + ( r.history_label().empty() ? "removed" : "deleted" ) + "\n";

    if( r.tracks_locked() > 0 )
        s += std::to_string( r.tracks_locked() ) + " locked track(s)/via(s) kept (include_locked deletes them too)\n";

    int shown = 0;

    for( const kb::MoveResult& m : r.moved() )
    {
        if( !aDetail && r.moved_size() > BRIEF_PARTS && shown++ >= BRIEF_SHOWN )
        {
            s += "... " + std::to_string( r.moved_size() - BRIEF_SHOWN ) + " more part(s) "
                 + ( r.added_size() == r.moved_size() ? "added" : "moved" ) + " (detail=true lists all)\n";
            break;
        }

        bool added = std::find( r.added().begin(), r.added().end(), m.ref() ) != r.added().end();
        s += m.ref() + ( added ? " added" : "" ) + " @" + pt( m.pos() )
             + ( m.rot_decideg() ? " r" + deg( m.rot_decideg() ) : "" ) + ( m.back() ? " B.Cu" : "" )
             + ( added && sourceInline ? ", from " + r.source() : "" ) + "\n";
    }

    if( !r.source().empty() && !sourceInline )
        s += "from " + r.source() + "\n";

    if( !r.created_nets().empty() )
    {
        s += "new nets:";

        for( const std::string& n : r.created_nets() )
            s += " " + n;

        s += "\n";
    }

    if( !r.removed_nets().empty() )
    {
        s += "nets left with nothing on them, removed:";

        for( const std::string& n : r.removed_nets() )
            s += " " + n;

        s += "\n";
    }

    if( !r.missing().empty() )
    {
        s += "not found:";

        for( const std::string& m : r.missing() )
            s += " " + m;

        s += "\n";
    }

    s += unconnectedOf( r ) + "\n";

    for( const std::string& n : r.notes() )
        s += "note: " + n + "\n";

    // the local DRC looks around the parts that moved; with none it looked at nothing
    if( r.moved().empty() && r.drc().empty() )
        s += "drc: not checked here (no part moved; run_drc checks the board)\n";
    else if( r.on_unfilled_pour() && r.drc().empty() )
        s += "drc: incomplete: nothing found around the moved parts, but they are on a pour that is not refilled "
             "(checked against its old shape): refill_zones, then run_drc\n";
    else
        s += ( r.on_unfilled_pour() ? "zones not refilled: the DRC below is against a pour's old shape (refill_zones, then run_drc)\n" : "" )
             + renderDrc( r.drc(), aDetail );

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

    if( size_t slash = file.find_last_of( "/\\" ); slash != std::string::npos )
        file = file.substr( slash + 1 );

    // the full unrouted count (deferred large nets rebuilt), without waiting out a busy editor
    // or a running auto_place: board_summary must answer at once
    kb::FlushRatsnestResponse f;
    std::string               ferr, rats;

    if( aIpc.Call( kb::FlushRatsnest(), f, ferr, false ) )
        rats = "ratsnest: " + std::string( f.nets_flushed() ? "rebuilt " + std::to_string( f.nets_flushed() ) + " deferred nets; " : "" )
               + renderUnconnected( f.unconnected(), {}, f.large_included() ) + "\n";
    else if( !ferr.empty() )
        rats = "ratsnest: not rebuilt now (" + ferr.substr( 0, ferr.find( ';' ) ) + ")\n";

    // vias by kind (a fab's drill and aspect-ratio limits are the human's: measured, not judged)
    std::string viaLine;

    if( r.via_kinds_size() > 0 )
    {
        int minThrough = 0;

        for( int k = 0; k < r.via_kinds_size(); ++k )
        {
            const kb::ViaKind& v = r.via_kinds( k );

            if( k < 5 )
                viaLine += std::string( k ? ", " : "" ) + v.type() + " " + mm( v.size_nm() ) + "/" + mm( v.drill_nm() )
                           + " x" + std::to_string( v.count() );

            if( v.type() == "through" && v.drill_nm() > 0 && ( !minThrough || v.drill_nm() < minThrough ) )
                minThrough = (int) v.drill_nm();
        }

        if( r.via_kinds_size() > 5 )
            viaLine += ", " + std::to_string( r.via_kinds_size() - 5 ) + " more kind(s)";

        viaLine = "vias (pad/drill mm): " + viaLine;

        if( minThrough && r.thickness_nm() > 0 )
        {
            char buf[32];
            std::snprintf( buf, sizeof buf, "%.1f", (double) r.thickness_nm() / minThrough );
            viaLine += "; board " + mm( r.thickness_nm() ) + " mm thick: through-via aspect ratio up to " + buf
                       + ":1 (thickness / drill)";
        }

        viaLine += "\n";
    }

    return { "file=" + file + " footprints=" + std::to_string( r.footprints() )
             + " tracks=" + std::to_string( r.tracks() ) + " vias=" + std::to_string( r.vias() )
             + " zones=" + std::to_string( r.zones() ) + " nets=" + std::to_string( r.nets() )
             + " cu_layers=" + std::to_string( r.copper_layers() )
             + " outline=" + pt( r.bbox_min() ) + ".." + pt( r.bbox_max() ) + "mm\n"
             + ( r.agent_paused() ? "note: the user paused the agent's edits (Co-pilot panel): edits are refused "
                                    "until resumed; reads work\n"
                                  : "" ) + viaLine + rats };
}


std::string boardFileName( IPC_CLIENT& aIpc, std::string& aErr )
{
    kb::BoardSummaryResponse r;

    if( !aIpc.Call( kb::GetBoardSummary(), r, aErr ) )
        return {};

    std::string file = r.file();

    if( size_t slash = file.find_last_of( "/\\" ); slash != std::string::npos )
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

    if( aArgs.contains( "via_net" ) && aArgs["via_net"].is_string() )
        req.set_via_net( aArgs["via_net"].get<std::string>() );

    kb::QueryResponse r;
    std::string       err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    if( req.via_net().empty() )
        return { renderQuery( r, req.radius() ) };

    // via spots: one line, nearest first
    std::string s = "via " + req.via_net() + " d" + mm( r.via_diameter() ) + "/" + mm( r.via_drill() )
                    + ": " + std::to_string( r.via_spots_size() ) + " spot(s)";

    if( r.via_spots_size() == 0 && r.via_tested() == 0 )
        return { s + ": the circle is off the board, or all on pads\n" };

    if( r.via_spots_size() == 0 )
        return { s + ", none free within " + mm( req.radius() ) + " mm (" + std::to_string( r.via_tested() )
                 + " grid points tested: each clashes with other-net copper on some layer or the edge). "
                   "Try a larger radius.\n" };

    s += ", nearest first:\n";
    int ring = 0;

    for( int i = 0; i < r.via_spots_size(); ++i )
    {
        const bool in = i < r.via_in_ring_size() && r.via_in_ring( i );
        s += ( i ? " " : "" ) + std::string( "@" ) + pt( r.via_spots( i ) ) + "~" + mm( r.via_dist( i ) ) + ( in ? "*" : "" );
        ring += in;
    }

    if( ring )
        s += "\n*: inside an exposed-pad IC's pad ring (under its body, off its pads): a via there wants tenting "
             "(the fab's default), and leaves less room for the escape";

    return { s + "\n" };
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

    // absolute: parts {"U1": {x, y, rotation, side}}
    if( aArgs.contains( "parts" ) )
    {
        if( !refs.empty() || aArgs.contains( "dx" ) || aArgs.contains( "dy" ) || aArgs.contains( "rotate" ) || aArgs.contains( "flip" ) )
            return error( "parts (absolute places) or refs with dx / dy / rotate / flip, not both" );

        if( !aArgs["parts"].is_object() || aArgs["parts"].empty() )
            return error( "parts: {\"U1\": {\"x\": 105, \"y\": 120, \"rotation\": 90, \"side\": \"bottom\"}} (mm, degrees)" );

        kb::MoveDelta req;

        for( const auto& [ref, p] : aArgs["parts"].items() )
        {
            if( !p.is_object() )
                return error( "parts." + ref + ": {x, y, rotation, side}" );

            kb::PartPose* pose = req.add_poses();
            pose->set_ref( ref );

            for( const auto& [k, v] : p.items() )
            {
                if( k == "x" && v.is_number() )
                    pose->set_x( toNm( v.get<double>() ) );
                else if( k == "y" && v.is_number() )
                    pose->set_y( toNm( v.get<double>() ) );
                else if( k == "rotation" && v.is_number() )
                    pose->set_rot_decideg( (int) std::lround( std::remainder( v.get<double>(), 360.0 ) * 10 ) );
                else if( k == "side" && v.is_string() && ( v == "top" || v == "bottom" || v == "front" || v == "back" ) )
                    pose->set_back( v == "bottom" || v == "back" );
                else
                    return error( "parts." + ref + "." + k + ": x / y (mm), rotation (degrees), side (\"top\" / \"bottom\")" );
            }
        }

        kb::MoveResponse r;
        std::string      err;

        if( !aIpc.Call( req, r, err ) )
            return error( err );

        if( r.moved().empty() && r.missing().empty() )
            return { "nothing to do: every part is already there\n" };

        return { renderMoveResult( r, detailOf( aArgs ) ), r.moved().empty() };
    }

    if( refs.empty() )
        return error( "refs (with dx / dy / rotate / flip) or parts (absolute places) is required" );

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

    return { renderMoveResult( r, detailOf( aArgs ) ), r.moved().empty() };
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

    return { renderMoveResult( r, detailOf( aArgs ) ), r.removed().empty() };
}


/// A route end from "U1.5" or [x, y] (mm)
bool routeEnd( const json& aArgs, const char* aKey, kb::RouteEnd& aOut, std::string& aErr )
{
    const json& v = aArgs[aKey];

    if( v.is_string() && v.get<std::string>() == "zone" && std::string( aKey ) == "to" )
    {
        aOut.set_zone( true );
        return true;
    }

    if( v.is_string() )
    {
        aOut.set_pad( v.get<std::string>() );
        return true;
    }

    if( v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number() )
    {
        aOut.mutable_pos()->set_x( std::llround( v[0].get<double>() * 1e6 ) );
        aOut.mutable_pos()->set_y( std::llround( v[1].get<double>() * 1e6 ) );
        return true;
    }

    aErr = std::string( aKey ) + ": a pad \"U1.5\" or a point [x, y] in mm";
    return false;
}


std::string ratio( int64_t aLength, int64_t aDirect )
{
    if( aDirect <= 0 )
        return "";

    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.2fx", (double) aLength / (double) aDirect );
    return buf;
}


std::string renderRoute( const kb::RouteResult& r, bool aDryRun )
{
    std::string s = r.from() + " -> " + r.to() + " [" + r.net() + "]: ";

    if( !r.ok() )
    {
        s += "not routed: " + r.error();

        if( r.has_reached() )
            s += "; got to " + pt( r.reached() );

        if( !r.blocked_by().empty() )
            s += "; in the way: " + r.blocked_by() + ( r.has_blocker_at() ? " (at " + pt( r.blocker_at() ) + ")" : "" )
                 + ( r.has_blocked_at() ? ", met at " + pt( r.blocked_at() ) : "" );

        return s + "\n";
    }

    // only a via (on the start point): no track
    if( r.segments() == 0 && r.vias() > 0 && r.length() == 0 )
    {
        s += std::string( aDryRun ? "would place" : "placed" ) + " " + std::to_string( r.vias() ) + " via(s), "
             + r.layer();

        if( r.min_gap() >= 0 )
            s += ", min gap " + mm( r.min_gap() ) + " to " + r.gap_to();

        return s + "\n";
    }

    // against the straight line from end to end; with waypoints, against the path through them too
    const bool viaStops = r.ends() > 0 && r.direct() > r.ends() + 1000;
    const std::string straight = ratio( r.length(), r.ends() > 0 ? r.ends() : r.direct() );
    s += std::string( aDryRun ? "would route" : "routed" ) + " on " + r.layer() + ", " + mm( r.length() ) + " mm"
         + ( straight.empty() || r.length() == 0 ? std::string()
                                                : " (" + straight + " the straight line"
                                                          + ( viaStops ? ", " + ratio( r.length(), r.direct() ) + " the path through the waypoints" : "" )
                                                          + ")" )
         + ", " + std::to_string( r.segments() ) + " segment(s), "
         + std::to_string( r.vias() ) + " via(s), width " + mm( r.width() )
         + ( r.width_from().empty() || r.width_from() == "asked" ? std::string() : " (" + r.width_from() + ")" )
         + ( r.neck().empty() ? std::string() : ", necked to " + r.neck() );

    if( r.min_gap() >= 0 )
        s += ", min gap " + mm( r.min_gap() ) + " to " + r.gap_to();

    // only with waypoints: without them going round things is what a route does
    if( r.stray() > 0 && viaStops )
        s += "; it leaves the way through the waypoints by " + mm( r.stray() ) + " mm at " + pt( r.stray_at() )
             + " (round something in the way; strict=true refuses that)";

    return s + "\n";
}


TOOL_RESULT routeTrack( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::RouteTrack req;
    std::string    err;

    if( !aArgs.contains( "from" ) || !routeEnd( aArgs, "from", *req.mutable_from(), err ) )
        return error( err.empty() ? "from: a pad \"U1.5\" or a point [x, y] in mm" : err );

    if( aArgs.contains( "to" ) && !routeEnd( aArgs, "to", *req.mutable_to(), err ) )
        return error( err );

    req.set_layer( aArgs.value( "layer", std::string() ) );
    req.set_dry_run( aArgs.value( "dry_run", false ) );
    req.set_strict( aArgs.value( "strict", false ) );

    if( double w = 0; getNumber( aArgs, "width", w ) )
        req.set_width( std::llround( w * 1e6 ) );

    auto addStop = [&]( const json& p, const std::string& aVia ) -> bool
    {
        if( !p.is_array() || p.size() < 2 || p.size() > 3 || !p[0].is_number() || !p[1].is_number()
            || ( p.size() == 3 && !p[2].is_string() ) )
            return false;

        kb::RouteStop* st = req.add_stops();
        st->mutable_pos()->set_x( std::llround( p[0].get<double>() * 1e6 ) );
        st->mutable_pos()->set_y( std::llround( p[1].get<double>() * 1e6 ) );
        st->set_via_to( p.size() == 3 ? p[2].get<std::string>() : aVia );
        return true;
    };

    if( aArgs.contains( "waypoints" ) )
    {
        if( !aArgs["waypoints"].is_array() )
            return error( "waypoints: [[x, y], [x, y, \"B.Cu\"], ...] in mm" );

        for( const json& p : aArgs["waypoints"] )
        {
            if( !addStop( p, "" ) )
                return error( "waypoints: [[x, y], [x, y, \"B.Cu\"], ...] in mm (a layer: a via there to it)" );
        }
    }

    if( aArgs.contains( "via" ) )
    {
        if( req.stops_size() )
            return error( "via is short for one via; with waypoints, put it among them: [x, y, \"B.Cu\"]" );

        if( !addStop( aArgs["via"], "other" ) )
            return error( "via: [x, y] in mm" );
    }

    kb::RouteResponse r;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s;
    bool        ok = true;

    for( const kb::RouteResult& rr : r.results() )
    {
        s += renderRoute( rr, r.dry_run() );
        ok = ok && rr.ok();
    }

    if( !r.drc().empty() )
        s += "the rules it would break:\n" + renderDrc( r.drc(), true );

    if( !r.history_label().empty() )
        s += unconnectedOf( r ) + "; undo: \"" + r.history_label()
             + "\"; refill_zones before run_drc\n";
    else if( !r.dry_run() )
        s += "nothing changed; " + unconnectedOf( r ) + "\n";

    return TOOL_RESULT{ s, !ok && !r.dry_run() };
}


TOOL_RESULT refillZones( IPC_CLIENT& aIpc );


/// Many routes: the failures in full, the successes in brief unless detail
std::string renderRoutes( const kb::RouteResponse& r, bool aDetail, const char* aWhat )
{
    int     ok = 0, vias = 0;
    int64_t length = 0, minGap = -1;
    double  worst = 0;
    std::string worstName, gapName, failed, done;

    for( const kb::RouteResult& rr : r.results() )
    {
        if( !rr.ok() )
        {
            failed += "  " + renderRoute( rr, r.dry_run() );
            continue;
        }

        ok++;
        vias += rr.vias();
        length += rr.length();

        if( rr.direct() > 0 && (double) rr.length() / rr.direct() > worst )
        {
            worst = (double) rr.length() / rr.direct();
            worstName = rr.from() + " -> " + rr.to();
        }

        if( rr.min_gap() >= 0 && ( minGap < 0 || rr.min_gap() < minGap ) )
        {
            minGap = rr.min_gap();
            gapName = rr.from() + " -> " + rr.to() + " to " + rr.gap_to();
        }

        done += "  " + renderRoute( rr, r.dry_run() );
    }

    const int total = r.results_size();
    std::string s = std::string( r.dry_run() ? "dry run: " : "" ) + std::to_string( total ) + " " + aWhat + ", "
                    + std::to_string( ok ) + ( r.dry_run() ? " would be routed" : " routed" ) + ", "
                    + std::to_string( total - ok ) + " not\n";

    if( ok )
    {
        char buf[64];
        std::snprintf( buf, sizeof( buf ), "%.2fx", worst );
        s += "  " + mm( length ) + " mm of track, " + std::to_string( vias ) + " via(s); most detour " + buf
             + " (" + worstName + ")" + ( minGap >= 0 ? "; min gap " + mm( minGap ) + " (" + gapName + ")" : "" ) + "\n";
    }

    if( !failed.empty() )
        s += "not routed:\n" + failed;

    if( aDetail || ok <= BRIEF_PARTS )
        s += done.empty() ? "" : ( r.dry_run() ? "would route:\n" : "routed:\n" ) + done;
    else
        s += "(detail=true lists every route)\n";

    if( !r.history_label().empty() )
        s += unconnectedOf( r ) + "; undo takes all of it back: \"" + r.history_label()
             + "\"\n";
    else if( !r.dry_run() )
        s += "nothing changed; " + unconnectedOf( r ) + "\n";

    return s;
}


/// "a, b, c" of the first aMax, then "(+N more)"
std::string joinList( const google::protobuf::RepeatedPtrField<std::string>& aList, int aMax )
{
    std::string s;

    for( int i = 0; i < aList.size() && i < aMax; ++i )
        s += ( i ? ", " : "" ) + aList[i];

    if( aList.size() > aMax )
        s += " (+" + std::to_string( aList.size() - aMax ) + " more)";

    return s;
}


TOOL_RESULT routeShort( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::RouteShort req;

    for( const std::string& r : getStrings( aArgs, "refs" ) )
        req.add_refs( r );

    for( const std::string& n : getStrings( aArgs, "nets" ) )
        req.add_nets( n );

    if( req.refs().empty() && req.nets().empty() )
        return error( "say which: refs and / or nets" );

    if( double l = 0; getNumber( aArgs, "max_length", l ) )
        req.set_max_length( std::llround( l * 1e6 ) );

    req.set_layer( aArgs.value( "layer", std::string() ) );
    req.set_dry_run( aArgs.value( "dry_run", false ) );
    req.set_no_vias( !aArgs.value( "vias", true ) );
    req.set_allow_under_ic( aArgs.value( "allow_under_ic", false ) );
    req.set_include_poured( aArgs.value( "include_poured", false ) );

    for( const std::string& g : getStrings( aArgs, "skip_nets" ) )
        req.add_skip_nets( g );

    kb::RouteResponse r;
    std::string       err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string skipped;

    if( !r.skipped_poured().empty() )
        skipped += "skipped (poured, the zone joins them; include_poured=true or name the net to route it): "
                   + joinList( r.skipped_poured(), 12 ) + "\n";

    if( !r.skipped_nets().empty() )
        skipped += "skipped (skip_nets): " + joinList( r.skipped_nets(), 12 ) + "\n";

    if( r.results().empty() )
        return { skipped + "no unrouted connections match" + std::string( req.max_length() ? " (max_length)" : "" ) + "\n" };

    std::string s = skipped + renderRoutes( r, detailOf( aArgs ), "connection(s), shortest first" );

    if( !r.history_label().empty() )
        s += "copper zones keep their old shape: refill_zones before run_drc\n";

    return { s };
}


TOOL_RESULT fanout( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::Fanout req;

    for( const std::string& r : getStrings( aArgs, "refs" ) )
        req.add_refs( r );

    for( const std::string& n : getStrings( aArgs, "nets" ) )
        req.add_nets( n );

    if( req.nets().empty() )
        return error( "nets: the nets to fan out, e.g. [\"GND\", \"+3V3\"]" );

    req.set_layer( aArgs.value( "layer", std::string() ) );
    req.set_dry_run( aArgs.value( "dry_run", false ) );

    // "already joined" reads the pour's fill: it must be today's (after routing it was not:
    // pads were skipped as joined that DRC called unconnected)
    if( aArgs.value( "refill", true ) )
    {
        TOOL_RESULT f = refillZones( aIpc );

        if( f.isError )
            return error( "refill_zones before the fanout failed: " + f.text );
    }

    kb::FanoutResponse r;
    std::string        err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s;

    // "VCC: R22.1, JP1.1" (a net with no zone on another layer, and its pads), or
    // "X (no such net)"
    for( const std::string& n : r.no_zone() )
    {
        size_t colon = n.find( ": " );

        if( colon == std::string::npos )
            s += "warning: " + n + "\n";
        else
            s += "warning: no copper zone of " + n.substr( 0, colon ) + " on another layer to fan out to; skipped "
                 + n.substr( colon + 2 ) + "\n";
    }

    if( r.skipped_tht() )
        s += std::to_string( r.skipped_tht() ) + " through-hole pad(s) skipped (they reach every layer)\n";

    if( r.skipped_done() )
        s += std::to_string( r.skipped_done() ) + " pad(s) already joined to the zone, skipped\n";

    if( r.routes().results().empty() )
        return { s + "no SMD pads to fan out\n" };

    s += renderRoutes( r.routes(), detailOf( aArgs ), "pad(s)" );

    if( !r.routes().history_label().empty() && aArgs.value( "refill", true ) )
    {
        TOOL_RESULT f = refillZones( aIpc );
        s += f.text;
    }

    return { s };
}


std::string mmText( double aMm )
{
    char b[32];
    std::snprintf( b, sizeof( b ), "%.2f", aMm );
    std::string s = b;

    while( s.back() == '0' )
        s.pop_back();

    if( s.back() == '.' )
        s.pop_back();

    return s;
}



TOOL_RESULT stitchVias( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::StitchVias req;
    req.set_net( aArgs.value( "net", std::string() ) );

    double v = 0;

    if( getNumber( aArgs, "spacing", v ) )
    {
        if( v <= 0 )
            return error( "spacing is in mm, more than 0" );

        req.set_spacing( toNm( v ) );
    }

    if( getNumber( aArgs, "max", v ) )
        req.set_max( (int) v );

    if( aArgs.contains( "area" ) )
    {
        const json& a = aArgs["area"];

        if( !a.is_array() || a.size() != 4 || !std::all_of( a.begin(), a.end(), []( const json& x ) { return x.is_number(); } ) )
            return error( "area is [x0, y0, x1, y1] in mm" );

        req.mutable_area_min()->set_x( toNm( a[0].get<double>() ) );
        req.mutable_area_min()->set_y( toNm( a[1].get<double>() ) );
        req.mutable_area_max()->set_x( toNm( a[2].get<double>() ) );
        req.mutable_area_max()->set_y( toNm( a[3].get<double>() ) );
    }

    if( aArgs.contains( "at" ) )
    {
        const json& at = aArgs["at"];
        auto isPoint = []( const json& p ) { return p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number(); };

        // one point [x, y] or a list of them
        std::vector<json> pts;

        if( isPoint( at ) )
            pts.push_back( at );
        else if( at.is_array() && !at.empty() && std::all_of( at.begin(), at.end(), isPoint ) )
            pts.assign( at.begin(), at.end() );
        else
            return error( "at: [x, y] mm, or a list of them [[x, y], ...]" );

        if( req.net().empty() )
            return error( "at: say the via's net (net: \"GND\")" );

        if( pts.size() > 200 )
            return error( "at: at most 200 points" );

        for( const json& p : pts )
        {
            kb::Point* q = req.add_at();
            q->set_x( toNm( p[0].get<double>() ) );
            q->set_y( toNm( p[1].get<double>() ) );
        }
    }

    req.set_under_parts( aArgs.value( "under_parts", false ) );
    req.set_no_islands( !aArgs.value( "islands", true ) );
    req.set_dry_run( aArgs.value( "dry_run", false ) );

    std::string head;

    // the vias go inside the fills: they must be today's
    if( aArgs.value( "refill", true ) )
    {
        TOOL_RESULT f = refillZones( aIpc );

        if( f.isError )
            return error( "refill_zones before stitching failed: " + f.text );
    }

    kb::StitchViasResponse r;
    std::string            err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    if( !req.at().empty() )
    {
        std::string s = std::string( r.dry_run() ? "would add " : "added " ) + std::to_string( r.vias_added() ) + " of "
                        + std::to_string( req.at_size() ) + " " + req.net() + " via(s) " + mmText( r.via_diameter() / 1e6 ) + "/"
                        + mmText( r.via_drill() / 1e6 ) + " mm (net class size)\n";

        for( const std::string& line : r.at_results() )
            s += "  " + line + "\n";

        if( !r.history_label().empty() )
        {
            s += unconnectedOf( r ) + "; undo takes it back\n";

            if( aArgs.value( "refill", true ) )
                s += refillZones( aIpc ).text;
        }

        return { s };
    }

    std::string layers;

    for( const std::string& l : r.layers() )
        layers += ( layers.empty() ? "" : " " ) + l;

    std::string s = std::string( r.dry_run() ? "would add " : "added " ) + std::to_string( r.vias_added() ) + " stitching via(s) "
                    + mmText( r.via_diameter() / 1e6 ) + "/" + mmText( r.via_drill() / 1e6 ) + " mm joining "
                    + layers + "\n";
    s += "grid: " + std::to_string( r.grid_points() ) + " point(s), " + std::to_string( r.in_copper() )
         + ( r.layers_size() == 1 ? " inside the net's copper; " : " inside the net's copper on 2+ layers; " ) + std::to_string( r.near_existing() )
         + " left out (a via or hole of the net near), " + std::to_string( r.blocked() )
         + " blocked (pads, tracks, parts, clearance)\n";

    if( r.islands_joined() )
        s += std::to_string( r.islands_joined() ) + " island(s) of copper with no via joined by one\n";

    if( !r.islands_left().empty() )
    {
        s += "islands still without a via (no free spot: parts, pads, tracks):";

        for( const std::string& i : r.islands_left() )
            s += " " + i + ";";

        s.back() = '\n';
    }

    for( const std::string& n : r.notes() )
        s += "note: " + n + "\n";

    if( !r.history_label().empty() )
    {
        s += unconnectedOf( r ) + "; undo takes it back\n";

        if( aArgs.value( "refill", true ) )
        {
            TOOL_RESULT f = refillZones( aIpc );
            s += f.text;
        }
    }

    return { s };
}


TOOL_RESULT teardrops( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::Teardrops req;
    req.set_remove( aArgs.value( "remove", false ) );
    req.set_no_vias( !aArgs.value( "vias", true ) );
    req.set_no_pth( !aArgs.value( "pth", true ) );
    req.set_smd( aArgs.value( "smd", false ) );
    req.set_dry_run( aArgs.value( "dry_run", false ) );

    for( const std::string& n : getStrings( aArgs, "nets" ) )
        req.add_nets( n );

    for( const std::string& r : getStrings( aArgs, "refs" ) )
        req.add_refs( r );

    kb::TeardropsResponse r;
    std::string           err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    const char* what = req.remove() ? "off" : "on";
    std::string s;

    if( r.dry_run() )
        s = "would switch teardrops " + std::string( what ) + " for " + std::to_string( r.changed() ) + " pad(s) / via(s)";
    else
        s = "teardrops switched " + std::string( what ) + " for " + std::to_string( r.changed() ) + " pad(s) / via(s): "
            + std::to_string( r.before() ) + " -> " + std::to_string( r.after() ) + " teardrop(s) on the board";

    if( r.already() )
        s += "; " + std::to_string( r.already() ) + " were already";

    s += "\n";

    for( const std::string& m : r.missing() )
        s += "not found: " + m + "\n";

    if( !r.history_label().empty() )
    {
        s += std::string( req.remove() ? "" : "they follow the parts and tracks from now on; " ) + "undo takes it back\n";
        TOOL_RESULT f = refillZones( aIpc );
        s += f.text;
    }

    return { s };
}


TOOL_RESULT deleteTracks( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::DeleteTracks req;
    req.set_all( aArgs.value( "all", false ) );
    req.set_include_locked( aArgs.value( "include_locked", false ) );
    req.set_keep_vias( aArgs.value( "keep_vias", false ) );

    for( const std::string& n : getStrings( aArgs, "nets" ) )
        req.add_nets( n );

    for( const std::string& r : getStrings( aArgs, "refs" ) )
        req.add_refs( r );

    if( aArgs.contains( "region" ) )
    {
        const json& box = aArgs["region"];

        if( !box.is_array() || box.size() != 4
            || !std::all_of( box.begin(), box.end(), []( const json& v ) { return v.is_number(); } ) )
            return error( "region: [x0, y0, x1, y1] in mm" );

        req.mutable_region_min()->set_x( std::llround( box[0].get<double>() * 1e6 ) );
        req.mutable_region_min()->set_y( std::llround( box[1].get<double>() * 1e6 ) );
        req.mutable_region_max()->set_x( std::llround( box[2].get<double>() * 1e6 ) );
        req.mutable_region_max()->set_y( std::llround( box[3].get<double>() * 1e6 ) );
    }

    if( !req.all() && req.nets().empty() && req.refs().empty() && !req.has_region_min() )
        return error( "say which: all, nets, refs or region" );

    kb::MoveResponse r;
    std::string      err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s = r.tracks_removed() == 0 ? std::string( "no tracks matched\n" ) : "";
    s += renderMoveResult( r, detailOf( aArgs ) );

    if( r.tracks_removed() > 0 )
        s += "undo: \"" + r.history_label() + "\"; copper zones keep their old shape: refill_zones before run_drc\n";

    return { s, r.tracks_removed() == 0 && r.tracks_locked() == 0 };
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

    return { renderMoveResult( r, detailOf( aArgs ) ), r.added().empty() };
}


TOOL_RESULT lcscFetch( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::FetchLcsc req;

    if( !aArgs.contains( "lcsc" ) || !aArgs["lcsc"].is_string() )
        return error( "lcsc (an LCSC part number, e.g. \"C8734\") is required" );

    req.set_lcsc( aArgs["lcsc"].get<std::string>() );
    req.set_refresh( aArgs.contains( "refresh" ) && aArgs["refresh"].is_boolean() && aArgs["refresh"].get<bool>() );
    req.set_no_model( aArgs.contains( "model" ) && aArgs["model"].is_boolean() && !aArgs["model"].get<bool>() );

    if( aArgs.contains( "unnumbered_pads" ) && aArgs["unnumbered_pads"].is_string() )
        req.set_unnumbered_pads( aArgs["unnumbered_pads"].get<std::string>() );

    if( aArgs.contains( "pin_types" ) )
    {
        if( !aArgs["pin_types"].is_object() )
            return error( "pin_types: {\"pin number or name\": \"power_in\", ...}" );

        for( const auto& [pin, type] : aArgs["pin_types"].items() )
        {
            if( !type.is_string() )
                return error( "pin_types " + pin + ": a type name, e.g. \"power_in\"" );

            ( *req.mutable_pin_types() )[pin] = type.get<std::string>();
        }
    }

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

    if( r.has_stock() )
    {
        const kb::LcscStock& st = r.stock();
        char price[64] = "";

        if( st.price_usd() >= 0 )
            std::snprintf( price, sizeof( price ), ", US$%.4g each (min %d)", st.price_usd(), st.min_qty() );

        out += "\n  LCSC stock " + ( st.stock() >= 0 ? std::to_string( st.stock() ) : std::string( "?" ) ) + price
               + "; JLC assembly: " + ( st.jlc_smt() ? "yes" : "no" )
               + ( st.jlc_class().empty() ? "" : " (" + st.jlc_class() + ")" ) + " -- EasyEDA, as of " + st.as_of()
               + ( r.from_cache() ? " (cached; refresh=true for today's)" : "" )
               + ( st.index_note().empty() ? "" : "\n  " + st.index_note().substr( 2 ) );
    }

    if( r.has_symbol() )
    {
        const kb::LcscSymbol& s = r.symbol();
        out += "\n  symbol " + s.lib_id() + " (" + s.file() + "), reference " + s.reference() + ", "
               + std::to_string( s.pins_size() ) + " pins"
               + ( s.units() > 1 ? ", " + std::to_string( s.units() ) + " units" : "" )
               + ( s.registered() ? "; LCSC library added to the project sym-lib-table" : "" )
               + ( s.pin_types().empty() ? "" : "\n  " + s.pin_types() );

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

    if( !r.model().empty() )
        out += "\n  " + r.model();

    out += "\n  " + r.attribution();
    return { out };
}


TOOL_RESULT pcbSetFields( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::FpSetFields req;
    std::string     bad;

    auto add = [&]( const std::string& aRef, const json& aSet )
    {
        kb::FpFieldEdit* e = req.add_parts();
        e->set_ref( aRef );

        for( const auto& [key, v] : aSet.items() )
        {
            if( key == "dnp" || key == "exclude_from_bom" || key == "exclude_from_pos" )
            {
                if( !v.is_boolean() )
                    bad = key + " is true or false";
                else if( key == "dnp" )
                    e->set_dnp( v.get<bool>() );
                else if( key == "exclude_from_bom" )
                    e->set_exclude_from_bom( v.get<bool>() );
                else
                    e->set_exclude_from_pos( v.get<bool>() );
            }
            else if( key == "silk" )
            {
                if( !v.is_string() )
                    bad = "silk is value_only, ref_only, both or none";
                else
                    e->set_silk( v.get<std::string>() );
            }
            else if( v.is_null() )
                e->add_remove( key );
            else if( v.is_string() )
                ( *e->mutable_fields() )[key] = v.get<std::string>();
            else if( v.is_number() )
                ( *e->mutable_fields() )[key] = v.dump();
            else if( v.is_object() )
            {
                // the field's layout (and its text, if given)
                kb::FieldLayout& l = ( *e->mutable_layout() )[key];
                double           d = 0;

                for( const auto& [k, x] : v.items() )
                {
                    if( k == "text" && ( x.is_string() || x.is_number() ) )
                        ( *e->mutable_fields() )[key] = x.is_string() ? x.get<std::string>() : x.dump();
                    else if( k == "layer" && x.is_string() )
                        l.set_layer( x.get<std::string>() );
                    else if( k == "visible" && x.is_boolean() )
                        l.set_visible( x.get<bool>() );
                    else if( k == "justify" && x.is_string() )
                        l.set_justify( x.get<std::string>() );
                    else if( k == "at" && x.is_array() && x.size() == 2 && x[0].is_number() && x[1].is_number() )
                        l.add_at( x[0].get<double>() ), l.add_at( x[1].get<double>() );
                    else if( ( k == "size" || k == "thickness" || k == "angle" ) && getNumber( v, k.c_str(), d ) )
                        k == "size" ? l.set_size( d ) : k == "thickness" ? l.set_thickness( d ) : l.set_angle( d );
                    else
                        bad = key + "." + k + ": a field's layout takes text, layer, visible, at [dx, dy], size, "
                              "thickness, justify, angle";
                }
            }
            else
                bad = key + ": a field's text is a string (null removes it), or its layout {layer, visible, at, ...}";
        }
    };

    if( aArgs.contains( "parts" ) && aArgs["parts"].is_object() )
    {
        for( const auto& [ref, set] : aArgs["parts"].items() )
        {
            if( set.is_object() )
                add( ref, set );
        }
    }

    if( aArgs.contains( "set" ) && aArgs["set"].is_object() )
    {
        for( const std::string& ref : getStrings( aArgs, "refs" ) )
            add( ref, aArgs["set"] );
    }

    if( !bad.empty() )
        return error( bad );

    if( req.parts().empty() )
        return error( "give parts {ref: {field: text}} or refs [...] with set {...}" );

    kb::MoveResponse r;
    std::string      err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string t = r.history_label().empty() ? "nothing changed" : r.history_label() + " (undo takes it back)";

    for( const std::string& n : r.notes() )
        t += "\nnote: " + n;

    if( !r.missing().empty() )
    {
        t += "\nnot found:";

        for( const std::string& m : r.missing() )
            t += " " + m;
    }

    return { t + "\n", r.history_label().empty() };
}


std::string renderRules( const kb::RulesResponse& r )
{
    auto len = []( double aMm ) { return " " + mm( (int64_t) std::llround( aMm * 1e6 ) ); };
    std::string t = "net classes:";

    for( const kb::NetclassRule& c : r.classes() )
    {
        std::string v;

        if( c.has_clearance() )       v += ", clearance" + len( c.clearance() );
        if( c.has_track_width() )     v += ", track" + len( c.track_width() );
        if( c.has_via_diameter() )    v += ", via" + len( c.via_diameter() );
        if( c.has_via_drill() )       v += " drill" + len( c.via_drill() );
        if( c.has_diff_pair_width() ) v += ", diff pair" + len( c.diff_pair_width() );
        if( c.has_diff_pair_gap() )   v += " gap" + len( c.diff_pair_gap() );

        t += "\n  " + c.name() + ( v.empty() ? " (all from Default)" : ":" + v.substr( 1 ) )
             + " [" + std::to_string( c.nets() ) + " net(s)]";
    }

    if( r.patterns().empty() )
        t += "\nassignments: none (by pattern)";
    else
    {
        t += "\nassignments:";

        for( const kb::NetclassAssign& a : r.patterns() )
            t += " " + a.pattern() + "->" + a.netclass() + ";";

        t.pop_back();
    }

    const kb::BoardConstraints& k = r.constraints();
    t += "\nconstraints: clearance" + len( k.min_clearance() ) + ", track" + len( k.min_track_width() )
         + ", via" + len( k.min_via_diameter() ) + ", drill" + len( k.min_through_drill() ) + ", annular"
         + len( k.min_annular_width() ) + ", hole clearance" + len( k.hole_clearance() ) + ", hole to hole"
         + len( k.hole_to_hole() ) + ", copper to edge" + len( k.copper_edge_clearance() ) + ", silk text"
         + len( k.min_silk_text_height() ) + " high" + len( k.min_silk_text_thickness() ) + " stroke (mm)";
    return t + "\n";
}


TOOL_RESULT getRules( IPC_CLIENT& aIpc )
{
    kb::RulesResponse r;
    std::string       err;

    if( !aIpc.Call( kb::GetRules(), r, err ) )
        return error( err );

    return { renderRules( r ) };
}


TOOL_RESULT setRules( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::SetRules req;

    auto num = []( const json& aObj, const char* aKey, double& aOut )
    {
        if( !aObj.contains( aKey ) )
            return false;

        if( aObj[aKey].is_null() )
        {
            aOut = -1;
            return true;
        }

        if( !aObj[aKey].is_number() )
            return false;

        aOut = aObj[aKey].get<double>();
        return true;
    };

    // the wrong shape (a list of {name, ...}) was ignored and read as "nothing changed"
    for( const auto& [key, shape] : { std::pair<const char*, const char*>{ "classes", "{\"USB\": {\"track_width\": 0.2}}" },
                                      { "assign", "{\"/USB_D*\": \"USB\"}" },
                                      { "constraints", "{\"min_clearance\": 0.15}" } } )
    {
        if( aArgs.contains( key ) && !aArgs[key].is_object() )
            return error( std::string( key ) + " is an object, e.g. " + shape );
    }

    if( aArgs.contains( "classes" ) && aArgs["classes"].is_object() )
    {
        for( const auto& [name, v] : aArgs["classes"].items() )
        {
            if( !v.is_object() )
                return error( "classes: {name: {clearance, track_width, ...}}" );

            kb::NetclassRule* c = req.add_classes();
            c->set_name( name );
            double d;

            for( const auto& [key, val] : v.items() )
            {
                if( key == "impedance_layer" )
                {
                    if( !val.is_string() )
                        return error( name + ".impedance_layer: a copper layer name, e.g. \"F.Cu\"" );

                    c->set_impedance_layer( val.get<std::string>() );
                    continue;
                }

                static const std::set<std::string> known = { "clearance", "track_width", "via_diameter", "via_drill",
                                                              "diff_pair_width", "diff_pair_gap", "current", "temp_rise",
                                                              "impedance", "diff_impedance" };

                if( !known.count( key ) )
                    return error( name + ": unknown net class value " + key + "; the values are clearance, track_width, via_diameter, via_drill, diff_pair_width, diff_pair_gap, current, temp_rise, impedance, diff_impedance, impedance_layer" );

                if( !num( v, key.c_str(), d ) )
                    return error( name + "." + key + ": a length in mm (null: from Default)" );

                if( key == "clearance" )            c->set_clearance( d );
                else if( key == "track_width" )     c->set_track_width( d );
                else if( key == "via_diameter" )    c->set_via_diameter( d );
                else if( key == "via_drill" )       c->set_via_drill( d );
                else if( key == "diff_pair_width" ) c->set_diff_pair_width( d );
                else if( key == "diff_pair_gap" )   c->set_diff_pair_gap( d );
                else if( key == "current" )         c->set_current( d );
                else if( key == "temp_rise" )       c->set_temp_rise( d );
                else if( key == "impedance" )       c->set_impedance( d );
                else if( key == "diff_impedance" )  c->set_diff_impedance( d );
                else
                    return error( "unknown net class value " + key + "; the values are clearance, track_width, via_diameter, via_drill, diff_pair_width, diff_pair_gap, current, temp_rise, impedance, diff_impedance, impedance_layer" );
            }
        }
    }

    for( const std::string& r : getStrings( aArgs, "remove_classes" ) )
        req.add_remove_classes( r );

    if( aArgs.contains( "assign" ) && aArgs["assign"].is_object() )
    {
        for( const auto& [pattern, nc] : aArgs["assign"].items() )
        {
            kb::NetclassAssign* a = req.add_assign();
            a->set_pattern( pattern );
            a->set_netclass( nc.is_string() ? nc.get<std::string>() : "" );
        }
    }

    if( aArgs.contains( "constraints" ) && aArgs["constraints"].is_object() )
    {
        const json&           k = aArgs["constraints"];
        kb::BoardConstraints* c = req.mutable_constraints();
        double                d;

        for( const auto& [key, val] : k.items() )
        {
            if( !num( k, key.c_str(), d ) || d < 0 )
                return error( "constraints." + key + ": a length in mm" );

            if( key == "min_clearance" )                c->set_min_clearance( d );
            else if( key == "min_track_width" )         c->set_min_track_width( d );
            else if( key == "min_via_diameter" )        c->set_min_via_diameter( d );
            else if( key == "min_through_drill" )       c->set_min_through_drill( d );
            else if( key == "min_annular_width" )       c->set_min_annular_width( d );
            else if( key == "hole_clearance" )          c->set_hole_clearance( d );
            else if( key == "hole_to_hole" )            c->set_hole_to_hole( d );
            else if( key == "copper_edge_clearance" )   c->set_copper_edge_clearance( d );
            else if( key == "min_silk_text_height" )    c->set_min_silk_text_height( d );
            else if( key == "min_silk_text_thickness" ) c->set_min_silk_text_thickness( d );
            else
                return error( "unknown constraint " + key );
        }
    }

    req.set_preset( aArgs.value( "preset", std::string() ) );

    const bool  drc = !aArgs.contains( "drc" ) || !aArgs["drc"].is_boolean() || aArgs["drc"].get<bool>();
    DRC_REPORT  before, after;
    std::string err;

    auto total = []( const std::map<std::string, int>& m )
    {
        int n = 0;

        for( const auto& [k, v] : m )
            n += v;

        return n;
    };

    if( drc && !RunFullDrc( aIpc, before, err ) )
        return error( "DRC before the change: " + err );

    kb::RulesResponse r;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string t = r.history_label().empty() ? std::string( "nothing changed\n" )
                                              : r.history_label() + " (undo takes it back; save_board keeps it, in the project file)\n";

    if( !r.note().empty() )
        t += ( req.preset().empty() ? "note: " : "preset: " ) + r.note() + "\n";

    if( drc && !r.history_label().empty() )
    {
        if( !RunFullDrc( aIpc, after, err ) )
            return error( "DRC after the change: " + err );

        t += "DRC errors " + std::to_string( total( before.errors ) ) + " -> " + std::to_string( total( after.errors ) );

        std::set<std::string> types;

        for( const auto& [k, v] : before.errors ) types.insert( k );
        for( const auto& [k, v] : after.errors )  types.insert( k );

        std::string diff;

        for( const std::string& k : types )
        {
            int a = before.errors.count( k ) ? before.errors.at( k ) : 0;
            int b = after.errors.count( k ) ? after.errors.at( k ) : 0;

            if( a != b )
                diff += ( diff.empty() ? "" : ", " ) + k + " " + std::to_string( a ) + "->" + std::to_string( b );
        }

        t += ( diff.empty() ? std::string( " (no change)" ) : " (" + diff + ")" ) + "; run_drc lists them\n";
    }

    for( const std::string& m : r.assign_matches() )
        t += "assign " + m + "\n";

    return { t + renderRules( r ) };
}


TOOL_RESULT refillZones( IPC_CLIENT& aIpc );


TOOL_RESULT zonesTool( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::Zones req;

    for( const std::string& n : getStrings( aArgs, "nets" ) )
        req.add_nets( n );

    for( const std::string& l : getStrings( aArgs, "layers" ) )
        req.add_layers( l );

    for( const std::string& i : getStrings( aArgs, "ids" ) )
        req.add_ids( i );

    req.set_all( aArgs.value( "all", false ) );
    double d;

    if( getNumber( aArgs, "clearance", d ) )     req.set_clearance( d );
    if( getNumber( aArgs, "min_width", d ) )     req.set_min_width( d );
    if( getNumber( aArgs, "priority", d ) )      req.set_priority( (int) d );
    if( getNumber( aArgs, "thermal_gap", d ) )   req.set_thermal_gap( d );
    if( getNumber( aArgs, "thermal_spoke", d ) ) req.set_thermal_spoke( d );

    if( aArgs.contains( "connection" ) && aArgs["connection"].is_string() )
        req.set_connection( aArgs["connection"].get<std::string>() );

    if( aArgs.contains( "create" ) )
    {
        const json& c = aArgs["create"];

        if( !c.is_object() )
            return error( "create: {net, layers, outline, priority, clearance, min_width, connection, name}" );

        kb::ZoneCreate* z = req.mutable_create();
        z->set_net( c.value( "net", std::string() ) );

        for( const std::string& l : getStrings( c, "layers" ) )
            z->add_layers( l );

        if( c.contains( "rect" ) )
        {
            const json& b = c["rect"];

            if( c.contains( "outline" ) || !b.is_array() || b.size() != 4
                || !std::all_of( b.begin(), b.end(), []( const json& v ) { return v.is_number(); } ) )
                return error( "create.rect: [x0, y0, x1, y1] in mm (or outline [[x, y], ...], not both)" );

            const double x0 = b[0].get<double>(), y0 = b[1].get<double>(), x1 = b[2].get<double>(),
                         y1 = b[3].get<double>();

            for( auto [x, y] : { std::pair{ x0, y0 }, std::pair{ x1, y0 }, std::pair{ x1, y1 }, std::pair{ x0, y1 } } )
            {
                kb::Point* pt = z->add_outline();
                pt->set_x( toNm( x ) );
                pt->set_y( toNm( y ) );
            }
        }

        if( c.contains( "outline" ) )
        {
            if( !c["outline"].is_array() )
                return error( "create.outline: [[x, y], ...] in mm (default: the board outline)" );

            for( const json& p : c["outline"] )
            {
                if( !p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number() )
                    return error( "create.outline: [[x, y], ...] in mm" );

                kb::Point* pt = z->add_outline();
                pt->set_x( std::llround( p[0].get<double>() * 1e6 ) );
                pt->set_y( std::llround( p[1].get<double>() * 1e6 ) );
            }
        }

        if( getNumber( c, "priority", d ) )  z->set_priority( (int) d );
        if( getNumber( c, "clearance", d ) ) z->set_clearance( d );
        if( getNumber( c, "min_width", d ) ) z->set_min_width( d );

        z->set_connection( c.value( "connection", std::string() ) );
        z->set_name( c.value( "name", std::string() ) );
    }

    kb::ZonesResponse r;
    std::string       err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string t;

    if( !r.history_label().empty() )
    {
        t = r.history_label() + " (undo takes it back)\n";

        for( const std::string& n : r.notes() )
            t += n + "\n";

        if( !aArgs.contains( "refill" ) || !aArgs["refill"].is_boolean() || aArgs["refill"].get<bool>() )
        {
            TOOL_RESULT f = refillZones( aIpc );
            t += f.text;

            // the list as it is after the fill
            kb::Zones again;

            for( const kb::ZoneInfo& z : r.zones() )
                again.add_ids( z.id() );

            kb::ZonesResponse filled;

            if( !again.ids().empty() && aIpc.Call( again, filled, err ) )
                *r.mutable_zones() = filled.zones();
        }
        else
        {
            t += "not refilled (refill_zones before run_drc)\n";
        }
    }

    t += std::to_string( r.zones_size() ) + " copper zone(s):";

    for( const kb::ZoneInfo& z : r.zones() )
    {
        std::string layers;

        for( const std::string& l : z.layers() )
            layers += ( layers.empty() ? "" : "," ) + l;

        auto len = []( double aMm ) { return mm( (int64_t) std::llround( aMm * 1e6 ) ); };
        t += "\n  " + z.id() + " " + ( z.net().empty() ? "(no net)" : z.net() ) + " " + layers
             + ( z.name().empty() ? "" : " \"" + z.name() + "\"" ) + " priority " + std::to_string( z.priority() )
             + ", clearance " + len( z.clearance() ) + ", min width " + len( z.min_width() ) + ", pads "
             + z.connection() + ( z.connection().rfind( "thermal", 0 ) == 0
                                          ? " (gap " + len( z.thermal_gap() ) + ", spoke " + len( z.thermal_spoke() ) + ")"
                                          : "" )
             + ( z.filled() ? "" : ", not filled" );
    }

    return { t + "\n" };
}


bool outlineShape( const json& aObj, kb::OutlineShape& aOut, std::string& aErr )
{
    for( const char* key : { "rect", "circle" } )
    {
        if( !aObj.contains( key ) )
            continue;

        if( !aObj[key].is_array() )
        {
            aErr = std::string( key ) + ": numbers in mm";
            return false;
        }

        for( const json& v : aObj[key] )
        {
            if( !v.is_number() )
            {
                aErr = std::string( key ) + ": numbers in mm";
                return false;
            }

            ( std::string( key ) == "rect" ? aOut.add_rect( v.get<double>() ) : aOut.add_circle( v.get<double>() ) );
        }
    }

    double r = 0;

    if( getNumber( aObj, "corner_radius", r ) )
        aOut.set_corner_radius( r );

    return true;
}


TOOL_RESULT boardOutline( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::BoardOutline req;
    std::string      err;

    if( aArgs.contains( "rect" ) || aArgs.contains( "circle" ) )
    {
        if( !outlineShape( aArgs, *req.mutable_outline(), err ) )
            return error( err );
    }

    if( aArgs.contains( "cutouts" ) )
    {
        if( !aArgs["cutouts"].is_array() )
            return error( "cutouts: [{\"rect\": [x0, y0, x1, y1], \"corner_radius\": r} | {\"circle\": [cx, cy, d]}]" );

        for( const json& c : aArgs["cutouts"] )
        {
            if( !c.is_object() || !outlineShape( c, *req.add_cutouts(), err ) )
                return error( err.empty() ? "cutouts: [{rect} | {circle}]" : "cutouts: " + err );
        }
    }

    if( aArgs.contains( "holes" ) )
    {
        if( !aArgs["holes"].is_array() )
            return error( "holes: [[x, y, d], {x, y, diameter, plated, net}]" );

        for( const json& h : aArgs["holes"] )
        {
            kb::MountingHole* m = req.add_holes();

            if( h.is_array() && ( h.size() == 2 || h.size() == 3 ) && h[0].is_number() && h[1].is_number() )
            {
                m->set_x( h[0].get<double>() );
                m->set_y( h[1].get<double>() );

                if( h.size() == 3 && h[2].is_number() )
                    m->set_diameter( h[2].get<double>() );
            }
            else if( h.is_object() && h.contains( "x" ) && h.contains( "y" ) )
            {
                double v = 0;
                getNumber( h, "x", v );
                m->set_x( v );
                getNumber( h, "y", v );
                m->set_y( v );

                if( getNumber( h, "diameter", v ) || getNumber( h, "d", v ) )
                    m->set_diameter( v );

                m->set_plated( h.value( "plated", false ) );
                m->set_net( h.value( "net", std::string() ) );
            }
            else
            {
                return error( "holes: [[x, y, d], {x, y, diameter, plated, net}]" );
            }
        }
    }

    double d = 0;

    if( getNumber( aArgs, "hole_inset", d ) )    req.set_hole_inset( d );
    if( getNumber( aArgs, "hole_diameter", d ) ) req.set_hole_diameter( d );

    req.set_keep_old( aArgs.value( "keep_old", false ) );

    kb::BoardOutlineResponse r;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    char head[160];
    std::snprintf( head, sizeof( head ), "board %.4g x %.4g mm, %.5g mm^2", r.width(), r.height(), r.area() );
    std::string t = std::string( head ) + ( r.removed_shapes() ? "; " + std::to_string( r.removed_shapes() )
                                                                    + " old Edge.Cuts shape(s) replaced" : "" ) + "\n";

    if( r.holes_size() )
    {
        t += "mounting holes:";

        for( const std::string& h : r.holes() )
            t += " " + h + ";";

        t.back() = '\n';
    }

    if( r.outside_size() )
    {
        t += "warning: " + std::to_string( r.outside_size() ) + " part(s) not (wholly) inside the outline:";

        for( int i = 0; i < r.outside_size() && i < 15; ++i )
            t += " " + r.outside( i );

        t += r.outside_size() > 15 ? " ...\n" : "\n";
    }

    for( const std::string& p : r.problems() )
        t += "warning: " + p + "\n";

    if( r.history_label().empty() )
        return { t + "nothing changed\n" };

    t += "undo takes it back: \"" + r.history_label() + "\"; zones create for a pour, refill_zones after moving parts\n";
    return { t };
}


TOOL_RESULT boardTransform( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::BoardTransform req;
    double             d = 0;

    if( getNumber( aArgs, "dx", d ) ) req.set_dx( d );
    if( getNumber( aArgs, "dy", d ) ) req.set_dy( d );

    req.set_center_on_sheet( aArgs.value( "center_on_sheet", false ) );
    req.set_dry_run( aArgs.value( "dry_run", false ) );

    if( !req.center_on_sheet() && req.dx() == 0 && req.dy() == 0 )
        return error( "give dx / dy (mm) or center_on_sheet=true" );

    kb::BoardTransformResponse r;
    std::string                err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    auto num = []( double aV )
    {
        char buf[32];
        std::snprintf( buf, sizeof( buf ), "%.4g", aV );
        return std::string( buf );
    };
    auto box = [&]( const auto& aB )
    {
        return aB.size() == 4 ? "[" + num( aB[0] ) + ", " + num( aB[1] ) + ", " + num( aB[2] ) + ", " + num( aB[3] ) + "]"
                              : std::string( "?" );
    };

    std::string what = std::to_string( r.footprints() ) + " footprint(s), " + std::to_string( r.tracks() ) + " track(s), "
                       + std::to_string( r.vias() ) + " via(s), " + std::to_string( r.zones() ) + " zone(s), "
                       + std::to_string( r.drawings() ) + " graphic(s) / text(s)"
                       + ( r.groups() ? ", " + std::to_string( r.groups() ) + " group(s)" : "" );
    std::string t = ( req.dry_run() ? "would move by " : "moved by " ) + num( r.dx() ) + ", " + num( r.dy() ) + " mm: "
                    + what + "\nboard " + box( r.box_before() ) + " -> " + box( r.box_after() ) + "\n";

    if( !req.dry_run() && !r.result().history_label().empty() )
        t += "unrouted " + std::to_string( r.unconnected_before() ) + " -> " + std::to_string( r.result().unconnected() )
             + "; zone fills moved with their zones; one undo takes it back\n";

    for( const std::string& n : r.notes() )
        t += "note: " + n + "\n";

    return { t };
}


TOOL_RESULT pcbText( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::PcbText req;
    std::string bad;

    auto spec = [&]( const json& aIn, kb::PcbTextSpec* aOut, const std::string& aWhat )
    {
        if( aIn.is_string() )
        {
            aOut->set_id( aIn.get<std::string>() );
            return;
        }

        if( !aIn.is_object() )
        {
            bad = aWhat + ": an object ({id | match, ...})";
            return;
        }

        auto pair = [&]( const char* aKey, google::protobuf::RepeatedField<double>* aTo )
        {
            if( !aIn.contains( aKey ) )
                return;

            const json& v = aIn[aKey];

            if( !v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number() )
                bad = aWhat + ": " + aKey + " is [x, y] mm";
            else
                aTo->Add( v[0].get<double>() ), aTo->Add( v[1].get<double>() );
        };

        double d = 0;
        aOut->set_id( aIn.value( "id", std::string() ) );
        aOut->set_match( aIn.value( "match", std::string() ) );

        if( aIn.contains( "text" ) )      aOut->set_text( aIn.value( "text", std::string() ) );
        if( aIn.contains( "layer" ) )     aOut->set_layer( aIn.value( "layer", std::string() ) );
        if( aIn.contains( "justify" ) )   aOut->set_justify( aIn.value( "justify", std::string() ) );
        if( getNumber( aIn, "size", d ) )      aOut->set_size( d );
        if( getNumber( aIn, "thickness", d ) ) aOut->set_thickness( d );
        if( getNumber( aIn, "angle", d ) )     aOut->set_angle( d );
        if( aIn.contains( "locked" ) && aIn["locked"].is_boolean() ) aOut->set_locked( aIn["locked"].get<bool>() );

        pair( "at", aOut->mutable_at() );
        pair( "by", aOut->mutable_by() );
    };

    auto list = [&]( const char* aKey, auto aAdd )
    {
        if( !aArgs.contains( aKey ) )
            return;

        const json& v = aArgs[aKey];

        if( v.is_array() )
            for( const json& s : v )
                spec( s, aAdd(), aKey );
        else
            spec( v, aAdd(), aKey );
    };

    list( "add", [&]() { return req.add_add(); } );
    list( "edit", [&]() { return req.add_edit(); } );
    list( "remove", [&]() { return req.add_remove(); } );
    req.set_layer( aArgs.value( "layer", std::string() ) );

    if( !bad.empty() )
        return error( bad );

    kb::PcbTextResponse r;
    std::string         err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    auto num = []( double aV )
    {
        char buf[32];
        std::snprintf( buf, sizeof( buf ), "%.4g", aV );
        return std::string( buf );
    };

    const bool  listing = req.add().empty() && req.edit().empty() && req.remove().empty();
    std::string t;

    if( listing )
        t = r.texts().empty() ? "no board texts" + std::string( req.layer().empty() ? "" : " on " + req.layer() )
                              : std::to_string( r.texts_size() ) + " board text(s) (id text @x,y layer size[/thickness] ...):";
    else
        t = r.history_label().empty() ? "nothing changed" : r.history_label() + " (undo takes it back)";

    for( const kb::PcbTextItem& i : r.texts() )
    {
        t += "\n  " + i.id() + " \"" + i.text() + "\"" + ( i.shown().empty() ? "" : " (shows \"" + i.shown() + "\")" )
             + " @" + num( i.x() ) + "," + num( i.y() ) + " " + i.layer() + " " + num( i.size() ) + "/" + num( i.thickness() )
             + ( i.angle() != 0 ? " " + num( i.angle() ) + "deg" : "" ) + ( i.justify() != "center" ? " " + i.justify() : "" )
             + ( i.mirrored() ? " mirrored" : "" ) + ( i.locked() ? " locked" : "" );
    }

    for( const std::string& rm : r.removed() )
        t += "\n  removed " + rm;

    if( !r.missing().empty() )
    {
        t += "\nnot found:";

        for( const std::string& m : r.missing() )
            t += " " + m + ";";

        t.pop_back();
    }

    for( const std::string& n : r.notes() )
        t += "\nnote: " + n;

    if( listing && r.textboxes() )
        t += "\n(" + std::to_string( r.textboxes() ) + " text box(es) too, not listed)";

    return { t + "\n", !listing && r.history_label().empty() && !r.missing().empty() };
}


TOOL_RESULT boardSetup( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::BoardSetup req;
    double         d = 0;

    if( getNumber( aArgs, "copper_layers", d ) )   req.set_copper_layers( (int) d );
    if( getNumber( aArgs, "thickness", d ) )       req.set_thickness( d );
    if( getNumber( aArgs, "outer_copper_oz", d ) ) req.set_outer_copper_oz( d );
    if( getNumber( aArgs, "inner_copper_oz", d ) ) req.set_inner_copper_oz( d );

    if( aArgs.contains( "stackup" ) && aArgs["stackup"].is_string() )
        req.set_stackup( aArgs["stackup"].get<std::string>() );

    kb::BoardSetupResponse r;
    std::string            err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    char head[128];

    if( r.nominal_thickness() > 0 && std::fabs( r.nominal_thickness() - r.thickness() ) > 0.0005 )
        std::snprintf( head, sizeof( head ), "%d copper layers, %.4g mm thick (the stackup's layers added up; nominal %.3g mm)",
                       r.copper_layers(), r.thickness(), r.nominal_thickness() );
    else
        std::snprintf( head, sizeof( head ), "%d copper layers, %.3g mm thick", r.copper_layers(), r.thickness() );
    std::string t = ( r.history_label().empty() ? std::string() : r.history_label() + " (undo takes it back)\n" ) + head
                    + "\nstackup (top to bottom):";

    for( const std::string& line : r.stackup() )
        t += "\n  " + line;

    if( r.stackup().empty() )
        t += " none defined (board_setup thickness=1.6 makes one)";

    if( !r.note().empty() )
        t += "\nnote: " + r.note();

    return { t + "\n" };
}


TOOL_RESULT impedanceTool( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::Impedance req;
    double        d = 0;

    if( getNumber( aArgs, "z", d ) )      req.set_z( d );
    if( getNumber( aArgs, "z_diff", d ) ) req.set_z_diff( d );
    if( getNumber( aArgs, "width", d ) )  req.set_width( d );
    if( getNumber( aArgs, "gap", d ) )    req.set_gap( d );
    if( getNumber( aArgs, "ground_gap", d ) ) req.set_ground_gap( d );

    for( const std::string& l : getStrings( aArgs, "layers" ) )
        req.add_layers( l );

    kb::ImpedanceResponse r;
    std::string           err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    const bool pair = req.z_diff() > 0 || ( req.z() <= 0 && req.has_gap() );
    std::string t;

    for( const kb::ImpedanceLayer& l : r.layers() )
    {
        char buf[200];

        t += l.layer() + ": ";

        if( !l.error().empty() )
        {
            t += l.error() + ( l.geometry().empty() ? "" : " (" + l.geometry() + ")" ) + "\n";
            continue;
        }

        if( pair )
            std::snprintf( buf, sizeof( buf ), "width %.3f mm, gap %.3f mm -> %.1f ohm differential, %.1f ps/cm",
                           l.width(), l.gap(), l.z(), l.delay() );
        else if( req.has_ground_gap() )
            std::snprintf( buf, sizeof( buf ), "width %.3f mm, ground %.3f mm beside it -> %.1f ohm (CPWG), %.1f ps/cm",
                           l.width(), req.ground_gap(), l.z(), l.delay() );
        else
            std::snprintf( buf, sizeof( buf ), "width %.3f mm -> %.1f ohm, %.1f ps/cm", l.width(), l.z(), l.delay() );

        t += std::string( buf ) + " (" + l.geometry() + ")";

        for( const std::string& p : l.unpoured_planes() )
            t += "; " + p + " has no copper zone yet";

        t += "\n";
    }

    if( !r.note().empty() )
        t += "note: " + r.note() + "\n";

    return { t };
}


TOOL_RESULT history( IPC_CLIENT& aIpc, bool aUndo, bool aDetail )
{
    kb::HistoryResponse r;
    std::string         err;
    bool                ok = aUndo ? aIpc.Call( kb::Undo(), r, err ) : aIpc.Call( kb::Redo(), r, err );

    if( !ok )
        return error( err );

    const char* verb = aUndo ? "undo" : "redo";

    if( !r.ok() && r.label().empty() )
        return { std::string( "nothing to " ) + verb + "\n" + renderHistory( r ) };

    const kb::MoveResponse& res = r.result();
    int                     already = res.already_there();

    // Parts someone else changed after this entry, listed briefly (an auto_place entry has ~60)
    auto changedList = [&]()
    {
        std::string s;
        int         n = 0;

        for( const std::string& m : res.missing() )
        {
            std::string ref = m.substr( 0, m.find( ' ' ) );

            if( n++ < 10 )
                s += ( s.empty() ? "" : ", " ) + ref;
        }

        if( n > 10 )
            s += ", ... (" + std::to_string( n ) + " in all)";

        return s;
    };

    if( !r.ok() && res.missing().empty() && already > 0 )
        return { "\"" + r.label() + "\" was already " + ( aUndo ? "undone" : "redone" )
                 + " (in the editor): all " + std::to_string( already )
                 + " part(s) are already there; nothing changed\n" + renderHistory( r ) };

    if( !r.ok() )   // the entry was there, but everything in it had been changed by someone else
    {
        std::string t = std::string( "did not " ) + verb + " \"" + r.label() + "\": "
                        + std::to_string( res.missing_size() )
                        + " part(s) were changed since (by the user or another edit) and are left as is: "
                        + changedList();

        if( already > 0 )
            t += "; " + std::to_string( already ) + " other part(s) are already in place";

        return { t + "\n" + renderHistory( r ) };
    }

    std::string t = std::string( aUndo ? "undid: " : "redid: " ) + r.label() + "\n";

    if( !res.missing().empty() )
        t += "  left as is (changed since): " + changedList() + "\n";

    if( already > 0 )
        t += "  " + std::to_string( already ) + " part(s) were already in place\n";

    if( res.changed() > 0 )
        t += "  " + std::to_string( res.changed() ) + " part(s) " + ( aUndo ? "put back as they were" : "changed again" )
             + " (values, fields, pad nets)\n";

    kb::MoveResponse shown = res;   // the changed parts are listed above, not as "not found"
    shown.clear_missing();

    // tracks or zones back as they were: the fills made for the other state would leave clearance
    // errors and islands (as route / fanout / delete_tracks do, the zones are refilled)
    std::string refilled;

    if( res.fills_stale() )
    {
        TOOL_RESULT f = refillZones( aIpc );

        if( !f.isError )
        {
            refilled = f.text;
            shown.clear_notes();

            for( const std::string& n : res.notes() )
            {
                if( n.find( "zone fills are out of date" ) == std::string::npos )
                    shown.add_notes( n );
            }
        }
    }

    return { t + renderMoveResult( shown, aDetail ) + refilled + renderHistory( r ) };
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

    return { s + renderDrc( r.drc(), detailOf( aArgs ) ) };
}


TOOL_RESULT flushRatsnest( IPC_CLIENT& aIpc )
{
    kb::FlushRatsnestResponse r;
    std::string               err;

    if( !aIpc.Call( kb::FlushRatsnest(), r, err ) )
        return error( err );

    return { "rebuilt " + std::to_string( r.nets_flushed() ) + " deferred nets; "
             + renderUnconnected( r.unconnected(), {}, r.large_included() ) + "\n" };
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

    if( !report.duplicateRefs.empty() )
    {
        s += "warning: the same reference on more than one footprint (the BOM and the placement file list "
             "it twice):";

        for( const std::string& d : report.duplicateRefs )
            s += " " + d;

        s += "\n";
    }

    int  shown = 0, matching = 0;
    bool filtered = !types.empty() || !refs.empty() || !warnings;
    std::map<std::string, int> fErrors, fWarnings;

    auto keep = [&]( const DRC_ENTRY& e )
    {
        return !( e.warning && !warnings ) && ( types.empty() || types.count( e.type ) ) && involves( e );
    };

    if( filtered )
    {
        int fe = 0, fw = 0;

        for( const DRC_ENTRY& e : report.entries )
        {
            if( !keep( e ) )
                continue;

            ++( e.warning ? fWarnings : fErrors )[e.type];
            ++( e.warning ? fw : fe );
        }

        std::string what;

        if( !types.empty() )
            what += std::to_string( types.size() ) + " type(s)";

        if( !refs.empty() )
            what += ( what.empty() ? "" : ", " ) + std::to_string( refs.size() ) + " ref(s)";

        if( !warnings )
            what += ( what.empty() ? "" : ", " ) + std::string( "no warnings" );

        s += "filter (" + what + "): " + std::to_string( fe ) + " errors, " + std::to_string( fw )
             + " warnings match; the counts above are the whole board\n";

        if( fe )
            s += "  errors: " + counts( fErrors ) + "\n";

        if( fw )
            s += "  warnings: " + counts( fWarnings ) + "\n";
    }

    for( const DRC_ENTRY& e : report.entries )
    {
        if( !keep( e ) )
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

    // a starved thermal: the two ways out, with the first one's pad and net
    for( const DRC_ENTRY& e : report.entries )
    {
        if( e.type != "starved_thermal" )
            continue;

        const int    n = ( report.errors.count( e.type ) ? report.errors.at( e.type ) : 0 )
                         + ( report.warnings.count( e.type ) ? report.warnings.at( e.type ) : 0 );
        const bool   pad = e.a.find( '.' ) != std::string::npos;
        const std::string who = pad ? e.a : e.b.find( '.' ) != std::string::npos ? e.b : std::string( "the pad" );
        const std::string net = !e.netA.empty() ? e.netA : e.netB.empty() ? std::string( "its net" ) : e.netB;
        const std::string ref = who.substr( 0, who.find( '.' ) );

        s += "starved_thermal (" + std::to_string( n ) + "): a pad's thermal relief has fewer spokes than its pour "
             "asks (something blocks one). Either change the pour: zones {\"nets\": [\"" + net
             + "\"], \"connection\": \"solid\"} (or a smaller thermal_gap / wider thermal_spoke), then refill; or a "
             "spoke by hand: route {\"from\": \"" + who + "\", \"to\": [x, y]} to a point in the pour past the relief "
             "gap (a track counts as a spoke), or a via into another layer's pour: route {\"from\": \"" + who
             + "\", \"to\": \"zone\", \"waypoints\": [[x, y, \"In1.Cu\"]]}\n";
        break;
    }

    return { s };
}


namespace
{

/// Each copper zone layer's fill (area, pieces), unfilled ones too; empty when it can't be had.
std::map<std::string, kicadd::render::ZoneFill> zoneFillSummary( IPC_CLIENT& aIpc, bool aIslands = false )
{
    kicadd::render::GetZoneFills req;
    req.set_summary( true );
    req.set_islands( aIslands );
    kicadd::render::ZoneFills    r;
    std::string                  err;
    std::map<std::string, kicadd::render::ZoneFill> out;

    if( aIpc.Call( req, r, err ) )
    {
        for( const kicadd::render::ZoneFill& zf : r.fills() )
            out[zf.id() + "|" + zf.layer()] = zf;
    }

    return out;
}


/// What a refill took away, one line per zone layer, worst first: a pour that came out empty or
/// lost most of its copper (islands removed, cut off by tracks) says so, or an agent reads
/// "refilled" as "the pour is there".
std::string fillLosses( const std::map<std::string, kicadd::render::ZoneFill>& aBefore,
                        const std::map<std::string, kicadd::render::ZoneFill>& aAfter )
{
    struct LOSS
    {
        double      order;
        std::string line;
    };

    std::vector<LOSS> losses;

    for( const auto& [key, now] : aAfter )
    {
        auto was = aBefore.find( key );
        const bool   hadFill = was != aBefore.end() && was->second.pieces() > 0;
        const double before = hadFill ? was->second.area_mm2() : 0.0;
        const std::string zone = ( now.net().empty() ? std::string( "no-net" ) : now.net() ) + " " + now.layer()
                                 + ( now.name().empty() ? "" : " \"" + now.name() + "\"" );
        char buf[320];

        if( now.pieces() == 0 )
        {
            std::string line = zone + ": no fill at all";

            if( hadFill )
            {
                std::snprintf( buf, sizeof( buf ), " (it had %.1f mm²)", before );
                line += buf;
            }

            std::snprintf( buf, sizeof( buf ), ", outline %.1f mm²", now.outline_mm2() );
            line += std::string( buf )
                    + ( now.islands_removed()
                                ? ": every piece is an island (nothing of its net — pad, via, track — in it, or cut "
                                  "off from it by other copper); add a via / track of the net into it, or widen the gaps"
                                : ": no room for copper inside (clearances, keepouts)" );
            losses.push_back( { -1e9, line } );
        }
        else if( now.floating() > 0 && now.floating() == now.pieces() )
        {
            std::snprintf( buf, sizeof( buf ),
                           "%s: all its fill (%d piece(s), %.1f mm²) is joined to nothing of its net: floating "
                           "copper (KiCad keeps a pour that is all islands); add a via / track of the net into it",
                           zone.c_str(), now.pieces(), now.area_mm2() );
            losses.push_back( { -1e8, buf } );
        }
        else if( hadFill && now.area_mm2() < 0.5 * before )
        {
            std::snprintf( buf, sizeof( buf ), "%s: fill %.1f -> %.1f mm² (%d piece(s) left)%s", zone.c_str(), before,
                           now.area_mm2(), now.pieces(),
                           now.islands_removed() ? ": pieces cut off from the net were removed as islands" : "" );
            losses.push_back( { now.area_mm2() / std::max( before, 1e-9 ), buf } );
        }
    }

    std::sort( losses.begin(), losses.end(), []( const LOSS& a, const LOSS& b ) { return a.order < b.order; } );

    std::string s;
    const size_t limit = 8;

    for( size_t i = 0; i < losses.size() && i < limit; ++i )
        s += "  " + losses[i].line + "\n";

    if( losses.size() > limit )
        s += "  " + std::to_string( losses.size() - limit ) + " more\n";

    return s.empty() ? s : "pours with copper missing or on no real connection after the refill:\n" + s;
}

} // namespace


TOOL_RESULT refillZones( IPC_CLIENT& aIpc )
{
    std::string err;
    std::string file = boardFileName( aIpc, err );

    if( file.empty() )
        return error( err.empty() ? "no board loaded" : err );

    kiapi::board::commands::RefillZones req;
    req.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    req.mutable_board()->set_board_filename( file );

    const auto before = zoneFillSummary( aIpc );

    google::protobuf::Empty r;
    auto                    t0 = std::chrono::steady_clock::now();

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    char buf[64];
    std::snprintf( buf, sizeof( buf ), "all zones refilled in %.1f s\n",
                   std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );

    // the first line stays "all zones refilled ..." (callers keep only it): the losses go after
    return { std::string( buf ) + fillLosses( before, zoneFillSummary( aIpc, true ) ) };
}


TOOL_RESULT lcscSearch( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::LcscSearch req;

    auto s = [&]( const char* aKey ) -> std::string
    {
        return aArgs.contains( aKey ) && aArgs[aKey].is_string() ? aArgs[aKey].get<std::string>() : "";
    };

    auto d = [&]( const char* aKey ) -> double
    {
        if( !aArgs.contains( aKey ) )
            return 0;

        if( aArgs[aKey].is_number() )
            return aArgs[aKey].get<double>();

        if( aArgs[aKey].is_string() )     // "16V", "1%"
            return std::atof( aArgs[aKey].get<std::string>().c_str() );

        return 0;
    };

    req.set_keyword( s( "keyword" ) );
    req.set_category( s( "category" ) );
    req.set_value( s( "value" ) );
    req.set_package( s( "package" ) );
    req.set_dielectric( s( "dielectric" ) );
    req.set_tolerance( d( "tolerance" ) );
    req.set_voltage_min( d( "voltage_min" ) );
    req.set_basic_only( aArgs.value( "basic_only", false ) );
    req.set_min_stock( (int64_t) d( "min_stock" ) );
    req.set_limit( (int) d( "limit" ) );
    req.set_update( aArgs.value( "update", false ) );

    if( aArgs.contains( "where" ) )
    {
        if( !aArgs["where"].is_object() )
            return error( "where: an object of parameter -> condition, e.g. {\"rds_on\": \"<=20m\", \"vds\": \">=30\"}" );

        for( const auto& [k, v] : aArgs["where"].items() )
        {
            if( v.is_number() )
                ( *req.mutable_where() )[k] = "=" + v.dump();
            else if( v.is_string() )
                ( *req.mutable_where() )[k] = v.get<std::string>();
            else
                return error( "where " + k + ": a condition string (\"<=20m\", \">=30\", \"1u..10u\") or a number" );
        }
    }

    if( aArgs.contains( "live" ) && aArgs["live"].is_number() )
        req.set_live( aArgs["live"].get<int>() <= 0 ? -1 : aArgs["live"].get<int>() );

    kb::LcscSearchResponse r;
    std::string            err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    // several keyword words and nothing has them all: which words match nothing (with the other
    // conditions), and the search again without them
    std::string relaxed;

    if( r.total() == 0 && !req.keyword().empty() )
    {
        std::vector<std::string> words, kept, dropped;
        std::string              w;

        for( char c : req.keyword() + " " )
        {
            if( std::isspace( (unsigned char) c ) || c == ',' )
            {
                if( !w.empty() )
                    words.push_back( w );

                w.clear();
            }
            else
            {
                w += c;
            }
        }

        if( words.size() > 1 && words.size() <= 8 )
        {
            // a keyword set's count with the other conditions (no live check, one row)
            auto count = [&]( const std::vector<std::string>& aWords ) -> int64_t
            {
                kb::LcscSearch         one = req;
                kb::LcscSearchResponse lr;
                std::string            e, k;

                for( const std::string& x : aWords )
                    k += ( k.empty() ? "" : " " ) + x;

                one.set_keyword( k );
                one.set_live( -1 );
                one.set_limit( 1 );
                return aIpc.Call( one, lr, e ) ? lr.total() : 0;
            };

            std::string counts;

            for( const std::string& word : words )
            {
                const int64_t n = count( { word } );
                counts += ( counts.empty() ? "" : ", " ) + ( "\"" + word + "\" " ) + std::to_string( n );
                ( n > 0 ? kept : dropped ).push_back( word );
            }

            // the words that match together: drop the least telling one (the most matches alone)
            // at a time while the rest match nothing together
            std::vector<std::string> keep = kept, cut = dropped;

            while( keep.size() > 1 && count( keep ) == 0 )
            {
                size_t  worst = 0;
                int64_t most = -1;

                for( size_t i = 0; i < keep.size(); i++ )
                {
                    std::vector<std::string> rest = keep;
                    rest.erase( rest.begin() + i );
                    const int64_t n = count( rest );

                    // the subset left that still matches with the fewest parts: the most telling
                    if( n > 0 && ( most < 0 || n < most ) )
                    {
                        most = n;
                        worst = i;
                    }
                }

                if( most < 0 )
                {
                    // no single drop helps: drop the word with the most matches alone
                    int64_t big = -1;

                    for( size_t i = 0; i < keep.size(); i++ )
                    {
                        const int64_t n = count( { keep[i] } );

                        if( n > big )
                        {
                            big = n;
                            worst = i;
                        }
                    }
                }

                cut.push_back( keep[worst] );
                keep.erase( keep.begin() + worst );
            }

            std::string list;

            for( const std::string& d : cut )
                list += ( list.empty() ? "\"" : ", \"" ) + d + "\"";

            if( !keep.empty() && !cut.empty() )
            {
                kb::LcscSearch again = req;
                std::string    k;

                for( const std::string& x : keep )
                    k += ( k.empty() ? "" : " " ) + x;

                again.set_keyword( k );
                kb::LcscSearchResponse r2;

                if( aIpc.Call( again, r2, err ) && r2.total() > 0 )
                {
                    r = r2;
                    relaxed = "nothing has every word (alone: " + counts + "); relaxed: dropped " + list + ", this is \""
                              + k + "\"";
                }
                else
                {
                    relaxed = "nothing has every word (alone: " + counts + ")";
                }
            }
            else
            {
                relaxed = "no word matches anything here (" + counts + "; keywords are matched as word prefixes in the "
                          "part number, description and package: try a part number or a plain word)";
            }
        }
    }

    std::string out = std::to_string( r.total() ) + " match(es) in stock at JLCPCB (parts index of " + r.index_date()
                      + ", jlcparts data; after the number its JLC assembly class)";

    if( !relaxed.empty() )
        out += "\n  " + relaxed;

    if( r.total() > r.hits_size() )
        out += "; the first " + std::to_string( r.hits_size() ) + ": basic, preferred, then by stock";

    if( !r.note().empty() )
        out += "\n  " + r.note();

    for( const kb::LcscHit& h : r.hits() )
    {
        char price[32] = "";

        if( h.price() >= 0 )
            std::snprintf( price, sizeof( price ), ", US$%.4f", h.price() );

        // the assembly class: the index's (basic / preferred / extended), unless the live check
        // says JLC does not assemble it now
        const bool        checked = h.live_stock() >= 0 || !h.live_class().empty();
        const std::string cls = checked && !h.live_smt() ? "NOT-for-assembly"
                                : h.basic()              ? "BASIC"
                                : h.preferred()          ? "preferred"
                                                         : "extended";

        out += "\n" + h.lcsc() + " " + cls + " "
               + h.mfr() + " (" + h.manufacturer() + ") " + h.package() + ", JLC stock " + std::to_string( h.stock() )
               + price;

        if( h.live_stock() >= 0 || !h.live_class().empty() )
        {
            out += "\n    now: LCSC stock " + ( h.live_stock() >= 0 ? std::to_string( h.live_stock() ) : std::string( "?" ) );

            if( !h.live_class().empty() )
                out += ", " + h.live_class();

            out += std::string( h.live_smt() ? ", JLC assembly yes" : ", JLC assembly no" );
        }
        else if( !h.live_note().empty() )
        {
            out += "\n    live check failed: " + h.live_note();
        }

        out += "\n    " + h.description();

        if( !h.params().empty() )
            out += "\n    " + h.params();
    }

    if( r.hits().empty() )
    {
        out += "\nnothing: loosen a condition (tolerance, voltage, basic_only, package)";

        // the data gives some parameters for few or no basic / preferred parts (inductor Isat:
        // none of them): basic_only with such a where finds nothing for that reason alone
        if( req.basic_only() && req.where_size() > 0 )
            out += "; basic_only with where: few basic parts have these parameters in the data (inductor Isat: none, "
                   "SRF: few), so try without basic_only, or drop that condition (Irated is given for most)";
    }

    return { out };
}


TOOL_RESULT boardRender( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::RenderBoard req;

    for( const std::string& l : getStrings( aArgs, "layers" ) )
        req.add_layers( l );

    for( const std::string& r : getStrings( aArgs, "refs" ) )
        req.add_refs( r );

    for( const std::string& n : getStrings( aArgs, "nets" ) )
        req.add_nets( n );

    if( aArgs.contains( "area" ) )
    {
        const json& a = aArgs["area"];

        if( !a.is_array() || a.size() != 4 || !std::all_of( a.begin(), a.end(), []( const json& v ) { return v.is_number(); } ) )
            return error( "area is [x0, y0, x1, y1] in mm" );

        req.mutable_area_min()->set_x( toNm( a[0].get<double>() ) );
        req.mutable_area_min()->set_y( toNm( a[1].get<double>() ) );
        req.mutable_area_max()->set_x( toNm( a[2].get<double>() ) );
        req.mutable_area_max()->set_y( toNm( a[3].get<double>() ) );
    }

    if( aArgs.contains( "marks" ) )
    {
        const json& ms = aArgs["marks"];
        auto        bad = []( const json& m )
        {
            return !m.is_array() || m.size() < 2 || m.size() > 3 || !m[0].is_number() || !m[1].is_number()
                   || ( m.size() == 3 && !m[2].is_string() );
        };

        if( !ms.is_array() || std::any_of( ms.begin(), ms.end(), bad ) )
            return error( "marks is [[x, y, \"label\"], ...] in mm (the label optional)" );

        for( const json& m : ms )
        {
            kb::RenderMark* mk = req.add_marks();
            mk->mutable_at()->set_x( toNm( m[0].get<double>() ) );
            mk->mutable_at()->set_y( toNm( m[1].get<double>() ) );

            if( m.size() == 3 )
                mk->set_label( m[2].get<std::string>() );
        }
    }

    req.set_zoom( aArgs.value( "zoom", req.refs_size() + req.nets_size() + req.marks_size() > 0 ) );
    req.set_no_ratsnest( !aArgs.value( "ratsnest", true ) );
    req.set_labels( aArgs.value( "labels", false ) );
    req.set_back( aArgs.value( "back", false ) );

    double v = 0;

    if( getNumber( aArgs, "size", v ) )
        req.set_size( (int) v );

    if( getNumber( aArgs, "margin", v ) )
        req.set_margin( v );

    if( aArgs.contains( "path" ) && aArgs["path"].is_string() )
    {
        if( std::string why = MissingFolder( aArgs["path"].get<std::string>() ); !why.empty() )
            return error( why );

        req.set_path( aArgs["path"].get<std::string>() );
    }

    kb::RenderBoardResponse r;
    std::string             err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s = "picture " + std::to_string( r.width() ) + "x" + std::to_string( r.height() ) + " px, "
                    + mmText( r.mm_per_px() * 1000 ) + " um/px; "
                    + ( req.back() ? "seen from below (mirrored): x grows to the left; " : "" ) + "x "
                    + mmText( r.origin().x() / 1e6 ) + " .. " + mmText( r.extent().x() / 1e6 ) + ", y "
                    + mmText( r.origin().y() / 1e6 ) + " .. " + mmText( r.extent().y() / 1e6 )
                    + " mm (rulers on the top and left)\nlayers, bottom first:";

    for( const std::string& l : r.layers() )
        s += " " + l;

    if( req.refs_size() )
        s += "\nmagenta frames: the parts asked for";

    if( req.nets_size() )
        s += "\ncyan: the nets asked for";

    if( req.marks_size() )
        s += "\norange rings: the points asked for";

    if( !req.no_ratsnest() )
        s += "\n" + std::to_string( r.unrouted() ) + " unrouted connection(s) as thin white lines";

    if( !r.missing().empty() )
    {
        s += "\nnot found:";

        for( const std::string& m : r.missing() )
            s += " " + m;
    }

    if( !r.path().empty() )
        s += "\nsaved: " + r.path();

    TOOL_RESULT out{ s };
    out.image = r.png();
    return out;
}


TOOL_RESULT exportFab( IPC_CLIENT& aIpc, const json& aArgs )
{
    std::string head;

    // the copper pours as they are now (a stale fill would be plotted as it is)
    if( aArgs.value( "refill", true ) )
    {
        TOOL_RESULT r = refillZones( aIpc );

        if( r.isError )
            return error( "refill_zones before the export failed: " + r.text );

        head = r.text;
    }

    // DRC of the board as it will be made: errors are reported first; strict refuses to write
    const bool strict = aArgs.value( "strict", false );
    std::string drcText, drcErrors;

    if( aArgs.value( "drc", true ) || strict )
    {
        DRC_REPORT  report;
        std::string derr;

        if( !RunFullDrc( aIpc, report, derr ) )
            return error( "DRC before the export failed: " + derr );

        int nErr = 0, nWarn = 0;
        std::string types;

        for( const auto& [k, v] : report.errors )
        {
            nErr += v;
            types += ( types.empty() ? "" : ", " ) + k + " " + std::to_string( v );
        }

        for( const auto& [k, v] : report.warnings )
            nWarn += v;

        if( nErr )
        {
            drcErrors = "DRC: " + std::to_string( nErr ) + " error(s) (" + types + ")";
            drcText = "warning: " + drcErrors + ": the board would be made with them; run_drc for the list, "
                      "fix them (or agree with the user that they may stay) before ordering\n";
        }
        else
        {
            drcText = "DRC: no errors, " + std::to_string( nWarn ) + " warning(s)\n";
        }
    }

    if( strict )
    {
        kb::ExportFab check;
        check.set_check_only( true );
        kb::ExportFabResponse c;
        std::string           cerr;

        if( !aIpc.Call( check, c, cerr ) )
            return error( cerr );

        std::string why;

        if( !drcErrors.empty() )
            why += "- " + drcErrors + ": run_drc for the list\n";

        for( const std::string& p : c.problems() )
            why += "- " + p + "\n";

        if( !why.empty() )
            return error( head + "nothing written (strict):\n" + why
                          + "fix them, or export {fab} without strict to write the files anyway" );
    }

    kb::ExportFab req;

    if( aArgs.contains( "dir" ) && aArgs["dir"].is_string() )
        req.set_dir( aArgs["dir"].get<std::string>() );

    req.set_no_zip( !aArgs.value( "zip", true ) );

    kb::ExportFabResponse r;
    std::string           err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s = head + drcText;

    for( const std::string& p : r.problems() )
        s += "warning: " + p + "\n";

    s += "fabrication files in " + r.dir() + ":\n";

    if( !r.zip().empty() )
        s += "  upload (Gerbers + drill): " + r.zip() + "\n";

    s += "  Gerbers: " + std::to_string( r.layers() ) + " layer file(s) + job file; drill: Excellon (mm)\n";
    s += "  BOM: " + r.bom() + " (" + std::to_string( r.bom_lines() ) + " lines, " + std::to_string( r.bom_parts() )
         + " parts)\n";
    s += "  CPL: " + r.cpl() + " (" + std::to_string( r.placed() ) + " placements)\n";

    if( !r.no_lcsc().empty() )
    {
        s += "  without an LCSC number (" + std::to_string( r.no_lcsc_size() ) + " line(s), JLCPCB will not "
             "assemble them unless you pick parts there):";

        int n = 0;

        for( const std::string& l : r.no_lcsc() )
        {
            if( n++ == 12 )
            {
                s += "\n    ...";
                break;
            }

            s += "\n    " + l;
        }

        s += "\n";
    }

    for( const std::string& note : r.notes() )
        s += "  note: " + note + "\n";

    return { s };
}


} // namespace


static TOOL_RESULT boardRender3d( IPC_CLIENT& aIpc, const json& a )
{
    kb::Render3D req;
    double       v = 0;
    req.set_side( a.value( "side", std::string() ) );
    req.set_quality( a.value( "quality", std::string() ) );
    req.set_floor( a.value( "floor", false ) );
    req.set_perspective( a.value( "perspective", false ) );

    if( getNumber( a, "rot_x", v ) ) req.set_rot_x( v );
    if( getNumber( a, "rot_y", v ) ) req.set_rot_y( v );
    if( getNumber( a, "rot_z", v ) ) req.set_rot_z( v );
    if( getNumber( a, "zoom", v ) ) req.set_zoom( v );
    if( getNumber( a, "width", v ) ) req.set_width( (int) v );
    if( getNumber( a, "height", v ) ) req.set_height( (int) v );

    kb::Render3DResponse r;
    std::string          err;

    if( !aIpc.Call( req, r, err ) )
        return { err, true };

    char head[160];
    std::snprintf( head, sizeof head, "3D %s, %d x %d px in %.1f s; %d part(s) with a model drawn", req.side().empty() ? "top" : req.side().c_str(),
                   r.width(), r.height(), r.seconds(), r.with_model() );
    std::string text = head;

    if( r.missing_size() )
    {
        // by reason, a few refs each
        std::map<std::string, std::vector<std::string>> by;

        for( const kb::Render3DMissing& m : r.missing() )
            by[m.why().substr( 0, m.why().find( " (" ) )].push_back( m.ref() );

        text += "; " + std::to_string( r.missing_size() ) + " drawn without a body:";

        for( const auto& [why, refs] : by )
        {
            text += "\n  " + why + ": " + std::to_string( refs.size() ) + " (";

            for( size_t i = 0; i < refs.size() && i < 12; ++i )
                text += ( i ? " " : "" ) + refs[i];

            text += refs.size() > 12 ? " ...)" : ")";
        }
    }

    if( a.contains( "path" ) && a["path"].is_string() && !a["path"].get<std::string>().empty() )
    {
        namespace fs = std::filesystem;
        fs::path p = a["path"].get<std::string>();

        if( p.is_relative() )
        {
            std::string              ferr;
            kb::BoardSummaryResponse s;

            if( aIpc.Call( kb::GetBoardSummary(), s, ferr ) )
                p = fs::path( s.file() ).parent_path() / p;
        }

        std::FILE* f = std::fopen( p.string().c_str(), "wb" );

        if( f && std::fwrite( r.png().data(), 1, r.png().size(), f ) == r.png().size() )
            text += "\nsaved " + p.string();
        else if( std::string why = MissingFolder( p.string() ); !why.empty() )
            text += "\nnot saved: " + why;
        else
            text += "\nnot saved: cannot write " + p.string();

        if( f )
            std::fclose( f );
    }

    TOOL_RESULT out{ text + "\n", false };
    out.image = r.png();
    return out;
}


std::string MissingFolder( const std::string& aPath )
{
    namespace fs = std::filesystem;
    fs::path        p( aPath );
    std::error_code ec;

    if( aPath.empty() || p.is_relative() || !p.has_parent_path() || fs::is_directory( p.parent_path(), ec ) )
        return {};

    return "path: the folder " + p.parent_path().string() + " does not exist (create it first)";
}


std::string UnsavedBoardChanges( IPC_CLIENT& aIpc )
{
    // what the unsaved changes are, so the choice is an informed one: undo back to the
    // saved state still counts as modified, and then board_diff finds nothing
    std::string           what;
    kb::BoardDiff         diffReq;
    kb::BoardDiffResponse diff;
    std::string           diffErr;
    diffReq.set_limit( 3 );

    if( !aIpc.Call( diffReq, diff, diffErr ) )
    {
        what = " (board_diff: " + diffErr + ")";
    }
    else if( diff.changes() == 0 )
    {
        what = "; board_diff finds it the same as the saved file (zone fills and net classes are not "
               "compared), e.g. undone back to it: discard=true loses nothing else";
    }
    else
    {
        what = "; board_diff: " + std::to_string( diff.changes() ) + " change(s) since saved:";

        for( const kb::BoardDiffSection& sec : diff.sections() )
        {
            if( sec.summary().empty() && sec.lines().empty() )
                continue;

            what += "\n  " + sec.name() + ": " + sec.summary();

            for( const std::string& line : sec.lines() )
                what += "\n    " + line;

            if( sec.more() > 0 )
                what += "\n    (" + std::to_string( sec.more() ) + " more)";
        }
    }

    return what;
}


namespace
{

/// project reload in the desktop editor: File > Revert for the board, and for the schematic
/// when the Schematic Editor has it open; the editors show the files as they are on disk.
TOOL_RESULT revertInEditor( IPC_CLIENT& aIpc, const std::string& aBoardFile )
{
    namespace kc = kiapi::common::commands;
    namespace kt = kiapi::common::types;
    namespace fs = std::filesystem;

    if( aBoardFile.empty() )
        return error( "no board is open to reload" );

    std::string             err;
    google::protobuf::Empty empty;
    kc::RevertDocument      pcb;
    pcb.mutable_document()->set_type( kt::DOCTYPE_PCB );
    pcb.mutable_document()->set_board_filename( fs::path( aBoardFile ).filename().string() );

    if( !aIpc.Call( pcb, empty, err ) )
        return error( "reloading the board failed: " + err );

    std::string note;
    kicadd::schematic::SchSummary sum;

    if( aIpc.Call( kicadd::schematic::GetSchSummary(), sum, err ) && !sum.file().empty() )
    {
        kc::RevertDocument sch;
        sch.mutable_document()->set_type( kt::DOCTYPE_SCHEMATIC );

        if( !aIpc.Call( sch, empty, err ) )
            note = "note: the schematic was not reloaded (" + err + ")\n";
        else
            note = "the schematic reloaded too\n";
    }

    TOOL_RESULT r = boardSummary( aIpc );
    r.text = "reloaded " + fs::path( aBoardFile ).filename().string() + " from disk in the editor (the agent's "
             "undo history starts over)\n" + note + r.text;
    return r;
}


TOOL_RESULT renameProject( IPC_CLIENT& aIpc, const json& aArgs );


TOOL_RESULT openBoard( IPC_CLIENT& aIpc, const json& aArgs )
{
    if( aArgs.contains( "rename" ) )
        return renameProject( aIpc, aArgs );

    namespace kc = kiapi::common::commands;
    namespace kt = kiapi::common::types;
    namespace fs = std::filesystem;

    const bool reload = aArgs.value( "reload", false );

    if( !reload && ( !aArgs.contains( "path" ) || !aArgs["path"].is_string() ) )
        return error( "path (absolute path to .kicad_pcb or .kicad_pro) is required" );

    const bool discard = aArgs.value( "discard", false );
    fs::path   pcb;

    if( reload )
    {
        kb::BoardSummaryResponse now;
        std::string              e;

        if( !aIpc.Call( kb::GetBoardSummary(), now, e ) || now.file().empty() )
            return error( "no board is open to reload" + ( e.empty() ? std::string() : ": " + e ) );

        pcb = fs::path( now.file() );
    }
    else
    {
        pcb = fs::path( aArgs["path"].get<std::string>() );
    }

    if( pcb.extension() == ".kicad_pro" )
        pcb.replace_extension( ".kicad_pcb" );

    if( pcb.extension() != ".kicad_pcb" )
        return error( "path must be a .kicad_pcb or .kicad_pro" );

    std::error_code ec;

    if( !pcb.is_absolute() )
        return error( "path must be absolute (on the server): " + pcb.string() );

    if( !fs::is_regular_file( pcb, ec ) )
        return error( "no such board: " + pcb.string() );

    // The board open now: unsaved changes are not thrown away unless asked
    std::string           err;
    kb::BoardSummaryResponse before;
    kb::HistoryResponse   history;

    if( !aIpc.Call( kb::GetBoardSummary(), before, err ) )
        before.Clear();
    else if( aIpc.Call( kb::GetHistory(), history, err ) && history.modified() && !discard )
    {
        const std::string what = UnsavedBoardChanges( aIpc );
        return error( "the open board (" + fs::path( before.file() ).filename().string()
                      + ") has unsaved changes: save_board first, or discard=true to drop them" + what );
    }

    // ... nor the schematic's
    {
        kicadd::schematic::SchSummary sum;
        std::string                   schErr;

        if( !discard && aIpc.Call( kicadd::schematic::GetSchSummary(), sum, schErr ) && sum.modified() )
            return error( "the open schematic has unsaved changes: sch_save first, or discard=true to drop them" );
    }

    // project reload in the desktop editor: KiCad's own File > Revert (asked first: closing
    // documents is refused there, and the user's Co-pilot panel would show that refusal)
    if( reload )
    {
        kb::GetCopilot   q;
        kb::CopilotState state;
        std::string      e;
        q.set_limit( 1 );

        if( aIpc.Call( q, state, e ) && state.desktop() )
            return revertInEditor( aIpc, before.file() );
    }

    // The server holds one project: close it (board, schematic, project), then open the other.
    // The desktop editor has no such command: the user opens files there.
    google::protobuf::Empty empty;

    if( !aIpc.Call( kc::CloseAllDocuments(), empty, err ) )
    {
        const bool desktop = err.find( "not available" ) != std::string::npos || err.starts_with( "AS_UNIMPLEMENTED" );

        // project reload in the desktop editor: KiCad's own revert (File > Revert), the editor
        // shows the file as it is on disk now
        if( desktop && aArgs.value( "reload", false ) )
            return revertInEditor( aIpc, before.file() );

        if( desktop )
            err += " (in the desktop editor, open the file in pcbnew; the agent works on the board that is open there)";

        return error( err );
    }

    auto open = [&]( const fs::path& aPath, kt::DocumentType aType, std::string& aErr )
    {
        kc::OpenDocument             req;
        kc::OpenDocumentResponse     r;
        req.set_type( aType );
        req.set_path( aPath.string() );
        return aIpc.Call( req, r, aErr );
    };

    // the board, then its schematic when there is one (the sch_ tools work on it)
    auto openProject = [&]( const fs::path& aPcb, std::string& aErr, std::string& aNote )
    {
        if( !open( aPcb, kt::DOCTYPE_PCB, aErr ) )
            return false;

        fs::path        sch = fs::path( aPcb ).replace_extension( ".kicad_sch" );
        std::error_code ec2;
        std::string     schErr;

        if( fs::is_regular_file( sch, ec2 ) && !open( sch, kt::DOCTYPE_SCHEMATIC, schErr ) )
            aNote = "note: the schematic did not open (" + schErr + "): sch_ tools have nothing to work on\n";

        return true;
    };

    std::string note;

    if( !openProject( pcb, err, note ) )
    {
        std::string back, backErr, backNote;

        if( !before.file().empty() && openProject( fs::path( before.file() ), backErr, backNote ) )
            back = "; " + fs::path( before.file() ).filename().string() + " is open again (as saved)";
        else if( !before.file().empty() )
            back = "; reopening " + before.file() + " failed too (" + backErr + "): project {open} it";

        return error( "could not open " + pcb.string() + ": " + err + back );
    }

    TOOL_RESULT r = boardSummary( aIpc );

    if( !r.isError && history.modified() && discard )
        note += "note: the unsaved changes of " + fs::path( before.file() ).filename().string() + " were dropped\n";

    if( reload )
        note = "reloaded " + pcb.filename().string() + " from disk (the agent's undo history starts over)\n" + note;

    r.text = note + r.text;
    return r;
}


/// project {rename}: the open project's files under a new name in the same folder (saved
/// first, then reopened). Its .kicad_pro / _pcb / _sch / _prl / _dru, the backups folder, and
/// a title block whose title is the old name (a ${PROJECTNAME} one follows by itself).
TOOL_RESULT renameProject( IPC_CLIENT& aIpc, const json& aArgs )
{
    namespace fs = std::filesystem;

    const std::string name = aArgs.value( "rename", std::string() );

    if( name.empty() || name.size() > 100 || name == "." || name == ".."
        || name.find_first_of( "/\\:*?\"<>|" ) != std::string::npos
        || std::any_of( name.begin(), name.end(), []( char c ) { return (unsigned char) c < 32; } ) )
        return error( "rename: the new project name, a file name without / \\ : * ? \" < > | (\"" + name + "\")" );

    {
        kb::GetCopilot   q;
        kb::CopilotState state;
        std::string      e;
        q.set_limit( 1 );

        if( aIpc.Call( q, state, e ) && state.desktop() )
            return error( "rename: not while the desktop editor has the project open (its files are KiCad's there): "
                          "the project manager's File > Save As makes the project under a new name" );
    }

    kb::BoardSummaryResponse now;
    std::string              err;

    if( !aIpc.Call( kb::GetBoardSummary(), now, err ) || now.file().empty() )
        return error( "no project is open to rename" + ( err.empty() ? std::string() : ": " + err ) );

    const fs::path    pcb( now.file() );
    const fs::path    dir = pcb.parent_path();
    const std::string old = pcb.stem().string();

    if( old == name )
        return error( "rename: the project is " + name + " already" );

    // the files that carry the name; the ones to be written must not be there
    std::vector<std::pair<fs::path, fs::path>> moves;
    std::error_code                            ec;

    for( const char* ext : { ".kicad_pro", ".kicad_pcb", ".kicad_sch", ".kicad_prl", ".kicad_dru" } )
    {
        fs::path from = dir / ( old + ext ), to = dir / ( name + ext );

        if( !fs::exists( from, ec ) )
            continue;

        if( fs::exists( to, ec ) )
            return error( "rename refused, nothing changed: " + to.filename().string() + " is there already" );

        moves.emplace_back( from, to );
    }

    if( fs::is_directory( dir / ( old + "-backups" ), ec ) && !fs::exists( dir / ( name + "-backups" ), ec ) )
        moves.emplace_back( dir / ( old + "-backups" ), dir / ( name + "-backups" ) );

    // saved first: what is on disk is what gets the new name
    if( TOOL_RESULT b = saveBoard( aIpc ); b.isError )
        return error( "rename refused, nothing changed: save_board failed: " + b.text );

    {
        kicadd::schematic::SchSummary sum;
        std::string                   e;

        if( aIpc.Call( kicadd::schematic::GetSchSummary(), sum, e ) && !sum.file().empty() )
        {
            kicadd::schematic::SchSave save;
            kicadd::schematic::SchConnectResponse r;

            if( !aIpc.Call( save, r, e ) )
                return error( "rename refused, nothing changed: sch_save failed: " + e );
        }
    }

    // let go of the files (lock files too), rename, open under the new name
    google::protobuf::Empty empty;

    if( !aIpc.Call( kiapi::common::commands::CloseAllDocuments(), empty, err ) )
        return error( "rename refused, nothing changed: could not close the project: " + err );

    std::vector<std::pair<fs::path, fs::path>> done;
    std::string                                failed;

    for( const auto& [from, to] : moves )
    {
        fs::rename( from, to, ec );

        if( ec )
        {
            failed = from.filename().string() + ": " + ec.message();
            break;
        }

        done.emplace_back( from, to );
    }

    auto readAll = []( const fs::path& f, std::string& t )
    {
        std::ifstream in( f, std::ios::binary );
        t.assign( std::istreambuf_iterator<char>( in ), {} );
        return (bool) in || in.eof();
    };

    auto writeAll = []( const fs::path& f, const std::string& t )
    {
        std::ofstream o( f, std::ios::binary | std::ios::trunc );
        o << t;
        return (bool) o;
    };

    std::vector<std::string> edits;

    if( failed.empty() )
    {
        // the project file's own name in it ("meta": {"filename": "old.kicad_pro"})
        for( const char* ext : { ".kicad_pro", ".kicad_prl" } )
        {
            fs::path   f = dir / ( name + ext );
            std::string t;

            if( !fs::exists( f, ec ) || !readAll( f, t ) )
                continue;

            try
            {
                json j = json::parse( t );

                if( j.contains( "meta" ) && j["meta"].is_object() && j["meta"].value( "filename", "" ) == old + ext )
                {
                    j["meta"]["filename"] = name + ext;
                    writeAll( f, j.dump( 2 ) + "\n" );
                }
            }
            catch( const std::exception& )
            {
            }
        }

        // a title block titled with the old name, written out (not ${PROJECTNAME})
        const std::string from = "(title \"" + old + "\")", to = "(title \"" + name + "\")";

        for( const char* ext : { ".kicad_pcb", ".kicad_sch" } )
        {
            fs::path    f = dir / ( name + ext );
            std::string t;

            if( !fs::exists( f, ec ) || !readAll( f, t ) )
                continue;

            if( size_t at = t.find( from ); at != std::string::npos )
            {
                t.replace( at, from.size(), to );

                if( writeAll( f, t ) )
                    edits.push_back( std::string( ext + 1 ) + " title block" );
            }
        }
    }
    else
    {
        for( auto it = done.rbegin(); it != done.rend(); ++it )
            fs::rename( it->second, it->first, ec );
    }

    const fs::path open = failed.empty() ? dir / ( name + ".kicad_pcb" ) : pcb;
    TOOL_RESULT    r = openBoard( aIpc, json{ { "path", open.string() }, { "discard", true } } );

    if( !failed.empty() )
        return error( "rename failed (" + failed + "): the files keep their old names; " + pcb.filename().string()
                      + ( r.isError ? " did not open again: " + r.text : " is open again" ) );

    std::string s = "renamed " + old + " -> " + name + " in " + dir.string() + " (saved first; reopened, the agent's undo "
                    "history starts over):";

    for( const auto& [from, to] : moves )
        s += " " + to.filename().string();

    s += "\n";

    if( !edits.empty() )
    {
        s += "  title \"" + old + "\" -> \"" + name + "\" in:";

        for( const std::string& e : edits )
            s += " " + e;

        s += "\n";
    }

    return { s + r.text, r.isError };
}


} // namespace


std::vector<TOOL> MakeTools( IPC_CLIENT& aIpc, bool aAllowLaunch )
{
    std::vector<TOOL> tools;

    tools.push_back( { "board_summary",
                       "Counts, layer count and board outline of the open PCB. Units: mm. KiCad's Y axis "
                       "points down. The vias by kind (type, pad / drill) and, with the board thickness, the "
                       "worst through-via aspect ratio (thickness / drill; your fab's limit judges it).", json::object(),
                       [&aIpc]( const json& ) { return boardSummary( aIpc ); }, "", std::vector<params::PARAM>{}} );

    tools.push_back( { "query_area",
                       "Items near a footprint (ref: the part itself first, then what is around it) or a point (x,y), nearest first. Lines: KIND handle [net] "
                       "layer @x,y (pads are REF.PADNUM WxH, the copper as placed; tracks start>end wWIDTH; ~DIST from the centre). "
                       "via_net=NET lists free via spots instead (off pads; in a QFN's pad ring too, marked *). "
                       "Use this instead of dumping the board.", json::object(),
                       [&aIpc]( const json& a ) { return queryArea( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Array( "kinds", params::Enum( "", { "footprint", "pad", "track", "arc", "via", "zone" } ), "Item kinds to include (default: footprint, pad, track, arc, via)" ),
                             params::Integer( "limit", "Max items (default 64; via spots 8)" ).Range( 0 ),
                             params::Number( "radius", "Radius, mm" ).Unit( "mm" ).Positive().Required(),
                             params::String( "ref", "Centre on this footprint" ),
                             params::String( "via_net", "Instead of items: free spots for a through via of this net (its class's size) within the radius, nearest first" ),
                             params::Number( "x", "Centre X, mm" ).Unit( "mm" ),
                             params::Number( "y", "Centre Y, mm" ).Unit( "mm" ) }} );

    tools.push_back( { "query_net",
                       "Pads, tracks, vias and zones on one net (topology of a single net).", json::object(),
                       [&aIpc]( const json& a ) { return queryNet( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Integer( "limit", "Max items (default 256)" ).Range( 0 ),
                             params::String( "net", "Net name, e.g. GND or /USB/D+" ).Required() }} );

    tools.push_back( { "move_footprints",
                       "Move/rotate/flip footprints as one undoable edit: refs with dx / dy / rotate / flip "
                       "(move, then rotate, then flip; each around its own anchor, or as a group with "
                       "pivot=group), or parts {\"U1\": {\"x\": 105, \"y\": 120, \"rotation\": 90, \"side\": "
                       "\"bottom\"}} to put each where it is to be (absolute: anchor mm, orientation as KiCad shows "
                       "it, side top / bottom; what is left out stays). Returns new positions (B.Cu = bottom side), the "
                       "unrouted-connection count and a local DRC of the moved parts (clearance, shorts, "
                       "hole clearance, courtyard overlap, board-edge clearance, copper outside the board). "
                       "Large nets (GND, power) skip ratsnest updates while you iterate; the count says which.", json::object(),
                       [&aIpc]( const json& a ) { return moveFootprints( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ),
                             params::Number( "dx", "Delta X, mm" ).Unit( "mm" ),
                             params::Number( "dy", "Delta Y, mm (down is +)" ).Unit( "mm" ),
                             params::Boolean( "flip", "Move to the other side (F.Cu <-> B.Cu), mirrored left-right" ),
                             params::Map( "parts", params::Any( "", {} ), "{ref: {x, y, rotation, side}}, absolute" ),
                             params::Enum( "pivot", { "each", "group" }, "Rotate/flip each part around its own anchor (default) or the parts as one group around their common centre" ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ),
                             params::Number( "rotate", "Rotation delta, degrees CCW" ).Unit( "°" ) }} );

    tools.push_back( { "lcsc_fetch",
                       "Get the footprint and schematic symbol of an LCSC part (e.g. C8734) from the "
                       "JLCEDA/EasyEDA Official Library: converted to KiCad, saved to the project "
                       "library LCSC (lcsc.pretty + lcsc.kicad_sym, added to fp-lib-table and "
                       "sym-lib-table; the symbol's Footprint field points at the footprint), cached for "
                       "offline use. Returns both lib_ids, size, pad numbers/positions and any pin/pad "
                       "mismatch. Its 3D model (EasyEDA's, converted to VRML in lcsc.3dshapes/, placed as "
                       "EasyEDA places it) comes along for the 3D viewer unless model=false. add_footprint "
                       "with lcsc does this by itself. Pin types: unspecified ones from their names, supplies the data "
                       "typed bidirectional too (said); pin_types {pin number or name: type} sets others (input, "
                       "output, bidirectional, tri_state, passive, power_in, power_out, open_collector, no_connect ...) "
                       "in the library symbol. Copper pads with no number (a USB-C shell) take the symbol's one pin with "
                       "no pad (EH), or unnumbered_pads=<pin>; said.", json::object(),
                       [&aIpc]( const json& a ) { return lcscFetch( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::String( "lcsc", "LCSC part number, e.g. C8734" ).Required(),
                             params::Boolean( "model", "the 3D model too (default true)" ),
                             params::Map( "pin_types", params::Any( "", {} ), "set pin types: {\"VREG_VOUT\": \"power_out\", \"12\": \"input\"}" ),
                             params::Boolean( "refresh", "Fetch again; replaces an edited footprint / 3D model" ),
                             params::String( "unnumbered_pads", "number shell pads with no number as this pin" ) }} );

    tools.push_back( { "add_footprint",
                       "Place a new footprint as one undoable edit: from a library (footprint = "
                       "\"Library:Name\", e.g. \"Capacitor_SMD:C_0603_1608Metric\"), from an LCSC part "
                       "number (lcsc = \"C8734\", fetched and saved to the project library), or as a copy "
                       "of a part already on the board (like = \"C12\"). ref \"C?\" takes the next free number. "
                       "nets maps pad numbers to nets; unknown nets are created and reported. Returns the "
                       "placement, unrouted count and local DRC.", json::object(),
                       [&aIpc]( const json& a ) { return addFootprint( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "back", "Place on the bottom side" ),
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ),
                             params::String( "footprint", "Library:Name" ),
                             params::String( "lcsc", "LCSC part number, e.g. C8734 (instead of footprint)" ),
                             params::String( "like", "Copy the footprint of this ref instead" ),
                             params::Map( "nets", params::String( "" ), "Pad number -> net name, e.g. {\"1\": \"+3V3\", \"2\": \"GND\"}" ).ItemSchema(),
                             params::String( "ref", "Reference, e.g. C301 or C? (default: prefix of like)" ),
                             params::Number( "rotate", "Orientation, degrees CCW" ).Unit( "°" ),
                             params::String( "value", "Value, e.g. 100nF" ),
                             params::Number( "x", "Position X, mm" ).Unit( "mm" ).Required(),
                             params::Number( "y", "Position Y, mm" ).Unit( "mm" ).Required() }} );

    tools.push_back( { "delete_footprints",
                       "Delete footprints as one undoable edit (undo restores them with their nets). "
                       "Tracks and vias stay; a net nothing else is on goes with them. Returns the new unrouted count.", json::object(),
                       [&aIpc]( const json& a ) { return deleteFootprints( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ).Required() }} );

    tools.push_back( { "delete_tracks",
                       "Delete routing (tracks, arcs, vias) as one undoable edit, e.g. before auto_place "
                       "on a routed board. all: every track; nets: by net name; refs: the copper running "
                       "from these parts' pads up to the next pad (rip up their connections); region: "
                       "[x0, y0, x1, y1] mm, anything crossing it. Given together, nets / refs / region narrow each "
                       "other (GND in this box). The answer lists what went per net. Locked tracks stay unless include_locked. "
                       "Zones are not touched (refill_zones afterwards). Returns the new unrouted count.", json::object(),
                       [&aIpc]( const json& a ) { return deleteTracks( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "all" ),
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ),
                             params::Boolean( "include_locked" ),
                             params::Boolean( "keep_vias" ),
                             params::Strings( "nets", "Net names, e.g. [\"GND\", \"/USB_D+\"]" ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ),
                             params::Array( "region", params::Number( "" ) ) }} );

    tools.push_back( { "route",
                       "Draw one connection the way the interactive router does (45-degree tracks around "
                       "obstacles; existing copper is never moved; the net class's width and via, the "
                       "clearance, keepout and edge rules). Not an autorouter: say what to connect. from: a "
                       "pad \"U1.5\" or a point [x, y] on the net's pad / track / via (inside a pad: the track leaves the "
                       "pad there, for a way out between close pads); to: likewise, or any point "
                       "(a free end), or \"zone\": the net's own pour, its nearest filled copper (on the start's layer, "
                       "else with a via down to the nearest layer that has it: a pad to a VBUS / GND pour); without to: "
                       "the nearest unrouted connection of from. waypoints: "
                       "[[x, y], ...] to pass through, [x, y, \"B.Cu\"] puts a via there and goes on on that "
                       "layer; via: [x, y] is short for one via to the other side (the end's layer); to \"zone\" with "
                       "the last via over the net's pour (on its layer; via: any layer): the route ends in that one via "
                       "(on the start point: only the via). layer: "
                       "where to start (default: the start pad's / the end's / F.Cu); width in mm. Reports "
                       "length vs the straight line, segments, vias and the narrowest gap; when blocked, how "
                       "far it got and what is in the way, and how far it leaves the box between two waypoints "
                       "(strict=true refuses that). One undoable edit; dry_run reports without "
                       "drawing. Mm, Y down.", json::object(),
                       [&aIpc]( const json& a ) { return routeTrack( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "dry_run" ),
                             params::Any( "from", {}, "\"U1.5\" or [x, y] (a point inside a pad: the track leaves the pad there, not at its centre)" ).Required(),
                             params::String( "layer" ),
                             params::Boolean( "strict", "refuse a route that strays off the waypoints" ),
                             params::Any( "to", {}, "\"C3.1\", [x, y], or \"zone\" (the net's pour); omit: ratsnest" ),
                             params::Array( "via", params::Number( "" ) ),
                             params::Array( "waypoints", params::Any( "", {} ) ),
                             params::Number( "width" ).Unit( "mm" ).Positive() }} );

    tools.push_back( { "route_short",
                       "Route the unrouted connections (ratsnest lines) of refs and / or nets, shortest "
                       "first (the short ones have the fewest ways round, so they go first and block the "
                       "least), each as route does; max_length (mm) leaves the long ones to the user. "
                       "Through-hole to through-hole: F.Cu, then B.Cu if blocked, unless layer is given; "
                       "when the nearest copper of the other end can't be reached (a track on the far "
                       "layer), the pads and vias joined to it, nearest first. "
                       "A connection that can't be done on one layer gets vias (vias=false: none): ends on "
                       "different layers one via on the way, on the same layer over the other side and back, "
                       "the spots along the straight line tried in turn, none inside the pads of an IC with an exposed / thermal "
                       "pad (a QFN: allow_under_ic=true allows it; an LQFP's body is free). The failures say how far they got and what is in the way "
                       "(delete_tracks or move the part). Nets with a copper zone that come in through refs are "
                       "skipped and listed (the pour joins them; include_poured=true, or name the net in nets, "
                       "to route them); skip_nets (globs) leaves others alone, e.g. a net to be poured later. "
                       "All of it is one undoable edit; dry_run reports "
                       "what would and would not route without drawing.", json::object(),
                       [&aIpc]( const json& a ) { return routeShort( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "allow_under_ic", "vias under a QFN's body too (default false)" ),
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ),
                             params::Boolean( "dry_run" ),
                             params::Boolean( "include_poured", "also nets with a zone (default: skipped)" ),
                             params::String( "layer" ),
                             params::Number( "max_length" ).Unit( "mm" ).Positive(),
                             params::Strings( "nets", "Net names, e.g. [\"GND\", \"/USB_D+\"]" ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ),
                             params::Strings( "skip_nets", "net globs left alone, e.g. [\"GND\"]" ),
                             params::Boolean( "vias", "default true" ) }} );

    tools.push_back( { "fanout",
                       "Fan out SMD pads of nets (e.g. GND, +3V3) to a copper zone of the same net on "
                       "another layer (an inner plane, or the other side of a 2-layer board): a short track "
                       "and a via next to each pad, inside that zone, as route would draw them. "
                       "Through-hole pads are skipped (they reach every layer), and so are pads already "
                       "joined to the zone; a net without such a zone gets a warning, not dangling vias. "
                       "refs: only these parts; layer: the zone's layer. One undoable edit; the zones are "
                       "refilled afterwards (refill=false to skip); dry_run reports without drawing.", json::object(),
                       [&aIpc]( const json& a ) { return fanout( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ),
                             params::Boolean( "dry_run" ),
                             params::String( "layer" ),
                             params::Strings( "nets", "Net names, e.g. [\"GND\", \"/USB_D+\"]" ).Required(),
                             params::Boolean( "refill" ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ) }} );

    tools.push_back( { "stitch_vias",
                       "Stitching vias for a net's pours (default GND): through vias on a grid (spacing, "
                       "default 5 mm; area [x0, y0, x1, y1] mm, default the whole pour), each inside the net's "
                       "filled copper on two or more layers (a net poured on one layer only, an inner plane: inside it, "
                       "area required), clear of pads, tracks, holes, the edge and rule "
                       "areas, and outside the parts' courtyards (under_parts=true allows them there). Then "
                       "one via in each island of the net's copper that has none (islands=false skips it). "
                       "Net class via size. The zones are refilled before and after; one undoable edit; "
                       "dry_run counts without adding. at: [x, y] or [[x, y], ...] mm (net required) puts one via "
                       "at each instead (no pour or grid needed; a via route can't place on its own), checked as "
                       "route's vias are, each said with what of the net it lands on (zone, track, pad, or nothing yet).", json::object(),
                       [&aIpc]( const json& a ) { return stitchVias( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Array( "area", params::Number( "" ) ),
                             params::Any( "at", { "array" }, "[x, y] mm or [[x, y], ...]: vias just there" ),
                             params::Boolean( "dry_run" ),
                             params::Boolean( "islands" ),
                             params::Integer( "max" ).Range( 0 ),
                             params::String( "net" ),
                             params::Boolean( "refill" ),
                             params::Number( "spacing", "mm" ).Unit( "mm" ).Positive(),
                             params::Boolean( "under_parts" ) }} );

    tools.push_back( { "teardrops",
                       "Teardrops (KiCad's own: copper that widens a track into its pad or via) on every "
                       "via and through-hole pad, or remove=true to take them off. smd=true adds SMD pads; "
                       "vias=false / pth=false leave those out; nets / refs narrow it (refs alone: those "
                       "parts' pads only). They follow later edits as in the editor. One undoable edit; the "
                       "zones are refilled.", json::object(),
                       [&aIpc]( const json& a ) { return teardrops( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "dry_run" ),
                             params::Strings( "nets", "Net names, e.g. [\"GND\", \"/USB_D+\"]" ),
                             params::Boolean( "pth" ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ),
                             params::Boolean( "remove" ),
                             params::Boolean( "smd" ),
                             params::Boolean( "vias" ) }} );

    tools.push_back( { "get_rules",
                       "The design rules: net classes (clearance, track width, via, diff pair; unset values "
                       "come from Default) with how many nets each has, the net class assignments by net name "
                       "pattern, and the board's minimum constraints. Lengths in mm.", json::object(),
                       [&aIpc]( const json& ) { return getRules( aIpc ); }, "", std::vector<params::PARAM>{}} );

    tools.push_back( { "set_rules",
                       "Change the design rules as one undoable edit (undo / redo; save_board writes them "
                       "to the project file). classes: {\"Power\": {\"track_width\": 0.5, \"clearance\": 0.25}} "
                       "adds or changes a class (only the values given; null: take it from Default); "
                       "{\"Power\": {\"current\": 2}} sets the track width a 2 A net needs (IPC-2221, the board's "
                       "outer copper, temp_rise 10 C unless given); {\"RF\": {\"impedance\": 50}} sets the track "
                       "width for 50 ohm and {\"USB\": {\"diff_impedance\": 90}} the diff pair width and gap for "
                       "90 ohm, from the board's stackup on impedance_layer (default F.Cu; board_setup stackup=... "
                       "first for a fab's numbers): the pair's width at diff_pair_gap if given, its gap at "
                       "diff_pair_width if given, else its width with the class's clearance as the gap; the note "
                       "gives the widths on the other layers and warns of reference layers not poured yet; "
                       ""
                       "remove_classes: [...]; assign: {\"+5V\": \"Power\", \"/USB_D*\": \"USB\", \"GND\": null} "
                       "sets or drops net name patterns; constraints: {\"min_clearance\", \"min_track_width\", "
                       "\"min_via_diameter\", \"min_through_drill\", \"min_annular_width\", \"hole_clearance\", "
                       "\"hole_to_hole\", \"copper_edge_clearance\", \"min_silk_text_height\", "
                       "\"min_silk_text_thickness\"} (mm). preset: a fab's standard process as the board minimums "
                       "(and the Default class raised to them): jlcpcb_2layer, jlcpcb_4layer, jlcpcb_6layer; the "
                       "rest of the call goes on top. Existing tracks keep their width. "
                       "drc (default true) runs DRC before and after and reports the change in errors.", json::object(),
                       [&aIpc]( const json& a ) { return setRules( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Map( "assign", params::Any( "", {} ) ),
                             params::Map( "classes", params::Any( "", {} ) ),
                             params::Map( "constraints", params::Any( "", {} ) ),
                             params::Boolean( "drc" ),
                             params::Enum( "preset", { "jlcpcb_2layer", "jlcpcb_4layer", "jlcpcb_6layer" } ),
                             params::Strings( "remove_classes", "Net class names to remove, e.g. [\"USB\"]" ) }} );

    tools.push_back( { "zones",
                       "Copper zones (pours): list, create, or change them. No arguments: the list (id, net, "
                       "layers, priority, clearance, minimum width, pad connection). create: a new zone, e.g. "
                       "{\"create\": {\"net\": \"GND\"}} pours GND on F.Cu and B.Cu over the whole board; "
                       "{\"create\": {\"net\": \"GND\", \"layers\": [\"In1.Cu\"]}} makes an inner plane; "
                       "rect [x0, y0, x1, y1] or outline [[x, y], ...] (mm) for a part of the board. Where zones of different nets "
                       "overlap, the higher priority is filled and the other keeps clear: a new zone inside a "
                       "bigger one is put above it, and an overlap at the same priority is warned about. "
                       "Change settings: choose with nets, layers, ids (from the list) or all=true, and give "
                       "clearance, min_width (mm), priority, connection (solid / thermal / none / thermal_tht), "
                       "thermal_gap, thermal_spoke (mm). Each is one undoable edit, refilled afterwards unless "
                       "refill=false.", json::object(),
                       [&aIpc]( const json& a ) { return zonesTool( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "all" ),
                             params::Number( "clearance" ).Unit( "mm" ).Range( 0 ),
                             params::Enum( "connection", { "solid", "thermal", "none", "thermal_tht" } ),
                             params::Object( "create",
                                 { params::Number( "clearance" ).Unit( "mm" ).Range( 0 ),
                                 params::Enum( "connection", { "solid", "thermal", "none", "thermal_tht" } ),
                                 params::Strings( "layers", "default [\"F.Cu\", \"B.Cu\"]; [\"all\"]: every copper layer" ),
                                 params::Number( "min_width" ).Unit( "mm" ).Positive(),
                                 params::String( "name" ),
                                 params::String( "net", "e.g. GND" ),
                                 params::Array( "outline", params::Any( "", {} ), "[[x, y], ...] in mm; default the board outline" ),
                                 params::Integer( "priority", "default 0, or above the bigger zones it lies in" ).Range( 0 ),
                                 params::Array( "rect", params::Number( "" ), "[x0, y0, x1, y1] in mm: a rectangle" ) }, "a new zone (not together with nets / layers / ids / all)" ),
                             params::Strings( "ids", "Zone ids from the list" ),
                             params::Strings( "layers", "Layer names, e.g. [\"F.Cu\", \"In1.Cu\"]" ),
                             params::Number( "min_width" ).Unit( "mm" ).Positive(),
                             params::Strings( "nets", "Net names, e.g. [\"GND\", \"/USB_D+\"]" ),
                             params::Integer( "priority" ).Range( 0 ),
                             params::Boolean( "refill" ),
                             params::Number( "thermal_gap" ).Unit( "mm" ).Range( 0 ),
                             params::Number( "thermal_spoke" ).Unit( "mm" ).Positive() }} );

    tools.push_back( { "board_outline",
                       "Draw the board outline on Edge.Cuts (the old board-level Edge.Cuts shapes are "
                       "replaced unless keep_old): rect [x0, y0, x1, y1] with corner_radius, or circle [cx, cy, "
                       "diameter] (mm, Y down). cutouts: [{rect, corner_radius} | {circle}] inside it. Mounting "
                       "holes: hole_inset (4 holes of hole_diameter, default 3.2 = M3, each centre hole_inset mm in from the "
                       "outline's corner along x and along y: its box's corner, a rounded corner's too; the hole's edge "
                       "is half its diameter nearer the edge) and / or "
                       "holes [[x, y, d], {x, y, diameter, plated, net}]: NPTH, or plated with a ring (e.g. to GND); "
                       "they become footprints H1, H2 ... out of the BOM and the CPL; one where a part already has "
                       "a hole is left out. Reports the size, area, the parts now outside or on a cut-out, and "
                       "warns of holes on parts or tracks and of tracks a cut-out or the outline cuts. One "
                       "undoable edit. Then zones create for a pour.", json::object(),
                       [&aIpc]( const json& a ) { return boardOutline( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Array( "circle", params::Number( "" ) ),
                             params::Number( "corner_radius" ).Unit( "mm" ).Range( 0 ),
                             params::Array( "cutouts", params::Any( "", {} ) ),
                             params::Number( "hole_diameter" ).Unit( "mm" ).Positive(),
                             params::Number( "hole_inset" ).Unit( "mm" ).Range( 0 ),
                             params::Array( "holes", params::Any( "", {} ) ),
                             params::Boolean( "keep_old" ),
                             params::Array( "rect", params::Number( "" ) ) }} );

    tools.push_back( { "board_transform",
                       "Move the whole board: footprints, tracks, vias, zones (their fills with them), graphics, "
                       "texts, dimensions, groups and the outline, and the drill / grid origins when set, as one "
                       "undoable edit. dx / dy (mm), or center_on_sheet=true: the outline's box to the middle of "
                       "the page (the shift to the nearest mm). Reports what moved, the board's box before and "
                       "after, and the unrouted count before and after (the same, or it says so). dry_run: what it "
                       "would do. Use it instead of editing the file.", json::object(),
                       [&aIpc]( const json& a ) { return boardTransform( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "center_on_sheet" ),
                             params::Boolean( "dry_run" ),
                             params::Number( "dx", "mm" ).Unit( "mm" ),
                             params::Number( "dy", "mm, Y down" ).Unit( "mm" ) }} );

    tools.push_back( { "pcb_text",
                       "Board texts (not a part's fields: pcb_set_fields). No arguments: the list, with ids "
                       "(layer: one layer only). add: [{text, at: [x, y], layer (default F.SilkS), size (mm, "
                       "height), thickness, angle (deg), justify (left / center / right), locked}]; edit: "
                       "[{id | match, and any of text, at, by: [dx, dy], layer, size, thickness, angle, justify, "
                       "locked}]; remove: [id | {match}]. match is the text as written; ${VARS} (e.g. "
                       "${REVISION}) stay as written and the list shows what they give. A back layer reads "
                       "mirrored, as in the editor. One undoable edit for the whole call.", json::object(),
                       [&aIpc]( const json& a ) { return pcbText( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Array( "add", params::Any( "", {} ) ),
                             params::Array( "edit", params::Any( "", {} ) ),
                             params::String( "layer" ),
                             params::Array( "remove", params::Any( "", {} ) ) }} );

    tools.push_back( { "board_setup",
                       "The board's layers and stackup. No arguments: what they are (copper layers, thickness, "
                       "each layer of the stackup). copper_layers (2, 4, 6, ...), thickness (mm, e.g. 1.6), "
                       "outer_copper_oz / inner_copper_oz (e.g. 1 and 0.5): the stackup is made anew like Board "
                       "Setup's default. stackup=\"JLC04161H-7628\" (or another of JLCPCB's impedance "
                       "stackups, 4 and 6 layers, nominal 1.6 mm, though the layers in JLCPCB's tables add up to 1.45-1.65 mm; stackup=\"list\" names them) sets the fab's own: "
                       "layer count, prepreg / core thicknesses and dielectric constants, for controlled-"
                       "impedance widths. Fewer layers only when the ones that go are empty. One undoable edit. "
                       "Pair with set_rules preset (e.g. jlcpcb_4layer) and zones create on the inner layers.", json::object(),
                       [&aIpc]( const json& a ) { return boardSetup( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Integer( "copper_layers" ).Range( 1, 32 ),
                             params::Number( "inner_copper_oz" ).Unit( "oz" ).Positive(),
                             params::Number( "outer_copper_oz" ).Unit( "oz" ).Positive(),
                             params::String( "stackup" ),
                             params::Number( "thickness" ).Unit( "mm" ).Positive() }} );

    tools.push_back( { "impedance",
                       "Controlled impedance from the board's stackup, per copper layer; changes nothing (set_rules "
                       "impedance / diff_impedance writes it into a net class). A layer's reference planes are the "
                       "nearest plane layers each way: zones over 40% of the board (an unpoured layer: typed power, "
                       "or empty), not by name; signal layers between are passed over (their copper in H, said). An "
                       "outer layer is a microstrip over its plane (with the solder mask), an inner one a stripline "
                       "between two, or an embedded microstrip with a plane on one side; the note lists each "
                       "layer's coverage. The planes must be poured (zones create) for it to hold. z: a track's "
                       "width for this Z0 (ohm); z_diff: a pair's width at gap (mm, default 0.2), or its gap at "
                       "width; neither: the impedance of width (and gap: of the pair). ground_gap (mm): ground "
                       "copper beside a single track on an outer layer (coplanar waveguide with ground, CPWG: a "
                       "2-layer board's way to a lower Z0 at a usable width; pairs with a pour beside them are not "
                       "worked out). layers: default all. Uses "
                       "KiCad's transmission line calculations (as pcb_calculator); set board_setup stackup=... "
                       "first for a fab's dielectrics, and check with the fab's calculator before ordering.", json::object(),
                       [&aIpc]( const json& a ) { return impedanceTool( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Number( "gap" ).Unit( "mm" ).Positive(),
                             params::Number( "ground_gap", "mm to ground copper beside it (CPWG)" ).Unit( "mm" ).Positive(),
                             params::Strings( "layers", "copper layers, e.g. [\"F.Cu\", \"In2.Cu\"]; default all" ),
                             params::Number( "width" ).Unit( "mm" ).Positive(),
                             params::Number( "z" ).Unit( "Ω" ).Positive(),
                             params::Number( "z_diff" ).Unit( "Ω" ).Positive() }} );

    tools.push_back( { "pcb_set_fields",
                       "Change footprints' fields and attributes on the board as one undoable edit. parts: "
                       "{\"R1\": {\"Value\": \"10k\", \"LCSC\": \"C25804\", \"dnp\": true}}, or refs [...] with "
                       "set {...} for many. Value is the value; other names are user fields (added hidden when "
                       "missing; null removes one). Reference renumbers the part: {\"R22\": {\"Reference\": \"R23\"}} "
                       "(to a reference no other part has; of two parts with the same one, the one placed by "
                       "hand). Attributes: dnp, exclude_from_bom, exclude_from_pos. The footprint is not "
                       "changed here. A board made from a schematic takes its fields and references from it: "
                       "change them there (sch_set_fields), then sync_pcb. Refs that are not found are listed; "
                       "the other parts are changed anyway. A field's layout: {\"R1\": {\"Value\": {\"layer\": "
                       "\"silk\", \"visible\": true, \"at\": [0, 1.5], \"size\": 0.8, \"justify\": \"left\"}}}: "
                       "layer a name or silk / fab (the part's side), at [dx, dy] mm from the part's origin (board "
                       "axes), size the height (mm), thickness, angle (deg, on the board), text too if wanted; only "
                       "what is given changes. silk: \"value_only\" / \"ref_only\" / \"both\" / \"none\": which of "
                       "Reference and Value are on the silkscreen, the other shown on Fab (e.g. refs [...] with set "
                       "{\"silk\": \"value_only\"}). sync_pcb keeps layouts.", json::object(),
                       [&aIpc]( const json& a ) { return pcbSetFields( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Map( "parts", params::Map( "", params::Any( "", {} ) ) ).ItemSchema(),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ),
                             params::Map( "set", params::Any( "", {} ) ) }} );

    tools.push_back( { "undo",
                       "Undo the last KiCadD edit (move/rotate/flip/add/delete/auto_place/delete_tracks), restoring the exact previous "
                       "state. Parts the user changed since are left alone; an edit the user already undid "
                       "in the editor (Ctrl+Z) is reported as such. Tracks, zones or the outline put back: the zones are refilled. "
                       "Returns what was undone, new positions and local DRC (around moved parts).", json::object(),
                       [&aIpc]( const json& a ) { return history( aIpc, true, detailOf( a ) ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ) }} );

    tools.push_back( { "redo",
                       "Redo the last undone edit.", json::object(),
                       [&aIpc]( const json& a ) { return history( aIpc, false, detailOf( a ) ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ) }} );

    tools.push_back( { "check_drc",
                       "Local DRC for the given footprints against their surroundings, without moving them.", json::object(),
                       [&aIpc]( const json& a ) { return checkDrc( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "detail", "List every part / violation (default: long lists cut short)" ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ).Required() }} );

    tools.push_back( { "flush_ratsnest",
                       "Rebuild the ratsnest of large nets deferred during moves; returns the complete "
                       "unrouted-connection count.", json::object(),
                       [&aIpc]( const json& ) { return flushRatsnest( aIpc ); }, "", std::vector<params::PARAM>{}} );

    tools.push_back( { "run_drc",
                       "Full-board DRC (all KiCad checks: clearance, shorts, unconnected, courtyards, "
                       "silk, edge, holes, ...). Takes seconds on large boards. Output: whole-board counts by type, "
                       "then (with types/refs/include_warnings=false) the counts matching the filter, "
                       "then violations (errors first) as 'E|W type itemA[net] x itemB[net] layer @x,y "
                       "detail'. Refill zones first if parts moved.", json::object(),
                       [&aIpc]( const json& a ) { return runDrc( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "include_warnings", "Default true" ),
                             params::Integer( "limit", "Max violations listed (default 40)" ).Range( 0 ),
                             params::Strings( "refs", "Only violations involving these footprints" ),
                             params::Strings( "types", "Only these types, e.g. [\"clearance\", \"shorting_items\"]" ) }} );

    tools.push_back( { "refill_zones",
                       "Refill all copper zones. Fills go stale when parts move; refill before judging "
                       "copper connectivity, running full DRC or saving for fabrication. Pours that come "
                       "out empty, lose over half their copper, or join nothing of their net are listed "
                       "(as after every tool that refills).", json::object(),
                       [&aIpc]( const json& ) { return refillZones( aIpc ); }, "", std::vector<params::PARAM>{}} );

    tools.push_back( { "lcsc_search",
                       "Find JLCPCB / LCSC parts by specification, e.g. value \"100nF\", package \"0402\", "
                       "dielectric \"X5R\", voltage_min 16, basic_only; or keyword \"AMS1117 3.3\". Searches "
                       "KiCadD's parts index (JLCPCB parts in stock, daily data from jlcparts; about 45 MB, "
                       "downloaded on first use, refreshed weekly), basic parts first, then preferred "
                       "(no extended fee), then by stock; the first (live, default 1; each is an EasyEDA request, rate-limited with lcsc_fetch) is checked "
                       "now against LCSC. Each hit lists its electrical parameters when the index has them "
                       "(MOSFETs, LDOs, op-amps, inductors, diodes: Rds(on), Qg, Ciss, dropout, PSRR, GBW, "
                       "slew, DCR, Isat, Vf, trr ...; the datasheet's single test condition after @). Filter "
                       "on them with where {\"rds_on\": \"<=20m\", \"vds\": \">=30\", \"vgs_th\": \"1..2.5\"}: "
                       "SI units, prefixes allowed; a part without the parameter does not match (a bad name "
                       "lists them). Inductor Isat is in the data for about half the inductors and for no "
                       "basic / preferred one: with where i_sat leave basic_only off (or filter on i_rated). "
                       "Then sch_add_part lcsc=... places one.", json::object(),
                       [&aIpc]( const json& a ) { return lcscSearch( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "basic_only" ),
                             params::String( "category", "resistor, capacitor, inductor, or a JLCPCB category word (\"LDO\", \"MOSFET\")" ),
                             params::String( "dielectric" ),
                             params::String( "keyword" ),
                             params::Integer( "limit" ).Range( 0 ),
                             params::Integer( "live" ),
                             params::Integer( "min_stock" ).Range( 0 ),
                             params::String( "package" ),
                             params::Number( "tolerance", "%, at most" ).Unit( "%" ).Range( 0 ),
                             params::Boolean( "update", "fetch the index again now" ),
                             params::String( "value", "100nF, 10k, 4k7, 2.2uH" ),
                             params::Number( "voltage_min", "V rating, at least" ).Unit( "V" ).Range( 0 ),
                             params::Map( "where", params::Any( "", {} ), "parameter -> condition: \"<=20m\", \">=30V\", \"1u..10u\", \"3.3\"" ) }} );

    tools.push_back( { "board_render",
                       "A picture (PNG) of the board as it is now, to look at what coordinates do not "
                       "show: silkscreen over pads or other text, part orientation, crowding, how tracks "
                       "run, what is left unrouted. KiCad's own plot of the layers (default front: all "
                       "copper, front silkscreen, outline; back=true from below, mirrored). refs are framed "
                       "and labelled, nets drawn in cyan, and with either the picture zooms to them (zoom=false "
                       "keeps the whole board); marks [[x, y, \"label\"], ...] mm rings points (audit's IR "
                       "hotspots: the worst load, the busiest via) and zooms to them too; area [x0, y0, x1, y1] mm for a region. Rulers in mm on the "
                       "top and left. size: pixels on the long side (default 1200). labels=true writes every "
                       "part's reference. path also saves the PNG (relative: in the project folder).", json::object(),
                       [&aIpc]( const json& a ) { return boardRender( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Array( "area", params::Number( "" ), "[x0, y0, x1, y1] mm" ),
                             params::Boolean( "back" ),
                             params::Boolean( "labels" ),
                             params::Strings( "layers", "front, back, copper, silk, fab, assembly, or layer names (F.Cu, In1.Cu, F.Silkscreen, F.Fab, ...)" ),
                             params::Number( "margin", "mm around a zoom (3)" ).Unit( "mm" ).Range( 0 ),
                             params::Array( "marks", params::Array( "", params::Any( "", {} ) ), "[[x, y, \"label\"], ...] mm" ),
                             params::Strings( "nets" ),
                             params::String( "path" ),
                             params::Boolean( "ratsnest", "default true" ),
                             params::Strings( "refs" ),
                             params::Integer( "size" ).Range( 0 ),
                             params::Boolean( "zoom" ) }} );

    tools.push_back( { "board_render_3d",
                       "A 3D picture (PNG) of the board as it is now (unsaved edits included), from KiCad's own "
                       "raytracer (as kicad-cli pcb render; no OpenGL or screen): to see the assembled board, "
                       "connectors at the edge, tall parts, the bottom side. side: top (default), bottom, front, "
                       "back, left, right; rot_x / rot_y / rot_z degrees after it (e.g. side top, rot_x -45 for a "
                       "tilted view); zoom (default 1); width / height pixels (default 1200 x 900); quality basic "
                       "or high (reflections, shadows: slower); floor; perspective. Only VRML models load in this "
                       "build: the parts drawn without a body are listed (no model, file not found, STEP). "
                       "In the desktop editor the window waits while it renders (seconds). path also saves the "
                       "PNG (relative: in the project folder).", json::object(),
                       [&aIpc]( const json& a ) { return boardRender3d( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "floor" ),
                             params::Integer( "height" ).Positive(),
                             params::String( "path" ),
                             params::Boolean( "perspective" ),
                             params::Enum( "quality", { "basic", "high" } ),
                             params::Number( "rot_x" ),
                             params::Number( "rot_y" ),
                             params::Number( "rot_z" ),
                             params::Enum( "side", { "top", "bottom", "front", "back", "left", "right" } ),
                             params::Integer( "width" ).Positive(),
                             params::Number( "zoom" ).Positive() }} );

    tools.push_back( { "export_assembly",
                       "Write the assembly drawing (PDF) from the board as it is now: a page for the top and, when "
                       "parts are on the bottom, one for the bottom seen from below, each with the title block and "
                       "scaled to the page. KiCad's plot of the Fab layer (outlines, reference text, pads sketched) and "
                       "the outline; parts with no Fab drawing (LCSC / EasyEDA footprints) get their outline, pin 1 and "
                       "reference from the courtyard; DNP parts are crossed out. values=true adds the values. dir "
                       "defaults to <project>/fab (<board>-assembly.pdf), path names the file.", json::object(),
                       [&aIpc]( const json& a ) -> TOOL_RESULT
                       {
                           kb::ExportAssembly req;
                           req.set_dir( a.value( "dir", std::string() ) );
                           req.set_path( a.value( "path", std::string() ) );
                           req.set_values( a.value( "values", false ) );
                           kb::ExportAssemblyResponse r;
                           std::string                err;

                           if( !aIpc.Call( req, r, err ) )
                               return { err, true };

                           std::string t = "assembly drawing " + r.file() + ": " + std::to_string( r.pages() ) + " page(s); "
                                           + std::to_string( r.front() ) + " part(s) on top, " + std::to_string( r.back() )
                                           + " on the bottom";

                           if( r.drawn() )
                               t += "; " + std::to_string( r.drawn() ) + " without a Fab outline drawn from their courtyard";

                           if( r.dnp() )
                               t += "; " + std::to_string( r.dnp() ) + " DNP crossed out";

                           return { t + "\n", false };
                       }, "", std::vector<params::PARAM>{
                             params::String( "dir" ),
                             params::String( "path" ),
                             params::Boolean( "values" ) }} );

    tools.push_back( { "export_fab",
                       "Write the fabrication files for JLCPCB from the board as it is now (unsaved edits "
                       "included): Gerbers (Protel extensions, X2, job file) and Excellon drill (mm) zipped "
                       "for the upload, BOM (Comment, Designator, Footprint, LCSC Part #) and CPL "
                       "(Designator, Mid X, Mid Y, Layer, Rotation) as CSV. Refills the zones first "
                       "(refill=false skips it), then runs the full DRC and puts its error count first "
                       "(drc=false skips it). strict=true writes nothing when DRC has errors, a reference "
                       "is on two footprints or a part is outside the outline. dir defaults to <project>/fab. "
                       "Notes name the DNP and exclude-from-BOM parts left out, and flag excluded parts wired "
                       "like fitted ones (pads on 2+ nets): nobody buys or places them.", json::object(),
                       [&aIpc]( const json& a ) { return exportFab( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::String( "dir" ),
                             params::Boolean( "drc", "default true" ),
                             params::Boolean( "refill", "default true" ),
                             params::Boolean( "strict", "refuse to write when DRC has errors or a reference / placement problem is found" ),
                             params::Boolean( "zip", "default true" ) }} );

    tools.push_back( { "open_board",
                       "Open a .kicad_pcb (or .kicad_pro) on the server (kicad-cli api-server), replacing the current "
                       "project: the board, its schematic when there is one, the project's rules. Reopening the same "
                       "file reloads it from disk. The agent history starts over. Refused when the open board has "
                       "unsaved changes, unless discard=true (the refusal lists what board_diff finds changed). Not available with the desktop editor: there the user "
                       "opens the file in pcbnew.", json::object(),
                       [&aIpc]( const json& a ) { return openBoard( aIpc, a ); }, "", std::vector<params::PARAM>{
                             params::Boolean( "discard", "Drop the open board's unsaved changes (default false)" ),
                             params::String( "path", "Absolute path on the server" ).Required() }} );

    tools.push_back( { "save_board",
                       "Write the in-memory board back to its .kicad_pcb file. It is written in this build's "
                       "file format (KiCad nightly, 10.99): a file from KiCad 9 / 10 is upgraded and those "
                       "versions may not open it any more (as when KiCad itself saves it).", json::object(),
                       [&aIpc]( const json& ) { return saveBoard( aIpc ); }, "", std::vector<params::PARAM>{}} );

    tools.push_back( { "silk_tidy",
                       "Move silkscreen references that are over an exposed pad (within 0.15 mm), over another "
                       "silkscreen text, or off the board, to the nearest free spot next to their part (above, "
                       "below, left, right, the corners, further out ring by ring; inside a big part last), "
                       "horizontal or vertical as the part reads; a spot nearer another part's courtyard than "
                       "its own costs more, and a row of like parts gets its texts on one side. Also counts "
                       "the texts nearer another part than their own (a reader takes them for that part's). refs: only these parts (default all); "
                       "all=true places them all, not just the ones with a problem; values=true does visible "
                       "values too; gap mm between texts (0.1); dry_run=true only says what would move. One "
                       "undoable edit. Look at it with board_render layers=[\"silk\"].", json::object(),
                       [&aIpc]( const json& a ) -> TOOL_RESULT
                       {
                           kb::SilkTidy req;

                           for( const std::string& r : getStrings( a, "refs" ) )
                               req.add_refs( r );

                           req.set_all( a.value( "all", false ) );
                           req.set_values( a.value( "values", false ) );
                           req.set_gap( a.value( "gap", 0.0 ) );
                           req.set_dry_run( a.value( "dry_run", false ) );

                           kb::SilkTidyResponse r;
                           std::string          err;

                           if( !aIpc.Call( req, r, err ) )
                               return error( err );

                           const bool detail = a.value( "detail", false );
                           std::string t = std::string( req.dry_run() ? "would move " : "moved " )
                                           + std::to_string( r.moved_size() ) + " text(s) of "
                                           + std::to_string( r.checked() ) + " checked; texts with a problem "
                                           + std::to_string( r.problems_before() ) + " -> "
                                           + std::to_string( r.problems_after() )
                                           + ( req.dry_run() ? " (as planned)" : "" );

                           if( !r.history_label().empty() )
                               t += " (undo takes it back)";

                           for( int i = 0; i < r.moved_size(); i++ )
                           {
                               if( !detail && i == 15 )
                               {
                                   t += "\n  ... " + std::to_string( r.moved_size() - 15 ) + " more (detail=true)";
                                   break;
                               }

                               t += "\n  " + r.moved( i );
                           }

                           for( const std::string& s : r.stuck() )
                               t += "\nleft as is: " + s;

                           if( r.nearer_before() || r.nearer_after() )
                           {
                               t += "\nnearer another part than their own: " + std::to_string( r.nearer_before() )
                                    + " -> " + std::to_string( r.nearer_after() )
                                    + ( r.nearer_after() ? " (all=true with refs places them)" : "" );

                               for( int i = 0; i < r.nearer_size() && i < ( detail ? 1000 : 8 ); i++ )
                                   t += "\n  " + r.nearer( i );

                               if( !detail && r.nearer_size() > 8 )
                                   t += "\n  ... " + std::to_string( r.nearer_size() - 8 ) + " more (detail=true)";
                           }

                           return { t + "\n" };
                       }, "", std::vector<params::PARAM>{
                             params::Boolean( "all" ),
                             params::Boolean( "detail", "every move (default: 15)" ),
                             params::Boolean( "dry_run" ),
                             params::Number( "gap" ).Unit( "mm" ).Range( 0 ),
                             params::Strings( "refs", "Reference designators, e.g. [\"U5\", \"C12\"]" ),
                             params::Boolean( "values" ) }} );

    tools.push_back( { "board_diff",
                       "What changed since the board was last saved (or against another .kicad_pcb, file; "
                       "relative: the project folder): footprints added / removed / moved / rotated / flipped / "
                       "renumbered, value, footprint, fields, texts on the part (moved / turned / hidden), DNP / BOM, "
                       "pads that changed net; nets added and "
                       "removed; tracks per net (segments, length, vias); zones; outline, layers, graphics. "
                       "Footprints are paired by UUID, else by reference. Read only. limit: lines per "
                       "section (20). Zone fills and net classes are not compared.", json::object(),
                       [&aIpc]( const json& a ) -> TOOL_RESULT
                       {
                           kb::BoardDiff req;
                           req.set_file( a.value( "file", std::string() ) );
                           req.set_limit( a.value( "limit", 0 ) );

                           kb::BoardDiffResponse r;
                           std::string           err;

                           if( !aIpc.Call( req, r, err ) )
                               return error( err );

                           std::string t = r.changes() == 0
                                                   ? "no changes against " + r.file()
                                                   : std::to_string( r.changes() ) + " change(s) against " + r.file();

                           if( !r.file_time().empty() )
                               t += " (saved " + r.file_time() + ")";

                           for( const kb::BoardDiffSection& sec : r.sections() )
                           {
                               t += "\n" + sec.name() + ": " + sec.summary();

                               for( const std::string& line : sec.lines() )
                                   t += "\n  " + line;

                               if( sec.more() > 0 )
                                   t += "\n  ... " + std::to_string( sec.more() ) + " more (limit)";
                           }

                           return { t + "\n" };
                       }, "", std::vector<params::PARAM>{
                             params::String( "file" ),
                             params::Integer( "limit" ).Range( 0 ) }} );

    AddSchematicTools( tools, aIpc );     // M5a, sch_tools.cpp
    tools.push_back( PdnTool( aIpc ) );           // phase 3 item 3, pdn.cpp
    tools.push_back( ReflectionTool( aIpc ) );    // phase 3 item 4, reflect.cpp
    AddLaunchTools( tools, aIpc, aAllowLaunch );  // phase 4 item 3, launch.cpp

    // batch: a list of the tools above in one call (a copy of the table: the list is handed
    // over to the server afterwards)
    auto table = std::make_shared<std::vector<TOOL>>( tools );

    // new_project (phase 4 item 3, project.cpp) runs the tools above from the table (and batch can run it)
    tools.push_back( NewProjectTool( table ) );
    table->push_back( tools.back() );

    // beta.13 phase 2: undo, set_rules, run_drc, board_summary, project, export, board_render
    // take over the thin tools of their domain (which go)
    MergeTools( tools );

    tools.push_back( { "batch",
                       "Run several tools in one call, in order: steps [{\"tool\": \"move_footprints\", "
                       "\"args\": {...}}, ...], repeated repeat times (default 1). Each step answers with its "
                       "first line and its warnings (show=\"full\": everything; \"last\": everything of the last step only). "
                       "Stops at the first error unless stop_on_error=false. At most 500 calls. For repeated "
                       "or routine work (the same edit on many parts, edit-check loops); a step's answer "
                       "cannot feed the next one's arguments.", json::object(),
                       [table]( const json& a ) -> TOOL_RESULT
                       {
                           const json& steps = a.value( "steps", json::array() );
                           const int   repeat = std::max( 1, a.value( "repeat", 1 ) );
                           const bool  stop = a.value( "stop_on_error", true );
                           const std::string show = a.value( "show", std::string( "line" ) );

                           if( !steps.is_array() || steps.empty() )
                               return error( "steps: [{\"tool\": name, \"args\": {...}}, ...]" );

                           if( steps.size() * repeat > 500 )
                               return error( "at most 500 calls (steps x repeat)" );

                           std::vector<const TOOL*> resolved;

                           for( const json& st : steps )
                           {
                               std::string name = st.is_object() ? st.value( "tool", std::string() ) : std::string();
                               auto it = std::find_if( table->begin(), table->end(),
                                                       [&]( const TOOL& t ) { return t.name == name; } );

                               if( name == "batch" || it == table->end() )
                                   return error( "no tool " + name + ( name == "batch" ? " inside a batch" : "" ) );

                               resolved.push_back( &*it );
                           }

                           const auto t0 = std::chrono::steady_clock::now();
                           std::string body;
                           int         done = 0, failed = 0, total = (int) steps.size() * repeat;
                           bool        stopped = false;
                           std::string lastText;

                           for( int r = 0; r < repeat && !stopped; ++r )
                           {
                               for( size_t i = 0; i < steps.size(); ++i )
                               {
                                   json args = steps[i].contains( "args" ) && steps[i]["args"].is_object()
                                                       ? steps[i]["args"] : json::object();
                                   TOOL_RESULT res = resolved[i]->run( args );
                                   done++;
                                   failed += res.isError;
                                   lastText = res.text;

                                   std::string tag = repeat > 1 ? std::to_string( r + 1 ) + "." + std::to_string( i + 1 )
                                                                : std::to_string( i + 1 );
                                   std::string text = res.text;

                                   while( !text.empty() && text.back() == '\n' )
                                       text.pop_back();

                                   if( show != "full" )
                                   {
                                       // the first line, and any warning further down (a value that
                                       // is not the part's, a part put down on other nets)
                                       std::string first = text.substr( 0, text.find( '\n' ) );
                                       size_t      at = first.size();

                                       while( at < text.size() )
                                       {
                                           size_t      end = text.find( '\n', at + 1 );
                                           std::string line = text.substr( at + 1, end == std::string::npos ? std::string::npos
                                                                                                           : end - at - 1 );

                                           if( line.find( "warning:" ) != std::string::npos )
                                               first += "\n    " + line.substr( line.find_first_not_of( ' ' ) );

                                           at = end == std::string::npos ? text.size() : end;
                                       }

                                       text = first;
                                   }

                                   body += "[" + tag + "] " + resolved[i]->name + ( res.isError ? " ERROR: " : ": " )
                                           + text + "\n";

                                   if( res.isError && stop )
                                   {
                                       stopped = true;
                                       break;
                                   }
                               }
                           }

                           const double secs = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
                           char head[160];
                           std::snprintf( head, sizeof( head ), "batch: %d of %d call(s) run, %d error(s), %.1f s%s\n",
                                          done, total, failed, secs, stopped ? " (stopped at the error)" : "" );

                           std::string out = head + body;

                           if( show == "last" )
                               out += "--- last step in full:\n" + lastText;

                           return { out, failed > 0 && stop };
                       }, "", std::vector<params::PARAM>{
                             params::Integer( "repeat" ).Range( 1 ),
                             params::Enum( "show", { "line", "last", "full" } ),
                             params::Array( "steps", params::Object( "",
                                     { params::Map( "args", params::Any( "", {} ) ),
                                     params::String( "tool" ).Required() } ) ).Required(),
                             params::Boolean( "stop_on_error" ) }} );

    // tool_help (tool_docs.cpp); then each tool's brief for tools/list, its full text kept in
    // details, and the table batch / new_project / tool_help run from made the same
    tools.push_back( ToolHelpTool( table ) );

    for( TOOL& t : tools )
        ApplyParams( t );

    ApplyBriefs( tools );
    *table = tools;

    return tools;
}

} // namespace kicadd::mcp
