#include <iostream>
#include <windows.h>
#include <thread>
#include "../Shared/Protocol.h"
#include "../Core/Network/TcpNetwork.h"

using namespace gupt;

bool g_IsConnected = false;

int main() {
    std::cout << "Starting Gupt Client Viewer..." << std::endl;

    gupt::core::network::TcpClient client;

    client.SetMessageCallback([&](shared::MessageType type, const std::vector<uint8_t>& payload) {
        if (type == shared::MessageType::ConnectResponse) {
            auto res = reinterpret_cast<const shared::ConnectResponse*>(payload.data());
            if (res->accepted) {
                std::cout << "Connected securely to Host!" << std::endl;
                g_IsConnected = true;
            } else {
                std::cout << "Connection rejected: " << res->reason << std::endl;
            }
        } else if (type == shared::MessageType::FrameData) {
            auto header = reinterpret_cast<const shared::FrameDataHeader*>(payload.data());
            // Here we would copy the pixel data to a D3D/OpenGL surface and render
            // size_t offset = sizeof(shared::FrameDataHeader);
            // Decode/Render Frame ...
        }
    });

    std::cout << "Connecting to localhost..." << std::endl;
    if (client.Connect("127.0.0.1", 8080)) {
        // Build connect request
        shared::ConnectRequest req;
        std::strncpy(req.sessionId, "DEMO_SESSION_123", sizeof(req.sessionId));
        std::strncpy(req.authenticationToken, "SECURE_TOKEN", sizeof(req.authenticationToken));

        client.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectRequest, req));

        // Wait to be accepted
        while (true) {
            if (g_IsConnected) {
                // Send dummy input for MVP test
                shared::MouseEvent me = {0.5f, 0.5f, 0, false, 0};
                client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent, me));
            }
            Sleep(100);
        }
    } else {
        std::cerr << "Failed to connect." << std::endl;
    }

    client.Disconnect();
    return 0;
}
