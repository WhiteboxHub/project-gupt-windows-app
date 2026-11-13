#include <cmath>
#include "InputInjector.h"

namespace gupt {
namespace core {
namespace input {

static void SyncInputDesktop() {
    static uint64_t s_lastSync = 0;
    uint64_t now = GetTickCount64();
    if (now - s_lastSync < 1000) return;
    s_lastSync = now;

    HDESK hCurrent = GetThreadDesktop(GetCurrentThreadId());
    HDESK hInput = OpenInputDesktop(0, FALSE, DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL | DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD | DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS);
    if (!hInput) {
        hInput = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
    }
    if (hInput) {
        if (hInput != hCurrent) {
            SetThreadDesktop(hInput);
        }
        CloseDesktop(hInput);
    }
}

InputInjector::InputInjector() {}
InputInjector::~InputInjector() {}

void InputInjector::Initialize() {
    SyncInputDesktop();
    m_OriginX = 0;
    m_OriginY = 0;
    m_ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
    m_ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void InputInjector::SetScreenDimensions(int width, int height) {
    SetMonitorBounds(0, 0, width, height);
}

void InputInjector::SetMonitorBounds(int originX, int originY, int width, int height) {
    if (width > 0 && height > 0) {
        m_OriginX = originX;
        m_OriginY = originY;
        m_ScreenWidth = width;
        m_ScreenHeight = height;
    }
}

void InputInjector::MapNormalizedPoint(float nx, float ny, int& outX, int& outY) const {
    float clampedX = (nx < 0.0f) ? 0.0f : (nx > 1.0f ? 1.0f : nx);
    float clampedY = (ny < 0.0f) ? 0.0f : (ny > 1.0f ? 1.0f : ny);
    int w = m_ScreenWidth.load();
    int h = m_ScreenHeight.load();
    if (w <= 0) w = GetSystemMetrics(SM_CXSCREEN);
    if (h <= 0) h = GetSystemMetrics(SM_CYSCREEN);
    outX = m_OriginX.load() + static_cast<int>(clampedX * (w > 1 ? (w - 1) : 1) + 0.5f);
    outY = m_OriginY.load() + static_cast<int>(clampedY * (h > 1 ? (h - 1) : 1) + 0.5f);
}

void InputInjector::IngestKeyboardEvent(const gupt::shared::KeyboardEvent& ev) {
    SyncInputDesktop();

    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = ev.virtualKey;
    input.ki.wScan = (WORD)MapVirtualKey(ev.virtualKey, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = (ev.isDown ? 0 : KEYEVENTF_KEYUP);

    if ((ev.virtualKey >= VK_PRIOR && ev.virtualKey <= VK_DOWN) ||
        (ev.virtualKey >= VK_INSERT && ev.virtualKey <= VK_DELETE) ||
        ev.virtualKey == VK_LWIN || ev.virtualKey == VK_RWIN ||
        ev.virtualKey == VK_RCONTROL || ev.virtualKey == VK_RMENU ||
        ev.virtualKey == VK_DIVIDE || ev.virtualKey == VK_NUMLOCK) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::IngestMouseEvent(const gupt::shared::MouseEvent& ev) {
    SyncInputDesktop();

    if (m_ScreenWidth.load() <= 0 || m_ScreenHeight.load() <= 0) {
        m_ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
        m_ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    int targetX = 0;
    int targetY = 0;
    MapNormalizedPoint(ev.normalizedX, ev.normalizedY, targetX, targetY);

    // 1. Instantly reposition cursor on active monitor desktop coordinates
    SetCursorPos(targetX, targetY);

    // 2. Query virtual screen metrics for multi-monitor / DPI-scaled absolute SendInput
    int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vWidth <= 0) vWidth = GetSystemMetrics(SM_CXSCREEN);
    if (vHeight <= 0) vHeight = GetSystemMetrics(SM_CYSCREEN);

    LONG absX = static_cast<LONG>(((targetX - vLeft) * 65536.0f) / (vWidth > 1 ? vWidth : 1));
    LONG absY = static_cast<LONG>(((targetY - vTop) * 65536.0f) / (vHeight > 1 ? vHeight : 1));

    // 3. Dispatch absolute mouse input event with MOVE flag so Windows routes to target window
    INPUT mi = {0};
    mi.type = INPUT_MOUSE;
    mi.mi.dx = absX;
    mi.mi.dy = absY;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    if (ev.buttonId != 255) {
        if (ev.isDown) {
            if (ev.buttonId == 0) mi.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
            else if (ev.buttonId == 1) mi.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
            else if (ev.buttonId == 2) mi.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
        } else {
            if (ev.buttonId == 0) mi.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
            else if (ev.buttonId == 1) mi.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
            else if (ev.buttonId == 2) mi.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
        }
    }

    if (ev.wheelDelta != 0) {
        // Direct instantaneous 1:1 scrolling:
        // Use native WHEEL_DELTA (multiples of 120 or touchpad deltas) without arbitrary downscaling
        // so Windows and browser accumulators trigger scroll steps immediately on the first physical notch.
        INPUT wheelInput = {0};
        wheelInput.type = INPUT_MOUSE;
        wheelInput.mi.dwFlags = (ev.buttonId == 254) ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        wheelInput.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(ev.wheelDelta));
        wheelInput.mi.dx = 0;
        wheelInput.mi.dy = 0;
        SendInput(1, &wheelInput, sizeof(INPUT));
    } else {
        SendInput(1, &mi, sizeof(INPUT));
    }
}

} // namespace input
} // namespace core
} // namespace gupt
