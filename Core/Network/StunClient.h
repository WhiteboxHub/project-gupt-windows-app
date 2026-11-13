#pragma once
#include <string>
#include <cstdint>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace gupt {
namespace core {
namespace network {

struct NatEndpoint {
    std::string localIp;
    uint16_t localPort = 0;
    std::string publicIp;
    uint16_t publicPort = 0;
    bool isDiscovered = false;

    std::string ToString() const {
        return publicIp + ":" + std::to_string(publicPort) + " (LAN: " + localIp + ":" + std::to_string(localPort) + ")";
    }
};

class StunClient {
public:
    // Resolves public NAT IP and mapped port for the given UDP socket using RFC 5389 STUN.
    // Automatically attempts primary STUN host (Google) and fails over to secondary (Cloudflare).
    static bool Discover(SOCKET udpSocket, NatEndpoint& outEndpoint,
                         const std::string& primaryStun = "stun.l.google.com",
                         uint16_t primaryPort = 19302,
                         const std::string& backupStun = "stun.cloudflare.com",
                         uint16_t backupPort = 3478,
                         int timeoutMs = 1200);

    // Creates and binds a non-blocking UDP socket suitable for STUN and P2P communication
    static SOCKET CreateBoundUdpSocket(uint16_t preferredPort = 0);

private:
    static bool QuerySingleStunServer(SOCKET udpSocket, const std::string& host, uint16_t port, 
                                      int timeoutMs, NatEndpoint& outEndpoint);
};

} // namespace network
} // namespace core
} // namespace gupt
