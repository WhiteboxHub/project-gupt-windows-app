#include <iostream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include <vector>
#include <mutex>
#include <wincodec.h>
#include <shlwapi.h>
#include "../Shared/Protocol.h"
#include "../Core/Network/TcpNetwork.h"

using namespace gupt;

// Core Globals
bool g_IsConnected = false;
gupt::core::network::TcpClient g_Client;
std::mutex g_FrameMutex;
std::vector<uint8_t> g_LatestFrame;
uint32_t g_FrameWidth = 0;
uint32_t g_FrameHeight = 0;
int g_DestX = 0, g_DestY = 0, g_DestW = 800, g_DestH = 600;

// Sidebar UI Globals
bool g_SidebarOpen = false;
int g_ScreenW = 0;
int g_ScreenH = 0;
int g_SidebarX = 0;
bool g_IsFullscreen = true;
RECT g_SavedRect = { 0, 0, 800, 600 };
int g_HoveredCard = -1;
RECT g_TabRect = { 0, 0, 0, 0 };
RECT g_Card1Rect = { 0, 0, 0, 0 };
RECT g_Card2Rect = { 0, 0, 0, 0 };
RECT g_BackBtnRect = { 0, 0, 0, 0 };
RECT g_CloseBtnRect = { 0, 0, 0, 0 };
RECT g_MinBtnRect = { 0, 0, 0, 0 };   // Minimize button (windowed mode only)
RECT g_MaxBtnRect = { 0, 0, 0, 0 };   // Maximize button (windowed mode only)

