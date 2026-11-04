#pragma once
#include <windows.h>
#include <strmif.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <mutex>

namespace gupt {
namespace core {
namespace video {

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    bool Initialize(uint32_t width, uint32_t height, uint32_t fps = 60, uint32_t bitrateBps = 2500000);
    void Shutdown();

    // Encodes a BGRA 32-bit frame into H.264 Annex-B NAL units
    bool EncodeBgraFrame(const uint8_t* bgra, uint32_t width, uint32_t height,
                         std::vector<uint8_t>& outNalUnits, bool forceKeyframe = false);

    void RequestKeyframe();
    void SetBitrate(uint32_t bitrateBps);

    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    bool IsInitialized() const { return m_Initialized; }

private:
    void ConvertBgraToNv12(const uint8_t* bgra, uint8_t* nv12Y, uint8_t* nv12Uv,
                           uint32_t width, uint32_t height);

    Microsoft::WRL::ComPtr<IMFTransform> m_Encoder;
    Microsoft::WRL::ComPtr<ICodecAPI> m_CodecApi;

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_AlignedHeight = 0;
    uint32_t m_Fps = 60;
    uint32_t m_Bitrate = 2500000;
    uint64_t m_FrameIndex = 0;
    bool m_Initialized = false;
    bool m_ForceNextKeyframe = true;

    std::vector<uint8_t> m_Nv12Buffer;
    std::recursive_mutex m_Mtx;
};

} // namespace video
} // namespace core
} // namespace gupt
