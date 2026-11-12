/*
 * KiCadD - compact, spatially scoped board API handler.
 *
 * Registered alongside upstream API_HANDLER_PCB on the same in-memory PCB_CONTEXT,
 * so both APIs see the same resident BOARD.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_HANDLER_PCB_H
#define KICADD_HANDLER_PCB_H

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <kiid.h>
#include <geometry/eda_angle.h>
#include <math/vector2d.h>

#include <api/api_handler.h>
#include <kicadd/board.pb.h>
#include "pcb/kicadd_rules.h"
#include "pcb/kicadd_setup.h"
#include <kicadd/render.pb.h>

#include "pcb/kicadd_autoplace.h"

class BOARD;
class BOARD_ITEM;
namespace KIGFX { class VIEW; }
class BOARD_COMMIT;
class FOOTPRINT;
class ZONE;
class NETINFO_ITEM;
class PCB_TRACK;
class PCB_CONTEXT;

namespace kicadd { class RENDER_FEED; }


class KICADD_HANDLER_PCB : public API_HANDLER
{
public:
    explicit KICADD_HANDLER_PCB( std::shared_ptr<PCB_CONTEXT> aContext );
    ~KICADD_HANDLER_PCB() override;

    /// The editor is about to replace (and delete) its board: stop listening to it and drop
    /// the history, whose item ids belong to it. The next request attaches to the new board.
    void DetachBoard();

private:
    BOARD* board() const;

    /// Change feed for remote views; created on first use for the current board.
    kicadd::RENDER_FEED* feed();

    /// Set when an edit must wait: in the desktop editor, while the user is in the middle of
    /// an interactive operation (routing, dragging, a modal dialog).
    std::optional<kiapi::common::ApiResponseStatus> busy() const;

    /// True in the desktop editor: commits go on its undo list, which then owns removed items.
    bool editorOwnsUndo() const;

    /// Desktop only: repaint the canvas and tell the user in the status bar what the agent did.
    void announce( const wxString& aWhat ) const;

    HANDLER_RESULT<kicadd::board::BoardSummaryResponse>
    handleGetBoardSummary( const HANDLER_CONTEXT<kicadd::board::GetBoardSummary>& aCtx );

    HANDLER_RESULT<kicadd::board::QueryResponse>
    handleQueryRadius( const HANDLER_CONTEXT<kicadd::board::QueryRadius>& aCtx );

    HANDLER_RESULT<kicadd::board::QueryResponse>
    handleQueryNet( const HANDLER_CONTEXT<kicadd::board::QueryNet>& aCtx );

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleMoveDelta( const HANDLER_CONTEXT<kicadd::board::MoveDelta>& aCtx );

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleSetFields( const HANDLER_CONTEXT<kicadd::board::FpSetFields>& aCtx );

    HANDLER_RESULT<kicadd::board::RulesResponse>
    handleGetRules( const HANDLER_CONTEXT<kicadd::board::GetRules>& aCtx );

    HANDLER_RESULT<kicadd::board::RulesResponse>
    handleSetRules( const HANDLER_CONTEXT<kicadd::board::SetRules>& aCtx );

    void packRules( kicadd::board::RulesResponse& aOut );

    HANDLER_RESULT<kicadd::board::ZonesResponse>
    handleZones( const HANDLER_CONTEXT<kicadd::board::Zones>& aCtx );

    /// zones create: the new zone, on the board (one undoable edit); null: aError
    ZONE* createZone( const kicadd::board::ZoneCreate& aReq, std::string& aError, std::string& aLabel );

    HANDLER_RESULT<kicadd::board::BoardOutlineResponse>
    handleBoardOutline( const HANDLER_CONTEXT<kicadd::board::BoardOutline>& aCtx );

    HANDLER_RESULT<kicadd::board::BoardSetupResponse>
    handleBoardSetup( const HANDLER_CONTEXT<kicadd::board::BoardSetup>& aCtx );

    /// The layers / stackup changed (board_setup, its undo): the editor's layer lists redone.
    void setupChanged();

    /// The rules changed (set_rules, its undo): nets re-resolved, the editor told.
    void rulesChanged();

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleDeleteFootprints( const HANDLER_CONTEXT<kicadd::board::DeleteFootprints>& aCtx );

    HANDLER_RESULT<kicadd::board::FpCreateResponse>
    handleFpCreate( const HANDLER_CONTEXT<kicadd::board::FpCreate>& aCtx );

    HANDLER_RESULT<kicadd::board::LcscSearchResponse>
    handleLcscSearch( const HANDLER_CONTEXT<kicadd::board::LcscSearch>& aCtx );

    HANDLER_RESULT<kicadd::board::ExportFabResponse>
    handleExportFab( const HANDLER_CONTEXT<kicadd::board::ExportFab>& aCtx );

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleDeleteTracks( const HANDLER_CONTEXT<kicadd::board::DeleteTracks>& aCtx );

    HANDLER_RESULT<kicadd::board::RouteResponse>
    handleRoute( const HANDLER_CONTEXT<kicadd::board::RouteTrack>& aCtx );

    HANDLER_RESULT<kicadd::board::RouteResponse>
    handleRouteShort( const HANDLER_CONTEXT<kicadd::board::RouteShort>& aCtx );

    HANDLER_RESULT<kicadd::board::FanoutResponse>
    handleFanout( const HANDLER_CONTEXT<kicadd::board::Fanout>& aCtx );

    /// The new tracks of a routing session (or a dry run's report): one commit, one undo entry
    void commitRoutes( std::vector<std::unique_ptr<PCB_TRACK>>&& aAdded, const std::vector<PCB_TRACK*>& aReplaced,
                       const std::string& aLabel, kicadd::board::RouteResponse& aOut );

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleAddFootprint( const HANDLER_CONTEXT<kicadd::board::AddFootprint>& aCtx );

    HANDLER_RESULT<kicadd::board::FetchLcscResponse>
    handleFetchLcsc( const HANDLER_CONTEXT<kicadd::board::FetchLcsc>& aCtx );

    HANDLER_RESULT<kicadd::board::AutoPlaceResponse>
    handleAutoPlace( const HANDLER_CONTEXT<kicadd::board::AutoPlace>& aCtx );

    HANDLER_RESULT<kicadd::board::AutoPlaceResponse>
    handleAutoPlaceStatus( const HANDLER_CONTEXT<kicadd::board::AutoPlaceStatus>& aCtx );

    /// An auto_place run: the model built from the board, the placer's result, what to report
    struct AUTOPLACE_JOB
    {
        std::vector<kicadd::autoplace::PART> parts;
        std::vector<KIID>                    ids;          ///< the footprint of each part
        int                                  netCount = 0;
        SHAPE_POLY_SET                       outline;
        kicadd::autoplace::OPTIONS           opt;
        kicadd::autoplace::PROGRESS          progress;
        bool                                 measureOnly = false;
        int                                  movable = 0;
        std::string                          groupSource;
        BOARD*                               board = nullptr;
        uint64_t                             revision = 0;  ///< the board's, at the start
        std::chrono::steady_clock::time_point started;
        kicadd::board::AutoPlaceResponse     out;           ///< filled while building the model

        std::thread                          worker;
        std::atomic<bool>                    done{ false };
        kicadd::autoplace::RESULT            result;
        double                               seconds = 0;
    };

    /// The placement itself (any thread: touches only the job)
    static void runAutoPlace( AUTOPLACE_JOB& aJob );

    /// Main thread: apply a finished job's result to the board as one undoable edit
    HANDLER_RESULT<kicadd::board::AutoPlaceResponse> finishAutoPlace( AUTOPLACE_JOB& aJob );

    kicadd::board::AutoPlaceResponse autoPlaceRunning( const AUTOPLACE_JOB& aJob ) const;

    /// Cancel a background run and wait for its thread
    void stopAutoPlace();

    HANDLER_RESULT<kicadd::board::SyncFromSchematicResponse>
    handleSyncFromSchematic( const HANDLER_CONTEXT<kicadd::board::SyncFromSchematic>& aCtx );

    HANDLER_RESULT<kicadd::board::LocalDrcResponse>
    handleCheckLocalDrc( const HANDLER_CONTEXT<kicadd::board::CheckLocalDrc>& aCtx );

    HANDLER_RESULT<kicadd::board::StateDigestResponse>
    handleGetStateDigest( const HANDLER_CONTEXT<kicadd::board::GetStateDigest>& aCtx );

    HANDLER_RESULT<kicadd::board::VerifyConnectivityResponse>
    handleVerifyConnectivity( const HANDLER_CONTEXT<kicadd::board::VerifyConnectivity>& aCtx );

    HANDLER_RESULT<kicadd::board::RatsnestDeferralResponse>
    handleSetRatsnestDeferral( const HANDLER_CONTEXT<kicadd::board::SetRatsnestDeferral>& aCtx );

    HANDLER_RESULT<kicadd::board::FlushRatsnestResponse>
    handleFlushRatsnest( const HANDLER_CONTEXT<kicadd::board::FlushRatsnest>& aCtx );

    HANDLER_RESULT<kicadd::render::RenderSnapshot>
    handleGetRenderSnapshot( const HANDLER_CONTEXT<kicadd::render::GetRenderSnapshot>& aCtx );

    HANDLER_RESULT<kicadd::render::RenderChanges>
    handleGetChanges( const HANDLER_CONTEXT<kicadd::render::GetChanges>& aCtx );

    HANDLER_RESULT<kicadd::render::Ratsnest>
    handleGetRatsnest( const HANDLER_CONTEXT<kicadd::render::GetRatsnest>& aCtx );

    HANDLER_RESULT<kicadd::render::Art>
    handleGetArt( const HANDLER_CONTEXT<kicadd::render::GetArt>& aCtx );

    HANDLER_RESULT<kicadd::render::ZoneFills>
    handleGetZoneFills( const HANDLER_CONTEXT<kicadd::render::GetZoneFills>& aCtx );

    HANDLER_RESULT<kicadd::board::HistoryResponse>
    handleUndo( const HANDLER_CONTEXT<kicadd::board::Undo>& aCtx );

    HANDLER_RESULT<kicadd::board::HistoryResponse>
    handleRedo( const HANDLER_CONTEXT<kicadd::board::Redo>& aCtx );

    HANDLER_RESULT<kicadd::board::HistoryResponse>
    handleGetHistory( const HANDLER_CONTEXT<kicadd::board::GetHistory>& aCtx );

    HANDLER_RESULT<kicadd::board::DescribeItemsResponse>
    handleDescribeItems( const HANDLER_CONTEXT<kicadd::board::DescribeItems>& aCtx );

    /// One commit built by aBuild (which returns the footprints to report on), with large-net
    /// ratsnest deferral; then fills aOut with their positions, the unconnected count and the
    /// local DRC around them.
    void runCommit( const std::function<std::vector<KIID>( BOARD_COMMIT& )>& aBuild,
                    const wxString& aCommitMessage, kicadd::board::MoveResponse& aOut );

    /// runCommit that modifies each target with aMutate.
    void applyFootprintEdit( const std::vector<FOOTPRINT*>& aTargets,
                             const std::function<void( FOOTPRINT* )>& aMutate,
                             const wxString& aCommitMessage, kicadd::board::MoveResponse& aOut );

    struct FP_STATE
    {
        KIID      id;
        VECTOR2I  pos;
        EDA_ANGLE orient;
        bool      back = false;

        /// pcb_set_fields: the fields too (Value and the user fields, name -> text) and the
        /// attributes (DNP, BOM, position files); compared and restored only then
        bool                                        withFields = false;
        std::vector<std::pair<wxString, wxString>>  fields;
        int                                         attrs = 0;

        bool operator==( const FP_STATE& aOther ) const
        {
            return pos == aOther.pos && orient == aOther.orient && back == aOther.back
                   && ( !withFields || ( fields == aOther.fields && attrs == aOther.attrs ) );
        }
    };

    /// A footprint off the board (deleted, or kept to redo an add). Owned by the history,
    /// detached from the board: no parent and no net pointers, nets kept by name per pad.
    struct FP_SNAPSHOT
    {
        std::shared_ptr<FOOTPRINT> fp;
        std::vector<wxString>      padNets;
    };

    /// A track, arc or via off the board (deleted), net kept by name.
    struct TRACK_SNAPSHOT
    {
        std::shared_ptr<PCB_TRACK> track;
        wxString                   net;
    };

    struct ZONE_STATE
    {
        KIID     id;
        int      clearance = 0, minWidth = 0, gap = 0, spoke = 0;
        unsigned priority = 0;
        int      connection = 0;

        bool operator==( const ZONE_STATE& ) const = default;
    };

    static ZONE_STATE zoneStateOf( const ZONE* aZone );
    static void       setZoneState( ZONE* aZone, const ZONE_STATE& aState );

    struct HISTORY_ENTRY
    {
        std::string                 label;
        std::vector<FP_STATE>       before;
        std::vector<FP_STATE>       after;
        std::vector<FP_SNAPSHOT>    added;     ///< removed again by undo
        std::vector<FP_SNAPSHOT>    removed;   ///< put back by undo
        std::vector<TRACK_SNAPSHOT> tracksRemoved;   ///< put back by undo
        std::vector<TRACK_SNAPSHOT> tracksAdded;     ///< taken out by undo (route)

        /// set_rules: the design rules before / after (nothing else in such an entry)
        std::shared_ptr<kicadd::rules::STATE> rulesBefore, rulesAfter;

        /// zones: their settings before / after
        std::vector<ZONE_STATE> zonesBefore, zonesAfter;

        /// sync_pcb: footprints it changed, as they were / are (undo / redo swaps them whole)
        std::vector<std::pair<FP_SNAPSHOT, FP_SNAPSHOT>> replaced;

        /// sync_pcb: tracks, vias and zones it moved to another net (a renamed net)
        struct NET_CHANGE
        {
            KIID     id;
            wxString before, after;
        };

        std::vector<NET_CHANGE> netChanges;

        /// sync_pcb: the nets it made; undo takes out those nothing is on any more
        std::vector<wxString> netsCreated;

        /// board_outline, zones create: board graphics and zones it added / removed (by KIID;
        /// a zone's net by name)
        struct ITEM_SNAPSHOT
        {
            std::shared_ptr<BOARD_ITEM> item;
            wxString                    net;
        };

        std::vector<ITEM_SNAPSHOT> itemsAdded, itemsRemoved;

        /// board_setup: layers and stackup before / after (nothing else in such an entry)
        std::shared_ptr<kicadd::setup::STATE> setupBefore, setupAfter;
    };

    /// A copy of a board item off the board (same KIID), its net kept by name.
    static HISTORY_ENTRY::ITEM_SNAPSHOT snapshotItem( const BOARD_ITEM* aItem );

    /// A copy of aFp (still on the board) into a snapshot.
    static FP_SNAPSHOT copyOf( const FOOTPRINT* aFp );

    /// aFp is as aSnap has it (shape, fields, attributes, place, pad nets).
    static bool matches( const FOOTPRINT* aFp, const FP_SNAPSHOT& aSnap );

    static FP_STATE stateOf( const FOOTPRINT* aFp, bool aWithFields = false );
    /// aView: the editor's canvas, which keeps a footprint's fields as items of their own
    /// (re-added when fields come and go); null headless
    static void     setState( FOOTPRINT* aFp, const FP_STATE& aState, KIGFX::VIEW* aView = nullptr );

    /// The editor's canvas, null headless.
    KIGFX::VIEW*    view() const;
    static void     addUserField( FOOTPRINT* aFp, const wxString& aName, const wxString& aText );

    /// Detach aFp (owned by the caller from now on) into a snapshot.
    static FP_SNAPSHOT detach( FOOTPRINT* aFp );

    /// A board-ready copy of aSnap, nets resolved by name (created through aCommit if gone).
    FOOTPRINT* instantiate( const FP_SNAPSHOT& aSnap, BOARD_COMMIT& aCommit,
                            kicadd::board::MoveResponse& aOut );

    NETINFO_ITEM* netFor( const wxString& aName, BOARD_COMMIT& aCommit, kicadd::board::MoveResponse& aOut );

    void record( HISTORY_ENTRY&& aEntry );

    /// Undo (aUndo) or redo aEntry; fills aOut. Returns false if nothing of it is left to apply.
    bool applyHistory( const HISTORY_ENTRY& aEntry, bool aUndo, kicadd::board::MoveResponse& aOut );

    void fillHistory( kicadd::board::HistoryResponse& aOut ) const;

    static constexpr size_t MAX_HISTORY = 200;

    std::vector<HISTORY_ENTRY> m_undo;
    std::vector<HISTORY_ENTRY> m_redo;

    std::shared_ptr<PCB_CONTEXT> m_context;
    std::unique_ptr<kicadd::RENDER_FEED> m_feed;

    std::vector<NETINFO_ITEM*> m_pendingNets;   ///< created by the commit being built

    std::unique_ptr<AUTOPLACE_JOB> m_autoPlace;   ///< running in the background

    bool m_deferLargeNets = true;
    int  m_deferMinPads = 50;
};

#endif // KICADD_HANDLER_PCB_H
