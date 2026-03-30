#include <iostream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <vector>
#include <mutex>
#include "../Shared/Protocol.h"
#include "../Core/Network/TcpNetwork.h"

using namespace gupt;

bool g_IsConnected = false;
gupt::core::network::TcpClient g_Client;

std::mutex g_FrameMutex;
std::vector<uint8_t> g_LatestFrame;
uint32_t g_FrameWidth = 0;
uint32_t g_FrameHeight = 0;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            
            std::lock_guard<std::mutex> lock(g_FrameMutex);
            if (!g_LatestFrame.empty() && g_FrameWidth > 0 && g_FrameHeight > 0) {
                RECT rect;
                GetClientRect(hWnd, &rect);
                int clientW = rect.right - rect.left;
                int clientH = rect.bottom - rect.top;

                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = g_FrameWidth;
                bmi.bmiHeader.biHeight = -static_cast<int>(g_FrameHeight); // Top-down
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                SetStretchBltMode(hdc, HALFTONE);
                StretchDIBits(hdc, 0, 0, clientW, clientH,
                              0, 0, g_FrameWidth, g_FrameHeight,
                              g_LatestFrame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            }
            EndPaint(hWnd, &ps);
            break;
        }
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            if (g_IsConnected) {
                shared::MouseEvent me = {};
                
                RECT rect;
                GetClientRect(hWnd, &rect);
                int clientW = rect.right - rect.left;
                int clientH = rect.bottom - rect.top;
                if (clientW == 0 || clientH == 0) break;

                int x = LOWORD(lParam);
                int y = HIWORD(lParam);

                me.normalizedX = static_cast<float>(x) / clientW;
                me.normalizedY = static_cast<float>(y) / clientH;
                me.wheelDelta = 0;
                
                if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) me.buttonId = 0;
                else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) me.buttonId = 1;

                me.isDown = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN);
                
                g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent, me));
            }
            break;
        }
        case WM_KEYDOWN:
        case WM_KEYUP: {
            if (g_IsConnected) {
                shared::KeyboardEvent ke = {};
                ke.virtualKey = wParam;
                ke.isDown = (message == WM_KEYDOWN);
                g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int main() {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    int nCmdShow = SW_SHOW;

    // By keeping it as a console app, we don't need to AllocConsole() but we'll leave it 
    // to preserve cout formatting (or just let the default console handle it).
    
    std::cout << "Starting Gupt Client Viewer..." << std::endl;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "GuptClientClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hWnd = CreateWindowA("GuptClientClass", "Gupt Remote Desktop Client",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1280, 720, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    g_Client.SetMessageCallback([hWnd](shared::MessageType type, const std::vector<uint8_t>& payload) {
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
            size_t offset = sizeof(shared::FrameDataHeader);
            
            if (payload.size() >= offset + (header->width * header->height * 4)) {
                std::lock_guard<std::mutex> lock(g_FrameMutex);
                g_FrameWidth = header->width;
                g_FrameHeight = header->height;
                g_LatestFrame.assign(payload.begin() + offset, payload.end());
                
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
    });

    std::cout << "Connecting to localhost..." << std::endl;
    if (g_Client.Connect("127.0.0.1", 8080)) {
        shared::ConnectRequest req = {};
        std::strncpy(req.sessionId, "DEMO_SESSION_123", sizeof(req.sessionId));
        std::strncpy(req.authenticationToken, "SECURE_TOKEN", sizeof(req.authenticationToken));
        g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectRequest, req));
    } else {
        std::cerr << "Failed to connect." << std::endl;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_Client.Disconnect();
    return static_cast<int>(msg.wParam);
}
