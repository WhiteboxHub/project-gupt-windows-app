#pragma once

#include <windows.h>
#include <unordered_set>
#include "../../Shared/Protocol.h"

namespace gupt {
namespace core {
namespace input {

class InputInjector {
public:
    InputInjector();
    ~InputInjector();

    void Initialize();
    void IngestKeyboardEvent(const gupt::shared::KeyboardEvent& ev);
    void IngestMouseEvent(const gupt::shared::MouseEvent& ev);
    
    // Release all currently pressed keys and buttons
    void ReleaseAll();

private:
    int m_ScreenWidth = 1920;
    int m_ScreenHeight = 1080;
    
    std::unordered_set<uint16_t> m_PressedKeys;
    uint8_t m_PressedButtons = 0; // Bitmask: bit 0 = Left, 1 = Right, 2 = Middle
};

} // namespace input
} // namespace core
} // namespace gupt
