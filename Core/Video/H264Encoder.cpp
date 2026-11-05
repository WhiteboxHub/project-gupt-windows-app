#include "H264Encoder.h"
#include <wmcodecdsp.h>
#include <iostream>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace gupt {
namespace core {
namespace video {

H264Encoder::H264Encoder() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
}

H264Encoder::~H264Encoder() {
    Shutdown();
    MFShutdown();
}

void H264Encoder::Shutdown() {
    std::lock_guard<std::recursive_mutex> lk(m_Mtx);
    m_CodecApi.Reset();
    m_Encoder.Reset();
    m_Nv12Buffer.clear();
    m_Initialized = false;
    m_FrameIndex = 0;
}

void H264Encoder::RequestKeyframe() {
    m_ForceNextKeyframe = true;
}

void H264Encoder::SetBitrate(uint32_t bitrateBps) {
    std::lock_guard<std::recursive_mutex> lk(m_Mtx);
    m_Bitrate = bitrateBps;
    if (m_CodecApi) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = bitrateBps;
        m_CodecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    }
}

bool H264Encoder::Initialize(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps) {
    std::lock_guard<std::recursive_mutex> lk(m_Mtx);
    Shutdown();

    m_Width = (width + 1) & ~1;
    m_Height = (height + 1) & ~1;
    m_Fps = (fps > 0) ? fps : 60;
    m_Bitrate = (bitrateBps > 0) ? bitrateBps : 12000000;

    size_t nv12Size = (size_t)m_Width * m_Height * 3 / 2;
    m_Nv12Buffer.resize(nv12Size);

    HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_Encoder));
    if (FAILED(hr) || !m_Encoder) {
        std::cerr << "  [H264Encoder] Could not instantiate H.264 MFT encoder (hr=0x" << std::hex << hr << std::dec << ")\n";
        return false;
    }

    // 2. Set low-latency real-time streaming properties
    m_Encoder.As(&m_CodecApi);
    if (m_CodecApi) {
        VARIANT var;
        VariantInit(&var);

        // Low latency: zero buffering, immediate NAL generation
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        m_CodecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);

        // Constant Bitrate (CBR) for predictable network streaming
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_CBR;
        m_CodecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

        // Set target bitrate (6 Mbps for pristine 1080p desktop clarity)
        var.vt = VT_UI4;
        var.ulVal = m_Bitrate;
        m_CodecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

        // Quality target (0-100)
        var.vt = VT_UI4;
        var.ulVal = 90;
        m_CodecApi->SetValue(&CODECAPI_AVEncCommonQuality, &var);

        // Enable CABAC entropy coding for sharp text and fine UI details
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        m_CodecApi->SetValue(&CODECAPI_AVEncH264CABACEnable, &var);

        // GOP size: periodic keyframe every 120 frames (2 seconds at 60fps)
        var.vt = VT_UI4;
        var.ulVal = m_Fps;
        m_CodecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

        // Frame rate adaptation mode
        var.vt = VT_UI4;
        var.ulVal = eAVEncAdaptiveMode_None;
        m_CodecApi->SetValue(&CODECAPI_AVEncAdaptiveMode, &var);
    }

    // 3. Configure Output Media Type (H.264 High Profile)
    Microsoft::WRL::ComPtr<IMFMediaType> outMediaType;
    if (FAILED(MFCreateMediaType(&outMediaType))) return false;
    outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outMediaType->SetUINT32(MF_MT_AVG_BITRATE, m_Bitrate);
    MFSetAttributeSize(outMediaType.Get(), MF_MT_FRAME_SIZE, m_Width, m_Height);
    MFSetAttributeRatio(outMediaType.Get(), MF_MT_FRAME_RATE, m_Fps, 1);
    outMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

    if (FAILED(m_Encoder->SetOutputType(0, outMediaType.Get(), 0))) {
        // Fallback to Base profile if Main profile is not supported on older MFT drivers
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        if (FAILED(m_Encoder->SetOutputType(0, outMediaType.Get(), 0))) {
            std::cerr << "  [H264Encoder] SetOutputType failed (hr=0x" << std::hex << hr << std::dec << ")\n";
            return false;
        }
    }

    // 4. Configure Input Media Type (NV12)
    Microsoft::WRL::ComPtr<IMFMediaType> inMediaType;
    if (FAILED(MFCreateMediaType(&inMediaType))) return false;
    inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, m_Width, m_Height);
    MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, m_Fps, 1);
    inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (FAILED(m_Encoder->SetInputType(0, inMediaType.Get(), 0))) {
        std::cerr << "  [H264Encoder] SetInputType failed (hr=0x" << std::hex << hr << std::dec << ")\n";
        return false;
    }

    // 5. Notify MFT that streaming is beginning
    m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_Initialized = true;
    m_ForceNextKeyframe = true;
    m_FrameIndex = 0;
    return true;
}

