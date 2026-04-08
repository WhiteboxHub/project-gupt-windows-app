/*
 * Gupt Remote Desktop - Unified Launcher
 * ========================================
 * Single .exe for both Host and Client modes.
 * Shows a native Win32 dialog to select mode and enter host IP.
 * Statically linked with /MT — no runtime DLL dependencies.
 */

// winsock2.h MUST come before windows.h — otherwise windows.h pulls in winsock.h
// which conflicts with winsock2.h used inside TcpNetwork.h
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <wincodec.h>
#include <shlwapi.h>
#include <iostream>

#include "../Shared/Protocol.h"
#include "../Core/Network/TcpNetwork.h"
#include "../Core/Input/InputInjector.h"
#include "../Core/Capture/ScreenCapturer.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "iphlpapi.lib")

// --- Portable Session ID Logic (Zero-Server Architecture) ---
static std::string GetLocalIP() {
    char name[256];
    if (gethostname(name, sizeof(name)) == 0) {
        struct addrinfo hints = {}, *info = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(name, nullptr, &hints, &info) == 0) {
            for (auto p = info; p != nullptr; p = p->ai_next) {
                struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
                std::string ip = inet_ntoa(addr->sin_addr);
                // Prefer 192.x, 10.x, 172.x (Local network)
                if (ip.find("192.") == 0 || ip.find("10.") == 0 || ip.find("172.") == 0) {
                    std::string res = ip;
                    freeaddrinfo(info);
                    return res;
                }
            }
            freeaddrinfo(info);
        }
    }
    return "127.0.0.1";
}

static std::string EncodeIP(const std::string& ip) {
    unsigned int a, b, c, d;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return "INVALID";
    char buf[16];
    // Encode as a "Premium" 8-char hex code
    sprintf(buf, "%02X%02X%02X%02X", a, b, c, d);
    return std::string(buf);
}

static std::string DecodeIP(const std::string& id) {
    if (id.length() != 8) return id; // Pass-through if it's already an IP
    unsigned int a, b, c, d;
    if (sscanf(id.c_str(), "%02X%02X%02X%02X", &a, &b, &c, &d) != 4) return "";
    char buf[16];
    sprintf(buf, "%u.%u.%u.%u", a, b, c, d);
    return std::string(buf);
}

using namespace gupt;

// ─────────────────────────────────────────────────────────────────────────────
//  Dialog Control IDs
// ─────────────────────────────────────────────────────────────────────────────
#define ID_RADIO_HOST      101
#define ID_RADIO_CLIENT    102
#define ID_EDIT_IP         103
#define ID_BTN_LAUNCH      104
#define ID_BTN_CANCEL      105
#define ID_LABEL_IP        106
#define ID_LABEL_MODE      107
#define ID_LABEL_TITLE     108
#define ID_LABEL_SUBTITLE  109

// ─────────────────────────────────────────────────────────────────────────────
//  Global launcher state
// ─────────────────────────────────────────────────────────────────────────────
static bool   g_LaunchAsHost = true;
static char   g_HostIp[128]  = ""; // Used for Session ID input now
static bool   g_Launched     = false;
static HFONT  g_FontTitle     = NULL;
static HFONT  g_FontSub       = NULL;
static HFONT  g_FontBody      = NULL;
static HFONT  g_FontBtn       = NULL;
static HBRUSH g_BrushBg       = NULL;
static HBRUSH g_BrushCard     = NULL;
static HBRUSH g_BrushAccent   = NULL;

// Colors  (dark premium theme)
static const COLORREF CLR_BG        = RGB(18,  18,  28);
static const COLORREF CLR_CARD      = RGB(28,  28,  44);
static const COLORREF CLR_BORDER    = RGB(60,  60,  90);
static const COLORREF CLR_ACCENT    = RGB(99,  102, 241); // indigo
static const COLORREF CLR_ACCENT2   = RGB(139, 92,  246); // violet
static const COLORREF CLR_TEXT      = RGB(240, 240, 255);
static const COLORREF CLR_SUBTEXT   = RGB(148, 148, 180);
static const COLORREF CLR_INPUT_BG  = RGB(38,  38,  58);
static const COLORREF CLR_INPUT_BOR = RGB(99,  102, 241);
static const COLORREF CLR_BTN_HOVR  = RGB(79,  82,  221);
static const COLORREF CLR_SUCCESS   = RGB(52,  211, 153);

// ─────────────────────────────────────────────────────────────────────────────
//  Rounded rectangle helper
// ─────────────────────────────────────────────────────────────────────────────
static void DrawRoundedRect(HDC hdc, RECT r, int rx, COLORREF fill, COLORREF border, int borderW = 1) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, borderW, border);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
    HPEN   op = (HPEN)SelectObject(hdc, pn);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rx, rx);
    SelectObject(hdc, ob); DeleteObject(br);
    SelectObject(hdc, op); DeleteObject(pn);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Custom-drawn launcher dialog
// ─────────────────────────────────────────────────────────────────────────────
static HWND g_hDlg       = NULL;
static HWND g_hRadioHost = NULL;
static HWND g_hRadioClient = NULL;
static HWND g_hEditIp    = NULL;
static HWND g_hLabelIp   = NULL;
static HWND g_hBtnLaunch = NULL;
static HWND g_hBtnCancel = NULL;
static bool g_BtnLaunchHover = false;
static bool g_BtnCancelHover = false;

// Functional Vector Logo Rendering (recreates the minimalist cube)
static void DrawBrandingLogo(HDC hdc, int x, int y, int size, COLORREF color) {
    HPEN hPen = CreatePen(PS_SOLID, 2, color);
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);
    int w = size;
    int h = (int)(size * 0.58f); // Isometric ratio

    // Points for the cube outline
    POINT p[7];
    p[0] = { x + w / 2, y };          // Top center
    p[1] = { x + w, y + h / 2 };      // Right center
    p[2] = { x + w, y + h + h / 2 };  // Right bottom
    p[3] = { x + w / 2, y + h * 2 };  // Bottom center
    p[4] = { x, y + h + h / 2 };      // Left bottom
    p[5] = { x, y + h / 2 };          // Left center
    p[6] = { x + w / 2, y + h };      // Inner joint

    // Draw the 3 faces
    MoveToEx(hdc, p[0].x, p[0].y, NULL); LineTo(hdc, p[1].x, p[1].y);
    LineTo(hdc, p[6].x, p[6].y); LineTo(hdc, p[5].x, p[5].y); LineTo(hdc, p[0].x, p[0].y); // Top/Left face

    MoveToEx(hdc, p[1].x, p[1].y, NULL); LineTo(hdc, p[2].x, p[2].y);
    LineTo(hdc, p[3].x, p[3].y); LineTo(hdc, p[6].x, p[6].y); // Right face

    MoveToEx(hdc, p[5].x, p[5].y, NULL); LineTo(hdc, p[4].x, p[4].y);
    LineTo(hdc, p[3].x, p[3].y); // Bottom left edge

    SelectObject(hdc, hOld); DeleteObject(hPen);
}

