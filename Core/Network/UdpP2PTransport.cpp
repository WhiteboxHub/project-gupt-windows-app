#include "UdpP2PTransport.h"
#include <iostream>
#include <chrono>

namespace gupt {
namespace core {
namespace network {

constexpr uint32_t UDP_MAGIC = 0x47555054; // "GUPT"
constexpr uint8_t PKT_PROBE_PING = 0;
constexpr uint8_t PKT_PROBE_PONG = 1;
constexpr uint8_t PKT_HEARTBEAT  = 2;
constexpr uint8_t PKT_DATA       = 3;
constexpr size_t MAX_PAYLOAD_CHUNK = 1180; // 1180 payload + 17 header < 1200 MTU

UdpP2PTransport::UdpP2PTransport() {
}

UdpP2PTransport::~UdpP2PTransport() {
    Close();
}

bool UdpP2PTransport::Initialize(uint16_t preferredPort) {
    Close();

    m_socket = StunClient::CreateBoundUdpSocket(preferredPort);
    if (m_socket == INVALID_SOCKET) {
        m_state = P2PState::Failed;
        return false;
    }

    m_state = P2PState::Discovering;
    if (!StunClient::Discover(m_socket, m_myEndpoint)) {
        // Fallback: Still usable locally if STUN unreachable
        sockaddr_in localAddr{};
        int len = sizeof(localAddr);
        if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&localAddr), &len) == 0) {
            m_myEndpoint.localPort = ntohs(localAddr.sin_port);
            m_myEndpoint.localIp = "127.0.0.1";
        }
    }

    // Set non-blocking or small timeout for socket
    DWORD tv = 50;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    // Expand socket buffers to 4MB to prevent drops during large full-frame bursts
    int bufSize = 4 * 1024 * 1024;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    m_running = true;
    m_recvThread = std::thread(&UdpP2PTransport::ReceiveLoop, this);
    m_state = P2PState::Idle;
    return true;
}

bool UdpP2PTransport::PunchHole(const NatEndpoint& peer, int timeoutMs) {
    // Wait briefly if STUN initialization is still finalizing
    for (int waitCount = 0; waitCount < 25 && (!m_running || m_socket == INVALID_SOCKET); ++waitCount) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    if (m_socket == INVALID_SOCKET || !m_running) {
        return false;
    }

    m_peerEndpoint = peer;
    m_state = P2PState::Punching;

    std::vector<sockaddr_in> candidateAddrs;

    // Public candidate and adjacent port prediction (traverses symmetric NAT, CGNAT, and port-restricted Wi-Fi routers)
    if (!peer.publicIp.empty() && peer.publicPort != 0) {
        for (int offset : { 0, 1, -1, 2, -2, 3, -3 }) {
            int targetPort = static_cast<int>(peer.publicPort) + offset;
            if (targetPort > 1024 && targetPort <= 65535) {
                sockaddr_in pubAddr{};
                pubAddr.sin_family = AF_INET;
                pubAddr.sin_port = htons(static_cast<uint16_t>(targetPort));
                inet_pton(AF_INET, peer.publicIp.c_str(), &pubAddr.sin_addr);
                candidateAddrs.push_back(pubAddr);
            }
        }
    }

    // LAN / local candidate
    if (!peer.localIp.empty() && peer.localPort != 0) {
        sockaddr_in lanAddr{};
        lanAddr.sin_family = AF_INET;
        lanAddr.sin_port = htons(peer.localPort);
        inet_pton(AF_INET, peer.localIp.c_str(), &lanAddr.sin_addr);
        candidateAddrs.push_back(lanAddr);
    }

    if (candidateAddrs.empty()) {
        m_state = P2PState::Failed;
        return false;
    }

    auto startTime = std::chrono::steady_clock::now();
    uint32_t probeSeq = 100;

    while (m_running && m_state != P2PState::Connected) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            break;
        }

        // Send rapid probe pings to both candidates
        for (const auto& target : candidateAddrs) {
            SendFragment(PKT_PROBE_PING, probeSeq++, 0, 1, 0, nullptr, 0, target);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    if (m_state == P2PState::Connected) {
        StartKeepAliveIfIdle();
        return true;
    }

    m_state = P2PState::Failed;
    return false;
}

