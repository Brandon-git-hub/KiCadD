/*
 * KiCadD MCP - tool definitions and compact text rendering.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "ipc_client.h"

#include <cmath>
#include <cstdio>
#include <map>

#include <kicadd/board.pb.h>
#include <api/common/commands/base_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
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

    if( dx == 0 && dy == 0 && rot == 0 )
        return error( "nothing to do: give dx, dy (mm) and/or rotate (deg)" );

    kb::MoveDelta req;

    for( const std::string& r : refs )
        req.add_refs( r );

    req.set_dx( toNm( dx ) );
    req.set_dy( toNm( dy ) );
    req.set_drot_decideg( static_cast<int>( std::lround( rot * 10 ) ) );

    kb::MoveResponse r;
    std::string      err;

    if( !aIpc.Call( req, r, err ) )
        return error( err );

    std::string s;

    for( const kb::MoveResult& m : r.moved() )
        s += m.ref() + " @" + pt( m.pos() ) + ( m.rot_decideg() ? " r" + deg( m.rot_decideg() ) : "" ) + "\n";

    if( !r.missing().empty() )
    {
        s += "not found:";

        for( const std::string& m : r.missing() )
            s += " " + m;

        s += "\n";
    }

    s += "unconnected=" + std::to_string( r.unconnected() ) + renderPending( r.ratsnest_pending() ) + "\n";
    s += renderDrc( r.drc() );
    return { s, r.moved().empty() };
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
        return error( err );

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
                       "Move/rotate footprints by a relative delta as one undoable edit. Returns new "
                       "positions, the unrouted-connection count and a local DRC of the moved parts "
                       "(clearance, shorts, hole clearance, courtyard overlap). Large nets (GND, power) "
                       "skip ratsnest updates while you iterate; the count says which.",
                       schema( { { "refs", refs },
                                 { "dx", { { "type", "number" }, { "description", "Delta X, mm" } } },
                                 { "dy", { { "type", "number" }, { "description", "Delta Y, mm (down is +)" } } },
                                 { "rotate", { { "type", "number" }, { "description", "Rotation delta, degrees CCW" } } } },
                               { "refs" } ),
                       [&aIpc]( const json& a ) { return moveFootprints( aIpc, a ); } } );

    tools.push_back( { "check_drc",
                       "Local DRC for the given footprints against their surroundings, without moving them.",
                       schema( { { "refs", refs } }, { "refs" } ),
                       [&aIpc]( const json& a ) { return checkDrc( aIpc, a ); } } );

    tools.push_back( { "flush_ratsnest",
                       "Rebuild the ratsnest of large nets deferred during moves; returns the complete "
                       "unrouted-connection count.",
                       schema( json::object() ),
                       [&aIpc]( const json& ) { return flushRatsnest( aIpc ); } } );

    tools.push_back( { "open_board",
                       "Open a .kicad_pcb (or .kicad_pro) on the server, replacing the current board.",
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
