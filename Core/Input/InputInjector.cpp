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
        m_PressedKeys.erase(ev.virtualKey);
    } else {
        m_PressedKeys.insert(ev.virtualKey);
    }
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::IngestMouseEvent(const gupt::shared::MouseEvent& ev) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(ev.normalizedX * 65535.0f);
    input.mi.dy = static_cast<LONG>(ev.normalizedY * 65535.0f);
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

    if (ev.wheelDelta != 0) {
        input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        input.mi.mouseData = ev.wheelDelta;
    }

    if (ev.buttonId != 255) {
        if (ev.isDown) {
            if (ev.buttonId == 0) { input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN; m_PressedButtons |= 1; }
            else if (ev.buttonId == 1) { input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN; m_PressedButtons |= 2; }
            else if (ev.buttonId == 2) { input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN; m_PressedButtons |= 4; }
        } else {
            if (ev.buttonId == 0) { input.mi.dwFlags |= MOUSEEVENTF_LEFTUP; m_PressedButtons &= ~1; }
            else if (ev.buttonId == 1) { input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP; m_PressedButtons &= ~2; }
            else if (ev.buttonId == 2) { input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP; m_PressedButtons &= ~4; }
        }
    }

    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::ReleaseAll() {
    std::vector<INPUT> inputs;
    
    // Release keys
    for (uint16_t key : m_PressedKeys) {
        INPUT input = {0};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    m_PressedKeys.clear();
    
    // Release mouse buttons
    if (m_PressedButtons != 0) {
        INPUT input = {0};
        input.type = INPUT_MOUSE;
        if (m_PressedButtons & 1) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        if (m_PressedButtons & 2) input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
        if (m_PressedButtons & 4) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
        inputs.push_back(input);
        m_PressedButtons = 0;
    }
    
    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

} // namespace input
} // namespace core
} // namespace gupt
