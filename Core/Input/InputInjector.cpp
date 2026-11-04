#include "InputInjector.h"

namespace gupt {
namespace core {
namespace input {

static void SyncInputDesktop() {
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
    m_ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
    m_ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void InputInjector::SetScreenDimensions(int width, int height) {
    if (width > 0 && height > 0) {
        m_ScreenWidth = width;
        m_ScreenHeight = height;
    }
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
        ev.virtualKey == VK_DIVIDE || ev.virtualKey == VK_NUMLOCK) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }

    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::IngestMouseEvent(const gupt::shared::MouseEvent& ev) {
    SyncInputDesktop();

    if (m_ScreenWidth <= 0 || m_ScreenHeight <= 0) {
        m_ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
        m_ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    float nx = ev.normalizedX < 0.0f ? 0.0f : (ev.normalizedX > 1.0f ? 1.0f : ev.normalizedX);
    float ny = ev.normalizedY < 0.0f ? 0.0f : (ev.normalizedY > 1.0f ? 1.0f : ev.normalizedY);

    int targetX = static_cast<int>(nx * (m_ScreenWidth > 0 ? (m_ScreenWidth - 1) : 1) + 0.5f);
    int targetY = static_cast<int>(ny * (m_ScreenHeight > 0 ? (m_ScreenHeight - 1) : 1) + 0.5f);

    // 1. Instantly reposition cursor on host desktop
    SetCursorPos(targetX, targetY);

    // 2. Query virtual screen metrics for multi-monitor / DPI-scaled absolute SendInput
    int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vWidth <= 0) vWidth = GetSystemMetrics(SM_CXSCREEN);
    if (vHeight <= 0) vHeight = GetSystemMetrics(SM_CYSCREEN);

    LONG absX = static_cast<LONG>(((targetX - vLeft) * 65536.0f) / vWidth);
    LONG absY = static_cast<LONG>(((targetY - vTop) * 65536.0f) / vHeight);

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
        mi.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        mi.mi.mouseData = ev.wheelDelta;
    }

    SendInput(1, &mi, sizeof(INPUT));
}

} // namespace input
} // namespace core
} // namespace gupt
