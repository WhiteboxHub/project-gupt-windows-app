#include <iostream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <vector>
#include <mutex>
#include "../Shared/Protocol.h"
#include "../Core/Network/TcpNetwork.h"

// Globals
using namespace gupt;

bool g_IsConnected = false;
gupt::core::network::TcpClient g_Client;
std::mutex g_FrameMutex;
std::vector<uint8_t> g_LatestFrame;
uint32_t g_FrameWidth = 0;
uint32_t g_FrameHeight = 0;

bool g_SidebarOpen = false;
int g_SidebarX = 0; // Updated in WinMain
bool g_IsFullscreen = true;
RECT g_WindowedRect = { 0, 0, 800, 600 };
bool g_ScaleToFit = true;
bool g_HighDPI = false;
RECT g_TabRect = {0,0,0,0};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            SetTimer(hWnd, 1, 10, NULL);
            break;
        case WM_TIMER:
            if (wParam == 1) {
                RECT r; GetClientRect(hWnd, &r);
                int w = r.right - r.left;
                int targetX = g_SidebarOpen ? w - 300 : w;
                if (g_SidebarX != targetX) {
                    if (g_SidebarX > targetX) g_SidebarX -= 30;
                    if (g_SidebarX < targetX) g_SidebarX += 30;
                    if (abs(g_SidebarX - targetX) < 30) g_SidebarX = targetX;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rect; GetClientRect(hWnd, &rect);
            int cw = rect.right - rect.left;
            int ch = rect.bottom - rect.top;

            // 1. Remote Frame (StretchDIBits)
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
                    
                    if (g_ScaleToFit) {
                        StretchDIBits(hdc, 0, 0, cw, ch, 0, 0, g_FrameWidth, g_FrameHeight, g_LatestFrame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                    } else {
                        int dx = (cw - g_FrameWidth) / 2;
                        int dy = (ch - g_FrameHeight) / 2;
                        StretchDIBits(hdc, dx, dy, g_FrameWidth, g_FrameHeight, 0, 0, g_FrameWidth, g_FrameHeight, g_LatestFrame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                    }
                }
            }

            // 2. Sidebar Panel
            if (g_SidebarX < cw) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hbmMem = CreateCompatibleBitmap(hdc, 300, ch);
                HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

                RECT sbRect = { 0, 0, 300, ch };
                HBRUSH wBrush = CreateSolidBrush(RGB(255, 255, 255));
                FillRect(hdcMem, &sbRect, wBrush);
                DeleteObject(wBrush);

                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
                HPEN oldPen = (HPEN)SelectObject(hdcMem, borderPen);
                MoveToEx(hdcMem, 0, 0, NULL); LineTo(hdcMem, 0, ch);
                SelectObject(hdcMem, oldPen); DeleteObject(borderPen);

                // Toolbar
                RECT tbRect = { 1, 0, 300, 48 };
                HBRUSH tbBrush = CreateSolidBrush(RGB(245, 245, 245));
                FillRect(hdcMem, &tbRect, tbBrush);
                DeleteObject(tbBrush);
                SetBkMode(hdcMem, TRANSPARENT);
                SetTextColor(hdcMem, RGB(50, 50, 50));
                HFONT font = CreateFontA(24, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
                HFONT oldFont = (HFONT)SelectObject(hdcMem, font);
                
                RECT backRect = { 10, 8, 42, 40 }; DrawTextA(hdcMem, "<", -1, &backRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                RECT pinRect = { 134, 8, 166, 40 }; DrawTextA(hdcMem, "+", -1, &pinRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                RECT closeRect = { 258, 8, 290, 40 }; DrawTextA(hdcMem, "X", -1, &closeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, oldFont); DeleteObject(font);

                // Cards
                font = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
                oldFont = (HFONT)SelectObject(hdcMem, font);
                HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(230,230,230));
                oldPen = (HPEN)SelectObject(hdcMem, cardPen);
                HBRUSH cBrush = CreateSolidBrush(RGB(255,255,255));
                HBRUSH oldB = (HBRUSH)SelectObject(hdcMem, cBrush);
                
                RoundRect(hdcMem, 10, 60, 140, 140, 8, 8); // Left
                RoundRect(hdcMem, 150, 60, 280, 140, 8, 8); // Right
                SelectObject(hdcMem, oldB); DeleteObject(cBrush);
                SelectObject(hdcMem, oldPen); DeleteObject(cardPen);

                RECT dc1 = {10, 110, 140, 140}; DrawTextA(hdcMem, "Disconnect", -1, &dc1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                const char* fsTxt = g_IsFullscreen ? "Exit full-screen" : "Full-screen";
                RECT fs1 = {150, 110, 280, 140}; DrawTextA(hdcMem, fsTxt, -1, &fs1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Section header
                HFONT fontB = CreateFontA(14, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
                SelectObject(hdcMem, fontB);
                RECT shRect = { 10, 160, 140, 190 }; DrawTextA(hdcMem, "Session options", -1, &shRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                RECT sh2Rect = { 260, 160, 290, 190 }; DrawTextA(hdcMem, "^", -1, &sh2Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcMem, font); DeleteObject(fontB);

                // Checkboxes
                RECT chk1 = { 10, 190, 280, 246 }; 
                const char* sftxt = g_ScaleToFit ? "[x] Scale to fit" : "[ ] Scale to fit";
                DrawTextA(hdcMem, sftxt, -1, &chk1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                
                RECT chk2 = { 10, 246, 280, 302 }; 
                const char* hdtxt = g_HighDPI ? "[x] High-DPI mode" : "[ ] High-DPI mode";
                DrawTextA(hdcMem, hdtxt, -1, &chk2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                
                SelectObject(hdcMem, oldFont); DeleteObject(font);
                BitBlt(hdc, g_SidebarX, 0, 300, ch, hdcMem, 0, 0, SRCCOPY);
                SelectObject(hdcMem, hbmOld); DeleteObject(hbmMem); DeleteDC(hdcMem);
            }

            // 3. Tab Icon (Top of everything)
            g_TabRect.left = g_SidebarX - 28;
            g_TabRect.top = ch / 2 - 28;
            g_TabRect.right = g_SidebarX;
            g_TabRect.bottom = ch / 2 + 28;

            HBRUSH tabBrush = CreateSolidBrush(RGB(50, 50, 50));
            HBRUSH oldTB = (HBRUSH)SelectObject(hdc, tabBrush);
            HPEN transparentPen = CreatePen(PS_NULL, 0, 0);
            HPEN oldTP = (HPEN)SelectObject(hdc, transparentPen);
            RoundRect(hdc, g_TabRect.left, g_TabRect.top, g_TabRect.right + 10, g_TabRect.bottom, 10, 10);
            SelectObject(hdc, oldTP); DeleteObject(transparentPen);
            SelectObject(hdc, oldTB); DeleteObject(tabBrush);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            HFONT tFont = CreateFontA(24, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
            HFONT oldTFont = (HFONT)SelectObject(hdc, tFont);
            RECT tr = { g_TabRect.left, g_TabRect.top, g_TabRect.left + 28, g_TabRect.bottom };
            const char* chev = g_SidebarOpen ? ">" : "<";
            DrawTextA(hdc, chev, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldTFont); DeleteObject(tFont);

            EndPaint(hWnd, &ps);
            break;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            int x = (short)LOWORD(lParam); int y = (short)HIWORD(lParam);
            RECT rect; GetClientRect(hWnd, &rect);
            int cw = rect.right - rect.left; int ch = rect.bottom - rect.top;
            if (cw == 0 || ch == 0) break;

            if (message == WM_LBUTTONDOWN) {
                // Tab Priority 1
                if (x >= g_TabRect.left && x <= g_TabRect.right && y >= g_TabRect.top && y <= g_TabRect.bottom) {
                    g_SidebarOpen = !g_SidebarOpen;
                    return 0;
                }

                if (g_SidebarOpen && x >= g_SidebarX) { // Inside Sidebar Priority 2
                    int lx = x - g_SidebarX;
                    if (lx >= 250 && y <= 48) { g_SidebarOpen = false; return 0; } // close
                    if (lx <= 50 && y <= 48) { g_SidebarOpen = false; return 0; } // back
                    if (lx >= 10 && lx <= 140 && y >= 60 && y <= 140) { // Disconnect
                        g_Client.Disconnect(); PostQuitMessage(0); return 0;
                    }
                    if (lx >= 150 && lx <= 280 && y >= 60 && y <= 140) { // Fullscreen
                        g_IsFullscreen = !g_IsFullscreen;
                        if (g_IsFullscreen) {
                            GetWindowRect(hWnd, &g_WindowedRect);
                            SetWindowLongA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_EX_TOOLWINDOW); // Must force popup tool again! 
                            SetWindowPos(hWnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW);
                        } else {
                            SetWindowLongA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                            SetWindowPos(hWnd, HWND_TOP, g_WindowedRect.left, g_WindowedRect.top, g_WindowedRect.right - g_WindowedRect.left, g_WindowedRect.bottom - g_WindowedRect.top, SWP_SHOWWINDOW);
                        }
                        g_SidebarOpen = false; return 0;
                    }
                    if (y >= 190 && y <= 246) { g_ScaleToFit = !g_ScaleToFit; InvalidateRect(hWnd, NULL, FALSE); return 0; }
                    if (y >= 246 && y <= 302) { g_HighDPI = !g_HighDPI; InvalidateRect(hWnd, NULL, FALSE); return 0; }
                    return 0;
                } else if (g_SidebarOpen && x < g_SidebarX && !(x >= g_TabRect.left && x <= g_TabRect.right && y >= g_TabRect.top && y <= g_TabRect.bottom)) {
                    // Click outside sidebar priority 3
                    g_SidebarOpen = false;
                }
            }

            if (g_SidebarOpen) return 0; // Input gating

            if (g_IsConnected) {
                shared::MouseEvent me = {};
                me.normalizedX = static_cast<float>(x) / cw;
                me.normalizedY = static_cast<float>(y) / ch;
                if (message == WM_MOUSEWHEEL) {
                    POINT pt = {x, y}; ScreenToClient(hWnd, &pt);
                    me.normalizedX = static_cast<float>(pt.x) / cw; me.normalizedY = static_cast<float>(pt.y) / ch;
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
                    SetWindowLongA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_EX_TOOLWINDOW); 
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
                shared::KeyboardEvent ke = {}; ke.virtualKey = wParam; ke.isDown = (message == WM_KEYDOWN);
                g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
            }
            break;
        }
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = "GuptClientClass"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN); int sh = GetSystemMetrics(SM_CYSCREEN);
    g_WindowedRect = { (sw - 800) / 2, (sh - 600) / 2, (sw + 800) / 2, (sh + 600) / 2 };
    g_SidebarX = sw; // Start closed
    HWND hWnd = CreateWindowExA(WS_EX_TOOLWINDOW, "GuptClientClass", "", WS_POPUP, 0, 0, sw, sh, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd);

    g_Client.SetMessageCallback([hWnd](shared::MessageType type, const std::vector<uint8_t>& payload) {
        if (type == shared::MessageType::ConnectResponse) {
            auto res = reinterpret_cast<const shared::ConnectResponse*>(payload.data());
            if (res->accepted) g_IsConnected = true;
        } else if (type == shared::MessageType::FrameData) {
            auto hd = reinterpret_cast<const shared::FrameDataHeader*>(payload.data());
            size_t off = sizeof(shared::FrameDataHeader);
            if (payload.size() >= off + (hd->width * hd->height * 4)) {
                std::lock_guard<std::mutex> lock(g_FrameMutex); g_FrameWidth = hd->width; g_FrameHeight = hd->height;
                g_LatestFrame.assign(payload.begin() + off, payload.end());
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
    });

    if (g_Client.Connect("127.0.0.1", 8080)) {
        shared::ConnectRequest req = {}; std::strncpy(req.sessionId, "DEMO_SESSION_123", sizeof(req.sessionId)); std::strncpy(req.authenticationToken, "SECURE_TOKEN", sizeof(req.authenticationToken));
        g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectRequest, req));
    }

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    g_Client.Disconnect(); return static_cast<int>(msg.wParam);
}
