/*
 * KiCadD web view hub: WebSocket sessions, change polling and edits from browsers.
 *
 * Threads:
 *   - the io thread runs every WebSocket session (Beast async; one thread, so session
 *     handlers never race each other);
 *   - one worker thread does all daemon IPC: snapshots for new sessions, moves requested by
 *     browsers, and the change poll (every POLL_MS, immediately after an edit).
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

    static constexpr int POLL_MS = 50;

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
};

} // namespace kicadd::mcp

#endif // KICADD_MCP_WEB_H