void H264Encoder::ConvertBgraToNv12(const uint8_t* bgra, uint8_t* nv12Y, uint8_t* nv12Uv,
                                    uint32_t width, uint32_t height) {
    size_t yIndex = 0;
    size_t uvIndex = 0;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row0 = bgra + (y * (size_t)width * 4);
        const uint8_t* row1 = (y + 1 < height) ? bgra + ((y + 1) * (size_t)width * 4) : row0;
        bool isEvenRow = ((y & 1) == 0);

        for (uint32_t x = 0; x < width; ++x) {
            int b0 = row0[x * 4 + 0];
            int g0 = row0[x * 4 + 1];
            int r0 = row0[x * 4 + 2];

            // BT.709 Limited Range: Y in [16,235], Cb/Cr in [16,240]
            int Y_val = 16 + ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8);
            nv12Y[yIndex++] = static_cast<uint8_t>(Y_val < 16 ? 16 : (Y_val > 235 ? 235 : Y_val));

            // Anti-aliased 2x2 box filtering for chroma (eliminates text edge noise & color fringing)
            if (isEvenRow && ((x & 1) == 0)) {
                uint32_t nextX = (x + 1 < width) ? (x + 1) : x;
                int b1 = row0[nextX * 4 + 0];
                int g1 = row0[nextX * 4 + 1];
                int r1 = row0[nextX * 4 + 2];

                int b2 = row1[x * 4 + 0];
                int g2 = row1[x * 4 + 1];
                int r2 = row1[x * 4 + 2];

                int b3 = row1[nextX * 4 + 0];
                int g3 = row1[nextX * 4 + 1];
                int r3 = row1[nextX * 4 + 2];

                int r_avg = (r0 + r1 + r2 + r3 + 2) >> 2;
                int g_avg = (g0 + g1 + g2 + g3 + 2) >> 2;
                int b_avg = (b0 + b1 + b2 + b3 + 2) >> 2;

                int u = 128 + ((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8);
                int v = 128 + ((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8);
                nv12Uv[uvIndex++] = static_cast<uint8_t>(u < 16 ? 16 : (u > 240 ? 240 : u));
                nv12Uv[uvIndex++] = static_cast<uint8_t>(v < 16 ? 16 : (v > 240 ? 240 : v));
            }
        }
    }
}

bool H264Encoder::EncodeBgraFrame(const uint8_t* bgra, uint32_t width, uint32_t height,
                                  std::vector<uint8_t>& outNalUnits, bool forceKeyframe) {
    std::lock_guard<std::recursive_mutex> lk(m_Mtx);
    if (!m_Initialized || !bgra || width == 0 || height == 0) return false;

    if (width != m_Width || height != m_Height) {
        if (!Initialize(width, height, m_Fps, m_Bitrate)) return false;
    }

    outNalUnits.clear();

    // 1. Convert BGRA to NV12
    uint8_t* pY = m_Nv12Buffer.data();
    uint8_t* pUv = pY + ((size_t)m_Width * m_Height);
    ConvertBgraToNv12(bgra, pY, pUv, m_Width, m_Height);

    // 2. Wrap NV12 buffer into IMFMediaBuffer and IMFSample
    Microsoft::WRL::ComPtr<IMFMediaBuffer> inMediaBuffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(m_Nv12Buffer.size()), &inMediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = NULL;
    DWORD maxLen = 0, curLen = 0;
    if (SUCCEEDED(inMediaBuffer->Lock(&pDst, &maxLen, &curLen))) {
        memcpy(pDst, m_Nv12Buffer.data(), m_Nv12Buffer.size());
        inMediaBuffer->Unlock();
        inMediaBuffer->SetCurrentLength(static_cast<DWORD>(m_Nv12Buffer.size()));
    } else {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample> inSample;
    if (FAILED(MFCreateSample(&inSample))) return false;
    inSample->AddBuffer(inMediaBuffer.Get());

    LONGLONG sampleTime = (m_FrameIndex * 10000000LL) / m_Fps;
    LONGLONG sampleDur = 10000000LL / m_Fps;
    inSample->SetSampleTime(sampleTime);
    inSample->SetSampleDuration(sampleDur);
    m_FrameIndex++;

    // Force keyframe if requested
    if (forceKeyframe || m_ForceNextKeyframe) {
        m_ForceNextKeyframe = false;
        if (m_CodecApi) {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_UI4;
            var.ulVal = 1;
            m_CodecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
        }
    }

    // 3. Process Input
    hr = m_Encoder->ProcessInput(0, inSample.Get(), 0);
    if (FAILED(hr)) return false;

    // 4. Drain Output
    MFT_OUTPUT_STREAM_INFO streamInfo = {0};
    m_Encoder->GetOutputStreamInfo(0, &streamInfo);

    while (true) {
        Microsoft::WRL::ComPtr<IMFSample> outSample;
        if (FAILED(MFCreateSample(&outSample))) break;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
        DWORD bufSize = (streamInfo.cbSize > 0) ? streamInfo.cbSize : (1024 * 1024);
        if (FAILED(MFCreateMemoryBuffer(bufSize, &outBuffer))) break;

        outSample->AddBuffer(outBuffer.Get());

        MFT_OUTPUT_DATA_BUFFER outputData = {0};
        outputData.pSample = outSample.Get();

        DWORD status = 0;
        hr = m_Encoder->ProcessOutput(0, 1, &outputData, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break; // Finished draining
        }
        if (FAILED(hr)) {
            break;
        }

        // Extract H.264 compressed NAL bytes
        IMFMediaBuffer* pContigBuffer = NULL;
        if (SUCCEEDED(outSample->ConvertToContiguousBuffer(&pContigBuffer)) && pContigBuffer) {
            BYTE* pData = NULL;
            DWORD dataLen = 0;
            if (SUCCEEDED(pContigBuffer->Lock(&pData, NULL, &dataLen)) && pData && dataLen > 0) {
                outNalUnits.insert(outNalUnits.end(), pData, pData + dataLen);
                pContigBuffer->Unlock();
            }
            pContigBuffer->Release();
        }

        if (outputData.pEvents) outputData.pEvents->Release();
    }

    return !outNalUnits.empty();
}

} // namespace video
} // namespace core
} // namespace gupt

