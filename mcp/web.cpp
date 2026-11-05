/*
 * KiCadD web view hub.
 *
 * Wire format (JSON over WebSocket), coordinates in integer µm, angles in degrees:
 *
 *   server -> client
 *     {"t":"snapshot","rev":R,"file":..,"outline":[[x,y,..],..], ITEMS}
 *     {"t":"delta","since":S,"rev":R, ITEMS, "rm":[id,..]}
 *     {"t":"moved","op":"move|undo|redo","ok":true,"ms":..,"unconnected":N,"pending":[..],"drc":[..],
 *      "label":".."}
 *     {"t":"history","undo":N,"redo":N,"next_undo":"..","next_redo":"..","modified":bool}
 *     {"t":"saved","ok":bool,"file":"..","error":".."}
 *     {"t":"fills","fills":[{"id","net","ly","polys":[[x0,y0,dx,dy,dx,dy,..],..]},..]}
 *          (polygon points delta-encoded: first point absolute, then differences, all µm)
 *     {"t":"refilled","ok":bool,"ms":..,"error":".."}
 *     {"t":"ratsnest","xy":[x1,y1,x2,y2,..],"net":[i,..],"nets":[..],"pending":[..],"unconnected":N,
 *      "final":bool}   (final=false: deferred large nets still missing, a final one follows)
 *     {"t":"error","error":".."}
 *   ITEMS = "nets":[..],"lys":[..]  (string tables; items reference them by index)
 *           "fp":[{id,ref,val,back,x,y,r,lk,bb:[x0,y0,x1,y1],cy:[..],
 *                  pads:[[num,net,shape,side,x,y,w,h,rot,rr,drill,poly|0],..]}]
 *           "trk":[[id,ly,net,ax,ay,bx,by,w(,mx,my)],..]   (10 entries = arc)
 *           "via":[[id,net,x,y,d,drill],..]
 *           "zone":[{id,net,ly:[..],ko,ol:[[..],..]}]
 *   client -> server
 *     {"t":"move","refs":[..],"dx":mm,"dy":mm,"rot":deg}
 *     {"t":"undo"} / {"t":"redo"} / {"t":"save"}
 *     {"t":"fills","ids":[..]}   (ids empty = all)   /   {"t":"refill"}
 *     {"t":"resync"}
 *
 * A client applies a delta when since <= its revision < rev, ignores it when rev <= its
 * revision, and asks for "resync" when since > its revision (a gap).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "web.h"
#include "ipc_client.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <kicadd/board.pb.h>
#include <kicadd/render.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/board/board_commands.pb.h>
#include <api/common/types/base_types.pb.h>
#include <google/protobuf/empty.pb.h>

using json = nlohmann::json;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace kr = kicadd::render;
namespace kb = kicadd::board;

namespace kicadd::mcp
{

// ---------------------------------------------------------------------------------------------
// Session

class WS_SESSION : public std::enable_shared_from_this<WS_SESSION>
{
public:
    WS_SESSION( tcp::socket aSocket, WEB_HUB& aHub ) :
            m_ws( std::move( aSocket ) ),
            m_hub( aHub )
    {}

    void Run( beast::http::request<beast::http::string_body> aRequest )
    {
        m_ws.set_option( websocket::stream_base::timeout::suggested( beast::role_type::server ) );

        // Snapshots and copper fills are MBs of JSON digits; browsers negotiate deflate.
        websocket::permessage_deflate pmd;
        pmd.server_enable = true;
        pmd.compLevel = 3;
        m_ws.set_option( pmd );
        m_ws.set_option( websocket::stream_base::decorator(
                []( websocket::response_type& aRes ) { aRes.set( beast::http::field::server, "kicadd" ); } ) );

        auto self = shared_from_this();
        auto req = std::make_shared<beast::http::request<beast::http::string_body>>( std::move( aRequest ) );

        m_ws.async_accept( *req,
                           [self, req]( beast::error_code ec )
                           {
                               if( ec )
                                   return;

                               self->m_hub.Joined( self );
                               self->doRead();
                           } );
    }

    /// Thread-safe: queue a text frame.
    void Send( std::shared_ptr<const std::string> aText )
    {
        auto self = shared_from_this();

        asio::post( m_ws.get_executor(),
                    [self, aText]()
                    {
                        if( self->m_closed )
                            return;

                        self->m_queue.push_back( aText );

                        if( self->m_queue.size() == 1 )
                            self->doWrite();
                    } );
    }

private:
    void doRead()
    {
        auto self = shared_from_this();

        m_ws.async_read( m_buffer,
                         [self]( beast::error_code ec, std::size_t )
                         {
                             if( ec )
                             {
                                 self->close();
                                 return;
                             }

                             std::string text = beast::buffers_to_string( self->m_buffer.data() );
                             self->m_buffer.consume( self->m_buffer.size() );
                             self->m_hub.Received( self, text );
                             self->doRead();
                         } );
    }

    void doWrite()
    {
        auto self = shared_from_this();
        m_ws.text( true );

        m_ws.async_write( asio::buffer( *m_queue.front() ),
                          [self]( beast::error_code ec, std::size_t )
                          {
                              if( ec )
                              {
                                  self->close();
                                  return;
                              }

                              self->m_queue.pop_front();

                              if( !self->m_queue.empty() )
                                  self->doWrite();
                          } );
    }

    void close()
    {
        if( m_closed )
            return;

        m_closed = true;
        m_queue.clear();
        m_hub.Left( shared_from_this() );
    }

    websocket::stream<tcp::socket>                 m_ws;
    WEB_HUB&                                       m_hub;
    beast::flat_buffer                             m_buffer;
    std::deque<std::shared_ptr<const std::string>> m_queue;
    bool                                           m_closed = false;
};


// ---------------------------------------------------------------------------------------------
// JSON conversion

namespace
{

int64_t um( int64_t aNm ) { return static_cast<int64_t>( std::llround( aNm / 1000.0 ) ); }


class TABLE
{
public:
    int Index( const std::string& aValue )
    {
        auto [it, inserted] = m_index.try_emplace( aValue, static_cast<int>( m_values.size() ) );

        if( inserted )
            m_values.push_back( aValue );

        return it->second;
    }

    const std::vector<std::string>& Values() const { return m_values; }

private:
    std::unordered_map<std::string, int> m_index;
    std::vector<std::string>             m_values;
};


/// First point absolute, then per-point differences (µm): fill polygons have ~500k points
/// on a large board and neighbouring points are close, so this roughly halves the JSON.
json deltaEncode( const kr::Polyline& aPoly )
{
    json    out = json::array();
    int64_t px = 0, py = 0;

    for( int i = 0; i + 1 < aPoly.xy_size(); i += 2 )
    {
        int64_t x = um( aPoly.xy( i ) ), y = um( aPoly.xy( i + 1 ) );
        out.push_back( x - px );
        out.push_back( y - py );
        px = x;
        py = y;
    }

    return out;
}


json polyline( const kr::Polyline& aPoly )
{
    json out = json::array();

    for( int64_t v : aPoly.xy() )
        out.push_back( um( v ) );

    return out;
}


void addItems( const kr::RenderItems& aItems, json& aOut )
{
    TABLE nets, layers;
    nets.Index( "" );   // index 0 = no net

    json fps = json::array();

    for( const kr::Footprint& f : aItems.footprints() )
    {
        json pads = json::array();

        for( const kr::Pad& p : f.pads() )
        {
            pads.push_back( { p.number(), nets.Index( p.net() ), static_cast<int>( p.shape() ),
                              static_cast<int>( p.side() ), um( p.pos().x() ), um( p.pos().y() ),
                              um( p.size().x() ), um( p.size().y() ), p.rot_decideg() / 10.0,
                              std::round( p.rr_ratio() * 1000 ) / 1000, um( p.drill() ),
                              p.has_poly() ? polyline( p.poly() ) : json( 0 ) } );
        }

        fps.push_back( { { "id", f.id() },
                         { "ref", f.ref() },
                         { "val", f.value() },
                         { "back", f.back() ? 1 : 0 },
                         { "x", um( f.pos().x() ) },
                         { "y", um( f.pos().y() ) },
                         { "r", f.rot_decideg() / 10.0 },
                         { "lk", f.locked() ? 1 : 0 },
                         { "bb", { um( f.bbox_min().x() ), um( f.bbox_min().y() ),
                                   um( f.bbox_max().x() ), um( f.bbox_max().y() ) } },
                         { "cy", polyline( f.courtyard() ) },
                         { "pads", std::move( pads ) } } );
    }

    json trks = json::array();

    for( const kr::Track& t : aItems.tracks() )
    {
        json row = { t.id(), layers.Index( t.layer() ), nets.Index( t.net() ), um( t.a().x() ),
                     um( t.a().y() ), um( t.b().x() ), um( t.b().y() ), um( t.width() ) };

        if( t.arc() )
        {
            row.push_back( um( t.mid().x() ) );
            row.push_back( um( t.mid().y() ) );
        }

        trks.push_back( std::move( row ) );
    }

    json vias = json::array();

    for( const kr::Via& v : aItems.vias() )
    {
        vias.push_back( { v.id(), nets.Index( v.net() ), um( v.pos().x() ), um( v.pos().y() ),
                          um( v.diameter() ), um( v.drill() ) } );
    }

    json zones = json::array();

    for( const kr::Zone& z : aItems.zones() )
    {
        json ly = json::array(), ol = json::array();

        for( const std::string& l : z.layers() )
            ly.push_back( layers.Index( l ) );

        for( const kr::Polyline& p : z.outlines() )
            ol.push_back( polyline( p ) );

        zones.push_back( { { "id", z.id() },
                           { "net", nets.Index( z.net() ) },
                           { "ly", std::move( ly ) },
                           { "ko", z.keepout() ? 1 : 0 },
                           { "ol", std::move( ol ) } } );
    }

    aOut["nets"] = nets.Values();
    aOut["lys"] = layers.Values();
    aOut["fp"] = std::move( fps );
    aOut["trk"] = std::move( trks );
    aOut["via"] = std::move( vias );
    aOut["zone"] = std::move( zones );
}


json snapshotJson( const kr::RenderSnapshot& aSnap )
{
    json out = { { "t", "snapshot" }, { "rev", aSnap.revision() }, { "file", aSnap.file() } };

    json copper = json::array();

    for( const kr::LayerInfo& l : aSnap.layers() )
        copper.push_back( l.name() );

    out["copper"] = std::move( copper );

    json outline = json::array();

    for( const kr::Polyline& p : aSnap.outline() )
        outline.push_back( polyline( p ) );

    out["outline"] = std::move( outline );
    addItems( aSnap.items(), out );
    return out;
}


const char* drcName( kb::DrcType aType )
{
    switch( aType )
    {
    case kb::DT_CLEARANCE:         return "clearance";
    case kb::DT_SHORT:             return "short";
    case kb::DT_HOLE_CLEARANCE:    return "hole";
    case kb::DT_COURTYARD_OVERLAP: return "courtyard";
    default:                       return "drc";
    }
}


std::string errorJson( const std::string& aMsg )
{
    return json{ { "t", "error" }, { "error", aMsg } }.dump();
}

} // namespace


// ---------------------------------------------------------------------------------------------
// Hub

WEB_HUB::WEB_HUB( IPC_CLIENT& aIpc, asio::io_context& aIoc ) :
        m_ipc( aIpc ),
        m_ioc( aIoc )
{
    m_ioThread = std::thread(
            [this]()
            {
                auto guard = asio::make_work_guard( m_ioc );
                m_ioc.run();
            } );

    m_worker = std::thread( [this]() { workerLoop(); } );
}


WEB_HUB::~WEB_HUB()
{
    m_running = false;
    m_jobsCv.notify_all();
    m_ioc.stop();

    if( m_worker.joinable() )
        m_worker.join();

    if( m_ioThread.joinable() )
        m_ioThread.join();
}


void WEB_HUB::Accept( tcp::socket aSocket, beast::http::request<beast::http::string_body> aRequest )
{
    std::make_shared<WS_SESSION>( std::move( aSocket ), *this )->Run( std::move( aRequest ) );
}


void WEB_HUB::Joined( const std::shared_ptr<WS_SESSION>& aSession )
{
    {
        std::lock_guard<std::mutex> lock( m_sessionsMutex );
        m_sessions.insert( aSession );
    }

    std::lock_guard<std::mutex> lock( m_jobsMutex );
    m_jobs.push_back( { aSession, json{ { "t", "snapshot" } } } );
    m_jobsCv.notify_one();
}


void WEB_HUB::Left( const std::shared_ptr<WS_SESSION>& aSession )
{
    std::lock_guard<std::mutex> lock( m_sessionsMutex );
    m_sessions.erase( aSession );
}


void WEB_HUB::Received( const std::shared_ptr<WS_SESSION>& aSession, const std::string& aText )
{
    json msg;

    try
    {
        msg = json::parse( aText );
    }
    catch( const json::parse_error& )
    {
        aSession->Send( std::make_shared<std::string>( errorJson( "malformed JSON" ) ) );
        return;
    }

    std::string t = msg.is_object() ? msg.value( "t", "" ) : "";

    if( t == "resync" )
        msg = json{ { "t", "snapshot" } };
    else if( t != "move" && t != "undo" && t != "redo" && t != "save" && t != "fills" && t != "refill" )
    {
        aSession->Send( std::make_shared<std::string>( errorJson( "unknown message type '" + t + "'" ) ) );
        return;
    }

    std::lock_guard<std::mutex> lock( m_jobsMutex );
    m_jobs.push_back( { aSession, std::move( msg ) } );
    m_jobsCv.notify_one();
}


void WEB_HUB::Kick()
{
    std::lock_guard<std::mutex> lock( m_jobsMutex );
    m_kick = true;
    m_jobsCv.notify_one();
}


std::set<std::shared_ptr<WS_SESSION>> WEB_HUB::sessions()
{
    std::lock_guard<std::mutex> lock( m_sessionsMutex );
    return m_sessions;
}


void WEB_HUB::broadcast( const std::string& aText )
{
    auto text = std::make_shared<const std::string>( aText );

    for( const auto& s : sessions() )
        s->Send( text );
}


void WEB_HUB::workerLoop()
{
    while( m_running )
    {
        std::deque<JOB> jobs;

        {
            std::unique_lock<std::mutex> lock( m_jobsMutex );
            m_jobsCv.wait_for( lock, std::chrono::milliseconds( POLL_MS ),
                               [this]() { return !m_running || !m_jobs.empty() || m_kick; } );
            jobs.swap( m_jobs );
            m_kick = false;
        }

        if( !m_running )
            break;

        for( const JOB& job : jobs )
            runJob( job );

        if( !sessions().empty() )
            poll();

        // Edits have gone quiet: rebuild the deferred large nets and send the full ratsnest.
        if( m_flushDue != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= m_flushDue )
        {
            m_flushDue = {};

            if( !sessions().empty() )
                broadcast( ratsnestJson( true ) );
        }
    }
}


void WEB_HUB::runJob( const JOB& aJob )
{
    std::shared_ptr<WS_SESSION> session = aJob.session.lock();

    if( !session )
        return;

    const std::string t = aJob.message.value( "t", "" );
    std::string       err;

    if( t == "snapshot" )
    {
        kr::RenderSnapshot snap;

        if( !m_ipc.Call( kr::GetRenderSnapshot(), snap, err ) )
        {
            session->Send( std::make_shared<std::string>( errorJson( err ) ) );
            return;
        }

        // Deltas are broadcast from m_revision; the snapshot is at least that recent.
        if( m_revision == 0 )
            m_revision = snap.revision();

        session->Send( std::make_shared<std::string>( snapshotJson( snap ).dump() ) );
        session->Send( std::make_shared<std::string>( historyJson() ) );

        bool pending = false;
        session->Send( std::make_shared<std::string>( ratsnestJson( false, &pending ) ) );
        scheduleRatsnestFlush( pending );
        return;
    }

    if( t == "undo" || t == "redo" )
    {
        auto                t0 = std::chrono::steady_clock::now();
        kb::HistoryResponse r;
        bool ok = t == "undo" ? m_ipc.Call( kb::Undo(), r, err ) : m_ipc.Call( kb::Redo(), r, err );
        double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();

        if( !ok )
            sendMoveResult( session, t.c_str(), false, err, nullptr, ms, "" );
        else if( !r.ok() )
            sendMoveResult( session, t.c_str(), false, "nothing to " + t, nullptr, ms, "" );
        else
            sendMoveResult( session, t.c_str(), true, "", &r.result(), ms, r.label() );

        poll();   // also broadcasts the new history state
        return;
    }

    if( t == "fills" )
    {
        kr::GetZoneFills req;

        if( aJob.message.contains( "ids" ) && aJob.message["ids"].is_array() )
            for( const json& id : aJob.message["ids"] )
                if( id.is_string() )
                    req.add_ids( id.get<std::string>() );

        kr::ZoneFills r;

        if( !m_ipc.Call( req, r, err ) )
        {
            session->Send( std::make_shared<std::string>( errorJson( err ) ) );
            return;
        }

        json fills = json::array();

        for( const kr::ZoneFill& zf : r.fills() )
        {
            json polys = json::array();

            for( const kr::FillPoly& fp : zf.polys() )
            {
                // Fills come fractured (holes bridged into the outline), but keep holes too.
                for( const kr::Polyline* pl : { &fp.outline() } )
                    polys.push_back( deltaEncode( *pl ) );

                for( const kr::Polyline& h : fp.holes() )
                    polys.push_back( deltaEncode( h ) );
            }

            fills.push_back( { { "id", zf.id() }, { "net", zf.net() }, { "ly", zf.layer() },
                               { "polys", std::move( polys ) } } );
        }

        session->Send( std::make_shared<std::string>(
                json{ { "t", "fills" }, { "full", req.ids().empty() }, { "fills", std::move( fills ) } }.dump() ) );
        return;
    }

    if( t == "refill" )
    {
        kb::BoardSummaryResponse summary;
        json                     reply = { { "t", "refilled" }, { "ok", false } };
        auto                     t0 = std::chrono::steady_clock::now();

        if( m_ipc.Call( kb::GetBoardSummary(), summary, err ) )
        {
            kiapi::board::commands::RefillZones req;
            req.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
            req.mutable_board()->set_board_filename( summary.file().substr( summary.file().find_last_of( '/' ) + 1 ) );
            google::protobuf::Empty empty;

            if( m_ipc.Call( req, empty, err ) )
                reply["ok"] = true;
        }

        reply["ms"] = std::round( std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );

        if( !reply["ok"] )
            reply["error"] = err;

        broadcast( reply.dump() );
        poll();   // zones changed: their ids go out in the delta; views re-request their fills
        return;
    }

    if( t == "save" )
    {
        kb::BoardSummaryResponse summary;
        json                     reply = { { "t", "saved" }, { "ok", false } };

        if( m_ipc.Call( kb::GetBoardSummary(), summary, err ) )
        {
            std::string file = summary.file().substr( summary.file().find_last_of( '/' ) + 1 );
            kiapi::common::commands::SaveDocument req;
            req.mutable_document()->set_type( kiapi::common::types::DOCTYPE_PCB );
            req.mutable_document()->set_board_filename( file );
            google::protobuf::Empty empty;

            if( m_ipc.Call( req, empty, err ) )
                reply = { { "t", "saved" }, { "ok", true }, { "file", file } };
        }

        if( !reply["ok"] )
            reply["error"] = err;

        session->Send( std::make_shared<std::string>( reply.dump() ) );
        broadcast( historyJson() );
        return;
    }

    if( t == "move" )
    {
        const json& m = aJob.message;
        kb::MoveDelta req;

        if( m.contains( "refs" ) && m["refs"].is_array() )
            for( const json& r : m["refs"] )
                if( r.is_string() )
                    req.add_refs( r.get<std::string>() );

        auto num = [&]( const char* aKey ) { return m.contains( aKey ) && m[aKey].is_number() ? m[aKey].get<double>() : 0.0; };

        req.set_dx( std::llround( num( "dx" ) * 1e6 ) );
        req.set_dy( std::llround( num( "dy" ) * 1e6 ) );
        req.set_drot_decideg( static_cast<int>( std::lround( num( "rot" ) * 10 ) ) );

        auto             t0 = std::chrono::steady_clock::now();
        kb::MoveResponse r;

        if( req.refs().empty() || !m_ipc.Call( req, r, err ) )
        {
            session->Send( std::make_shared<std::string>(
                    json{ { "t", "moved" }, { "ok", false }, { "error", err.empty() ? "no refs" : err } }.dump() ) );
            return;
        }

        double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
        sendMoveResult( session, "move", r.moved_size() > 0, r.moved_size() ? "" : "no footprint moved", &r, ms,
                        r.history_label() );

        poll();   // push the geometry change (and history state) to every view right away
    }
}


std::string WEB_HUB::historyJson()
{
    kb::HistoryResponse r;
    std::string         err;

    if( !m_ipc.Call( kb::GetHistory(), r, err ) )
        return json{ { "t", "history" }, { "error", err } }.dump();

    return json{ { "t", "history" },
                 { "undo", r.undo_size() },
                 { "redo", r.redo_size() },
                 { "next_undo", r.undo_size() ? r.undo( r.undo_size() - 1 ) : "" },
                 { "next_redo", r.redo_size() ? r.redo( r.redo_size() - 1 ) : "" },
                 { "modified", r.modified() } }.dump();
}


void WEB_HUB::sendMoveResult( const std::shared_ptr<WS_SESSION>& aSession, const char* aOp, bool aOk,
                              const std::string& aError, const void* aMoveResponse, double aMs,
                              const std::string& aLabel )
{
    json out = { { "t", "moved" }, { "op", aOp }, { "ok", aOk }, { "ms", std::round( aMs * 10 ) / 10 },
                 { "label", aLabel } };

    if( !aError.empty() )
        out["error"] = aError;

    json drc = json::array(), missing = json::array(), pending = json::array();

    if( aMoveResponse )
    {
        const kb::MoveResponse& r = *static_cast<const kb::MoveResponse*>( aMoveResponse );

        for( const kb::DrcViolation& v : r.drc() )
        {
            drc.push_back( { { "type", drcName( v.type() ) }, { "a", v.a() }, { "b", v.b() },
                             { "na", v.net_a() }, { "nb", v.net_b() }, { "ly", v.layer() },
                             { "need", um( v.required() ) }, { "have", um( v.actual() ) },
                             { "x", um( v.pos().x() ) }, { "y", um( v.pos().y() ) } } );
        }

        for( const std::string& s : r.missing() )
            missing.push_back( s );

        for( const std::string& s : r.ratsnest_pending() )
            pending.push_back( s );

        out["unconnected"] = r.unconnected();
    }

    out["drc"] = std::move( drc );
    out["missing"] = std::move( missing );
    out["pending"] = std::move( pending );
    aSession->Send( std::make_shared<std::string>( out.dump() ) );
}


void WEB_HUB::poll()
{
    kr::GetChanges req;
    req.set_since( m_revision );

    kr::RenderChanges r;
    std::string       err;

    if( !m_ipc.Call( req, r, err ) )
        return;   // daemon gone or busy; next poll retries

    if( r.resync() )
    {
        kr::RenderSnapshot snap;

        if( m_ipc.Call( kr::GetRenderSnapshot(), snap, err ) )
        {
            m_revision = snap.revision();
            broadcast( snapshotJson( snap ).dump() );
        }

        return;
    }

    if( r.revision() <= m_revision )
        return;

    json out = { { "t", "delta" }, { "since", m_revision }, { "rev", r.revision() } };
    addItems( r.upsert(), out );

    json rm = json::array();

    for( const std::string& id : r.removed() )
        rm.push_back( id );

    out["rm"] = std::move( rm );
    m_revision = r.revision();
    broadcast( out.dump() );
    broadcast( historyJson() );   // undo/redo availability and the unsaved flag changed too

    bool pending = false;
    broadcast( ratsnestJson( false, &pending ) );
    scheduleRatsnestFlush( pending );
}

} // namespace kicadd::mcp


namespace kicadd::mcp
{

std::string WEB_HUB::ratsnestJson( bool aFlush, bool* aPending )
{
    kr::GetRatsnest req;
    req.set_flush( aFlush );

    kr::Ratsnest r;
    std::string  err;

    if( !m_ipc.Call( req, r, err ) )
        return json{ { "t", "ratsnest" }, { "error", err } }.dump();

    json xy = json::array(), net = json::array(), nets = json::array(), pending = json::array();

    for( int64_t v : r.xy() )
        xy.push_back( um( v ) );

    for( int n : r.net() )
        net.push_back( n );

    for( const std::string& n : r.nets() )
        nets.push_back( n );

    for( const std::string& n : r.pending() )
        pending.push_back( n );

    if( aPending )
        *aPending = r.pending_size() > 0;

    return json{ { "t", "ratsnest" }, { "xy", std::move( xy ) }, { "net", std::move( net ) },
                 { "nets", std::move( nets ) }, { "pending", std::move( pending ) },
                 { "unconnected", r.unconnected() }, { "final", r.pending_size() == 0 } }.dump();
}


void WEB_HUB::scheduleRatsnestFlush( bool aPending )
{
    // Every change pushes the deadline back, so the flush runs once edits stop.
    m_flushDue = aPending ? std::chrono::steady_clock::now() + std::chrono::milliseconds( RATSNEST_IDLE_MS )
                          : std::chrono::steady_clock::time_point{};
}

} // namespace kicadd::mcp
