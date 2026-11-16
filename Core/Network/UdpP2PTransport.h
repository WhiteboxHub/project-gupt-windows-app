#pragma once
#include "StunClient.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <chrono>
#include <functional>

namespace gupt {
namespace core {
namespace network {

enum class P2PState {
    Idle,
    Discovering,
    Punching,
    Connected,
    Failed,
    Closed
};

#pragma pack(push, 1)
struct UdpPacketHeader {
    uint32_t magic;         // 0x47555054 ("GUPT")
    uint8_t  packetType;    // 0: ProbePing, 1: ProbePong, 2: Heartbeat, 3: Data
    uint32_t packetSeq;     // Rolling sequence number
    uint16_t fragIndex;     // Fragment index (0-based)
    uint16_t totalFrags;    // Total fragments in this logical message
    uint32_t totalSize;     // Total size of unfragmented payload
};
#pragma pack(pop)

class UdpP2PTransport {
public:
    using MessageCallback = std::function<void(const std::vector<uint8_t>& data)>;

    UdpP2PTransport();
    ~UdpP2PTransport();

    // Initializes UDP socket and discovers NAT endpoint via STUN
    bool Initialize(uint16_t preferredPort = 0);

    // Returns discovered local & public endpoint
    const NatEndpoint& GetEndpoint() const { return m_myEndpoint; }

    // Initiates bidirectional NAT hole-punching to peer endpoint
    // Returns true if bidirectional P2P connectivity is verified within timeoutMs
    bool PunchHole(const NatEndpoint& peer, int timeoutMs = 8000);

    // Send payload (automatically fragments if > MTU)
    bool Send(const void* data, size_t len);

    // Set callback for incoming reassembled messages
    void SetMessageCallback(MessageCallback cb) { m_onMessage = cb; }

    // Check if P2P transport is currently connected
    bool IsConnected() const { return m_state == P2PState::Connected; }

    P2PState GetState() const { return m_state.load(); }

    uint64_t GetLastPeerActivityTick() const { return m_lastPeerActivityTick.load(); }

    void StartKeepAliveIfIdle();

    void Close();

private:
    void ReceiveLoop();
    void KeepAliveLoop();
    bool SendFragment(uint8_t type, uint32_t seq, uint16_t fragIdx, uint16_t totalFrags,
                     uint32_t totalSize, const void* payload, size_t payloadLen,
                     const sockaddr_in& destAddr);

    SOCKET m_socket = INVALID_SOCKET;
    NatEndpoint m_myEndpoint;
    NatEndpoint m_peerEndpoint;
    sockaddr_in m_activeRemoteAddr{};
    bool m_hasActiveRemote = false;

    std::atomic<P2PState> m_state{P2PState::Idle};
    std::atomic<bool> m_running{false};
    std::atomic<uint32_t> m_nextSeq{1};
    std::atomic<uint64_t> m_lastPeerActivityTick{0};

    std::thread m_recvThread;
    std::thread m_keepAliveThread;

    std::mutex m_sendMtx;
    std::mutex m_stateMtx;
    MessageCallback m_onMessage;

    // Multi-fragment reassembly state
    struct IncompleteMessage {
        uint32_t packetSeq;
        uint16_t totalFrags;
        uint32_t totalSize;
        std::chrono::steady_clock::time_point startTime;
        std::vector<uint8_t> buffer;
        std::vector<bool> receivedFrags;
        uint16_t fragsReceivedCount = 0;
    };
    std::mutex m_reassemblyMtx;
    std::unordered_map<uint32_t, IncompleteMessage> m_reassemblyMap;
};

} // namespace network
} // namespace core
} // namespace gupt