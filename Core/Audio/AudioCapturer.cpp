#include "AudioCapturer.h"
#include <cmath>
#include <algorithm>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace gupt {
namespace core {
namespace audio {

AudioCapturer::AudioCapturer() {}

AudioCapturer::~AudioCapturer() {
    Stop();
}

bool AudioCapturer::Start(AudioCallback callback) {
    if (m_Running.load()) return true;
    m_Callback = callback;
    m_Running.store(true);
    m_CaptureThread = std::thread(&AudioCapturer::CaptureLoop, this);
    return true;
}

void AudioCapturer::Stop() {
    m_Running.store(false);
    if (m_CaptureThread.joinable()) {
        m_CaptureThread.join();
    }
    if (m_AudioClient) {
        m_AudioClient->Stop();
        m_AudioClient->Release();
        m_AudioClient = nullptr;
    }
    if (m_CaptureClient) {
        m_CaptureClient->Release();
        m_CaptureClient = nullptr;
    }
    if (m_WaveFormat) {
        CoTaskMemFree(m_WaveFormat);
        m_WaveFormat = nullptr;
    }
}

void AudioCapturer::CaptureLoop() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    timeBeginPeriod(1); // 1ms high-precision scheduling for zero-jitter audio streaming

    while (m_Running.load()) {
        IMMDeviceEnumerator* pEnumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
        if (FAILED(hr) || !pEnumerator) {
            Sleep(500);
            continue;
        }

        IMMDevice* pDevice = nullptr;
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        pEnumerator->Release();
        if (FAILED(hr) || !pDevice) {
            Sleep(500);
            continue;
        }

        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_AudioClient);
        pDevice->Release();
        if (FAILED(hr) || !m_AudioClient) {
            Sleep(500);
            continue;
        }

        hr = m_AudioClient->GetMixFormat(&m_WaveFormat);
        if (FAILED(hr) || !m_WaveFormat) {
            if (m_AudioClient) { m_AudioClient->Release(); m_AudioClient = nullptr; }
            Sleep(500);
            continue;
        }

        // Initialize in loopback shared mode (100ms buffer)
        REFERENCE_TIME hnsRequestedDuration = 1000000; // 100ms
        hr = m_AudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, hnsRequestedDuration, 0, m_WaveFormat, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(m_WaveFormat); m_WaveFormat = nullptr;
            m_AudioClient->Release(); m_AudioClient = nullptr;
            Sleep(500);
            continue;
        }

        hr = m_AudioClient->GetService(IID_PPV_ARGS(&m_CaptureClient));
        if (FAILED(hr) || !m_CaptureClient) {
            CoTaskMemFree(m_WaveFormat); m_WaveFormat = nullptr;
            m_AudioClient->Release(); m_AudioClient = nullptr;
            Sleep(500);
            continue;
        }

        hr = m_AudioClient->Start();
        if (FAILED(hr)) {
            m_CaptureClient->Release(); m_CaptureClient = nullptr;
            CoTaskMemFree(m_WaveFormat); m_WaveFormat = nullptr;
            m_AudioClient->Release(); m_AudioClient = nullptr;
            Sleep(500);
            continue;
        }

        uint32_t sampleRate = m_WaveFormat->nSamplesPerSec;
        uint16_t srcChannels = m_WaveFormat->nChannels;
        bool isFloat = false;
        if (m_WaveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        } else if (m_WaveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto pExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_WaveFormat);
            if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                isFloat = true;
            }
        }

        std::vector<int16_t> stereoPcmBuffer;

        while (m_Running.load()) {
            Sleep(15);
            if (!m_Running.load()) break;

            UINT32 packetLength = 0;
            hr = m_CaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                // Device invalidated or unplugged: break to outer recovery loop
                break;
            }

            while (SUCCEEDED(hr) && packetLength > 0 && m_Running.load()) {
                BYTE* pData = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;
                hr = m_CaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr) && pData && numFramesAvailable > 0) {
                    bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    stereoPcmBuffer.resize(numFramesAvailable * 2);

                    if (isSilent) {
                        std::fill(stereoPcmBuffer.begin(), stereoPcmBuffer.end(), 0);
                    } else if (isFloat) {
                        const float* pFloat = reinterpret_cast<const float*>(pData);
                        for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                            const float* frame = pFloat + (f * srcChannels);
                            float left = 0.0f, right = 0.0f;
                            if (srcChannels == 1) {
                                left = right = frame[0];
                            } else if (srcChannels == 2) {
                                left = frame[0]; right = frame[1];
                            } else if (srcChannels >= 6) {
                                float center = frame[2] * 0.707f;
                                left  = (frame[0] + center + frame[4] * 0.707f) * 0.5f;
                                right = (frame[1] + center + frame[5] * 0.707f) * 0.5f;
                            } else {
                                left = frame[0]; right = (srcChannels > 1) ? frame[1] : frame[0];
                            }
                            if (left > 1.0f) left = 1.0f - expf(-left); else if (left < -1.0f) left = -1.0f + expf(left);
                            if (right > 1.0f) right = 1.0f - expf(-right); else if (right < -1.0f) right = -1.0f + expf(right);

                            stereoPcmBuffer[f * 2]     = static_cast<int16_t>(left * 32767.0f);
                            stereoPcmBuffer[f * 2 + 1] = static_cast<int16_t>(right * 32767.0f);
                        }
                    } else if (m_WaveFormat->wBitsPerSample == 16) {
                        const int16_t* pIn16 = reinterpret_cast<const int16_t*>(pData);
                        for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                            const int16_t* frame = pIn16 + (f * srcChannels);
                            stereoPcmBuffer[f * 2]     = frame[0];
                            stereoPcmBuffer[f * 2 + 1] = (srcChannels > 1) ? frame[1] : frame[0];
                        }
                    } else if (m_WaveFormat->wBitsPerSample == 32) {
                        const int32_t* pIn32 = reinterpret_cast<const int32_t*>(pData);
                        for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                            const int32_t* frame = pIn32 + (f * srcChannels);
                            stereoPcmBuffer[f * 2]     = static_cast<int16_t>(frame[0] >> 16);
                            stereoPcmBuffer[f * 2 + 1] = static_cast<int16_t>((srcChannels > 1 ? frame[1] : frame[0]) >> 16);
                        }
                    }

                    if (m_Callback) {
                        m_Callback(sampleRate, 2, 16, stereoPcmBuffer.data(), stereoPcmBuffer.size());
                    }

                    m_CaptureClient->ReleaseBuffer(numFramesAvailable);
                }
                hr = m_CaptureClient->GetNextPacketSize(&packetLength);
            }
        }

        // Clean up before auto-reconnecting on the next cycle
        if (m_CaptureClient) { m_CaptureClient->Release(); m_CaptureClient = nullptr; }
        if (m_AudioClient) { m_AudioClient->Stop(); m_AudioClient->Release(); m_AudioClient = nullptr; }
        if (m_WaveFormat) { CoTaskMemFree(m_WaveFormat); m_WaveFormat = nullptr; }
    }

    timeEndPeriod(1);
    CoUninitialize();
}

} // namespace audio
} // namespace core
} // namespace gupt
