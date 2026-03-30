#include "InputInjector.h"

namespace gupt {
namespace core {
namespace input {

InputInjector::InputInjector() {}
InputInjector::~InputInjector() {}

void InputInjector::Initialize() {
    m_ScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_ScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
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
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(ev.normalizedX * 65535.0f);
    input.mi.dy = static_cast<LONG>(ev.normalizedY * 65535.0f);
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK;

    if (ev.wheelDelta != 0) {
        input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        input.mi.mouseData = ev.wheelDelta;
    }

    if (ev.isDown) {
        if (ev.buttonId == 0) input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        else if (ev.buttonId == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
        else if (ev.buttonId == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
    } else {
        if (ev.buttonId == 0) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        else if (ev.buttonId == 1) input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
        else if (ev.buttonId == 2) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
    }

    SendInput(1, &input, sizeof(INPUT));
}

} // namespace input
} // namespace core
} // namespace gupt
