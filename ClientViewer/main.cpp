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

bool g_SidebarActive = false;
int g_SidebarOffset = 0;
bool g_IsFullscreen = true;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            SetTimer(hWnd, 1, 16, NULL);
            break;
        case WM_TIMER:
            if (wParam == 1) {
                bool changed = false;
                if (g_SidebarActive && g_SidebarOffset < 220) {
                    g_SidebarOffset += 20;
                    if (g_SidebarOffset > 220) g_SidebarOffset = 220;
                    changed = true;
                } else if (!g_SidebarActive && g_SidebarOffset > 0) {
                    g_SidebarOffset -= 20;
                    if (g_SidebarOffset < 0) g_SidebarOffset = 0;
                    changed = true;
                }
                if (changed) InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            
            RECT rect;
            GetClientRect(hWnd, &rect);
            int clientW = rect.right - rect.left;
            int clientH = rect.bottom - rect.top;

            {
                std::lock_guard<std::mutex> lock(g_FrameMutex);
                if (!g_LatestFrame.empty() && g_FrameWidth > 0 && g_FrameHeight > 0) {
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
            }

            if (g_SidebarOffset > 0) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hbmMem = CreateCompatibleBitmap(hdc, g_SidebarOffset, clientH);
                HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

                RECT sbRect = { 0, 0, g_SidebarOffset, clientH };
                HBRUSH darkBrush = CreateSolidBrush(RGB(30, 30, 30));
                FillRect(hdcMem, &sbRect, darkBrush);

                int startY = (clientH - 250) / 2;
                SetBkMode(hdcMem, TRANSPARENT);
                SetTextColor(hdcMem, RGB(255, 255, 255));
                HFONT hFont = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

                const char* btnTexts[] = { "Disconnect", "Fullscreen", "Exit Fullscreen", "Send Ctrl+Alt+Del" };
                for (int i = 0; i < 4; ++i) {
                    RECT btnRect = { 10, startY + i * 60, 210, startY + 40 + i * 60 };
                    HBRUSH btnBrush = CreateSolidBrush(RGB(70, 70, 70));
                    FillRect(hdcMem, &btnRect, btnBrush);
                    DeleteObject(btnBrush);
                    DrawTextA(hdcMem, btnTexts[i], -1, &btnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                BLENDFUNCTION bf = {};
                bf.BlendOp = AC_SRC_OVER;
                bf.SourceConstantAlpha = 220;
                
                AlphaBlend(hdc, 0, 0, g_SidebarOffset, clientH,
                           hdcMem, 0, 0, g_SidebarOffset, clientH, bf);

                SelectObject(hdcMem, hOldFont);
                DeleteObject(hFont);
                DeleteObject(darkBrush);
                SelectObject(hdcMem, hbmOld);
                DeleteObject(hbmMem);
                DeleteDC(hdcMem);
            }

            EndPaint(hWnd, &ps);
            break;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            int x, y;
            if (message == WM_MOUSEWHEEL) {
                POINT pt;
                pt.x = (short)LOWORD(lParam);
                pt.y = (short)HIWORD(lParam);
                ScreenToClient(hWnd, &pt);
                x = pt.x;
                y = pt.y;
            } else {
                x = (short)LOWORD(lParam);
                y = (short)HIWORD(lParam);
            }

            if (message == WM_MOUSEMOVE) {
                if (x < 5 && !g_SidebarActive) {
                    g_SidebarActive = true;
                } else if (x > 220 && g_SidebarActive) {
                    g_SidebarActive = false;
                }
            }

            if (g_SidebarActive) {
                if (message == WM_LBUTTONDOWN && g_SidebarOffset > 0 && x <= g_SidebarOffset) {
                    RECT rect;
                    GetClientRect(hWnd, &rect);
                    int clientH = rect.bottom - rect.top;
                    int startY = (clientH - 250) / 2;
                    
                    if (y >= startY && y <= startY + 40) {
                        g_Client.Disconnect();
                        PostQuitMessage(0);
                    } else if (y >= startY + 60 && y <= startY + 100) {
                        g_IsFullscreen = true;
                        int screenW = GetSystemMetrics(SM_CXSCREEN);
                        int screenH = GetSystemMetrics(SM_CYSCREEN);
                        SetWindowPos(hWnd, HWND_TOP, 0, 0, screenW, screenH, SWP_SHOWWINDOW);
                    } else if (y >= startY + 120 && y <= startY + 160) {
                        g_IsFullscreen = false;
                        int screenW = GetSystemMetrics(SM_CXSCREEN);
                        int screenH = GetSystemMetrics(SM_CYSCREEN);
                        SetWindowPos(hWnd, HWND_TOP, (screenW - 800) / 2, (screenH - 600) / 2, 800, 600, SWP_SHOWWINDOW);
                    } else if (y >= startY + 180 && y <= startY + 220) {
                        if (g_IsConnected) {
                            shared::KeyboardEvent ke = {};
                            ke.virtualKey = VK_CONTROL; ke.isDown = true; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                            ke.virtualKey = VK_MENU; ke.isDown = true; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                            ke.virtualKey = VK_DELETE; ke.isDown = true; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                            ke.virtualKey = VK_DELETE; ke.isDown = false; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                            ke.virtualKey = VK_MENU; ke.isDown = false; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                            ke.virtualKey = VK_CONTROL; ke.isDown = false; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                        }
                    }
                }
                return 0; // absorb all mouse events while sidebar is active
            }

            if (g_IsConnected) {
                shared::MouseEvent me = {};
                
                RECT rect;
                GetClientRect(hWnd, &rect);
                int clientW = rect.right - rect.left;
                int clientH = rect.bottom - rect.top;
                if (clientW == 0 || clientH == 0) break;

                me.normalizedX = static_cast<float>(x) / clientW;
                me.normalizedY = static_cast<float>(y) / clientH;
                
                if (message == WM_MOUSEWHEEL) me.wheelDelta = (short)HIWORD(wParam);
                else me.wheelDelta = 0;

                if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) me.buttonId = 0;
                else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) me.buttonId = 1;
                else me.buttonId = 255;

                me.isDown = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN);
                
                g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent, me));
            }
            break;
        }
        case WM_KEYDOWN:
        case WM_KEYUP: {
            if (wParam == VK_F11 && message == WM_KEYDOWN) {
                g_IsFullscreen = !g_IsFullscreen;
                if (g_IsFullscreen) {
                    int screenW = GetSystemMetrics(SM_CXSCREEN);
                    int screenH = GetSystemMetrics(SM_CYSCREEN);
                    SetWindowPos(hWnd, HWND_TOP, 0, 0, screenW, screenH, SWP_SHOWWINDOW);
                } else {
                    int screenW = GetSystemMetrics(SM_CXSCREEN);
                    int screenH = GetSystemMetrics(SM_CYSCREEN);
                    SetWindowPos(hWnd, HWND_TOP, (screenW - 800) / 2, (screenH - 600) / 2, 800, 600, SWP_SHOWWINDOW);
                }
                return 0;
            }

            if (g_SidebarActive) return 0; // absorb all keyboard events while sidebar is active

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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    std::cout << "Starting Gupt Client Viewer..." << std::endl;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "GuptClientClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hWnd = CreateWindowExA(WS_EX_TOOLWINDOW, "GuptClientClass", "",
                              WS_POPUP, 0, 0, screenW, screenH,
                              NULL, NULL, hInstance, NULL);
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
