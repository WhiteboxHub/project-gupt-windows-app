#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <mutex>

namespace gupt {
namespace core {
namespace video {

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    bool Initialize(uint32_t width, uint32_t height);
    void Shutdown();

    // Decodes H.264 Annex-B NAL units into a 32-bit BGRA frame
    bool DecodeNal(const uint8_t* nalData, size_t nalSize,
                   std::vector<uint8_t>& outBgraPixels, uint32_t& outW, uint32_t& outH);

    bool IsInitialized() const { return m_Initialized; }
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }

private:
    void ConvertNv12ToBgra(const uint8_t* nv12Y, const uint8_t* nv12Uv,
                           uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride);

    Microsoft::WRL::ComPtr<IMFTransform> m_Decoder;

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_OutputsRgb32 = false;
    bool m_Initialized = false;
    std::recursive_mutex m_Mtx;
};

} // namespace video
} // namespace core
} // namespace gupt
