#include <iostream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <atomic>
#include <mutex>
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
    // Physical pixel coordinates: without this, GDI on a DPI-scaled host captures
    // at logical (scaled) resolution and mouse injection hits wrong positions.
    SetProcessDPIAware();

    std::cout << "Starting Gupt Host Agent..." << std::endl;

    gupt::core::network::TcpServer server(8080);
    gupt::core::input::InputInjector injector;
    gupt::core::capture::ScreenCapturer capturer;
    
    injector.Initialize();
    capturer.Initialize();

    // Echo-guard: tracks text the host clipboard just received FROM the client.
    // The clipboard watcher thread checks against this to avoid sending it back.
    std::string lastReceivedFromClient;
    std::mutex  clipMutex;

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
            } else if (type == shared::MessageType::ClipboardText) {
                if (!payload.empty()) {
                    std::string utf8(reinterpret_cast<const char*>(payload.data()), payload.size());
                    { std::lock_guard<std::mutex> lk(clipMutex); lastReceivedFromClient = utf8; }
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
                    if (wlen > 0) {
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(wchar_t));
                        if (hMem) {
                            wchar_t* dst = (wchar_t*)GlobalLock(hMem);
                            if (dst) { MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dst, wlen); GlobalUnlock(hMem);
                                if (OpenClipboard(NULL)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, hMem); CloseClipboard(); }
                                else { GlobalFree(hMem); }
                            } else { GlobalFree(hMem); }
                        }
                    }
                }
            } else if (type == shared::MessageType::ClipboardImage) {
                 // Client sent a "Copy Image" from Google Lens result (DIB data)
                if (!payload.empty()) {
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, payload.size());
                    if (hMem) {
                        void* dst = GlobalLock(hMem);
                        if (dst) {
                            std::memcpy(dst, payload.data(), payload.size());
                            GlobalUnlock(hMem);
                            if (OpenClipboard(NULL)) { EmptyClipboard(); SetClipboardData(CF_DIB, hMem); CloseClipboard(); }
                            else { GlobalFree(hMem); }
                        } else { GlobalFree(hMem); }
                    }
                }
            } else if (type == shared::MessageType::Disconnect) {
                std::cout << "Client disconnected." << std::endl;
                g_SessionActive = false;
            }
        }
    });

    if (server.Start()) {
        std::cout << "Listening on port 8080..." << std::endl;
        
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

        // --- Host clipboard watcher thread ---
        std::thread clipWatcher([&]() {
            std::string lastSentText;
            while (true) {
                Sleep(500);
                if (!g_SessionActive) { lastSentText.clear(); continue; }

                if (!OpenClipboard(NULL)) continue;
                
                // Monitor Text
                if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        wchar_t* wText = (wchar_t*)GlobalLock(hData);
                        if (wText) {
                            int len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, NULL, 0, NULL, NULL);
                            if (len > 1) {
                                std::string currentText(len - 1, '\0');
                                WideCharToMultiByte(CP_UTF8, 0, wText, -1, &currentText[0], len, NULL, NULL);
                                {
                                    std::lock_guard<std::mutex> lk(clipMutex);
                                    if (currentText != lastSentText && currentText != lastReceivedFromClient) {
                                        lastSentText = currentText;
                                        server.SendRaw(shared::SerializeClipboardText(currentText));
                                    }
                                }
                            }
                            GlobalUnlock(hData);
                        }
                    }
                }
                
                // Monitor Image (DIB format)
                static DWORD s_lastImageSequence = 0;
                DWORD currentSequence = GetClipboardSequenceNumber();
                if (currentSequence != s_lastImageSequence && IsClipboardFormatAvailable(CF_DIB)) {
                    HANDLE hData = GetClipboardData(CF_DIB);
                    if (hData) {
                        size_t size = GlobalSize(hData);
                        void* ptr = GlobalLock(hData);
                        if (ptr) {
                            std::vector<uint8_t> dib(size);
                            std::memcpy(dib.data(), ptr, size);
                            GlobalUnlock(hData);
                            server.SendRaw(shared::SerializeClipboardImage(dib));
                            s_lastImageSequence = currentSequence;
                        }
                    }
                }

                CloseClipboard();
            }
        });
        clipWatcher.detach();

        // MVP: Send frames loop
        ULONGLONG lastSendDuration = 0;
        while (true) {
            if (g_SessionActive) {
                if (lastSendDuration > 66) {
                    // Previous send was slow — skip this frame to drain stale queue
                    Sleep(33);
                    lastSendDuration = 0;
                    continue;
                }

                ULONGLONG startTime = GetTickCount64();

                std::vector<uint8_t> jpegPixels;
                uint32_t w, h;

                if (capturer.CaptureNextFrameJpeg(jpegPixels, w, h, 85)) {
                    shared::FrameDataHeader header{0, w, h, 32, false, GetTickCount64()};
                    ULONGLONG startSend = GetTickCount64();
                    server.SendRaw(shared::SerializeFrame(header, jpegPixels));
                    lastSendDuration = GetTickCount64() - startSend;
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
