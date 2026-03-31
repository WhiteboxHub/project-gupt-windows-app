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

bool g_SidebarOpen = false;
int g_SidebarOffsetY = -2000;
int g_HoveredButton = -1;
bool g_IsFullscreen = true;
RECT g_WindowedRect = {0, 0, 800, 600};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            SetTimer(hWnd, 1, 10, NULL);
            break;
        case WM_TIMER:
            if (wParam == 1) {
                RECT rect; GetClientRect(hWnd, &rect);
                int fullHeight = rect.bottom - rect.top;
                
                int targetY = g_SidebarOpen ? 0 : -fullHeight;
                if (g_SidebarOffsetY != targetY) {
                    if (g_SidebarOffsetY < targetY) g_SidebarOffsetY += (targetY - g_SidebarOffsetY) / 3 + 4;
                    if (g_SidebarOffsetY > targetY) g_SidebarOffsetY -= (g_SidebarOffsetY - targetY) / 3 + 4;
                    if (abs(g_SidebarOffsetY - targetY) < 5) g_SidebarOffsetY = targetY;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
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
                    bmi.bmiHeader.biHeight = -static_cast<int>(g_FrameHeight);
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;

                    SetStretchBltMode(hdc, HALFTONE);
                    StretchDIBits(hdc, 0, 0, clientW, clientH,
                                  0, 0, g_FrameWidth, g_FrameHeight,
                                  g_LatestFrame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                }
            }

            if (g_SidebarOffsetY > -clientH) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hbmMem = CreateCompatibleBitmap(hdc, 260, clientH);
                HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

                RECT sbRect = { 0, 0, 260, clientH };
                HBRUSH darkBrush = CreateSolidBrush(RGB(30, 30, 30));
                FillRect(hdcMem, &sbRect, darkBrush);

                SetBkMode(hdcMem, TRANSPARENT);
                
                SetTextColor(hdcMem, RGB(255, 255, 255));
                HFONT hHeaderFont = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                HFONT hOldFont = (HFONT)SelectObject(hdcMem, hHeaderFont);
                RECT headerRect = { 20, 10, 240, 50 };
                DrawTextA(hdcMem, "Gupt", -1, &headerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, hOldFont);
                DeleteObject(hHeaderFont);

                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
                HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
                MoveToEx(hdcMem, 20, 50, NULL);
                LineTo(hdcMem, 240, 50);
                SelectObject(hdcMem, hOldPen);
                DeleteObject(hPen);

                HFONT hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                hOldFont = (HFONT)SelectObject(hdcMem, hFont);

                const char* btnText1 = g_IsFullscreen ? "  Exit Fullscreen" : "  Enter Fullscreen";
                const char* btnTexts[] = { btnText1, "  Send Ctrl+Alt+Del", "  Disconnect" };
                
                for (int i = 0; i < 3; ++i) {
                    RECT btnRect = { 0, 60 + i * 44, 260, 60 + (i + 1) * 44 };
                    if (g_HoveredButton == i) {
                        HBRUSH hoverBrush = CreateSolidBrush(RGB(60, 60, 60));
                        FillRect(hdcMem, &btnRect, hoverBrush);
                        DeleteObject(hoverBrush);
                    }
                    RECT textRect = { 20, btnRect.top, 260, btnRect.bottom };
                    DrawTextA(hdcMem, btnTexts[i], -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }

                SetTextColor(hdcMem, RGB(150, 150, 150));
                RECT statusRect = { 20, clientH - 40, 240, clientH - 10 };
                DrawTextA(hdcMem, "Connected", -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdcMem, hOldFont);
                DeleteObject(hFont);
                DeleteObject(darkBrush);

                BLENDFUNCTION bf = {};
                bf.BlendOp = AC_SRC_OVER;
                bf.SourceConstantAlpha = 216;
                AlphaBlend(hdc, clientW / 2 - 130, g_SidebarOffsetY, 260, clientH, hdcMem, 0, 0, 260, clientH, bf);

                SelectObject(hdcMem, hbmOld);
                DeleteObject(hbmMem);
                DeleteDC(hdcMem);
            }

            int tabX = clientW / 2 - 14;
            HBRUSH tabBrush = CreateSolidBrush(RGB(20, 20, 20));
            HPEN tabPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
            HBRUSH hOldTABrush = (HBRUSH)SelectObject(hdc, tabBrush);
            HPEN hOldTAPen = (HPEN)SelectObject(hdc, tabPen);
            RoundRect(hdc, tabX, -10, tabX + 28, 60, 10, 10);
            SelectObject(hdc, hOldTAPen);
            SelectObject(hdc, hOldTABrush);
            DeleteObject(tabPen);
            DeleteObject(tabBrush);

            HPEN chevPen = CreatePen(PS_SOLID, 2, RGB(200, 200, 200));
            HPEN oldChevPen = (HPEN)SelectObject(hdc, chevPen);
            POINT p1[] = { {tabX + 8, 20}, {tabX + 14, 26}, {tabX + 20, 20} };
            Polyline(hdc, p1, 3);
            POINT p2[] = { {tabX + 8, 30}, {tabX + 14, 36}, {tabX + 20, 30} };
            Polyline(hdc, p2, 3);
            SelectObject(hdc, oldChevPen);
            DeleteObject(chevPen);

            EndPaint(hWnd, &ps);
            break;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            int x = (short)LOWORD(lParam);
            int y = (short)HIWORD(lParam);

            RECT rect;
            GetClientRect(hWnd, &rect);
            int clientW = rect.right - rect.left;
            int clientH = rect.bottom - rect.top;

            int tabX = clientW / 2 - 14;
            bool hitTab = (x >= tabX && x <= tabX + 28 && y >= 0 && y <= 60);

            if (message == WM_LBUTTONDOWN) {
                if (hitTab) {
                    g_SidebarOpen = !g_SidebarOpen;
                    return 0;
                }
                
                if (g_SidebarOpen) {
                    int sbLeft = clientW / 2 - 130;
                    if (x >= sbLeft && x <= sbLeft + 260) {
                        for (int i = 0; i < 3; ++i) {
                            int btnTop = g_SidebarOffsetY + 60 + i * 44;
                            if (y >= btnTop && y <= btnTop + 44) {
                                if (i == 0) {
                                    g_IsFullscreen = !g_IsFullscreen;
                                    if (g_IsFullscreen) {
                                        GetWindowRect(hWnd, &g_WindowedRect);
                                        SetWindowLongA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                                        SetWindowPos(hWnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW);
                                    } else {
                                        SetWindowLongA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                                        SetWindowPos(hWnd, HWND_TOP, g_WindowedRect.left, g_WindowedRect.top, g_WindowedRect.right - g_WindowedRect.left, g_WindowedRect.bottom - g_WindowedRect.top, SWP_SHOWWINDOW);
                                    }
                                    g_SidebarOpen = false;
                                } else if (i == 1) {
                                    if (g_IsConnected) {
                                        shared::KeyboardEvent ke = {};
                                        ke.virtualKey = VK_CONTROL; ke.isDown = true; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                                        ke.virtualKey = VK_MENU; ke.isDown = true; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                                        ke.virtualKey = VK_DELETE; ke.isDown = true; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                                        ke.virtualKey = VK_DELETE; ke.isDown = false; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                                        ke.virtualKey = VK_MENU; ke.isDown = false; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                                        ke.virtualKey = VK_CONTROL; ke.isDown = false; g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
                                    }
                                } else if (i == 2) {
                                    g_Client.Disconnect();
                                    PostQuitMessage(0);
                                }
                                return 0;
                            }
                        }
                        return 0;
                    } else {
                        g_SidebarOpen = false;
                    }
                }
            } else if (message == WM_MOUSEMOVE) {
                if (g_SidebarOpen) {
                    int oldHover = g_HoveredButton;
                    g_HoveredButton = -1;
                    int sbLeft = clientW / 2 - 130;
                    if (x >= sbLeft && x <= sbLeft + 260) {
                        for (int i = 0; i < 3; ++i) {
                            int btnTop = g_SidebarOffsetY + 60 + i * 44;
                            if (y >= btnTop && y <= btnTop + 44) {
                                g_HoveredButton = i;
                                break;
                            }
                        }
                    }
                    if (oldHover != g_HoveredButton) InvalidateRect(hWnd, NULL, FALSE);
                }
            }

            if (g_SidebarOpen) {
                return 0;
            }

            if (g_IsConnected) {
                shared::MouseEvent me = {};
                
                if (clientW == 0 || clientH == 0) break;

                me.normalizedX = static_cast<float>(x) / clientW;
                me.normalizedY = static_cast<float>(y) / clientH;
                
                if (message == WM_MOUSEWHEEL) {
                    POINT pt = {x, y};
                    ScreenToClient(hWnd, &pt);
                    me.normalizedX = static_cast<float>(pt.x) / clientW;
                    me.normalizedY = static_cast<float>(pt.y) / clientH;
                    me.wheelDelta = (short)HIWORD(wParam);
                } else me.wheelDelta = 0;

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
                    GetWindowRect(hWnd, &g_WindowedRect);
                    SetWindowLongA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                    SetWindowPos(hWnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW);
                } else {
                    SetWindowLongA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                    SetWindowPos(hWnd, HWND_TOP, g_WindowedRect.left, g_WindowedRect.top, g_WindowedRect.right - g_WindowedRect.left, g_WindowedRect.bottom - g_WindowedRect.top, SWP_SHOWWINDOW);
                }
                g_SidebarOpen = false;
                return 0;
            }

            if (g_SidebarOpen) return 0;

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
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "GuptClientClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    g_WindowedRect = { (screenW - 800) / 2, (screenH - 600) / 2, (screenW + 800) / 2, (screenH + 600) / 2 };

    HWND hWnd = CreateWindowExA(WS_EX_TOOLWINDOW, "GuptClientClass", "",
                              WS_POPUP, 0, 0, screenW, screenH,
                              NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    g_Client.SetMessageCallback([hWnd](shared::MessageType type, const std::vector<uint8_t>& payload) {
        if (type == shared::MessageType::ConnectResponse) {
            auto res = reinterpret_cast<const shared::ConnectResponse*>(payload.data());
            if (res->accepted) {
                g_IsConnected = true;
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

    if (g_Client.Connect("127.0.0.1", 8080)) {
        shared::ConnectRequest req = {};
        std::strncpy(req.sessionId, "DEMO_SESSION_123", sizeof(req.sessionId));
        std::strncpy(req.authenticationToken, "SECURE_TOKEN", sizeof(req.authenticationToken));
        g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectRequest, req));
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_Client.Disconnect();
    return static_cast<int>(msg.wParam);
}
