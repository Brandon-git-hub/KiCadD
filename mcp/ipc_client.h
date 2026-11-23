/*
 * KiCadD MCP - synchronous IPC client for the KiCad API server (protobuf over NNG req/rep).
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#ifndef KICADD_MCP_IPC_CLIENT_H
#define KICADD_MCP_IPC_CLIENT_H

#include <mutex>
#include <string>

#include <google/protobuf/message.h>
#include <nng/nng.h>

namespace kicadd::mcp
{

class IPC_CLIENT
{
public:
    /// aClientName goes in every request's header: KiCad's Co-pilot log leaves out the web
    /// view's own reads ("kicadd-web")
    IPC_CLIENT( std::string aUrl, int aTimeoutMs, std::string aClientName = "kicadd-mcp" );
    ~IPC_CLIENT();

    IPC_CLIENT( const IPC_CLIENT& ) = delete;
    IPC_CLIENT& operator=( const IPC_CLIENT& ) = delete;

    /// Dial the server; NNG reconnects on its own afterwards.
    bool Connect( std::string& aError );

    /**
     * Send aRequest wrapped in an ApiRequest envelope and unpack the reply into aResponse.
     * Calls are serialised (one REQ socket). Retries briefly while the server reports
     * AS_NOT_READY / AS_BUSY.
     * @return false with aError set on transport failure or a non-OK API status.
     */
    bool Call( const google::protobuf::Message& aRequest, google::protobuf::Message& aResponse,
               std::string& aError );

    const std::string& Url() const { return m_url; }
    int                TimeoutMs() const { return m_timeoutMs; }

private:
    std::string m_url;
    int         m_timeoutMs;
    std::string m_clientName;
    nng_socket  m_socket = NNG_SOCKET_INITIALIZER;
    bool        m_open = false;
    std::mutex  m_mutex;
};

} // namespace kicadd::mcp

#endif // KICADD_MCP_IPC_CLIENT_H
