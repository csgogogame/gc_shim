#include "stdafx.h"
#include "gc_edge_transport.h"
#include "platform.h"
#include "csgogo/edge.pb.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace edge = csgogo::edge;

namespace
{
#if defined(_WIN32)
bool EnsureWinsock()
{
    static bool initialized = []
    {
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }();
    return initialized;
}

void CloseSocket(socket_t s) { closesocket(s); }
#else
bool EnsureWinsock() { return true; }
void CloseSocket(socket_t s) { close(s); }
#endif

constexpr uint32_t kMaxFrameBytes = 4u << 20; // 4 MiB, matches the gateway
} // namespace

EdgeTransport::~EdgeTransport()
{
    Close();
}

void EdgeTransport::Close()
{
    if (m_sock != -1)
    {
        CloseSocket(static_cast<socket_t>(m_sock));
        m_sock = -1;
    }
    m_connected = false;
}

bool EdgeTransport::Connect(const std::string &host, uint16_t port, uint32_t role,
    uint64_t steamId, const std::vector<uint8_t> &authTicket)
{
    if (!EnsureWinsock())
    {
        Platform::Print("EdgeTransport: winsock init failed\n");
        return false;
    }

    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%u", port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &result) != 0 || !result)
    {
        Platform::Print("EdgeTransport: cannot resolve %s:%u\n", host.c_str(), port);
        return false;
    }

    socket_t s = kInvalidSocket;
    for (addrinfo *ai = result; ai; ai = ai->ai_next)
    {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket)
        {
            continue;
        }
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
        {
            break;
        }
        CloseSocket(s);
        s = kInvalidSocket;
    }
    freeaddrinfo(result);

    if (s == kInvalidSocket)
    {
        Platform::Print("EdgeTransport: connect to %s:%u failed\n", host.c_str(), port);
        return false;
    }
    m_sock = static_cast<intptr_t>(s);

    // Hello handshake.
    edge::EdgeFrame helloFrame;
    edge::EdgeHello *hello = helloFrame.mutable_hello();
    hello->set_role(static_cast<edge::EdgeRole>(role));
    hello->set_client_version(1573); // pinned protocol
    hello->set_steam_id(steamId);
    if (!authTicket.empty())
    {
        hello->set_auth_ticket(authTicket.data(), authTicket.size());
    }
    hello->set_shim_version("0.1.0");

    if (!WriteFrameBytes(helloFrame.SerializeAsString()))
    {
        Close();
        return false;
    }

    std::string respBytes;
    if (!ReadFrameBytes(respBytes))
    {
        Close();
        return false;
    }
    edge::EdgeFrame respFrame;
    if (!respFrame.ParseFromString(respBytes) || !respFrame.has_hello_response())
    {
        Platform::Print("EdgeTransport: expected HelloResponse\n");
        Close();
        return false;
    }
    const edge::EdgeHelloResponse &resp = respFrame.hello_response();
    if (!resp.accepted())
    {
        Platform::Print("EdgeTransport: gateway rejected hello: %s\n", resp.reject_reason().c_str());
        Close();
        return false;
    }

    m_connected = true;
    Platform::Print("EdgeTransport: connected to %s:%u (session %llu)\n",
        host.c_str(), port, static_cast<unsigned long long>(resp.session_id()));
    return true;
}

bool EdgeTransport::Request(uint32_t msgType, uint64_t jobId, uint64_t steamId,
    const std::string &payload, uint32_t &outMsgType, std::string &outPayload)
{
    if (!m_connected)
    {
        return false;
    }

    edge::EdgeFrame frame;
    edge::EdgeEnvelope *env = frame.mutable_envelope();
    env->set_direction(edge::EDGE_DIRECTION_TO_GC);
    env->set_msg_type(msgType);
    env->set_job_id(jobId);
    env->set_steam_id(steamId);
    env->set_payload(payload);

    if (!WriteFrameBytes(frame.SerializeAsString()))
    {
        return false;
    }

    // Read frames until we get an envelope reply (skip pongs/keepalives).
    for (;;)
    {
        std::string bytes;
        if (!ReadFrameBytes(bytes))
        {
            return false;
        }
        edge::EdgeFrame reply;
        if (!reply.ParseFromString(bytes))
        {
            return false;
        }
        if (reply.has_envelope())
        {
            const edge::EdgeEnvelope &re = reply.envelope();
            outMsgType = re.msg_type();
            outPayload = re.payload();
            return true;
        }
        if (reply.has_error())
        {
            Platform::Print("EdgeTransport: backend error %u: %s\n",
                reply.error().code(), reply.error().message().c_str());
            return false;
        }
        // ping/pong or anything else: keep reading.
    }
}

bool EdgeTransport::SendAll(const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        int n = send(static_cast<socket_t>(m_sock), data + sent,
            static_cast<int>(size - sent), 0);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool EdgeTransport::RecvAll(char *data, size_t size)
{
    size_t got = 0;
    while (got < size)
    {
        int n = recv(static_cast<socket_t>(m_sock), data + got,
            static_cast<int>(size - got), 0);
        if (n <= 0)
        {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool EdgeTransport::WriteFrameBytes(const std::string &frame)
{
    if (frame.size() > kMaxFrameBytes)
    {
        return false;
    }
    uint8_t hdr[4];
    uint32_t n = static_cast<uint32_t>(frame.size());
    hdr[0] = static_cast<uint8_t>(n & 0xff);
    hdr[1] = static_cast<uint8_t>((n >> 8) & 0xff);
    hdr[2] = static_cast<uint8_t>((n >> 16) & 0xff);
    hdr[3] = static_cast<uint8_t>((n >> 24) & 0xff);
    return SendAll(reinterpret_cast<const char *>(hdr), sizeof(hdr))
        && SendAll(frame.data(), frame.size());
}

bool EdgeTransport::ReadFrameBytes(std::string &frame)
{
    uint8_t hdr[4];
    if (!RecvAll(reinterpret_cast<char *>(hdr), sizeof(hdr)))
    {
        return false;
    }
    uint32_t n = static_cast<uint32_t>(hdr[0])
        | (static_cast<uint32_t>(hdr[1]) << 8)
        | (static_cast<uint32_t>(hdr[2]) << 16)
        | (static_cast<uint32_t>(hdr[3]) << 24);
    if (n > kMaxFrameBytes)
    {
        return false;
    }
    frame.resize(n);
    if (n == 0)
    {
        return true;
    }
    return RecvAll(&frame[0], n);
}