bool UdpP2PTransport::SendFragment(uint8_t type, uint32_t seq, uint16_t fragIdx, uint16_t totalFrags,
                                  uint32_t totalSize, const void* payload, size_t payloadLen,
                                  const sockaddr_in& destAddr) {
    if (m_socket == INVALID_SOCKET) {
        return false;
    }

    std::vector<uint8_t> buffer(sizeof(UdpPacketHeader) + payloadLen);
    UdpPacketHeader* hdr = reinterpret_cast<UdpPacketHeader*>(buffer.data());
    hdr->magic = htonl(UDP_MAGIC);
    hdr->packetType = type;
    hdr->packetSeq = htonl(seq);
    hdr->fragIndex = htons(fragIdx);
    hdr->totalFrags = htons(totalFrags);
    hdr->totalSize = htonl(totalSize);

    if (payload && payloadLen > 0) {
        memcpy(buffer.data() + sizeof(UdpPacketHeader), payload, payloadLen);
    }

    std::lock_guard<std::mutex> lock(m_sendMtx);
    int sent = sendto(m_socket, reinterpret_cast<const char*>(buffer.data()),
                      static_cast<int>(buffer.size()), 0,
                      reinterpret_cast<const sockaddr*>(&destAddr), sizeof(destAddr));
    return sent == static_cast<int>(buffer.size());
}

bool UdpP2PTransport::Send(const void* data, size_t len) {
    if (m_state != P2PState::Connected || !m_hasActiveRemote) {
        return false;
    }

    uint32_t seq = m_nextSeq.fetch_add(1);
    const uint8_t* bytePtr = reinterpret_cast<const uint8_t*>(data);

    if (len <= MAX_PAYLOAD_CHUNK) {
        return SendFragment(PKT_DATA, seq, 0, 1, static_cast<uint32_t>(len),
                            data, len, m_activeRemoteAddr);
    }

    uint16_t totalFrags = static_cast<uint16_t>((len + MAX_PAYLOAD_CHUNK - 1) / MAX_PAYLOAD_CHUNK);
    size_t remaining = len;
    uint16_t fragIdx = 0;

    while (remaining > 0 && m_running) {
        size_t chunkSize = (remaining > MAX_PAYLOAD_CHUNK) ? MAX_PAYLOAD_CHUNK : remaining;
        if (!SendFragment(PKT_DATA, seq, fragIdx, totalFrags, static_cast<uint32_t>(len),
                          bytePtr + (fragIdx * MAX_PAYLOAD_CHUNK), chunkSize, m_activeRemoteAddr)) {
            return false;
        }
        remaining -= chunkSize;
        fragIdx++;

        // High-precision micro-pacing: when transmitting multi-fragment bursts (e.g. keyframes),
        // pace every 6 fragments with a ~40us yield to prevent bufferbloat and packet drops on Wi-Fi routers.
        if (totalFrags > 4 && (fragIdx % 6) == 0 && remaining > 0) {
            static LARGE_INTEGER s_freq = []{
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return f;
            }();
            LARGE_INTEGER start, cur;
            QueryPerformanceCounter(&start);
            int64_t targetTicks = (s_freq.QuadPart * 40) / 1000000;
            do {
                YieldProcessor();
                QueryPerformanceCounter(&cur);
            } while ((cur.QuadPart - start.QuadPart) < targetTicks);
        }
    }

    return true;
}

