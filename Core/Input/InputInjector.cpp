#include "InputInjector.h"

namespace gupt {
namespace core {
namespace input {

InputInjector::InputInjector() {}
InputInjector::~InputInjector() {}

void InputInjector::Initialize() {
    m_ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
    m_ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void InputInjector::IngestKeyboardEvent(const gupt::shared::KeyboardEvent& ev) {
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = ev.virtualKey;
    if (!ev.isDown) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::IngestMouseEvent(const gupt::shared::MouseEvent& ev) {
    if (m_ScreenWidth == 0 || m_ScreenHeight == 0) {
        m_ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
        m_ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    int screenX = static_cast<int>(ev.normalizedX * m_ScreenWidth);
    int screenY = static_cast<int>(ev.normalizedY * m_ScreenHeight);

    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(ev.normalizedX * 65535.0f);
    input.mi.dy = static_cast<LONG>(ev.normalizedY * 65535.0f);
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK;

    if (ev.wheelDelta != 0) {
        input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        input.mi.mouseData = ev.wheelDelta;
    }

    if (ev.buttonId != 255) {
        if (ev.isDown) {
            if (ev.buttonId == 0) input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
            else if (ev.buttonId == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
            else if (ev.buttonId == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
        } else {
            if (ev.buttonId == 0) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
            else if (ev.buttonId == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
            else if (ev.buttonId == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
        }
    }

    UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent == 0) {
        // Fallback for elevated window/UIPI edge cases: directly force hardware cursor position
        SetCursorPos(screenX, screenY);
        if (ev.buttonId != 255) {
            DWORD flags = 0;
            if (ev.isDown) {
                if (ev.buttonId == 0) flags = MOUSEEVENTF_LEFTDOWN;
                else if (ev.buttonId == 1) flags = MOUSEEVENTF_RIGHTDOWN;
                else if (ev.buttonId == 2) flags = MOUSEEVENTF_MIDDLEDOWN;
            } else {
                if (ev.buttonId == 0) flags = MOUSEEVENTF_LEFTUP;
                else if (ev.buttonId == 1) flags = MOUSEEVENTF_RIGHTUP;
                else if (ev.buttonId == 2) flags = MOUSEEVENTF_MIDDLEUP;
            }
            if (flags != 0) mouse_event(flags, 0, 0, 0, 0);
        }
    }
}

} // namespace input
} // namespace core
} // namespace gupt
