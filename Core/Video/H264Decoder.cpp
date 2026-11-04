#include "H264Decoder.h"
#include <wmcodecdsp.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace gupt {
namespace core {
namespace video {

H264Decoder::H264Decoder() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
}

H264Decoder::~H264Decoder() {
    Shutdown();
    MFShutdown();
}

void H264Decoder::Shutdown() {
    std::lock_guard<std::recursive_mutex> lk(m_Mtx);
    m_Decoder.Reset();
    m_Initialized = false;
}

bool H264Decoder::Initialize(uint32_t width, uint32_t height) {
    std::lock_guard<std::recursive_mutex> lk(m_Mtx);
    Shutdown();

    m_Width = (width + 1) & ~1;
    m_Height = height;
    m_AlignedHeight = ((height + 15) / 16) * 16;

    // 1. Create H.264 Decoder MFT
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_Decoder));
    if (FAILED(hr) || !m_Decoder) {
        return false;
    }

    // Enable low latency on decoder
    Microsoft::WRL::ComPtr<IMFAttributes> pAttrs;
    if (SUCCEEDED(m_Decoder->GetAttributes(&pAttrs)) && pAttrs) {
        pAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    // 2. Set Input Type (H.264)
    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, m_Width, m_Height);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (FAILED(m_Decoder->SetInputType(0, inType.Get(), 0))) {
        return false;
    }

    // 3. Try to configure Output Type (prefer RGB32, fallback to NV12)
    m_OutputsRgb32 = false;
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    if (SUCCEEDED(MFCreateMediaType(&outType))) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, m_Width, m_Height);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        if (SUCCEEDED(m_Decoder->SetOutputType(0, outType.Get(), 0))) {
            m_OutputsRgb32 = true;
        }
    }

    if (!m_OutputsRgb32) {
        // Fallback to NV12 output
        outType.Reset();
        if (FAILED(MFCreateMediaType(&outType))) return false;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, m_Width, m_Height);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        if (FAILED(m_Decoder->SetOutputType(0, outType.Get(), 0))) {
            return false;
        }
    }

    m_Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_Initialized = true;
    return true;
}

void H264Decoder::ConvertNv12ToBgra(const uint8_t* nv12Y, const uint8_t* nv12Uv,
                                    uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride) {
    // Fast integer BT.601 NV12 to BGRA color conversion with stride awareness
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* yRow = nv12Y + (y * stride);
        const uint8_t* uvRow = nv12Uv + ((y >> 1) * stride);
        uint8_t* dstRow = bgra + (y * width * 4);

        for (uint32_t x = 0; x < width; ++x) {
            int Y = static_cast<int>(yRow[x]) - 16;
            if (Y < 0) Y = 0;

            size_t uvIdx = (x & ~1);
            int U = static_cast<int>(uvRow[uvIdx]) - 128;
            int V = static_cast<int>(uvRow[uvIdx + 1]) - 128;

            int C = 298 * Y;
            int r = (C + 409 * V + 128) >> 8;
            int g = (C - 100 * U - 208 * V + 128) >> 8;
            int b = (C + 516 * U + 128) >> 8;

            dstRow[0] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
            dstRow[1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
            dstRow[2] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            dstRow[3] = 0xFF; // Alpha
            dstRow += 4;
        }
    }
}

bool H264Decoder::DecodeNal(const uint8_t* nalData, size_t nalSize,
                            std::vector<uint8_t>& outBgraPixels, uint32_t& outW, uint32_t& outH) {
    std::lock_guard<std::recursive_mutex> lk(m_Mtx);
    if (!m_Initialized || !nalData || nalSize == 0) return false;

    // 1. Create input sample
    Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nalSize), &inBuffer))) return false;

    BYTE* pDst = NULL;
    if (SUCCEEDED(inBuffer->Lock(&pDst, NULL, NULL))) {
        memcpy(pDst, nalData, nalSize);
        inBuffer->Unlock();
        inBuffer->SetCurrentLength(static_cast<DWORD>(nalSize));
    } else {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample> inSample;
    if (FAILED(MFCreateSample(&inSample))) return false;
    inSample->AddBuffer(inBuffer.Get());

    HRESULT hr = m_Decoder->ProcessInput(0, inSample.Get(), 0);
    if (FAILED(hr)) return false;

    // 2. Drain output
    MFT_OUTPUT_STREAM_INFO streamInfo = {0};
    m_Decoder->GetOutputStreamInfo(0, &streamInfo);

    Microsoft::WRL::ComPtr<IMFSample> outSample;
    if (FAILED(MFCreateSample(&outSample))) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
    DWORD bufSize = (streamInfo.cbSize > 0) ? streamInfo.cbSize : (m_Width * (m_AlignedHeight > 0 ? m_AlignedHeight : m_Height) * 4);
    if (FAILED(MFCreateMemoryBuffer(bufSize, &outBuffer))) return false;
    outSample->AddBuffer(outBuffer.Get());

    MFT_OUTPUT_DATA_BUFFER outputData = {0};
    outputData.pSample = outSample.Get();

    DWORD status = 0;
    hr = m_Decoder->ProcessOutput(0, 1, &outputData, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        Microsoft::WRL::ComPtr<IMFMediaType> newType;
        if (SUCCEEDED(m_Decoder->GetOutputAvailableType(0, 0, &newType))) {
            m_Decoder->SetOutputType(0, newType.Get(), 0);
        }
        hr = m_Decoder->ProcessOutput(0, 1, &outputData, &status);
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }

    IMFMediaBuffer* pContigBuffer = NULL;
    if (SUCCEEDED(outSample->ConvertToContiguousBuffer(&pContigBuffer)) && pContigBuffer) {
        Microsoft::WRL::ComPtr<IMF2DBuffer> p2D;
        BYTE* pDecoded = NULL;
        LONG pitch = 0;
        bool locked2D = false;

        if (SUCCEEDED(pContigBuffer->QueryInterface(IID_PPV_ARGS(&p2D))) && p2D) {
            if (SUCCEEDED(p2D->Lock2D(&pDecoded, &pitch)) && pDecoded) {
                locked2D = true;
            }
        }

        DWORD decodedLen = 0;
        if (!locked2D) {
            pContigBuffer->Lock(&pDecoded, NULL, &decodedLen);
            pitch = m_Width;
        }

        if (pDecoded) {
            outW = m_Width;
            outH = m_Height;
            size_t targetBgraBytes = (size_t)m_Width * m_Height * 4;
            outBgraPixels.resize(targetBgraBytes);

            uint32_t stride = (pitch > 0) ? static_cast<uint32_t>(pitch) : m_Width;
            uint32_t alignH = (m_AlignedHeight > 0) ? m_AlignedHeight : ((m_Height + 15) / 16) * 16;
            const uint8_t* pY = pDecoded;
            const uint8_t* pUv = pY + ((size_t)stride * alignH);
            ConvertNv12ToBgra(pY, pUv, outBgraPixels.data(), m_Width, m_Height, stride);

            if (locked2D) {
                p2D->Unlock2D();
            } else {
                pContigBuffer->Unlock();
            }
        }
        pContigBuffer->Release();
    }

    if (outputData.pEvents) outputData.pEvents->Release();
    return !outBgraPixels.empty();
}

} // namespace video
} // namespace core
} // namespace gupt