// Subclass proc for Launch button (owner-draw)
static WNDPROC g_OldBtnProc = NULL;
static LRESULT CALLBACK BtnSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSEMOVE) { g_BtnLaunchHover = true; InvalidateRect(hWnd, NULL, FALSE); }
    if (msg == WM_MOUSELEAVE) { g_BtnLaunchHover = false; InvalidateRect(hWnd, NULL, FALSE); }
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
    }
    return CallWindowProc(g_OldBtnProc, hWnd, msg, wParam, lParam);
}

static void UpdateIpVisibility() {
    bool showIp = !g_LaunchAsHost;
    ShowWindow(g_hLabelIp, showIp ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hEditIp,  showIp ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_hDlg, NULL, FALSE);
}

LRESULT CALLBACK LauncherWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        g_hDlg = hWnd;

        // Create fonts
        g_FontTitle = CreateFontA(28, 0, 0, 0, FW_BOLD,  FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        g_FontSub   = CreateFontA(13, 0, 0, 0, FW_NORMAL,FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        g_FontBody  = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD,FALSE,FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        g_FontBtn   = CreateFontA(15, 0, 0, 0, FW_BOLD,  FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

        // Brushes
        g_BrushBg     = CreateSolidBrush(CLR_BG);
        g_BrushCard   = CreateSolidBrush(CLR_CARD);
        g_BrushAccent = CreateSolidBrush(CLR_ACCENT);

        // Radio buttons — using BS_OWNERDRAW so we can theme them
        g_hRadioHost = CreateWindowA("BUTTON", "Host (Share my screen)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            40, 120, 340, 28, hWnd, (HMENU)ID_RADIO_HOST, NULL, NULL);
        SendMessage(g_hRadioHost, WM_SETFONT, (WPARAM)g_FontBody, TRUE);
        SendMessage(g_hRadioHost, BM_SETCHECK, BST_CHECKED, 0);

        g_hRadioClient = CreateWindowA("BUTTON", "Client (View remote screen)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            40, 158, 340, 28, hWnd, (HMENU)ID_RADIO_CLIENT, NULL, NULL);
        SendMessage(g_hRadioClient, WM_SETFONT, (WPARAM)g_FontBody, TRUE);

        // Session ID label
        g_hLabelIp = CreateWindowA("STATIC", "Join Session ID:",
            WS_CHILD | SS_LEFT,
            40, 205, 340, 20, hWnd, (HMENU)ID_LABEL_IP, NULL, NULL);
        SendMessage(g_hLabelIp, WM_SETFONT, (WPARAM)g_FontBody, TRUE);

        // IP input
        g_hEditIp = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_HostIp,
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            40, 228, 340, 32, hWnd, (HMENU)ID_EDIT_IP, NULL, NULL);
        SendMessage(g_hEditIp, WM_SETFONT, (WPARAM)g_FontBody, TRUE);
        SendMessage(g_hEditIp, EM_SETLIMITTEXT, 63, 0);

        // Launch button  — y=292 gives 24px gap below the IP edit box (edit ends at ~268)
        g_hBtnLaunch = CreateWindowA("BUTTON", "Launch Gupt",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            40, 292, 160, 42, hWnd, (HMENU)ID_BTN_LAUNCH, NULL, NULL);
        SendMessage(g_hBtnLaunch, WM_SETFONT, (WPARAM)g_FontBtn, TRUE);
        g_OldBtnProc = (WNDPROC)SetWindowLongPtrA(g_hBtnLaunch, GWLP_WNDPROC, (LONG_PTR)BtnSubclassProc);

        // Cancel button
        g_hBtnCancel = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            220, 292, 160, 42, hWnd, (HMENU)ID_BTN_CANCEL, NULL, NULL);
        SendMessage(g_hBtnCancel, WM_SETFONT, (WPARAM)g_FontBtn, TRUE);

        // Hide IP controls (Host is default)
        UpdateIpVisibility();
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_BG);
        return (LRESULT)g_BrushBg;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, OPAQUE);
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_INPUT_BG);
        static HBRUSH editBr = CreateSolidBrush(CLR_INPUT_BG);
        return (LRESULT)editBr;
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetBkColor(hdc, CLR_BG);
        return (LRESULT)g_BrushBg;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_BrushBg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);

        // Background
        FillRect(hdc, &rc, g_BrushBg);

        // ── Header gradient bar (top 90px) ────────────────────────────────
        {
            TRIVERTEX vtx[2] = {
                { 0,   0,  (COLOR16)(CLR_ACCENT  >> 0  & 0xFF) << 8, (COLOR16)(CLR_ACCENT  >> 8  & 0xFF) << 8, (COLOR16)(CLR_ACCENT  >> 16 & 0xFF) << 8, 0xFF00 },
                { rc.right, 90, (COLOR16)(CLR_ACCENT2 >> 0  & 0xFF) << 8, (COLOR16)(CLR_ACCENT2 >> 8  & 0xFF) << 8, (COLOR16)(CLR_ACCENT2 >> 16 & 0xFF) << 8, 0xFF00 },
            };
            // RGB macro packs as BGR — fix channel order for TRIVERTEX
            vtx[0].Red   = (COLOR16)(GetRValue(CLR_ACCENT))  << 8;
            vtx[0].Green = (COLOR16)(GetGValue(CLR_ACCENT))  << 8;
            vtx[0].Blue  = (COLOR16)(GetBValue(CLR_ACCENT))  << 8;
            vtx[1].Red   = (COLOR16)(GetRValue(CLR_ACCENT2)) << 8;
            vtx[1].Green = (COLOR16)(GetGValue(CLR_ACCENT2)) << 8;
            vtx[1].Blue  = (COLOR16)(GetBValue(CLR_ACCENT2)) << 8;
            GRADIENT_RECT gr = { 0, 1 };
            GradientFill(hdc, vtx, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        }

        // ── Logo / Title ───────────────────────────────────────────────────
        int titleX = 85;
        DrawBrandingLogo(hdc, 40, 20, 32, RGB(255, 255, 255));

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        SelectObject(hdc, g_FontTitle);
        RECT titleR = { titleX, 18, rc.right - 20, 60 };
        DrawTextA(hdc, "Gupt Remote Desktop", -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // ── Subtitle ───────────────────────────────────────────────────────
        SelectObject(hdc, g_FontSub);
        SetTextColor(hdc, RGB(210, 210, 240));
        RECT subR = { 40, 60, rc.right - 20, 85 };
        DrawTextA(hdc, "Secure, lightweight screen sharing", -1, &subR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // ── Card area ─────────────────────────────────────────────────────
        RECT cardR = { 20, 100, rc.right - 20, 270 };
        DrawRoundedRect(hdc, cardR, 10, CLR_CARD, CLR_BORDER, 1);

        // ── "Select Mode" label ───────────────────────────────────────────
        SelectObject(hdc, g_FontBody);
        SetTextColor(hdc, CLR_SUBTEXT);
        RECT modeR = { 40, 104, 300, 124 };
        DrawTextA(hdc, "SELECT MODE", -1, &modeR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // ── Radio button background highlight ─────────────────────────────
        if (g_LaunchAsHost) {
            RECT hl = { 32, 116, rc.right - 32, 148 };
            DrawRoundedRect(hdc, hl, 6, RGB(38, 38, 58), CLR_BORDER, 1);
        } else {
            RECT hl = { 32, 152, rc.right - 32, 184 };
            DrawRoundedRect(hdc, hl, 6, RGB(38, 38, 58), CLR_BORDER, 1);
        }

        // ── IP section separator ──────────────────────────────────────────
        if (!g_LaunchAsHost) {
            HPEN sepPen = CreatePen(PS_SOLID, 1, CLR_BORDER);
            HPEN oldPen = (HPEN)SelectObject(hdc, sepPen);
            MoveToEx(hdc, 40, 198, NULL);
            LineTo(hdc, rc.right - 40, 198);
            SelectObject(hdc, oldPen);
            DeleteObject(sepPen);
        }

        // ── Status dot ────────────────────────────────────────────────────
        {
            HBRUSH dotBr = CreateSolidBrush(CLR_SUCCESS);
            HPEN   dotPn = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, dotBr);
            SelectObject(hdc, dotPn);
            Ellipse(hdc, rc.right - 26, 14, rc.right - 10, 30);
            DeleteObject(dotBr);
            DeleteObject(dotPn);
        }

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlID == ID_BTN_LAUNCH) {
            HDC hdc = dis->hDC;
            RECT r = dis->rcItem;
            bool pressed = (dis->itemState & 0x0001 /*ODS_SELECT*/) != 0;
            COLORREF bg = (g_BtnLaunchHover || pressed) ? CLR_BTN_HOVR : CLR_ACCENT;
            DrawRoundedRect(hdc, r, 8, bg, bg, 0);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            SelectObject(hdc, g_FontBtn);
            DrawTextA(hdc, "Launch Gupt", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        if (dis->CtlID == ID_BTN_CANCEL) {
            HDC hdc = dis->hDC;
            RECT r = dis->rcItem;
            DrawRoundedRect(hdc, r, 8, CLR_CARD, CLR_BORDER, 1);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, CLR_SUBTEXT);
            SelectObject(hdc, g_FontBtn);
            DrawTextA(hdc, "Cancel", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        if (id == ID_RADIO_HOST) {
            g_LaunchAsHost = true;
            UpdateIpVisibility();
        }
        if (id == ID_RADIO_CLIENT) {
            g_LaunchAsHost = false;
            UpdateIpVisibility();
        }
        if (id == ID_BTN_LAUNCH) {
            if (!g_LaunchAsHost) {
                GetWindowTextA(g_hEditIp, g_HostIp, sizeof(g_HostIp));
                if (strlen(g_HostIp) == 0) {
                    MessageBoxA(hWnd, "Please enter the Session ID.", "Gupt", MB_ICONWARNING | MB_OK);
                    return 0;
                }
            }
            g_Launched = true;
            DestroyWindow(hWnd);
        }
        if (id == ID_BTN_CANCEL) {
            DestroyWindow(hWnd);
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Show the launcher dialog and wait for user choice
// ─────────────────────────────────────────────────────────────────────────────
static bool ShowLauncherDialog(HINSTANCE hInstance) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = LauncherWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = "GuptLauncherClass";
    wc.hIcon         = LoadIconA(hInstance, MAKEINTRESOURCEA(101));
    wc.hbrBackground = CreateSolidBrush(CLR_BG);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassA(&wc);

    // Client area we want: 420 wide × 360 tall (buttons at y=292+42=334, +26px footer padding)
    // AdjustWindowRect adds the caption/border so the client area is exactly what we specify.
    const DWORD dlgStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    int clientW = 420, clientH = 360;
    RECT adjust = { 0, 0, clientW, clientH };
    AdjustWindowRect(&adjust, dlgStyle, FALSE);
    int dlgW = adjust.right  - adjust.left;
    int dlgH = adjust.bottom - adjust.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - dlgW) / 2;
    int y = (screenH - dlgH) / 2;

    HWND hWnd = CreateWindowExA(
        WS_EX_APPWINDOW,
        "GuptLauncherClass",
        "Gupt Remote Desktop",
        dlgStyle,
        x, y, dlgW, dlgH,
        NULL, NULL, hInstance, NULL
    );

    // Override the Launch and Cancel buttons to be owner-drawn
    LONG_PTR style;
    style = GetWindowLongPtrA(g_hBtnLaunch, GWL_STYLE);
    SetWindowLongPtrA(g_hBtnLaunch, GWL_STYLE, style | BS_OWNERDRAW);
    style = GetWindowLongPtrA(g_hBtnCancel, GWL_STYLE);
    SetWindowLongPtrA(g_hBtnCancel, GWL_STYLE, style | BS_OWNERDRAW);

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Cleanup fonts/brushes
    if (g_FontTitle) { DeleteObject(g_FontTitle); g_FontTitle = NULL; }
    if (g_FontSub)   { DeleteObject(g_FontSub);   g_FontSub   = NULL; }
    if (g_FontBody)  { DeleteObject(g_FontBody);  g_FontBody  = NULL; }
    if (g_FontBtn)   { DeleteObject(g_FontBtn);   g_FontBtn   = NULL; }
    if (g_BrushBg)   { DeleteObject(g_BrushBg);   g_BrushBg   = NULL; }
    if (g_BrushCard) { DeleteObject(g_BrushCard); g_BrushCard = NULL; }
    if (g_BrushAccent){ DeleteObject(g_BrushAccent); g_BrushAccent = NULL; }

    return g_Launched;
}

// ─────────────────────────────────────────────────────────────────────────────
//  HOST MODE  (ported from HostAgent/main.cpp)
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_SessionActive{false};

static bool RequestConsent(const std::string& peerIp) {
    std::string msg = "A remote user at " + peerIp + " is requesting to view and control your desktop.\n\nDo you grant permission?";
    int result = MessageBoxA(NULL, msg.c_str(), "Gupt Remote Desktop - Incoming Connection", MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
    return result == IDYES;
}

static void ShowSessionIndicatorThread() {
    while (g_SessionActive) {
        // Flash taskbar or show a toast in production; for now a periodic log.
        Sleep(5000);
    }
}

// Helper: read current host clipboard as UTF-8 string (empty if none/not text)
static std::string GetHostClipboardText() {
    std::string result;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return result;
    if (!OpenClipboard(NULL)) return result;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        wchar_t* wText = (wchar_t*)GlobalLock(hData);
        if (wText) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, NULL, 0, NULL, NULL);
            if (len > 1) {
                result.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, wText, -1, &result[0], len, NULL, NULL);
            }
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return result;
}

static void RunHostMode() {
    SetProcessDPIAware();

    // Self-generate Session ID based on local networking
    std::string localIp = GetLocalIP();
    std::string sessionId = EncodeIP(localIp);

    std::string msg = "Gupt Host is active!\n\nYour Session ID is: " + sessionId + "\nLocal IP: " + localIp + "\n\nShare this ID with the client. The listener is now running on port 8080.";
    MessageBoxA(NULL, msg.c_str(), "Gupt Host Startup", MB_ICONINFORMATION | MB_OK);

    gupt::core::network::TcpServer server(8080);
    gupt::core::input::InputInjector injector;
    gupt::core::capture::ScreenCapturer capturer;

    injector.Initialize();
    capturer.Initialize();

    // Echo-guard: tracks text the host clipboard just received FROM the client.
    // The clipboard watcher thread checks against this to avoid sending it back.
    std::string g_lastReceivedFromClient;
    std::mutex  g_clipMutex; // protects g_lastReceivedFromClient

    server.SetMessageCallback([&](shared::MessageType type, const std::vector<uint8_t>& payload) {
        if (type == shared::MessageType::ConnectRequest) {
            if (RequestConsent("Remote Peer")) {
                shared::ConnectResponse res{true, "Welcome"};
                server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
                g_SessionActive = true;
                std::thread(ShowSessionIndicatorThread).detach();
            } else {
                shared::ConnectResponse res{false, "User Denied Permission"};
                server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
            }
        } else if (g_SessionActive) {
            if (type == shared::MessageType::MouseEvent) {
                auto ev = reinterpret_cast<const shared::MouseEvent*>(payload.data());
                injector.IngestMouseEvent(*ev);
            } else if (type == shared::MessageType::KeyboardEvent) {
                auto ev = reinterpret_cast<const shared::KeyboardEvent*>(payload.data());
                injector.IngestKeyboardEvent(*ev);
            } else if (type == shared::MessageType::ClipboardText) {
                // Client sent clipboard text → set on host clipboard
                if (!payload.empty()) {
                    std::string utf8(reinterpret_cast<const char*>(payload.data()), payload.size());
                    // Store as echo-guard BEFORE setting the clipboard (so watcher sees it first)
                    { std::lock_guard<std::mutex> lk(g_clipMutex); g_lastReceivedFromClient = utf8; }
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
                    if (wlen > 0) {
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(wchar_t));
                        if (hMem) {
                            wchar_t* dst = (wchar_t*)GlobalLock(hMem);
                            if (dst) {
                                MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dst, wlen);
                                GlobalUnlock(hMem);
                                if (OpenClipboard(NULL)) {
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                    CloseClipboard();
                                } else { GlobalFree(hMem); }
                            } else { GlobalFree(hMem); }
                        }
                    }
                }
            } else if (type == shared::MessageType::ClipboardImage) {
                // Client sent clipboard image
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
                g_SessionActive = false;
            }
        }
    });

    if (server.Start()) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

        // ── Host clipboard watcher thread ──────────────────────────────────────
        // Polls every 500ms. When the host copies new text (and it wasn't just
        // received from the client), it sends it to the connected client.
        std::thread clipWatcher([&]() {
            std::string lastSent;
            while (true) {
                Sleep(500);
                if (!g_SessionActive) { lastSent.clear(); continue; }

                std::string current = GetHostClipboardText();
                if (current.empty() || current == lastSent) continue;

                // Echo-guard: skip if this text came from the client
                {
                    std::lock_guard<std::mutex> lk(g_clipMutex);
                    if (current == g_lastReceivedFromClient) continue;
                }

                lastSent = current;
                server.SendRaw(shared::SerializeClipboardText(current));
            }
        });
        clipWatcher.detach();
        ULONGLONG lastSendDuration = 0;
        while (true) {
            if (g_SessionActive) {
                if (lastSendDuration > 66) { Sleep(33); lastSendDuration = 0; continue; }
                ULONGLONG t0 = GetTickCount64();
                std::vector<uint8_t> jpegPixels;
                uint32_t w, h;
                if (capturer.CaptureNextFrameJpeg(jpegPixels, w, h, 85)) {
                    shared::FrameDataHeader header{0, w, h, 32, false, GetTickCount64()};
                    ULONGLONG ts = GetTickCount64();
                    server.SendRaw(shared::SerializeFrame(header, jpegPixels));
                    lastSendDuration = GetTickCount64() - ts;
                }
                ULONGLONG elapsed = GetTickCount64() - t0;
                if (elapsed < 33) Sleep((DWORD)(33 - elapsed));
            } else {
                lastSendDuration = 0;
                Sleep(100);
            }
        }
    }
    server.Stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  CLIENT MODE  (ported from ClientViewer/main.cpp)
// ─────────────────────────────────────────────────────────────────────────────
static bool g_IsConnected = false;
static gupt::core::network::TcpClient g_Client;
// Echo-guard: text most recently received from host — WM_CLIPBOARDUPDATE checks this
// to avoid sending host-originated text back to the host and creating a ping-pong loop
std::string g_clientLastFromHost;
static std::mutex g_FrameMutex;
static std::vector<uint8_t> g_LatestFrame;
static uint32_t g_FrameWidth = 0, g_FrameHeight = 0;
static int g_DestX = 0, g_DestY = 0, g_DestW = 800, g_DestH = 600;
static bool g_SidebarOpen = false;
static int  g_ScreenW = 0, g_ScreenH = 0, g_SidebarX = 0;
static bool g_IsFullscreen = true;
static RECT g_SavedRect = {0, 0, 800, 600};
static int  g_HoveredCard = -1;
static RECT g_TabRect = {}, g_Card1Rect = {}, g_Card2Rect = {};
static RECT g_BackBtnRect = {}, g_CloseBtnRect = {};
static RECT g_MinBtnRect = {}, g_MaxBtnRect = {};
static HDC     g_BackDC  = NULL;
static HBITMAP g_BackBmp = NULL;
static int     g_BackW = 0, g_BackH = 0;

static std::string g_clientLastSent;

LRESULT CALLBACK ClientWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == 2) {
            KillTimer(hWnd, 2);
            // Standalone Direct Decoding
            std::string actualIp = DecodeIP(std::string(g_HostIp));

            if (actualIp.empty()) {
                MessageBoxA(hWnd, "Invalid Session ID format.", "Connection Error", MB_ICONERROR | MB_OK);
                PostQuitMessage(0);
                return 0;
            }

            if (!g_Client.Connect(actualIp, 8080)) {
                MessageBoxA(hWnd, "Discovered Host IP but failed to establish a direct connection.", "Connection Error", MB_ICONERROR | MB_OK);
                PostQuitMessage(0);
            } else {
                g_IsConnected = true;
                shared::ConnectRequest req = {};
                g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectRequest, req));
            }
            return 0;
        }
        if (wParam == 1) {
            int targetX = g_SidebarOpen ? g_ScreenW - 260 : g_ScreenW;
            if (g_SidebarX != targetX) {
                if (g_SidebarX > targetX) { g_SidebarX -= 50; if (g_SidebarX < targetX) g_SidebarX = targetX; }
                else { g_SidebarX += 50; if (g_SidebarX > targetX) g_SidebarX = targetX; }
                InvalidateRect(hWnd, NULL, FALSE);
            }
            if (g_SidebarX == targetX) KillTimer(hWnd, 1);
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        {
            RECT cr; GetClientRect(hWnd, &cr);
            int aW = cr.right, aH = cr.bottom;
            if (aW > 0 && aH > 0 && (aW != g_ScreenW || aH != g_ScreenH)) {
                g_ScreenW = aW; g_ScreenH = aH;
                g_SidebarX = g_SidebarOpen ? g_ScreenW - 260 : g_ScreenW;
            }
        }
        if (g_ScreenW < 1 || g_ScreenH < 1) { EndPaint(hWnd, &ps); break; }

        if (!g_BackDC || g_BackW != g_ScreenW || g_BackH != g_ScreenH) {
            if (g_BackDC) { SelectObject(g_BackDC, GetStockObject(BLACK_BRUSH)); DeleteObject(g_BackBmp); DeleteDC(g_BackDC); }
            g_BackDC  = CreateCompatibleDC(hdc);
            g_BackBmp = CreateCompatibleBitmap(hdc, g_ScreenW, g_ScreenH);
            SelectObject(g_BackDC, g_BackBmp);
            g_BackW = g_ScreenW; g_BackH = g_ScreenH;
        }
        HDC hdcBack = g_BackDC;

        // 1. Remote frame
        {
            std::lock_guard<std::mutex> lock(g_FrameMutex);
            HBRUSH bb = (HBRUSH)GetStockObject(BLACK_BRUSH);
            RECT fr = {0, 0, g_ScreenW, g_ScreenH};
            FillRect(hdcBack, &fr, bb);
            if (!g_LatestFrame.empty() && g_FrameWidth > 0 && g_FrameHeight > 0) {
                float ha = (float)g_FrameWidth / g_FrameHeight;
                float ca = (g_ScreenH > 0) ? (float)g_ScreenW / g_ScreenH : 1.f;
                if (ca > ha) { g_DestH = g_ScreenH; g_DestW = (int)(g_ScreenH * ha); g_DestX = (g_ScreenW - g_DestW) / 2; g_DestY = 0; }
                else          { g_DestW = g_ScreenW; g_DestH = (int)(g_ScreenW / ha); g_DestX = 0; g_DestY = (g_ScreenH - g_DestH) / 2; }
                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = g_FrameWidth;
                bmi.bmiHeader.biHeight = -(int)g_FrameHeight;
                bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(hdcBack, HALFTONE); SetBrushOrgEx(hdcBack, 0, 0, NULL);
                StretchDIBits(hdcBack, g_DestX, g_DestY, g_DestW, g_DestH, 0, 0, g_FrameWidth, g_FrameHeight, g_LatestFrame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            }
        }

        // 2. Sidebar
        if (g_SidebarX < g_ScreenW) {
            HDC hdcSb = CreateCompatibleDC(hdcBack);
            int sbw = g_ScreenW - g_SidebarX;
            HBITMAP hbmSb = CreateCompatibleBitmap(hdcBack, sbw, g_ScreenH);
            HBITMAP hbmSbOld = (HBITMAP)SelectObject(hdcSb, hbmSb);
            RECT sbRect = {0, 0, sbw, g_ScreenH};
            HBRUSH wb = CreateSolidBrush(RGB(255,255,255)); FillRect(hdcSb, &sbRect, wb); DeleteObject(wb);

            RECT headR = {0,0,sbw,52};
            HBRUSH hb = CreateSolidBrush(RGB(247,247,247)); FillRect(hdcSb, &headR, hb); DeleteObject(hb);
            HPEN bp = CreatePen(PS_SOLID,1,RGB(220,220,220)); HPEN op = (HPEN)SelectObject(hdcSb,bp);
            MoveToEx(hdcSb,0,52,NULL); LineTo(hdcSb,sbw,52);
            SelectObject(hdcSb,op); DeleteObject(bp);
            SetBkMode(hdcSb,TRANSPARENT); SetTextColor(hdcSb,RGB(40,40,40));
            HFONT hf = CreateFontA(20,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,"Arial");
            HFONT of = (HFONT)SelectObject(hdcSb,hf);
            g_BackBtnRect  = {g_SidebarX+10,8,g_SidebarX+46,44};
            g_CloseBtnRect = {g_ScreenW-46,8,g_ScreenW-10,44};
            RECT br={10,8,46,44}; DrawTextA(hdcSb,"<",-1,&br,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            RECT cr={sbw-46,8,sbw-10,44}; DrawTextA(hdcSb,"X",-1,&cr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if (!g_IsFullscreen) {
                g_MaxBtnRect = {g_ScreenW-90,8,g_ScreenW-50,44};
                RECT mr={sbw-90,8,sbw-50,44}; DrawTextA(hdcSb,"[]",-1,&mr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                g_MinBtnRect = {g_ScreenW-134,8,g_ScreenW-94,44};
                RECT nr={sbw-134,8,sbw-94,44}; DrawTextA(hdcSb,"_",-1,&nr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            } else { g_MinBtnRect={}; g_MaxBtnRect={}; }
            RECT tr={0,0,sbw,52}; DrawTextA(hdcSb,"Gupt",-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(hdcSb,of); DeleteObject(hf);

            int cW=(sbw-42)/2;
            if(cW>0){
                g_Card1Rect={g_SidebarX+14,66,g_SidebarX+14+cW,66+88};
                g_Card2Rect={g_SidebarX+14+cW+14,66,g_SidebarX+14+cW+14+cW,66+88};
                HBRUSH nb=CreateSolidBrush(RGB(255,255,255)), hov=CreateSolidBrush(RGB(245,245,245));
                HPEN cp=CreatePen(PS_SOLID,1,RGB(220,220,220)); op=(HPEN)SelectObject(hdcSb,cp);
                SelectObject(hdcSb,g_HoveredCard==1?hov:nb); RoundRect(hdcSb,14,66,14+cW,66+88,8,8);
                SelectObject(hdcSb,g_HoveredCard==2?hov:nb); RoundRect(hdcSb,14+cW+14,66,14+cW+14+cW,66+88,8,8);
                SelectObject(hdcSb,op); DeleteObject(cp);
                HFONT cf=CreateFontA(14,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,"Arial");
                of=(HFONT)SelectObject(hdcSb,cf);
                SetTextColor(hdcSb,RGB(220,50,50));
                RECT r1={14,66,14+cW,66+88}; DrawTextA(hdcSb,"Disconnect",-1,&r1,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                SetTextColor(hdcSb,RGB(50,100,220));
                RECT r2={14+cW+14,66,14+cW+14+cW,66+88};
                DrawTextA(hdcSb,g_IsFullscreen?"Exit Full-screen":"Full-screen",-1,&r2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                SelectObject(hdcSb,of); DeleteObject(cf);
                DeleteObject(nb); DeleteObject(hov);
            }
            BitBlt(hdcBack,g_SidebarX,0,sbw,g_ScreenH,hdcSb,0,0,SRCCOPY);
            SelectObject(hdcSb,hbmSbOld); DeleteObject(hbmSb); DeleteDC(hdcSb);
        }

        // 3. Tab
        g_TabRect = {g_SidebarX-22, g_ScreenH/2-28, g_SidebarX, g_ScreenH/2+28};
        {
            HBRUSH tb=CreateSolidBrush(RGB(45,45,45)); HBRUSH otb=(HBRUSH)SelectObject(hdcBack,tb);
            HPEN tn=CreatePen(PS_NULL,0,0); HPEN otn=(HPEN)SelectObject(hdcBack,tn);
            RoundRect(hdcBack,g_TabRect.left,g_TabRect.top,g_TabRect.right+10,g_TabRect.bottom,12,12);
            SelectObject(hdcBack,otn); DeleteObject(tn);
            SelectObject(hdcBack,otb); DeleteObject(tb);
            SetBkMode(hdcBack,TRANSPARENT); SetTextColor(hdcBack,RGB(255,255,255));
            HFONT tf=CreateFontA(16,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,"Arial");
            HFONT otf=(HFONT)SelectObject(hdcBack,tf);
            RECT tr={g_TabRect.left,g_TabRect.top,g_TabRect.right,g_TabRect.bottom};
            DrawTextA(hdcBack,g_SidebarOpen?">":"<",-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(hdcBack,otf); DeleteObject(tf);
        }

        BitBlt(hdc,0,0,g_ScreenW,g_ScreenH,hdcBack,0,0,SRCCOPY);
        EndPaint(hWnd,&ps);
        break;
    }

    case WM_MOUSEMOVE: {
        int x=(short)LOWORD(lParam), y=(short)HIWORD(lParam);
        if(g_SidebarOpen&&x>=g_SidebarX){
            POINT pt={x,y}; int hov=-1;
            if(PtInRect(&g_Card1Rect,pt)) hov=1;
            else if(PtInRect(&g_Card2Rect,pt)) hov=2;
            if(hov!=g_HoveredCard){g_HoveredCard=hov;InvalidateRect(hWnd,NULL,FALSE);}
        }
        if(g_SidebarOpen) return 0;
        if(g_IsConnected&&x>=g_DestX&&x<g_DestX+g_DestW&&y>=g_DestY&&y<g_DestY+g_DestH){
            shared::MouseEvent me={};
            me.normalizedX=(float)(x-g_DestX)/g_DestW;
            me.normalizedY=(float)(y-g_DestY)/g_DestH;
            me.wheelDelta=0; me.buttonId=255; me.isDown=false;
            g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent,me));
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if(g_SidebarOpen) return 0;
        int x=(short)LOWORD(lParam), y=(short)HIWORD(lParam);
        POINT pt={x,y}; ScreenToClient(hWnd,&pt);
        if(g_IsConnected&&pt.x>=g_DestX&&pt.x<g_DestX+g_DestW&&pt.y>=g_DestY&&pt.y<g_DestY+g_DestH){
            shared::MouseEvent me={};
            me.normalizedX=(float)(pt.x-g_DestX)/g_DestW;
            me.normalizedY=(float)(pt.y-g_DestY)/g_DestH;
            me.wheelDelta=(short)HIWORD(wParam); me.buttonId=255; me.isDown=false;
            g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent,me));
        }
        break;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        int x=(short)LOWORD(lParam), y=(short)HIWORD(lParam); POINT pt={x,y};
        if(message==WM_LBUTTONDOWN){
            if(PtInRect(&g_TabRect,pt)){g_SidebarOpen=!g_SidebarOpen;SetTimer(hWnd,1,10,NULL);return 0;}
            if(g_SidebarOpen&&x>=g_SidebarX){
                if(PtInRect(&g_BackBtnRect,pt)||PtInRect(&g_CloseBtnRect,pt)){g_SidebarOpen=false;SetTimer(hWnd,1,10,NULL);return 0;}
                if(!g_IsFullscreen&&PtInRect(&g_MinBtnRect,pt)){ShowWindow(hWnd,SW_MINIMIZE);g_SidebarOpen=false;SetTimer(hWnd,1,10,NULL);return 0;}
                if(!g_IsFullscreen&&PtInRect(&g_MaxBtnRect,pt)){ShowWindow(hWnd,IsZoomed(hWnd)?SW_RESTORE:SW_MAXIMIZE);g_SidebarOpen=false;SetTimer(hWnd,1,10,NULL);return 0;}
                if(PtInRect(&g_Card1Rect,pt)){g_Client.Disconnect();PostQuitMessage(0);return 0;}
                if(PtInRect(&g_Card2Rect,pt)){
                    g_IsFullscreen=!g_IsFullscreen;
                                if(g_IsFullscreen){GetWindowRect(hWnd,&g_SavedRect);SetWindowLongPtrA(hWnd,GWL_STYLE,WS_POPUP|WS_VISIBLE);SetWindowLongPtrA(hWnd,GWL_EXSTYLE,WS_EX_TOOLWINDOW);SetWindowPos(hWnd,HWND_TOP,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_SHOWWINDOW|SWP_FRAMECHANGED);}
                    else{SetWindowLongPtrA(hWnd,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE);SetWindowLongPtrA(hWnd,GWL_EXSTYLE,WS_EX_APPWINDOW);SetWindowPos(hWnd,HWND_NOTOPMOST,g_SavedRect.left,g_SavedRect.top,g_SavedRect.right-g_SavedRect.left,g_SavedRect.bottom-g_SavedRect.top,SWP_SHOWWINDOW|SWP_FRAMECHANGED);}
                    g_SidebarOpen=false; SetTimer(hWnd,1,10,NULL); return 0;
                }
                return 0;
            } else if(g_SidebarOpen&&x<g_SidebarX){g_SidebarOpen=false;SetTimer(hWnd,1,10,NULL);}
        }
        if(g_SidebarOpen) return 0;
        if(g_IsConnected&&x>=g_DestX&&x<g_DestX+g_DestW&&y>=g_DestY&&y<g_DestY+g_DestH){
            shared::MouseEvent me={};
            me.normalizedX=(float)(x-g_DestX)/g_DestW;
            me.normalizedY=(float)(y-g_DestY)/g_DestH;
            if(message==WM_LBUTTONDOWN||message==WM_LBUTTONUP) me.buttonId=0; else me.buttonId=1;
            me.isDown=(message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN); me.wheelDelta=0;
            g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::MouseEvent,me));
        }
        break;
    }
    case WM_CHANGECBCHAIN:
    case WM_CLIPBOARDUPDATE: {
        // Clipboard changed on this client machine — grab the text and send it to the host
        // but SKIP if this change was triggered by us setting clipboard data received FROM the host
        if (g_IsConnected && OpenClipboard(hWnd)) {
            // Handle Text
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* wText = (wchar_t*)GlobalLock(hData);
                    if (wText) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, NULL, 0, NULL, NULL);
                        if (len > 1) {
                            std::string utf8(len - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, wText, -1, &utf8[0], len, NULL, NULL);
                            if (utf8 != g_clientLastFromHost && utf8 != g_clientLastSent) {
                                g_clientLastSent = utf8;
                                g_Client.SendRaw(shared::SerializeClipboardText(utf8));
                            }
                        }
                        GlobalUnlock(hData);
                    }
                }
            }
            // Handle Image (DIB)
            static DWORD s_lastImageSeq = 0;
            DWORD currentSeq = GetClipboardSequenceNumber();
            if (currentSeq != s_lastImageSeq && IsClipboardFormatAvailable(CF_DIB)) {
                HANDLE hData = GetClipboardData(CF_DIB);
                if (hData) {
                    size_t size = GlobalSize(hData);
                    void* ptr = GlobalLock(hData);
                    if (ptr) {
                        std::vector<uint8_t> dib(size);
                        std::memcpy(dib.data(), ptr, size);
                        GlobalUnlock(hData);
                        g_Client.SendRaw(shared::SerializeClipboardImage(dib));
                        s_lastImageSeq = currentSeq;
                    }
                }
            }
            CloseClipboard();
        }
        break;
    }
    case WM_KEYDOWN: case WM_KEYUP: {
        if(wParam==VK_F11&&message==WM_KEYDOWN){
            g_IsFullscreen=!g_IsFullscreen;
            if(g_IsFullscreen){GetWindowRect(hWnd,&g_SavedRect);SetWindowLongPtrA(hWnd,GWL_STYLE,WS_POPUP|WS_VISIBLE);SetWindowLongPtrA(hWnd,GWL_EXSTYLE,WS_EX_TOOLWINDOW);SetWindowPos(hWnd,HWND_TOP,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_SHOWWINDOW|SWP_FRAMECHANGED);}
            else{SetWindowLongPtrA(hWnd,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE);SetWindowLongPtrA(hWnd,GWL_EXSTYLE,WS_EX_APPWINDOW);SetWindowPos(hWnd,HWND_NOTOPMOST,g_SavedRect.left,g_SavedRect.top,g_SavedRect.right-g_SavedRect.left,g_SavedRect.bottom-g_SavedRect.top,SWP_SHOWWINDOW|SWP_FRAMECHANGED);}
            g_SidebarOpen=false; SetTimer(hWnd,1,10,NULL); return 0;
        }
        if(g_SidebarOpen) return 0;
        if(g_IsConnected){
            shared::KeyboardEvent ke={}; ke.virtualKey=(uint8_t)wParam; ke.isDown=(message==WM_KEYDOWN);
            g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::KeyboardEvent,ke));
        }
        break;
    }
    case WM_SIZE: {
        int nW=LOWORD(lParam), nH=HIWORD(lParam);
        if(nW<1||nH<1) break;
        g_ScreenW=nW; g_ScreenH=nH;
        g_SidebarX=g_SidebarOpen?g_ScreenW-260:g_ScreenW;
        g_BackW=0; g_BackH=0;
        InvalidateRect(hWnd,NULL,FALSE);
        break;
    }
    case WM_CLOSE:
        g_Client.Disconnect(); PostQuitMessage(0); return 0;
    case WM_ACTIVATE:
        if(LOWORD(wParam)!=WA_INACTIVE&&g_IsFullscreen)
            SetWindowPos(hWnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
            if (g_SidebarOpen || pt.x < g_SidebarX && PtInRect(&g_TabRect, pt)) {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                return TRUE;
            }
            return TRUE; // No cursor for remote area
        }
        return DefWindowProcA(hWnd, message, wParam, lParam);
    }
    case WM_DESTROY:
        PostQuitMessage(0); break;
    default:
        return DefWindowProcA(hWnd,message,wParam,lParam);
    }
    return 0;
}

