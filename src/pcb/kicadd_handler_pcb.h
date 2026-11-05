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

#include <functional>
#include <memory>
#include <vector>

#include <kiid.h>
#include <geometry/eda_angle.h>
#include <math/vector2d.h>

#include <api/api_handler.h>
#include <kicadd/board.pb.h>
#include <kicadd/render.pb.h>

class BOARD;
class BOARD_COMMIT;
class FOOTPRINT;
class NETINFO_ITEM;
class PCB_CONTEXT;

namespace kicadd { class RENDER_FEED; }


class KICADD_HANDLER_PCB : public API_HANDLER
{
public:
    explicit KICADD_HANDLER_PCB( std::shared_ptr<PCB_CONTEXT> aContext );
    ~KICADD_HANDLER_PCB() override;

private:
    BOARD* board() const;

    HANDLER_RESULT<kicadd::board::BoardSummaryResponse>
    handleGetBoardSummary( const HANDLER_CONTEXT<kicadd::board::GetBoardSummary>& aCtx );

    HANDLER_RESULT<kicadd::board::QueryResponse>
    handleQueryRadius( const HANDLER_CONTEXT<kicadd::board::QueryRadius>& aCtx );

    HANDLER_RESULT<kicadd::board::QueryResponse>
    handleQueryNet( const HANDLER_CONTEXT<kicadd::board::QueryNet>& aCtx );

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleMoveDelta( const HANDLER_CONTEXT<kicadd::board::MoveDelta>& aCtx );

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleDeleteFootprints( const HANDLER_CONTEXT<kicadd::board::DeleteFootprints>& aCtx );

    HANDLER_RESULT<kicadd::board::MoveResponse>
    handleAddFootprint( const HANDLER_CONTEXT<kicadd::board::AddFootprint>& aCtx );

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
    };

    /// A footprint off the board (deleted, or kept to redo an add). Owned by the history,
    /// detached from the board: no parent and no net pointers, nets kept by name per pad.
    struct FP_SNAPSHOT
    {
        std::shared_ptr<FOOTPRINT> fp;
        std::vector<wxString>      padNets;
    };

    struct HISTORY_ENTRY
    {
        std::string              label;
        std::vector<FP_STATE>    before;
        std::vector<FP_STATE>    after;
        std::vector<FP_SNAPSHOT> added;     ///< removed again by undo
        std::vector<FP_SNAPSHOT> removed;   ///< put back by undo
    };

    static FP_STATE stateOf( const FOOTPRINT* aFp );
    static void     setState( FOOTPRINT* aFp, const FP_STATE& aState );

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

    bool m_deferLargeNets = true;
    int  m_deferMinPads = 50;
};

#endif // KICADD_HANDLER_PCB_H
