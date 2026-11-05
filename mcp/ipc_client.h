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
    IPC_CLIENT( std::string aUrl, int aTimeoutMs );
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

private:
    std::string m_url;
    int         m_timeoutMs;
    nng_socket  m_socket = NNG_SOCKET_INITIALIZER;
    bool        m_open = false;
    std::mutex  m_mutex;
};

} // namespace kicadd::mcp

#endif // KICADD_MCP_IPC_CLIENT_H