// Cached persistent back buffer (created once, reused every frame — avoids per-frame alloc)
HDC    g_BackDC  = NULL;
HBITMAP g_BackBmp = NULL;
int    g_BackW   = 0;
int    g_BackH   = 0;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == 1) {
                int targetX = g_SidebarOpen ? g_ScreenW - 260 : g_ScreenW;
                if (g_SidebarX != targetX) {
                    if (g_SidebarX > targetX) {
                        g_SidebarX -= 50;  // 50px/tick @ 10ms = 5000px/s — fast enough
                        if (g_SidebarX < targetX) g_SidebarX = targetX;
                    } else {
                        g_SidebarX += 50;
                        if (g_SidebarX > targetX) g_SidebarX = targetX;
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                if (g_SidebarX == targetX) {
                    KillTimer(hWnd, 1);
                }
            }
            break;
        case WM_ERASEBKGND:
            return 1; // Suppress background erase to prevent white-flash flicker

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            // Always read the TRUE current client rect — never trust stale g_ScreenW/H
            // (cross-resolution connections, DPI changes, window transitions can all
            //  cause WM_SIZE to lag behind the actual paint size)
            {
                RECT cr; GetClientRect(hWnd, &cr);
                int actualW = cr.right;
                int actualH = cr.bottom;
                if (actualW > 0 && actualH > 0 && (actualW != g_ScreenW || actualH != g_ScreenH)) {
                    g_ScreenW = actualW;
                    g_ScreenH = actualH;
                    // Reposition sidebar snap immediately
                    g_SidebarX = g_SidebarOpen ? g_ScreenW - 260 : g_ScreenW;
                }
            }
            if (g_ScreenW < 1 || g_ScreenH < 1) { EndPaint(hWnd, &ps); break; }

            // Reuse persistent back buffer — (re)create only when dimensions change
            if (!g_BackDC || g_BackW != g_ScreenW || g_BackH != g_ScreenH) {
                if (g_BackDC) { SelectObject(g_BackDC, GetStockObject(BLACK_BRUSH)); DeleteObject(g_BackBmp); DeleteDC(g_BackDC); }
                g_BackDC  = CreateCompatibleDC(hdc);
                g_BackBmp = CreateCompatibleBitmap(hdc, g_ScreenW, g_ScreenH);
                SelectObject(g_BackDC, g_BackBmp);
                g_BackW = g_ScreenW; g_BackH = g_ScreenH;
            }
            HDC hdcBack = g_BackDC;


            // 1. Remote Frame (StretchDIBits into back buffer)
            {
                std::lock_guard<std::mutex> lock(g_FrameMutex);
                if (!g_LatestFrame.empty() && g_FrameWidth > 0 && g_FrameHeight > 0) {
                    float hostAspect = (float)g_FrameWidth / (float)g_FrameHeight;
                    float clientAspect = (g_ScreenH > 0) ? (float)g_ScreenW / (float)g_ScreenH : 1.f;

                    if (clientAspect > hostAspect) {
                        g_DestH = g_ScreenH;
                        g_DestW = static_cast<int>(g_ScreenH * hostAspect);
                        g_DestX = (g_ScreenW - g_DestW) / 2;
                        g_DestY = 0;
                    } else {
                        g_DestW = g_ScreenW;
                        g_DestH = static_cast<int>(g_ScreenW / hostAspect);
                        g_DestX = 0;
                        g_DestY = (g_ScreenH - g_DestH) / 2;
                    }

                    // Fill black letterbox bars
                    HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
                    RECT fullRect = {0, 0, g_ScreenW, g_ScreenH};
                    FillRect(hdcBack, &fullRect, blackBrush);

                    BITMAPINFO bmi = {};
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = g_FrameWidth;
                    bmi.bmiHeader.biHeight = -static_cast<int>(g_FrameHeight);
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;
                    // COLORONCOLOR is ~10x faster than HALFTONE; sufficient for remote desktop
                    SetStretchBltMode(hdcBack, COLORONCOLOR);

                    StretchDIBits(hdcBack, g_DestX, g_DestY, g_DestW, g_DestH,
                                  0, 0, g_FrameWidth, g_FrameHeight,
                                  g_LatestFrame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                } else {
                    HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
                    RECT fullRect = {0, 0, g_ScreenW, g_ScreenH};
                    FillRect(hdcBack, &fullRect, blackBrush);
                }
            }

            // 2. Sidebar Panel into back buffer
            if (g_SidebarX < g_ScreenW) {
                HDC hdcSb = CreateCompatibleDC(hdcBack);
                int sbw = g_ScreenW - g_SidebarX;
                HBITMAP hbmSb = CreateCompatibleBitmap(hdcBack, sbw, g_ScreenH);
                HBITMAP hbmSbOld = (HBITMAP)SelectObject(hdcSb, hbmSb);

                RECT sbRect = { 0, 0, sbw, g_ScreenH };
                HBRUSH wBrush = CreateSolidBrush(RGB(255, 255, 255));
                FillRect(hdcSb, &sbRect, wBrush);
                DeleteObject(wBrush);

                // Header Row
                RECT headRect = { 0, 0, sbw, 52 };
                HBRUSH thBrush = CreateSolidBrush(RGB(247, 247, 247));
                FillRect(hdcSb, &headRect, thBrush);
                DeleteObject(thBrush);
                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
                HPEN oldPen = (HPEN)SelectObject(hdcSb, borderPen);
                MoveToEx(hdcSb, 0, 52, NULL); LineTo(hdcSb, sbw, 52);
                SelectObject(hdcSb, oldPen); DeleteObject(borderPen);

                SetBkMode(hdcSb, TRANSPARENT);
                SetTextColor(hdcSb, RGB(40, 40, 40));
                HFONT hFont = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
                HFONT oldFont = (HFONT)SelectObject(hdcSb, hFont);

                g_BackBtnRect  = { g_SidebarX + 10, 8, g_SidebarX + 46, 44 };
                g_CloseBtnRect = { g_ScreenW - 46,  8, g_ScreenW - 10, 44 };

                RECT brect = { 10, 8, 46, 44 };
                DrawTextA(hdcSb, "<", -1, &brect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                RECT crect = { sbw - 46, 8, sbw - 10, 44 };
                DrawTextA(hdcSb, "X", -1, &crect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Minimize (_) and Maximize (□) only visible in windowed mode
                if (!g_IsFullscreen) {
                    // Max button: left of X
                    g_MaxBtnRect = { g_ScreenW - 90, 8, g_ScreenW - 50, 44 };
                    RECT maxR = { sbw - 90, 8, sbw - 50, 44 };
                    DrawTextA(hdcSb, "\x5B\x5D", -1, &maxR, DT_CENTER | DT_VCENTER | DT_SINGLELINE); // []
                    // Min button: left of Max
                    g_MinBtnRect = { g_ScreenW - 134, 8, g_ScreenW - 94, 44 };
                    RECT minR = { sbw - 134, 8, sbw - 94, 44 };
                    DrawTextA(hdcSb, "_", -1, &minR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else {
                    g_MinBtnRect = {0,0,0,0};
                    g_MaxBtnRect = {0,0,0,0};
                }

                RECT trect = { 0, 0, sbw, 52 };
                DrawTextA(hdcSb, "Gupt", -1, &trect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcSb, oldFont); DeleteObject(hFont);

                int cW = (sbw - 42) / 2;
                if (cW > 0) {
                    g_Card1Rect = { g_SidebarX + 14, 66, g_SidebarX + 14 + cW, 66 + 88 };
                    g_Card2Rect = { g_SidebarX + 14 + cW + 14, 66, g_SidebarX + 14 + cW + 14 + cW, 66 + 88 };

                    HBRUSH normBrush = CreateSolidBrush(RGB(255, 255, 255));
                    HBRUSH hovBrush  = CreateSolidBrush(RGB(245, 245, 245));
                    HPEN cPen = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
                    oldPen = (HPEN)SelectObject(hdcSb, cPen);
                    SelectObject(hdcSb, g_HoveredCard == 1 ? hovBrush : normBrush);
                    RoundRect(hdcSb, 14, 66, 14 + cW, 66 + 88, 8, 8);
                    SelectObject(hdcSb, g_HoveredCard == 2 ? hovBrush : normBrush);
                    RoundRect(hdcSb, 14 + cW + 14, 66, 14 + cW + 14 + cW, 66 + 88, 8, 8);
                    SelectObject(hdcSb, oldPen); DeleteObject(cPen);

                    HFONT cFont = CreateFontA(14, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
                    oldFont = (HFONT)SelectObject(hdcSb, cFont);
                    SetTextColor(hdcSb, RGB(220, 50, 50));
                    RECT cr1 = { 14, 66, 14 + cW, 66 + 88 };
                    DrawTextA(hdcSb, "Disconnect", -1, &cr1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SetTextColor(hdcSb, RGB(50, 100, 220));
                    RECT cr2 = { 14 + cW + 14, 66, 14 + cW + 14 + cW, 66 + 88 };
                    const char* fsTxt = g_IsFullscreen ? "Exit Full-screen" : "Full-screen";
                    DrawTextA(hdcSb, fsTxt, -1, &cr2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdcSb, oldFont); DeleteObject(cFont);
                    DeleteObject(normBrush); DeleteObject(hovBrush);
                }

                BitBlt(hdcBack, g_SidebarX, 0, sbw, g_ScreenH, hdcSb, 0, 0, SRCCOPY);
                SelectObject(hdcSb, hbmSbOld); DeleteObject(hbmSb); DeleteDC(hdcSb);
            }

            // 3. Tab Button into back buffer
            g_TabRect.left   = g_SidebarX - 22;
            g_TabRect.right  = g_SidebarX;
            g_TabRect.top    = g_ScreenH / 2 - 28;
            g_TabRect.bottom = g_ScreenH / 2 + 28;

            {
                HBRUSH tabBrush = CreateSolidBrush(RGB(45, 45, 45));
                HBRUSH oldTB = (HBRUSH)SelectObject(hdcBack, tabBrush);
                HPEN tPen = CreatePen(PS_NULL, 0, 0);
                HPEN oldTP = (HPEN)SelectObject(hdcBack, tPen);
                RoundRect(hdcBack, g_TabRect.left, g_TabRect.top, g_TabRect.right + 10, g_TabRect.bottom, 12, 12);
                SelectObject(hdcBack, oldTP); DeleteObject(tPen);
                SelectObject(hdcBack, oldTB); DeleteObject(tabBrush);

                SetBkMode(hdcBack, TRANSPARENT);
                SetTextColor(hdcBack, RGB(255, 255, 255));
                HFONT tFont = CreateFontA(16, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
                HFONT oldTFont = (HFONT)SelectObject(hdcBack, tFont);
                RECT tr = { g_TabRect.left, g_TabRect.top, g_TabRect.right, g_TabRect.bottom };
                const char* chev = g_SidebarOpen ? ">" : "<";
                DrawTextA(hdcBack, chev, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdcBack, oldTFont); DeleteObject(tFont);
            }

            // Final single blit to screen — eliminates all flickering
            BitBlt(hdc, 0, 0, g_ScreenW, g_ScreenH, hdcBack, 0, 0, SRCCOPY);
            // NOTE: hdcBack is persistent (g_BackDC) — do NOT delete it here


            EndPaint(hWnd, &ps);
            break;
        }
        case WM_MOUSEMOVE: {
            int x = (short)LOWORD(lParam); int y = (short)HIWORD(lParam);
            if (g_SidebarOpen && x >= g_SidebarX) {
                int hover = -1;
                POINT pt = {x, y};
                if (PtInRect(&g_Card1Rect, pt)) hover = 1;
                else if (PtInRect(&g_Card2Rect, pt)) hover = 2;
                if (hover != g_HoveredCard) {
                    g_HoveredCard = hover;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            if (g_SidebarOpen) return 0; // Gate forwarding
            if (g_IsConnected) {
                if (x >= g_DestX && x < g_DestX + g_DestW && y >= g_DestY && y < g_DestY + g_DestH) {
                    shared::MouseEvent me = {};
                    me.normalizedX = static_cast<float>(x - g_DestX) / g_DestW;
                    me.normalizedY = static_cast<float>(y - g_DestY) / g_DestH;
                    me.wheelDelta = 0; me.buttonId = 255; me.isDown = false;
                    g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent, me));
                }
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            if (g_SidebarOpen) return 0;
            if (g_IsConnected) {
                int x = (short)LOWORD(lParam); int y = (short)HIWORD(lParam);
                POINT pt = {x, y}; ScreenToClient(hWnd, &pt);
                if (pt.x >= g_DestX && pt.x < g_DestX + g_DestW && pt.y >= g_DestY && pt.y < g_DestY + g_DestH) {
                    shared::MouseEvent me = {};
                    me.normalizedX = static_cast<float>(pt.x - g_DestX) / g_DestW;
                    me.normalizedY = static_cast<float>(pt.y - g_DestY) / g_DestH;
                    me.wheelDelta = (short)HIWORD(wParam);
                    me.buttonId = 255; me.isDown = false;
                    g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent, me));
                }
            }
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            int x = (short)LOWORD(lParam); int y = (short)HIWORD(lParam); POINT pt = {x, y};
            if (message == WM_LBUTTONDOWN) {
                if (PtInRect(&g_TabRect, pt)) {
                    g_SidebarOpen = !g_SidebarOpen;
                    SetTimer(hWnd, 1, 10, NULL);
                    return 0;
                }
                if (g_SidebarOpen && x >= g_SidebarX) {
                    if (PtInRect(&g_BackBtnRect, pt) || PtInRect(&g_CloseBtnRect, pt)) {
                        g_SidebarOpen = false;
                        SetTimer(hWnd, 1, 10, NULL);
                        return 0;
                    }
                    // Minimize / Maximize (windowed mode only)
                    if (!g_IsFullscreen && PtInRect(&g_MinBtnRect, pt)) {
                        ShowWindow(hWnd, SW_MINIMIZE);
                        g_SidebarOpen = false; SetTimer(hWnd, 1, 10, NULL);
                        return 0;
                    }
                    if (!g_IsFullscreen && PtInRect(&g_MaxBtnRect, pt)) {
                        ShowWindow(hWnd, IsZoomed(hWnd) ? SW_RESTORE : SW_MAXIMIZE);
                        g_SidebarOpen = false; SetTimer(hWnd, 1, 10, NULL);
                        return 0;
                    }
                    if (PtInRect(&g_Card1Rect, pt)) {
                        g_Client.Disconnect(); PostQuitMessage(0); return 0;
                    }
                    if (PtInRect(&g_Card2Rect, pt)) {
                        g_IsFullscreen = !g_IsFullscreen;
                        if (g_IsFullscreen) {
                            GetWindowRect(hWnd, &g_SavedRect);
                            SetWindowLongPtrA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                            SetWindowLongPtrA(hWnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW); // hide from Alt+Tab in fullscreen
                            SetWindowPos(hWnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW | SWP_FRAMECHANGED);
                        } else {
                            SetWindowLongPtrA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                            SetWindowLongPtrA(hWnd, GWL_EXSTYLE, WS_EX_APPWINDOW); // show in Alt+Tab/taskbar
                            SetWindowPos(hWnd, HWND_NOTOPMOST, g_SavedRect.left, g_SavedRect.top, g_SavedRect.right - g_SavedRect.left, g_SavedRect.bottom - g_SavedRect.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
                        }
                        g_SidebarOpen = false;
                        SetTimer(hWnd, 1, 10, NULL);
                        return 0;
                    }
                    return 0; // Click inside sidebar but not on any control
                } else if (g_SidebarOpen && x < g_SidebarX) {
                    g_SidebarOpen = false;
                    SetTimer(hWnd, 1, 10, NULL);
                    // Fall through!
                }
            }

            if (g_SidebarOpen) return 0;

            if (g_IsConnected) {
                if (x >= g_DestX && x < g_DestX + g_DestW && y >= g_DestY && y < g_DestY + g_DestH) {
                    shared::MouseEvent me = {};
                    me.normalizedX = static_cast<float>(x - g_DestX) / g_DestW;
                    me.normalizedY = static_cast<float>(y - g_DestY) / g_DestH;
                    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) me.buttonId = 0;
                    else me.buttonId = 1;
                    me.isDown = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN);
                    me.wheelDelta = 0;
                    g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent, me));
                }
            }
            break;
        }
        case WM_KEYDOWN:
        case WM_KEYUP: {
            if (wParam == VK_F11 && message == WM_KEYDOWN) {
                g_IsFullscreen = !g_IsFullscreen;
                if (g_IsFullscreen) {
                    GetWindowRect(hWnd, &g_SavedRect);
                    SetWindowLongPtrA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                    SetWindowLongPtrA(hWnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW);
                    SetWindowPos(hWnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_SHOWWINDOW | SWP_FRAMECHANGED);
                } else {
                    SetWindowLongPtrA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                    SetWindowLongPtrA(hWnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
                    SetWindowPos(hWnd, HWND_NOTOPMOST, g_SavedRect.left, g_SavedRect.top, g_SavedRect.right - g_SavedRect.left, g_SavedRect.bottom - g_SavedRect.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
                }
                g_SidebarOpen = false;
                SetTimer(hWnd, 1, 10, NULL);
                return 0;
            }
            if (g_SidebarOpen) return 0;
            if (g_IsConnected) {
                shared::KeyboardEvent ke = {}; ke.virtualKey = (uint8_t)wParam; ke.isDown = (message == WM_KEYDOWN);
                g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent, ke));
            }
            break;
        }
        case WM_SIZE: {
            int newW = LOWORD(lParam);
            int newH = HIWORD(lParam);
            if (newW < 1 || newH < 1) break; // ignore zero-size events
            g_ScreenW = newW;
            g_ScreenH = newH;
            if (!g_SidebarOpen) g_SidebarX = g_ScreenW;
            else g_SidebarX = g_ScreenW - 260;
            // Force cached back buffer to be recreated at new size on next WM_PAINT
            g_BackW = 0; g_BackH = 0;
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        }
        case WM_CLOSE:
            // Alt+F4 or system close: disconnect properly instead of abrupt kill
            g_Client.Disconnect();
            PostQuitMessage(0);
            return 0;
        case WM_ACTIVATE:
            // When deactivated by Alt+Tab: keep running, do not close.
            // If becoming active again while fullscreen, ensure we stay on top.
            if (LOWORD(wParam) != WA_INACTIVE && g_IsFullscreen)
                SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSA wc = {}; 
    wc.lpfnWndProc = WndProc; 
    wc.hInstance = hInstance; 
    wc.lpszClassName = "GuptClientClass"; 
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_ScreenW = GetSystemMetrics(SM_CXSCREEN); 
    g_ScreenH = GetSystemMetrics(SM_CYSCREEN);
    g_SidebarX = g_ScreenW; 
    
    HWND hWnd = CreateWindowExA(WS_EX_TOOLWINDOW, "GuptClientClass", "", WS_POPUP | WS_VISIBLE, 0, 0, g_ScreenW, g_ScreenH, NULL, NULL, hInstance, NULL);
    SetWindowPos(hWnd, HWND_TOP, 0, 0, g_ScreenW, g_ScreenH, SWP_SHOWWINDOW);
    UpdateWindow(hWnd);

    g_Client.SetMessageCallback([hWnd](shared::MessageType type, const std::vector<uint8_t>& payload) {
        // COINIT_MULTITHREADED is required: this runs on a raw std::thread with no message pump.
        // Apartment-threaded COM requires a message pump; without one, CoCreateInstance (WIC) silently fails.
        thread_local bool t_comInit = (CoInitializeEx(NULL, COINIT_MULTITHREADED), true);
        if (type == shared::MessageType::ConnectResponse) {
            auto res = reinterpret_cast<const shared::ConnectResponse*>(payload.data());
            if (res->accepted) g_IsConnected = true;
        } else if (type == shared::MessageType::FrameData) {
            auto hd = reinterpret_cast<const shared::FrameDataHeader*>(payload.data());
            size_t off = sizeof(shared::FrameDataHeader);
            if (payload.size() > off) {
                IStream* pStream = SHCreateMemStream(payload.data() + off, static_cast<UINT>(payload.size() - off));
                if (pStream) {
                    IWICImagingFactory* pFactory = NULL;
                    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
                        IWICBitmapDecoder* pDecoder = NULL;
                        if (SUCCEEDED(pFactory->CreateDecoderFromStream(pStream, NULL, WICDecodeMetadataCacheOnDemand, &pDecoder))) {
                            IWICBitmapFrameDecode* pFrame = NULL;
                            if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame))) {
                                IWICFormatConverter* pConverter = NULL;
                                if (SUCCEEDED(pFactory->CreateFormatConverter(&pConverter))) {
                                    if (SUCCEEDED(pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.f, WICBitmapPaletteTypeCustom))) {
                                        UINT w, h;
                                        pConverter->GetSize(&w, &h);
                                        std::vector<uint8_t> decoded(w * h * 4);
                                        if (SUCCEEDED(pConverter->CopyPixels(NULL, w * 4, static_cast<UINT>(decoded.size()), decoded.data()))) {
                                            std::lock_guard<std::mutex> lock(g_FrameMutex);
                                            g_FrameWidth = w;
                                            g_FrameHeight = h;
                                            g_LatestFrame = std::move(decoded);
                                            InvalidateRect(hWnd, NULL, FALSE);
                                        }
                                    }
                                    pConverter->Release();
                                }
                                pFrame->Release();
                            }
                            pDecoder->Release();
                        }
                        pFactory->Release();
                    }
                    pStream->Release();
                }
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
