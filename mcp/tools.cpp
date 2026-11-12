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
#include <memory>

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


// Brief output (the default): long lists are cut to their first lines; detail=true lists all.
constexpr int BRIEF_PARTS = 8;    // more moved parts than this: the first BRIEF_SHOWN, then a count
constexpr int BRIEF_SHOWN = 5;
constexpr int BRIEF_DRC = 10;


bool detailOf( const json& aArgs )
{
    return aArgs.is_object() && aArgs.contains( "detail" ) && aArgs["detail"].is_boolean()
           && aArgs["detail"].get<bool>();
}


const json DETAIL_PROP = { { "type", "boolean" },
                           { "description", "List every part / violation (default: long lists cut short)" } };


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


std::string renderPending( const google::protobuf::RepeatedPtrField<std::string>& aPending )
{
    if( aPending.empty() )
        return "";

    std::string s = " (excl. deferred nets:";

    for( const std::string& n : aPending )
        s += " " + n;

    return s + "; call flush_ratsnest for the full count)";
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

    if( !r.missing().empty() )
    {
        s += "not found:";

        for( const std::string& m : r.missing() )
            s += " " + m;

        s += "\n";
    }

    s += "unconnected=" + std::to_string( r.unconnected() ) + renderPending( r.ratsnest_pending() ) + "\n";
    s += renderDrc( r.drc(), aDetail );
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
            s += "; in the way: " + r.blocked_by() + ( r.has_blocked_at() ? " at " + pt( r.blocked_at() ) : "" );

        return s + "\n";
    }

    s += std::string( aDryRun ? "would route" : "routed" ) + " on " + r.layer() + ", " + mm( r.length() ) + " mm ("
         + ratio( r.length(), r.direct() ) + " the straight line), " + std::to_string( r.segments() ) + " segment(s), "
         + std::to_string( r.vias() ) + " via(s), width " + mm( r.width() );

    if( r.min_gap() >= 0 )
        s += ", min gap " + mm( r.min_gap() ) + " to " + r.gap_to();

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
        s += "unconnected=" + std::to_string( r.unconnected() ) + "; undo: \"" + r.history_label()
             + "\"; refill_zones before run_drc\n";
    else if( !r.dry_run() )
        s += "nothing changed; unconnected=" + std::to_string( r.unconnected() ) + "\n";

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
        s += "unconnected=" + std::to_string( r.unconnected() ) + "; undo takes all of it back: \"" + r.history_label()
             + "\"\n";
    else if( !r.dry_run() )
        s += "nothing changed; unconnected=" + std::to_string( r.unconnected() ) + "\n";

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

    kb::RouteResponse r;
    std::string       err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    if( r.results().empty() )
        return { "no unrouted connections match" + std::string( req.max_length() ? " (max_length)" : "" ) + "\n" };

    std::string s = renderRoutes( r, detailOf( aArgs ), "connection(s), shortest first" );

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
        s += f.text.substr( 0, f.text.find( '\n' ) ) + "\n";
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
               + ( st.jlc_class().empty() ? "" : " (" + st.jlc_class() + ")" ) + " -- as of " + st.as_of()
               + ( r.from_cache() ? " (cached; refresh=true for today's)" : "" );
    }

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
            else if( v.is_null() )
                e->add_remove( key );
            else if( v.is_string() )
                ( *e->mutable_fields() )[key] = v.get<std::string>();
            else if( v.is_number() )
                ( *e->mutable_fields() )[key] = v.dump();
            else
                bad = key + ": a field's text is a string (null removes it)";
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
                if( !num( v, key.c_str(), d ) )
                    return error( name + "." + key + ": a length in mm (null: from Default)" );

                if( key == "clearance" )            c->set_clearance( d );
                else if( key == "track_width" )     c->set_track_width( d );
                else if( key == "via_diameter" )    c->set_via_diameter( d );
                else if( key == "via_drill" )       c->set_via_drill( d );
                else if( key == "diff_pair_width" ) c->set_diff_pair_width( d );
                else if( key == "diff_pair_gap" )   c->set_diff_pair_gap( d );
                else
                    return error( "unknown net class value " + key );
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
        t += "preset: " + r.note() + "\n";

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

        if( !aArgs.contains( "refill" ) || !aArgs["refill"].is_boolean() || aArgs["refill"].get<bool>() )
        {
            TOOL_RESULT f = refillZones( aIpc );
            t += f.text.substr( 0, f.text.find( '\n' ) ) + "\n";

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

    t += "undo takes it back: \"" + r.history_label() + "\"; zones create for a pour, refill_zones after moving parts\n";
    return { t };
}


