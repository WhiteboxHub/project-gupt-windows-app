#pragma once

#include <windows.h>
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

private:
    int m_ScreenWidth = 0;
    int m_ScreenHeight = 0;
};

} // namespace input
} // namespace core
} // namespace gupt
