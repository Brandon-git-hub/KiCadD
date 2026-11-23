/*
 * KiCadD web view hub: WebSocket sessions, change polling and edits from browsers.
 *
 * Threads:
 *   - the io thread runs every WebSocket session (Beast async; one thread, so session
 *     handlers never race each other);
 *   - one worker thread does all daemon IPC: snapshots for new sessions, moves requested by
 *     browsers, and the change poll: when KiCad says something changed (its "<socket>.ev"
 *     notifications, see src/common/kicadd_events.h), right after an edit here, and every
 *     EVENT_POLL_MS as a fallback (every POLL_MS while the notifications are not connected:
 *     an older KiCadD, a tcp:// socket);
 *   - one thread waits on the notifications.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_WEB_H
#define KICADD_MCP_WEB_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace kicadd::mcp
{

class IPC_CLIENT;
class WS_SESSION;
namespace plot { struct CHART; }

/// A chart as the Chart tab's JSON, each series thinned to at most aMaxPoints (per bucket its
/// min and max, in order: peaks survive)
nlohmann::json ChartJson( const plot::CHART& aChart, const std::string& aTool, size_t aMaxPoints );

class WEB_HUB
{
public:
    WEB_HUB( IPC_CLIENT& aIpc, boost::asio::io_context& aIoc );
    ~WEB_HUB();

    /// Take over an upgraded HTTP connection (called from an HTTP connection thread).
    void Accept( boost::asio::ip::tcp::socket aSocket,
                 boost::beast::http::request<boost::beast::http::string_body> aRequest );

    // Called by sessions (io thread).
    void Joined( const std::shared_ptr<WS_SESSION>& aSession );
    void Left( const std::shared_ptr<WS_SESSION>& aSession );
    void Received( const std::shared_ptr<WS_SESSION>& aSession, const std::string& aText );

    /// Ask the worker to poll now (e.g. after an MCP edit in this process).
    void Kick();

    /// A tool's chart (plot::Publish) for the Chart tab: sent to every view, the last
    /// MAX_CHARTS kept for views that join later. Any thread.
    void PublishChart( const nlohmann::json& aChart );
    static constexpr size_t MAX_CHARTS = 8;
    static constexpr size_t MAX_CHART_POINTS = 4000;   ///< per series; more are thinned (min / max kept)

    static constexpr int POLL_MS = 50;
    static constexpr int EVENT_POLL_MS = 1000;

    /// The notifications are connected (KiCad pushes its changes)
    bool Pushed() const { return m_evPipes.load() > 0; }

private:
    struct JOB
    {
        std::weak_ptr<WS_SESSION> session;
        nlohmann::json            message;   // {"t":"snapshot"} or a client request
    };

    void workerLoop();
    void runJob( const JOB& aJob );
    void poll();
    void broadcast( const std::string& aText );
    std::string historyJson();
    /// Ratsnest message; aFlush rebuilds deferred large nets first. Sets aPending.
    std::string ratsnestJson( bool aFlush, bool* aPending = nullptr );
    void        scheduleRatsnestFlush( bool aPending );
    void sendMoveResult( const std::shared_ptr<WS_SESSION>& aSession, const char* aOp, bool aOk,
                         const std::string& aError, const void* aMoveResponse, double aMs,
                         const std::string& aLabel );
    std::set<std::shared_ptr<WS_SESSION>> sessions();
    void        eventsLoop();
    std::string statusJson();
    std::string copilotJson();          ///< {"t":"copilot",...}, empty when KiCad has none (older)
    void        sendCopilotIfChanged();
    void        sendStatusIfChanged();

    std::unique_ptr<IPC_CLIENT> m_ownIpc;   // the view's own connection ("kicadd-web"): its reads are not the agent's
    IPC_CLIENT&              m_ipc;
    boost::asio::io_context& m_ioc;

    std::mutex                            m_sessionsMutex;
    std::set<std::shared_ptr<WS_SESSION>> m_sessions;

    std::mutex              m_jobsMutex;
    std::condition_variable m_jobsCv;
    std::deque<JOB>         m_jobs;
    bool                    m_kick = false;

    uint64_t          m_revision = 0;   // worker thread only
    std::chrono::steady_clock::time_point m_flushDue{};   // worker only; epoch = none
    static constexpr int RATSNEST_IDLE_MS = 400;
    std::atomic<bool> m_running{ true };
    std::thread       m_ioThread;
    std::thread       m_worker;
    std::thread       m_events;
    std::atomic<int>  m_evPipes{ 0 };
    std::mutex        m_chartsMutex;
    std::deque<std::shared_ptr<const std::string>> m_charts;
    int               m_chartSeq = 0;
    std::string       m_lastStatus;         // worker only
    std::string       m_lastCopilot;        // worker only
    std::atomic<bool> m_copilotDue{ true };
    std::atomic<bool> m_schDue{ false };
    std::chrono::steady_clock::time_point m_copilotPolled{};   // worker only (fallback without push)
};

} // namespace kicadd::mcp

#endif // KICADD_MCP_WEB_H
