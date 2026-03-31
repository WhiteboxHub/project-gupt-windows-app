#include <iostream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <atomic>
#include "../Shared/Protocol.h"
#include "../Core/Network/TcpNetwork.h"
#include "../Core/Input/InputInjector.h"
#include "../Core/Capture/ScreenCapturer.h"

using namespace gupt;

std::atomic<bool> g_SessionActive{false};

// Request User Consent
// As per constraints: "The system must require user awareness and consent"
bool RequestConsent(const std::string& peerIp) {
    std::string msg = "A remote user at " + peerIp + " is requesting to view and control your desktop.\n\nDo you grant permission?";
    int result = MessageBoxA(NULL, msg.c_str(), "Gupt Remote Desktop - Incoming Connection", MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
    return result == IDYES;
}

// Visible session indicator
void ShowSessionIndicatorThread() {
    // Shows a small persistent, non-clickable borderless window tracking active session
    // For MVP phase, we just log and flash a console message.
    while (g_SessionActive) {
        std::cout << "[SECURITY] REMOTE SESSION IS ACTIVE!" << std::endl;
        Sleep(5000);
    }
}

int main() {
    std::cout << "Starting Gupt Host Agent..." << std::endl;

    gupt::core::network::TcpServer server(8080);
    gupt::core::input::InputInjector injector;
    gupt::core::capture::ScreenCapturer capturer;
    
    injector.Initialize();
    capturer.Initialize();

    server.SetMessageCallback([&](shared::MessageType type, const std::vector<uint8_t>& payload) {
        if (type == shared::MessageType::ConnectRequest) {
            auto req = reinterpret_cast<const shared::ConnectRequest*>(payload.data());
            std::cout << "Received connection request. Authenticating..." << std::endl;

            // Security constraint: Must prompt user for awareness
            if (RequestConsent("Remote Peer")) {
                shared::ConnectResponse res{true, "Welcome"};
                server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
                
                g_SessionActive = true;
                std::thread indicatorThread(ShowSessionIndicatorThread);
                indicatorThread.detach();

                std::cout << "Session accepted." << std::endl;
            } else {
                shared::ConnectResponse res{false, "User Denied Permission"};
                server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
                std::cout << "Session rejected." << std::endl;
            }
        } else if (g_SessionActive) {
            if (type == shared::MessageType::MouseEvent) {
                auto ev = reinterpret_cast<const shared::MouseEvent*>(payload.data());
                injector.IngestMouseEvent(*ev);
            } else if (type == shared::MessageType::KeyboardEvent) {
                auto ev = reinterpret_cast<const shared::KeyboardEvent*>(payload.data());
                injector.IngestKeyboardEvent(*ev);
            } else if (type == shared::MessageType::Disconnect) {
                std::cout << "Client disconnected." << std::endl;
                g_SessionActive = false;
            }
        }
    });

    if (server.Start()) {
        std::cout << "Listening on port 8080..." << std::endl;
        
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

        // MVP: Send frames loop
        ULONGLONG lastSendDuration = 0;
        while (true) {
            if (g_SessionActive) {
                ULONGLONG startTime = GetTickCount64();
                
                std::vector<uint8_t> pixels;
                uint32_t w, h;
                
                if (capturer.CaptureNextFrameJpeg(pixels, w, h, 40)) {
                    if (lastSendDuration > 66) {
                        // Skip this frame to prevent queue buildup
                        lastSendDuration = 0; 
                    } else {
                        // delta encoding is a future improvement
                        shared::FrameDataHeader header{0, w, h, 32, false, GetTickCount64()};
                        ULONGLONG startSend = GetTickCount64();
                        server.SendRaw(shared::SerializeFrame(header, pixels));
                        lastSendDuration = GetTickCount64() - startSend;
                    }
                }
                
                ULONGLONG elapsed = GetTickCount64() - startTime;
                if (elapsed < 33) {
                    Sleep(static_cast<DWORD>(33 - elapsed));
                }
            } else {
                lastSendDuration = 0;
                Sleep(100);
            }
        }
    }

    server.Stop();
    return 0;
}