static void RunClientMode(HINSTANCE hInstance, const char* hostIp) {
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = ClientWndProc;
    wc.hInstance   = hInstance;
    wc.lpszClassName = "GuptClientClass";
    wc.hCursor     = NULL; // Manual cursor for Lens/Host compatibility
    RegisterClassA(&wc);

    g_ScreenW = GetSystemMetrics(SM_CXSCREEN);
    g_ScreenH = GetSystemMetrics(SM_CYSCREEN);
    g_SidebarX = g_ScreenW;

    HWND hWnd = CreateWindowExA(WS_EX_APPWINDOW, "GuptClientClass", "Gupt Client Viewer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, g_ScreenW, g_ScreenH, NULL, NULL, hInstance, NULL);
    g_IsFullscreen = false;
    ShowWindow(hWnd, SW_MAXIMIZE);
    UpdateWindow(hWnd);

    // Register for clipboard change notifications — fires WM_CLIPBOARDUPDATE when user copies
    AddClipboardFormatListener(hWnd);

    g_Client.SetMessageCallback([hWnd](shared::MessageType type, const std::vector<uint8_t>& payload) {
        thread_local bool t_comInit = (CoInitializeEx(NULL,COINIT_MULTITHREADED),true);
        if (type == shared::MessageType::ConnectResponse) {
            auto res = reinterpret_cast<const shared::ConnectResponse*>(payload.data());
            if (res->accepted) {
                g_IsConnected = true;
                SetWindowTextA(hWnd, "Gupt - Connected");
            }
        } else if (type == shared::MessageType::FrameData) {
            auto hd = reinterpret_cast<const shared::FrameDataHeader*>(payload.data());
            size_t off = sizeof(shared::FrameDataHeader);
            if (payload.size() > off) {
                IStream* pStream = SHCreateMemStream(payload.data()+off,(UINT)(payload.size()-off));
                if (pStream) {
                    IWICImagingFactory* pFac=NULL;
                    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&pFac)))) {
                        IWICBitmapDecoder* pDec=NULL;
                        if (SUCCEEDED(pFac->CreateDecoderFromStream(pStream,NULL,WICDecodeMetadataCacheOnDemand,&pDec))) {
                            IWICBitmapFrameDecode* pFrm=NULL;
                            if (SUCCEEDED(pDec->GetFrame(0,&pFrm))) {
                                IWICFormatConverter* pConv=NULL;
                                if (SUCCEEDED(pFac->CreateFormatConverter(&pConv))) {
                                    if (SUCCEEDED(pConv->Initialize(pFrm,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,NULL,0.f,WICBitmapPaletteTypeCustom))) {
                                        UINT w,h; pConv->GetSize(&w,&h);
                                        std::vector<uint8_t> dec(w*h*4);
                                        if (SUCCEEDED(pConv->CopyPixels(NULL,w*4,(UINT)dec.size(),dec.data()))) {
                                            std::lock_guard<std::mutex> lk(g_FrameMutex);
                                            g_FrameWidth=w; g_FrameHeight=h;
                                            g_LatestFrame=std::move(dec);
                                            InvalidateRect(hWnd,NULL,FALSE);
                                        }
                                    }
                                    pConv->Release();
                                }
                                pFrm->Release();
                            }
                            pDec->Release();
                        }
                        pFac->Release();
                    }
                    pStream->Release();
                }
            }
        } else if (type == shared::MessageType::ClipboardText) {
            // Host sent us clipboard text — set it on the client clipboard
            if (!payload.empty()) {
                std::string utf8(reinterpret_cast<const char*>(payload.data()), payload.size());
                g_clientLastFromHost = utf8; // echo-guard: must happen before SetClipboardData
                int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
                if (wlen > 0) {
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(wchar_t));
                    if (hMem) {
                        wchar_t* dst = (wchar_t*)GlobalLock(hMem);
                        if (dst) {
                            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dst, wlen);
                            GlobalUnlock(hMem);
                            if (OpenClipboard(NULL)) {
                                EmptyClipboard();
                                SetClipboardData(CF_UNICODETEXT, hMem);
                                CloseClipboard();
                            } else { GlobalFree(hMem); }
                        } else { GlobalFree(hMem); }
                    }
                }
            }
        } else if (type == shared::MessageType::ClipboardImage) {
            // Host sent us an image result
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
        }
    });

    if (g_Client.Connect(hostIp, 8080)) {
        shared::ConnectRequest req={};
        std::strncpy(req.sessionId,"DEMO_SESSION_123",sizeof(req.sessionId));
        std::strncpy(req.authenticationToken,"SECURE_TOKEN",sizeof(req.authenticationToken));
        g_Client.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectRequest,req));
    } else {
        MessageBoxA(hWnd, ("Could not connect to host: " + std::string(hostIp) + "\nMake sure the host is running Gupt and port 8080 is reachable.").c_str(), "Gupt - Connection Failed", MB_ICONERROR | MB_OK);
        PostQuitMessage(1);
    }

    MSG msg;
    while (GetMessage(&msg,NULL,0,0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    RemoveClipboardFormatListener(hWnd);
    g_Client.Disconnect();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry Point
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (!ShowLauncherDialog(hInstance)) return 0; // User cancelled

    if (g_LaunchAsHost) {
        RunHostMode();
    } else {
        std::string inputId = std::string(g_HostIp);
        std::string actualIp = DecodeIP(inputId);

        if (actualIp.empty()) {
            MessageBoxA(NULL, "Invalid Session ID format.", "Gupt Error", MB_ICONERROR | MB_OK);
            return 1;
        }

        RunClientMode(hInstance, actualIp.c_str());
    }

    CoUninitialize();
    return 0;
}
