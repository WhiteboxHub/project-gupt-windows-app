#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
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
    void Reset();
    bool CaptureNextFrame(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);
    bool CaptureNextFrameGdi(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);
    bool GetLastDirtyBounds(uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) const;
    bool CaptureNextFrameJpeg(std::vector<uint8_t>& outJpeg, uint32_t& outWidth, uint32_t& outHeight, int quality);
    bool CaptureRegionJpeg(const std::vector<uint8_t>& rawPixels, uint32_t fullWidth, uint32_t fullHeight, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>& outJpeg, int quality, bool force444 = false);
    void ReleaseFrame();

private:
    void CleanupGdi();

    ComPtr<ID3D11Device> m_Device;
    ComPtr<ID3D11DeviceContext> m_DeviceContext;
    ComPtr<IDXGIOutputDuplication> m_DeskDupl;
    ComPtr<ID3D11Texture2D> m_StagingTexture;
    uint32_t m_StagingWidth = 0;
    uint32_t m_StagingHeight = 0;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    RECT m_LastDirtyBounds = { 0, 0, 0, 0 };
    bool m_HasLastDirtyBounds = false;
    std::vector<uint8_t> m_CachedFrame;
    POINT m_LastCursorPos = { -1, -1 };
    uint64_t m_LastForceCaptureTick = 0;

    // Fast persistent GDI fallback
    HDC m_hGdiScreenDC = NULL;
    HDC m_hGdiMemDC = NULL;
    HBITMAP m_hGdiBitmap = NULL;
    HBITMAP m_hGdiOldBitmap = NULL;
    void* m_pGdiBits = NULL;
    int m_GdiWidth = 0;
    int m_GdiHeight = 0;
};

} // namespace capture
} // namespace core
} // namespace gupt
