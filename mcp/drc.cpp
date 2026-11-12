/*
 * KiCadD MCP - full-board DRC via the upstream DRC task, with readable item handles.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "drc.h"
#include "ipc_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <unordered_map>

#include <kicadd/board.pb.h>
#include <api/board/board_rules.pb.h>
#include <api/board/board_tasks.pb.h>
#include <api/board/board_types.pb.h>
#include <api/common/tasks.pb.h>
#include <api/common/types/base_types.pb.h>

namespace kb = kicadd::board;

namespace kicadd::mcp
{

namespace
{

std::string typeName( kiapi::board::DrcErrorType aType )
{
    std::string n = kiapi::board::DrcErrorType_Name( aType );   // DRCET_SHORTING_ITEMS

    if( n.rfind( "DRCET_", 0 ) == 0 )
        n = n.substr( 6 );

    std::transform( n.begin(), n.end(), n.begin(), []( unsigned char c ) { return std::tolower( c ); } );
    return n;
}


std::string layerName( kiapi::board::types::BoardLayer aLayer )
{
    std::string n = kiapi::board::types::BoardLayer_Name( aLayer );   // BL_In1_Cu

    if( n.rfind( "BL_", 0 ) == 0 )
        n = n.substr( 3 );

    if( n == "UNKNOWN" || n == "UNDEFINED" || n == "UNSELECTED" )
        return {};

    std::replace( n.begin(), n.end(), '_', '.' );
    return n;
}

} // namespace


bool RunFullDrc( IPC_CLIENT& aIpc, DRC_REPORT& aReport, std::string& aError )
{
    auto t0 = std::chrono::steady_clock::now();

    kb::BoardSummaryResponse summary;

    if( !aIpc.Call( kb::GetBoardSummary(), summary, aError ) )
        return false;

    kiapi::board::tasks::StartDrcTask start;
    start.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    start.mutable_board()->set_board_filename( summary.file().substr( summary.file().find_last_of( '/' ) + 1 ) );

    kiapi::common::tasks::StartTaskResponse started;

    if( !aIpc.Call( start, started, aError ) )
        return false;

    for( ;; )
    {
        kiapi::common::tasks::GetTaskStatus  q;
        kiapi::common::tasks::TaskStatusResponse st;
        *q.mutable_token() = started.token();

        if( !aIpc.Call( q, st, aError ) )
            return false;

        if( st.state() == kiapi::common::tasks::TS_RUNNING )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 150 ) );
            continue;
        }

        if( st.state() != kiapi::common::tasks::TS_COMPLETED )
        {
            aError = "DRC task " + kiapi::common::tasks::TaskState_Name( st.state() )
                     + ( st.error_message().empty() ? "" : ": " + st.error_message() );
            return false;
        }

        break;
    }

    kiapi::board::tasks::GetDrcTaskResult get;
    *get.mutable_token() = started.token();
    kiapi::board::tasks::DrcTaskResult result;

    if( !aIpc.Call( get, result, aError ) )
        return false;

    aReport.ignored = result.ignored_count();
    aReport.duplicateRefs.assign( summary.duplicate_refs().begin(), summary.duplicate_refs().end() );
    aReport.excluded = result.excluded_count();

    // Translate every referenced KIID in one round trip.
    kb::DescribeItems describe;

    for( const auto& m : result.violations() )
        for( const auto& id : m.items() )
            describe.add_ids( id.value() );

    kb::DescribeItemsResponse described;
    std::unordered_map<std::string, const kb::DescribedItem*> byId;

    if( describe.ids_size() && aIpc.Call( describe, described, aError ) )
    {
        for( const kb::DescribedItem& d : described.items() )
            byId[d.id()] = &d;
    }

    for( const auto& m : result.violations() )
    {
        if( m.excluded() )
            continue;

        DRC_ENTRY e;
        e.type = typeName( m.error_type() );
        e.warning = m.severity() != kiapi::common::types::RS_ERROR;
        e.layer = layerName( m.layer() );
        e.description = m.description();
        e.x = m.position().x_nm();
        e.y = m.position().y_nm();

        for( int i = 0; i < m.items_size() && i < 2; ++i )
        {
            auto        it = byId.find( m.items( i ).value() );
            std::string handle = it != byId.end() && !it->second->handle().empty() ? it->second->handle()
                                                                                   : m.items( i ).value().substr( 0, 8 );
            std::string net = it != byId.end() ? it->second->net() : "";

            ( i == 0 ? e.a : e.b ) = handle;
            ( i == 0 ? e.netA : e.netB ) = net;
        }

        ( e.warning ? aReport.warnings : aReport.errors )[e.type]++;
        aReport.entries.push_back( std::move( e ) );
    }

    // Most consequential first: what breaks the circuit, then what breaks fabrication.
    static const std::vector<std::string> priority = {
        "shorting_items", "tracks_crossing", "clearance", "hole_clearance", "edge_clearance", "copper_outside_board",
        "unconnected_items", "zones_intersect", "drill_out_of_range", "annular_width",
        "overlapping_footprints", "track_width", "via_diameter", "soldermask_bridge"
    };

    auto rank = [&]( const std::string& aType )
    {
        auto it = std::find( priority.begin(), priority.end(), aType );
        return it == priority.end() ? priority.size() : static_cast<size_t>( it - priority.begin() );
    };

    std::stable_sort( aReport.entries.begin(), aReport.entries.end(),
                      [&]( const DRC_ENTRY& a, const DRC_ENTRY& b )
                      {
                          if( a.warning != b.warning )
                              return !a.warning;

                          if( rank( a.type ) != rank( b.type ) )
                              return rank( a.type ) < rank( b.type );

                          return a.type < b.type;
                      } );

    aReport.seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
    return true;
}

} // namespace kicadd::mcp
