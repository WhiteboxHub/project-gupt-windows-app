#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include "../../Shared/Protocol.h"

namespace gupt {
namespace core {
namespace capture {

using Microsoft::WRL::ComPtr;

class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();

    bool Initialize();
    bool CaptureNextFrame(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);
    bool CaptureNextFrameJpeg(std::vector<uint8_t>& outJpeg, uint32_t& outWidth, uint32_t& outHeight, int quality);
    bool CaptureRegionJpeg(const std::vector<uint8_t>& rawPixels, uint32_t fullWidth, uint32_t fullHeight, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>& outJpeg, int quality);
    void ReleaseFrame();

private:
    ComPtr<ID3D11Device> m_Device;
    ComPtr<ID3D11DeviceContext> m_DeviceContext;
    ComPtr<IDXGIOutputDuplication> m_DeskDupl;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
};

} // namespace capture
} // namespace core
} // namespace gupt
