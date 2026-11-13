#pragma once

#include <windows.h>
#include <atomic>
#include "../../Shared/Protocol.h"

namespace gupt {
namespace core {
namespace input {

class InputInjector {
public:
    InputInjector();
    ~InputInjector();

    void Initialize();
    void SetScreenDimensions(int width, int height);
    void SetMonitorBounds(int originX, int originY, int width, int height);
    void MapNormalizedPoint(float nx, float ny, int& outX, int& outY) const;
    void IngestKeyboardEvent(const gupt::shared::KeyboardEvent& ev);
    void IngestMouseEvent(const gupt::shared::MouseEvent& ev);

private:
    std::atomic<int> m_OriginX{0};
    std::atomic<int> m_OriginY{0};
    std::atomic<int> m_ScreenWidth{0};
    std::atomic<int> m_ScreenHeight{0};
};

} // namespace input
} // namespace core
} // namespace gupt
