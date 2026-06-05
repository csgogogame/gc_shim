#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// EdgeTransport is the client half of the EdgeTransport wire (see
// gc_contracts/docs/WIRE_PROTOCOL.md): it connects the in-process shim to the
// gamecoordinator backend over (optionally TLS) TCP, performs the Hello
// handshake, and exchanges length-prefixed EdgeFrame records.
//
// It is full duplex. After Connect, a background receive loop reads frames:
// replies are matched to in-flight Request() calls by job id, and unsolicited
// server pushes (queue updates, reservations) are delivered to the push handler.
class EdgeTransport
{
public:
    EdgeTransport() = default;
    ~EdgeTransport();

    EdgeTransport(const EdgeTransport &) = delete;
    EdgeTransport &operator=(const EdgeTransport &) = delete;

    // Connect to host:port, run the Hello handshake, and start the receive loop.
    // role is the csgogo::edge::EdgeRole value (1 = client, 2 = dedicated).
    bool Connect(const std::string &host, uint16_t port, uint32_t role,
        uint64_t steamId, const std::vector<uint8_t> &authTicket);

    bool Connected() const { return m_connected; }

    // Handler for unsolicited GC messages pushed by the backend. Called on the
    // receive thread with (msgType, bare protobuf body). Set before Connect.
    void SetPushHandler(std::function<void(uint32_t, const std::string &)> handler)
    {
        m_onPush = std::move(handler);
    }

    // Fire-and-forget: forward a GC message to the backend, no reply awaited.
    bool Send(uint32_t msgType, uint64_t steamId, const std::string &payload);

    // Send one GC message and block for its correlated reply. payload and
    // outPayload are bare serialized Valve protobuf bodies (no GC framing).
    bool Request(uint32_t msgType, uint64_t steamId, const std::string &payload,
        uint32_t &outMsgType, std::string &outPayload);

    void Close();

private:
    bool SendAll(const char *data, size_t size);
    bool RecvAll(char *data, size_t size);
    bool WriteFrameBytes(const std::string &frame);
    bool ReadFrameBytes(std::string &frame);
    bool SendEnvelope(uint32_t msgType, uint64_t jobId, uint64_t steamId, const std::string &payload);
    void ReceiveLoop();

    // Wraps the connected socket in TLS. Enabled by CSGOGC_BACKEND_TLS; verifies
    // against CSGOGC_BACKEND_CACERT unless CSGOGC_BACKEND_TLS_INSECURE is set.
    bool TlsHandshake(const std::string &host);

    intptr_t m_sock{ -1 };
    std::atomic<bool> m_connected{ false };

    struct TlsState; // opaque mbedTLS state, defined in the .cpp
    TlsState *m_tls{ nullptr };

    // Full-duplex machinery.
    std::function<void(uint32_t, const std::string &)> m_onPush;
    std::thread m_recvThread;
    std::atomic<bool> m_running{ false };
    std::mutex m_sendMutex;

    struct Pending
    {
        bool done{ false };
        bool failed{ false };
        uint32_t type{ 0 };
        std::string payload;
    };
    std::mutex m_pendingMutex;
    std::condition_variable m_pendingCv;
    std::unordered_map<uint64_t, std::shared_ptr<Pending>> m_pending;
    std::atomic<uint64_t> m_nextJobId{ 1 };
};
