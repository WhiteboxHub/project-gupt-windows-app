#include "StunClient.h"
#include <iostream>
#include <vector>
#include <random>
#include <chrono>

namespace gupt {
namespace core {
namespace network {

namespace {

constexpr uint16_t STUN_BINDING_REQUEST = 0x0001;
constexpr uint16_t STUN_BINDING_RESPONSE = 0x0101;
constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442;
constexpr uint16_t ATTR_MAPPED_ADDRESS = 0x0001;
constexpr uint16_t ATTR_XOR_MAPPED_ADDRESS = 0x0020;

#pragma pack(push, 1)
struct StunHeader {
    uint16_t msgType;
    uint16_t msgLength;
    uint32_t magicCookie;
    uint8_t transactionId[12];
};
#pragma pack(pop)

std::string GetLocalIpForSocket(SOCKET s, const sockaddr_in& remoteAddr) {
    SOCKET probeSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probeSock != INVALID_SOCKET) {
        if (connect(probeSock, (const sockaddr*)&remoteAddr, sizeof(remoteAddr)) != SOCKET_ERROR) {
            sockaddr_in localAddr{};
            int addrLen = sizeof(localAddr);
            if (getsockname(probeSock, (sockaddr*)&localAddr, &addrLen) == 0) {
                char ipBuf[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &localAddr.sin_addr, ipBuf, sizeof(ipBuf));
                closesocket(probeSock);
                return std::string(ipBuf);
            }
        }
        closesocket(probeSock);
    }
    return "127.0.0.1";
}

} // namespace

SOCKET StunClient::CreateBoundUdpSocket(uint16_t preferredPort) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(preferredPort);

    if (bind(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        if (preferredPort != 0) {
            addr.sin_port = 0;
            if (bind(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
                closesocket(s);
                return INVALID_SOCKET;
            }
        } else {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    return s;
}

bool StunClient::QuerySingleStunServer(SOCKET udpSocket, const std::string& host, uint16_t port,
                                      int timeoutMs, NatEndpoint& outEndpoint) {
    // 1. Resolve host
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || !result) {
        return false;
    }

    sockaddr_in remoteAddr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);

    // 2. Build RFC 5389 Binding Request
    StunHeader req{};
    req.msgType = htons(STUN_BINDING_REQUEST);
    req.msgLength = 0;
    req.magicCookie = htonl(STUN_MAGIC_COOKIE);

    // Generate 12-byte cryptographically-decent Transaction ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    uint32_t* tidPtr = reinterpret_cast<uint32_t*>(req.transactionId);
    tidPtr[0] = dis(gen);
    tidPtr[1] = dis(gen);
    tidPtr[2] = dis(gen);

    // 3. Send request
    int bytesSent = sendto(udpSocket, reinterpret_cast<const char*>(&req), sizeof(req), 0,
                           reinterpret_cast<const sockaddr*>(&remoteAddr), sizeof(remoteAddr));
    if (bytesSent != sizeof(req)) {
        return false;
    }

    // 4. Configure socket receive timeout
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    // 5. Read response (retry on stray packets)
    auto startTime = std::chrono::steady_clock::now();
    uint8_t buffer[1024];

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            return false;
        }

        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);
        int bytesRecv = recvfrom(udpSocket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                 reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

        if (bytesRecv < static_cast<int>(sizeof(StunHeader))) {
            return false; // Socket error or timeout
        }

        const StunHeader* resp = reinterpret_cast<const StunHeader*>(buffer);
        if (ntohs(resp->msgType) != STUN_BINDING_RESPONSE) {
            continue;
        }

        if (ntohl(resp->magicCookie) != STUN_MAGIC_COOKIE) {
            continue;
        }

        if (memcmp(resp->transactionId, req.transactionId, 12) != 0) {
            continue;
        }

        // 6. Parse attributes
        int remaining = bytesRecv - sizeof(StunHeader);
        const uint8_t* ptr = buffer + sizeof(StunHeader);

        std::string resolvedPublicIp;
        uint16_t resolvedPublicPort = 0;
        bool foundXor = false;

        while (remaining >= 4) {
            uint16_t attrType = (ptr[0] << 8) | ptr[1];
            uint16_t attrLen = (ptr[2] << 8) | ptr[3];
            ptr += 4;
            remaining -= 4;

            if (remaining < attrLen) {
                break;
            }

            if (attrType == ATTR_XOR_MAPPED_ADDRESS && attrLen >= 8) {
                uint8_t family = ptr[1];
                if (family == 0x01) { // IPv4
                    uint16_t xorPort = (ptr[2] << 8) | ptr[3];
                    resolvedPublicPort = xorPort ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);

                    uint32_t xorIp = (static_cast<uint32_t>(ptr[4]) << 24) |
                                     (static_cast<uint32_t>(ptr[5]) << 16) |
                                     (static_cast<uint32_t>(ptr[6]) << 8)  |
                                     static_cast<uint32_t>(ptr[7]);
                    uint32_t realIp = xorIp ^ STUN_MAGIC_COOKIE;

                    in_addr inAddr;
                    inAddr.s_addr = htonl(realIp);
                    char ipStr[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &inAddr, ipStr, sizeof(ipStr));
                    resolvedPublicIp = ipStr;
                    foundXor = true;
                    break;
                }
            } else if (attrType == ATTR_MAPPED_ADDRESS && !foundXor && attrLen >= 8) {
                uint8_t family = ptr[1];
                if (family == 0x01) { // IPv4
                    resolvedPublicPort = (ptr[2] << 8) | ptr[3];
                    in_addr inAddr;
                    memcpy(&inAddr, ptr + 4, 4);
                    char ipStr[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &inAddr, ipStr, sizeof(ipStr));
                    resolvedPublicIp = ipStr;
                }
            }

            // Align to 4-byte boundary
            int padding = (4 - (attrLen % 4)) % 4;
            ptr += attrLen + padding;
            remaining -= (attrLen + padding);
        }

