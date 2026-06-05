#pragma once

#include <cstdint>
#include <string>
#include <vector>

// EdgeTransport is the client half of the EdgeTransport wire (see
// gc_contracts/docs/WIRE_PROTOCOL.md): it connects the in-process shim to the
// gamecoordinator backend over TCP, performs the Hello handshake, and exchanges
// length-prefixed EdgeFrame records.
//
// Phase 1 is intentionally minimal: plaintext TCP and a synchronous
// request/reply call used for the matchmaking Hello. TLS and asynchronous
// server-pushed messages come in later phases.
class EdgeTransport
{
public:
    EdgeTransport() = default;
    ~EdgeTransport();

    EdgeTransport(const EdgeTransport &) = delete;
    EdgeTransport &operator=(const EdgeTransport &) = delete;

    // Connect to host:port and run the Hello handshake. role is the
    // csgogo::edge::EdgeRole value (1 = client, 2 = dedicated). Returns true only
    // if the gateway accepted the Hello.
    bool Connect(const std::string &host, uint16_t port, uint32_t role,
        uint64_t steamId, const std::vector<uint8_t> &authTicket);

    bool Connected() const { return m_connected; }

    // Send one GC message and block for the single GC reply. payload and
    // outPayload are bare serialized Valve protobuf bodies (no GC framing).
    // Returns false on any transport/protocol error.
    bool Request(uint32_t msgType, uint64_t jobId, uint64_t steamId,
        const std::string &payload, uint32_t &outMsgType, std::string &outPayload);

    void Close();

private:
    bool SendAll(const char *data, size_t size);
    bool RecvAll(char *data, size_t size);
    bool WriteFrameBytes(const std::string &frame);
    bool ReadFrameBytes(std::string &frame);

    intptr_t m_sock{ -1 };
    bool m_connected{ false };
};