void UdpP2PTransport::ReceiveLoop() {
    uint8_t buffer[2048];

    while (m_running) {
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);
        int bytes = recvfrom(m_socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                             reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

        if (bytes < static_cast<int>(sizeof(UdpPacketHeader))) {
            continue; // Timeout or invalid size
        }

        const UdpPacketHeader* hdr = reinterpret_cast<const UdpPacketHeader*>(buffer);
        if (ntohl(hdr->magic) != UDP_MAGIC) {
            continue; // Not a Gupt P2P packet
        }

        uint8_t type = hdr->packetType;
        uint32_t seq = ntohl(hdr->packetSeq);
        uint16_t fragIdx = ntohs(hdr->fragIndex);
        uint16_t totalFrags = ntohs(hdr->totalFrags);
        uint32_t totalSize = ntohl(hdr->totalSize);
        size_t payloadLen = bytes - sizeof(UdpPacketHeader);
        const uint8_t* payloadPtr = buffer + sizeof(UdpPacketHeader);

        m_lastPeerActivityTick = GetTickCount64();

        if (type == PKT_PROBE_PING) {
            // Reply with Pong to the exact source address
            SendFragment(PKT_PROBE_PONG, seq, 0, 1, 0, nullptr, 0, fromAddr);

            {
                std::lock_guard<std::mutex> lock(m_stateMtx);
                m_activeRemoteAddr = fromAddr;
                m_hasActiveRemote = true;
                m_state = P2PState::Connected;
            }
            StartKeepAliveIfIdle();
        } else if (type == PKT_PROBE_PONG) {
            {
                std::lock_guard<std::mutex> lock(m_stateMtx);
                m_activeRemoteAddr = fromAddr;
                m_hasActiveRemote = true;
                m_state = P2PState::Connected;
            }
            StartKeepAliveIfIdle();
        } else if (type == PKT_HEARTBEAT) {
            // Keep-alive datagram received; activity tick already refreshed
        } else if (type == PKT_DATA) {
            if (totalFrags <= 1) {
                if (m_onMessage && payloadLen > 0) {
                    std::vector<uint8_t> msg(payloadPtr, payloadPtr + payloadLen);
                    m_onMessage(msg);
                }
            } else {
                // Multi-fragment packet reassembly
                std::vector<uint8_t> completedMsg;
                {
                    std::lock_guard<std::mutex> rLock(m_reassemblyMtx);

                    // Garbage-collect incomplete messages older than 1500ms (tolerates internet/Wi-Fi jitter)
                    auto now = std::chrono::steady_clock::now();
                    for (auto it = m_reassemblyMap.begin(); it != m_reassemblyMap.end(); ) {
                        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.startTime).count();
                        if (age > 1500) {
                            it = m_reassemblyMap.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    auto& slot = m_reassemblyMap[seq];
                    if (slot.buffer.empty()) {
                        slot.packetSeq = seq;
                        slot.totalFrags = totalFrags;
                        slot.totalSize = totalSize;
                        slot.startTime = now;
                        slot.buffer.resize(totalSize);
                        slot.receivedFrags.resize(totalFrags, false);
                    }

                    if (fragIdx < totalFrags && !slot.receivedFrags[fragIdx]) {
                        size_t offset = fragIdx * MAX_PAYLOAD_CHUNK;
                        if (offset + payloadLen <= slot.buffer.size()) {
                            memcpy(slot.buffer.data() + offset, payloadPtr, payloadLen);
                            slot.receivedFrags[fragIdx] = true;
                            slot.fragsReceivedCount++;

                            if (slot.fragsReceivedCount == slot.totalFrags) {
                                completedMsg = std::move(slot.buffer);
                                m_reassemblyMap.erase(seq);
                            }
                        }
                    }
                }

                if (!completedMsg.empty() && m_onMessage) {
                    m_onMessage(completedMsg);
                }
            }
        }
    }
}

void UdpP2PTransport::StartKeepAliveIfIdle() {
    std::lock_guard<std::mutex> lock(m_stateMtx);
    if (m_running && m_state == P2PState::Connected && !m_keepAliveThread.joinable()) {
        m_keepAliveThread = std::thread(&UdpP2PTransport::KeepAliveLoop, this);
    }
}

void UdpP2PTransport::KeepAliveLoop() {
    while (m_running && m_state == P2PState::Connected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (m_hasActiveRemote && m_running) {
            sockaddr_in dest{};
            {
                std::lock_guard<std::mutex> lock(m_stateMtx);
                dest = m_activeRemoteAddr;
            }
            SendFragment(PKT_HEARTBEAT, 0, 0, 1, 0, nullptr, 0, dest);
        }
    }
}

void UdpP2PTransport::Close() {
    m_running = false;
    m_state = P2PState::Closed;

    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }

    if (m_keepAliveThread.joinable()) {
        m_keepAliveThread.join();
    }

    std::lock_guard<std::mutex> rLock(m_reassemblyMtx);
    m_reassemblyMap.clear();
    m_hasActiveRemote = false;
}

} // namespace network
} // namespace core
} // namespace gupt