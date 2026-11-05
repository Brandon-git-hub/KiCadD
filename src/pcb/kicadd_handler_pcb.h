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
class FOOTPRINT;
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

    HANDLER_RESULT<kicadd::board::HistoryResponse>
    handleUndo( const HANDLER_CONTEXT<kicadd::board::Undo>& aCtx );

    HANDLER_RESULT<kicadd::board::HistoryResponse>
    handleRedo( const HANDLER_CONTEXT<kicadd::board::Redo>& aCtx );

    HANDLER_RESULT<kicadd::board::HistoryResponse>
    handleGetHistory( const HANDLER_CONTEXT<kicadd::board::GetHistory>& aCtx );

    /// One commit that applies aMutate to each target, with large-net ratsnest deferral,
    /// then fills aOut with positions, unconnected count and the local DRC of the targets.
    void applyFootprintEdit( const std::vector<FOOTPRINT*>& aTargets,
                             const std::function<void( FOOTPRINT* )>& aMutate,
                             const wxString& aCommitMessage, kicadd::board::MoveResponse& aOut );

    struct FP_STATE
    {
        KIID      id;
        VECTOR2I  pos;
        EDA_ANGLE orient;
    };

    struct HISTORY_ENTRY
    {
        std::string           label;
        std::vector<FP_STATE> before;
        std::vector<FP_STATE> after;
    };

    static FP_STATE stateOf( const FOOTPRINT* aFp );

    /// Restore aStates; fills aOut. Returns false if none of the footprints still exist.
    bool restore( const std::vector<FP_STATE>& aStates, const std::string& aLabel,
                  kicadd::board::MoveResponse& aOut );

    void fillHistory( kicadd::board::HistoryResponse& aOut ) const;

    static constexpr size_t MAX_HISTORY = 200;

    std::vector<HISTORY_ENTRY> m_undo;
    std::vector<HISTORY_ENTRY> m_redo;

    std::shared_ptr<PCB_CONTEXT> m_context;
    std::unique_ptr<kicadd::RENDER_FEED> m_feed;

    bool m_deferLargeNets = true;
    int  m_deferMinPads = 50;
};

#endif // KICADD_HANDLER_PCB_H