TOOL_RESULT boardSetup( IPC_CLIENT& aIpc, const json& aArgs )
{
    kb::BoardSetup req;
    double         d = 0;

    if( getNumber( aArgs, "copper_layers", d ) )   req.set_copper_layers( (int) d );
    if( getNumber( aArgs, "thickness", d ) )       req.set_thickness( d );
    if( getNumber( aArgs, "outer_copper_oz", d ) ) req.set_outer_copper_oz( d );
    if( getNumber( aArgs, "inner_copper_oz", d ) ) req.set_inner_copper_oz( d );

    kb::BoardSetupResponse r;
    std::string            err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    char head[96];
    std::snprintf( head, sizeof( head ), "%d copper layers, %.3g mm thick", r.copper_layers(), r.thickness() );
    std::string t = ( r.history_label().empty() ? std::string() : r.history_label() + " (undo takes it back)\n" ) + head
                    + "\nstackup (top to bottom):";

    for( const std::string& line : r.stackup() )
        t += "\n  " + line;

    if( r.stackup().empty() )
        t += " none defined (board_setup thickness=1.6 makes one)";

    return { t + "\n" };
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
    return { t + renderMoveResult( shown, aDetail ) + renderHistory( r ) };
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

    if( aArgs.contains( "live" ) && aArgs["live"].is_number() )
        req.set_live( aArgs["live"].get<int>() <= 0 ? -1 : aArgs["live"].get<int>() );

    kb::LcscSearchResponse r;
    std::string            err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string out = std::to_string( r.total() ) + " match(es) in stock at JLCPCB (parts index of " + r.index_date()
                      + ", jlcparts data)";

    if( r.total() > r.hits_size() )
        out += "; the first " + std::to_string( r.hits_size() ) + ": basic, preferred, then by stock";

    if( !r.note().empty() )
        out += "\n  " + r.note();

    for( const kb::LcscHit& h : r.hits() )
    {
        char price[32] = "";

        if( h.price() >= 0 )
            std::snprintf( price, sizeof( price ), ", US$%.4f", h.price() );

        out += "\n" + h.lcsc() + " " + ( h.basic() ? "BASIC" : h.preferred() ? "preferred" : "extended" ) + " "
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
    }

    if( r.hits().empty() )
        out += "\nnothing: loosen a condition (tolerance, voltage, basic_only, package)";

    return { out };
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

    kb::ExportFab req;

    if( aArgs.contains( "dir" ) && aArgs["dir"].is_string() )
        req.set_dir( aArgs["dir"].get<std::string>() );

    req.set_no_zip( !aArgs.value( "zip", true ) );

    kb::ExportFabResponse r;
    std::string           err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s = head;

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
                                                               "the parts as one group around their common centre" } } },
                                 { "detail", DETAIL_PROP } },
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
                                             { "description", "Pad number -> net name, e.g. {\"1\": \"+3V3\", \"2\": \"GND\"}" } } },
                                 { "detail", DETAIL_PROP } },
                               { "x", "y" } ),
                       [&aIpc]( const json& a ) { return addFootprint( aIpc, a ); } } );

    tools.push_back( { "delete_footprints",
                       "Delete footprints as one undoable edit (undo restores them with their nets). "
                       "Tracks and vias stay. Returns the new unrouted count.",
                       schema( { { "refs", refs }, { "detail", DETAIL_PROP } }, { "refs" } ),
                       [&aIpc]( const json& a ) { return deleteFootprints( aIpc, a ); } } );

    tools.push_back( { "delete_tracks",
                       "Delete routing (tracks, arcs, vias) as one undoable edit, e.g. before auto_place "
                       "on a routed board. all: every track; nets: by net name; refs: the copper running "
                       "from these parts' pads up to the next pad (rip up their connections); region: "
                       "[x0, y0, x1, y1] mm, anything crossing it. Locked tracks stay unless include_locked. "
                       "Zones are not touched (refill_zones afterwards). Returns the new unrouted count.",
                       schema( { { "all", { { "type", "boolean" } } },
                                 { "nets", refs },
                                 { "refs", refs },
                                 { "region", { { "type", "array" }, { "items", { { "type", "number" } } } } },
                                 { "include_locked", { { "type", "boolean" } } },
                                 { "keep_vias", { { "type", "boolean" } } },
                                 { "detail", DETAIL_PROP } } ),
                       [&aIpc]( const json& a ) { return deleteTracks( aIpc, a ); } } );

    tools.push_back( { "route",
                       "Draw one connection the way the interactive router does (45-degree tracks around "
                       "obstacles; existing copper is never moved; the net class's width and via, the "
                       "clearance, keepout and edge rules). Not an autorouter: say what to connect. from: a "
                       "pad \"U1.5\" or a point [x, y] on the net's pad / track / via (inside a pad: the track leaves the "
                       "pad there, for a way out between close pads); to: likewise, or any point "
                       "(a free end); without to: the nearest unrouted connection of from. waypoints: "
                       "[[x, y], ...] to pass through, [x, y, \"B.Cu\"] puts a via there and goes on on that "
                       "layer; via: [x, y] is short for one via to the other side (the end's layer). layer: "
                       "where to start (default: the start pad's / the end's / F.Cu); width in mm. Reports "
                       "length vs the straight line, segments, vias and the narrowest gap; when blocked, how "
                       "far it got and what is in the way. One undoable edit; dry_run reports without "
                       "drawing. Mm, Y down.",
                       schema( { { "from", { { "description", "\"U1.5\" or [x, y] (a point inside a pad: the track leaves the pad there, not at its centre)" } } },
                                 { "to", { { "description", "\"C3.1\" or [x, y]; omit for the ratsnest" } } },
                                 { "waypoints", { { "type", "array" } } },
                                 { "via", { { "type", "array" }, { "items", { { "type", "number" } } } } },
                                 { "layer", { { "type", "string" } } },
                                 { "width", { { "type", "number" } } },
                                 { "dry_run", { { "type", "boolean" } } } },
                               { "from" } ),
                       [&aIpc]( const json& a ) { return routeTrack( aIpc, a ); } } );

    tools.push_back( { "route_short",
                       "Route the unrouted connections (ratsnest lines) of refs and / or nets, shortest "
                       "first (the short ones have the fewest ways round, so they go first and block the "
                       "least), each as route does; max_length (mm) leaves the long ones to the user. "
                       "Through-hole to through-hole: F.Cu, then B.Cu if blocked, unless layer is given; "
                       "when the nearest copper of the other end can't be reached (a track on the far "
                       "layer), the pads and vias joined to it, nearest first. "
                       "No vias are added. The failures say how far they got and what is in the way "
                       "(delete_tracks or move the part). All of it is one undoable edit; dry_run reports "
                       "what would and would not route without drawing.",
                       schema( { { "refs", refs },
                                 { "nets", refs },
                                 { "max_length", { { "type", "number" } } },
                                 { "layer", { { "type", "string" } } },
                                 { "dry_run", { { "type", "boolean" } } },
                                 { "detail", DETAIL_PROP } } ),
                       [&aIpc]( const json& a ) { return routeShort( aIpc, a ); } } );

    tools.push_back( { "fanout",
                       "Fan out SMD pads of nets (e.g. GND, +3V3) to a copper zone of the same net on "
                       "another layer (an inner plane, or the other side of a 2-layer board): a short track "
                       "and a via next to each pad, inside that zone, as route would draw them. "
                       "Through-hole pads are skipped (they reach every layer), and so are pads already "
                       "joined to the zone; a net without such a zone gets a warning, not dangling vias. "
                       "refs: only these parts; layer: the zone's layer. One undoable edit; the zones are "
                       "refilled afterwards (refill=false to skip); dry_run reports without drawing.",
                       schema( { { "nets", refs },
                                 { "refs", refs },
                                 { "layer", { { "type", "string" } } },
                                 { "dry_run", { { "type", "boolean" } } },
                                 { "refill", { { "type", "boolean" } } },
                                 { "detail", DETAIL_PROP } },
                               { "nets" } ),
                       [&aIpc]( const json& a ) { return fanout( aIpc, a ); } } );

    tools.push_back( { "get_rules",
                       "The design rules: net classes (clearance, track width, via, diff pair; unset values "
                       "come from Default) with how many nets each has, the net class assignments by net name "
                       "pattern, and the board's minimum constraints. Lengths in mm.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return getRules( aIpc ); } } );

    tools.push_back( { "set_rules",
                       "Change the design rules as one undoable edit (undo / redo; save_board writes them "
                       "to the project file). classes: {\"Power\": {\"track_width\": 0.5, \"clearance\": 0.25}} "
                       "adds or changes a class (only the values given; null: take it from Default); "
                       "remove_classes: [...]; assign: {\"+5V\": \"Power\", \"/USB_D*\": \"USB\", \"GND\": null} "
                       "sets or drops net name patterns; constraints: {\"min_clearance\", \"min_track_width\", "
                       "\"min_via_diameter\", \"min_through_drill\", \"min_annular_width\", \"hole_clearance\", "
                       "\"hole_to_hole\", \"copper_edge_clearance\", \"min_silk_text_height\", "
                       "\"min_silk_text_thickness\"} (mm). preset: a fab's standard process as the board minimums "
                       "(and the Default class raised to them): jlcpcb_2layer, jlcpcb_4layer, jlcpcb_6layer; the "
                       "rest of the call goes on top. Existing tracks keep their width. "
                       "drc (default true) runs DRC before and after and reports the change in errors.",
                       schema( { { "preset", { { "type", "string" }, { "enum", { "jlcpcb_2layer", "jlcpcb_4layer", "jlcpcb_6layer" } } } },
                                 { "classes", { { "type", "object" } } },
                                 { "remove_classes", refs },
                                 { "assign", { { "type", "object" } } },
                                 { "constraints", { { "type", "object" } } },
                                 { "drc", { { "type", "boolean" } } } } ),
                       [&aIpc]( const json& a ) { return setRules( aIpc, a ); } } );

    tools.push_back( { "zones",
                       "Copper zones: list them (id, net, layers, priority, clearance, minimum width, pad "
                       "connection), or change their settings as one undoable edit. Choose with nets, layers, "
                       "ids (from the list) or all=true; change clearance, min_width (mm), priority, "
                       "connection (solid / thermal / none / thermal_tht), thermal_gap, thermal_spoke (mm). "
                       "Refilled afterwards unless refill=false.",
                       schema( { { "nets", refs },
                                 { "layers", refs },
                                 { "ids", refs },
                                 { "all", { { "type", "boolean" } } },
                                 { "clearance", { { "type", "number" } } },
                                 { "min_width", { { "type", "number" } } },
                                 { "priority", { { "type", "integer" } } },
                                 { "connection", { { "type", "string" }, { "enum", { "solid", "thermal", "none", "thermal_tht" } } } },
                                 { "thermal_gap", { { "type", "number" } } },
                                 { "thermal_spoke", { { "type", "number" } } },
                                 { "create", { { "type", "object" },
                                               { "description", "a new zone: {net, layers (default [\"F.Cu\", \"B.Cu\"]; "
                                                                "\"all\"), outline [[x, y], ...] (default the board "
                                                                "outline), priority, clearance, min_width, connection, name}" } } },
                                 { "refill", { { "type", "boolean" } } } } ),
                       [&aIpc]( const json& a ) { return zonesTool( aIpc, a ); } } );

    tools.push_back( { "board_outline",
                       "Draw the board outline on Edge.Cuts (the old board-level Edge.Cuts shapes are "
                       "replaced unless keep_old): rect [x0, y0, x1, y1] with corner_radius, or circle [cx, cy, "
                       "diameter] (mm, Y down). cutouts: [{rect, corner_radius} | {circle}] inside it. Mounting "
                       "holes: hole_inset (mm from the corners, 4 holes of hole_diameter, default 3.2 = M3) and / or "
                       "holes [[x, y, d], {x, y, diameter, plated, net}]: NPTH, or plated with a ring (e.g. to GND); "
                       "they become footprints H1, H2 ... out of the BOM and the CPL. Reports the size, area, and "
                       "the parts now outside. One undoable edit. Then zones create for a pour.",
                       schema( { { "rect", { { "type", "array" }, { "items", { { "type", "number" } } } } },
                                 { "circle", { { "type", "array" }, { "items", { { "type", "number" } } } } },
                                 { "corner_radius", { { "type", "number" } } },
                                 { "cutouts", { { "type", "array" } } },
                                 { "holes", { { "type", "array" } } },
                                 { "hole_inset", { { "type", "number" } } },
                                 { "hole_diameter", { { "type", "number" } } },
                                 { "keep_old", { { "type", "boolean" } } } } ),
                       [&aIpc]( const json& a ) { return boardOutline( aIpc, a ); } } );

    tools.push_back( { "board_setup",
                       "The board's layers and stackup. No arguments: what they are (copper layers, thickness, "
                       "each layer of the stackup). copper_layers (2, 4, 6, ...), thickness (mm, e.g. 1.6), "
                       "outer_copper_oz / inner_copper_oz (e.g. 1 and 0.5): the stackup is made anew like Board "
                       "Setup's default. Fewer layers only when the ones that go are empty. One undoable edit. "
                       "Pair with set_rules preset (e.g. jlcpcb_4layer) and zones create on the inner layers.",
                       schema( { { "copper_layers", { { "type", "integer" } } },
                                 { "thickness", { { "type", "number" } } },
                                 { "outer_copper_oz", { { "type", "number" } } },
                                 { "inner_copper_oz", { { "type", "number" } } } } ),
                       [&aIpc]( const json& a ) { return boardSetup( aIpc, a ); } } );

    tools.push_back( { "pcb_set_fields",
                       "Change footprints' fields and attributes on the board as one undoable edit. parts: "
                       "{\"R1\": {\"Value\": \"10k\", \"LCSC\": \"C25804\", \"dnp\": true}}, or refs [...] with "
                       "set {...} for many. Value is the value; other names are user fields (added hidden when "
                       "missing; null removes one). Attributes: dnp, exclude_from_bom, exclude_from_pos. The "
                       "reference and footprint are not changed here. A board made from a schematic takes its "
                       "fields from it: change them there (sch_set_fields), then sync_pcb. Refs that are not "
                       "found are listed; the other parts are changed anyway.",
                       schema( { { "parts", { { "type", "object" }, { "additionalProperties", { { "type", "object" } } } } },
                                 { "refs", refs },
                                 { "set", { { "type", "object" } } } } ),
                       [&aIpc]( const json& a ) { return pcbSetFields( aIpc, a ); } } );

    tools.push_back( { "undo",
                       "Undo the last KiCadD edit (move/rotate/flip/add/delete/auto_place/delete_tracks), restoring the exact previous "
                       "state. Parts the user changed since are left alone; an edit the user already undid "
                       "in the editor (Ctrl+Z) is reported as such. Returns what was undone, new positions and local DRC.",
                       schema( { { "detail", DETAIL_PROP } } ),
                       [&aIpc]( const json& a ) { return history( aIpc, true, detailOf( a ) ); } } );

    tools.push_back( { "redo",
                       "Redo the last undone edit.",
                       schema( { { "detail", DETAIL_PROP } } ),
                       [&aIpc]( const json& a ) { return history( aIpc, false, detailOf( a ) ); } } );

    tools.push_back( { "check_drc",
                       "Local DRC for the given footprints against their surroundings, without moving them.",
                       schema( { { "refs", refs }, { "detail", DETAIL_PROP } }, { "refs" } ),
                       [&aIpc]( const json& a ) { return checkDrc( aIpc, a ); } } );

    tools.push_back( { "flush_ratsnest",
                       "Rebuild the ratsnest of large nets deferred during moves; returns the complete "
                       "unrouted-connection count.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return flushRatsnest( aIpc ); } } );

    tools.push_back( { "run_drc",
                       "Full-board DRC (all KiCad checks: clearance, shorts, unconnected, courtyards, "
                       "silk, edge, holes, ...). Takes seconds on large boards. Output: whole-board counts by type, "
                       "then (with types/refs/include_warnings=false) the counts matching the filter, "
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

    tools.push_back( { "lcsc_search",
                       "Find JLCPCB / LCSC parts by specification, e.g. value \"100nF\", package \"0402\", "
                       "dielectric \"X5R\", voltage_min 16, basic_only; or keyword \"AMS1117 3.3\". Searches "
                       "KiCadD's parts index (JLCPCB parts in stock, daily data from jlcparts; about 45 MB, "
                       "downloaded on first use, refreshed weekly), basic parts first, then preferred "
                       "(no extended fee), then by stock; the first few (live, default 3) are checked "
                       "now against LCSC. Then sch_add_part lcsc=... places one.",
                       schema( { { "keyword", { { "type", "string" } } },
                                 { "category", { { "type", "string" },
                                                 { "description", "resistor, capacitor, inductor, or a JLCPCB category word (\"LDO\", \"MOSFET\")" } } },
                                 { "value", { { "type", "string" }, { "description", "100nF, 10k, 4k7, 2.2uH" } } },
                                 { "package", { { "type", "string" } } },
                                 { "tolerance", { { "type", "number" }, { "description", "%, at most" } } },
                                 { "voltage_min", { { "type", "number" }, { "description", "V rating, at least" } } },
                                 { "dielectric", { { "type", "string" } } },
                                 { "basic_only", { { "type", "boolean" } } },
                                 { "min_stock", { { "type", "integer" } } },
                                 { "limit", { { "type", "integer" } } },
                                 { "live", { { "type", "integer" } } },
                                 { "update", { { "type", "boolean" }, { "description", "fetch the index again now" } } } } ),
                       [&aIpc]( const json& a ) { return lcscSearch( aIpc, a ); } } );

    tools.push_back( { "export_fab",
                       "Write the fabrication files for JLCPCB from the board as it is now (unsaved edits "
                       "included): Gerbers (Protel extensions, X2, job file) and Excellon drill (mm) zipped "
                       "for the upload, BOM (Comment, Designator, Footprint, LCSC Part #) and CPL "
                       "(Designator, Mid X, Mid Y, Layer, Rotation) as CSV. Refills the zones first "
                       "(refill=false skips it). dir defaults to <project>/fab. Run run_drc before.",
                       schema( { { "dir", { { "type", "string" } } },
                                 { "zip", { { "type", "boolean" }, { "description", "default true" } } },
                                 { "refill", { { "type", "boolean" }, { "description", "default true" } } } } ),
                       [&aIpc]( const json& a ) { return exportFab( aIpc, a ); } } );

    tools.push_back( { "open_board",
                       "Open a .kicad_pcb (or .kicad_pro) on the server (kicad-cli api-server), replacing the current board. "
                       "Not available with the desktop editor: there the user opens the file in pcbnew.",
                       schema( { { "path", { { "type", "string" }, { "description", "Absolute path on the server" } } } },
                               { "path" } ),
                       [&aIpc]( const json& a ) { return openBoard( aIpc, a ); } } );

    tools.push_back( { "save_board",
                       "Write the in-memory board back to its .kicad_pcb file. It is written in this build's "
                       "file format (KiCad nightly, 10.99): a file from KiCad 9 / 10 is upgraded and those "
                       "versions may not open it any more (as when KiCad itself saves it).",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return saveBoard( aIpc ); } } );

    AddSchematicTools( tools, aIpc );     // M5a, sch_tools.cpp

    // batch: a list of the tools above in one call (a copy of the table: the list is handed
    // over to the server afterwards)
    auto table = std::make_shared<std::vector<TOOL>>( tools );

    tools.push_back( { "batch",
                       "Run several tools in one call, in order: steps [{\"tool\": \"move_footprints\", "
                       "\"args\": {...}}, ...], repeated repeat times (default 1). Each step answers with its "
                       "first line (show=\"full\": everything; \"last\": everything of the last step only). "
                       "Stops at the first error unless stop_on_error=false. At most 500 calls. For repeated "
                       "or routine work (the same edit on many parts, edit-check loops); a step's answer "
                       "cannot feed the next one's arguments.",
                       schema( { { "steps", { { "type", "array" },
                                              { "items", { { "type", "object" },
                                                           { "properties", { { "tool", { { "type", "string" } } },
                                                                             { "args", { { "type", "object" } } } } },
                                                           { "required", { "tool" } } } } } },
                                 { "repeat", { { "type", "integer" } } },
                                 { "stop_on_error", { { "type", "boolean" } } },
                                 { "show", { { "type", "string" }, { "enum", { "line", "last", "full" } } } } },
                               { "steps" } ),
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
                                       text = text.substr( 0, text.find( '\n' ) );

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
                       } } );

    return tools;
}

} // namespace kicadd::mcp