        if (!resolvedPublicIp.empty() && resolvedPublicPort != 0) {
            outEndpoint.publicIp = resolvedPublicIp;
            outEndpoint.publicPort = resolvedPublicPort;

            // Determine bound local port
            sockaddr_in localAddr{};
            int localLen = sizeof(localAddr);
            if (getsockname(udpSocket, reinterpret_cast<sockaddr*>(&localAddr), &localLen) == 0) {
                outEndpoint.localPort = ntohs(localAddr.sin_port);
            }

            outEndpoint.localIp = GetLocalIpForSocket(udpSocket, remoteAddr);
            outEndpoint.isDiscovered = true;
            return true;
        }
    }

    return false;
}

bool StunClient::Discover(SOCKET udpSocket, NatEndpoint& outEndpoint,
                         const std::string& primaryStun, uint16_t primaryPort,
                         const std::string& backupStun, uint16_t backupPort,
                         int timeoutMs) {
    // Allow dynamic zero-hardcoding STUN server overrides via GUPT_STUN_SERVERS ("host:port,host2:port")
    char envBuf[512] = {0};
    DWORD len = GetEnvironmentVariableA("GUPT_STUN_SERVERS", envBuf, sizeof(envBuf));
    if (len > 0 && len < sizeof(envBuf)) {
        std::string list(envBuf);
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find_first_of(",; ", start);
            if (end == std::string::npos) end = list.size();
            std::string entry = list.substr(start, end - start);
            start = end + 1;
            if (entry.empty()) continue;

            std::string host = entry;
            uint16_t port = primaryPort;
            size_t colon = entry.rfind(':');
            if (colon != std::string::npos && colon > 0 && colon + 1 < entry.size()) {
                int parsed = atoi(entry.substr(colon + 1).c_str());
                if (parsed > 0 && parsed <= 65535) {
                    host = entry.substr(0, colon);
                    port = static_cast<uint16_t>(parsed);
                }
            }
            if (QuerySingleStunServer(udpSocket, host, port, timeoutMs, outEndpoint)) {
                return true;
            }
        }
    }

    // Fallback to caller-provided primary & backup STUN servers
    if (QuerySingleStunServer(udpSocket, primaryStun, primaryPort, timeoutMs, outEndpoint)) {
        return true;
    }
    if (QuerySingleStunServer(udpSocket, backupStun, backupPort, timeoutMs, outEndpoint)) {
        return true;
    }

    return false;
}

} // namespace network
} // namespace core
} // namespace gupt