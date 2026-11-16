#pragma once
#include <cstdint>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace gupt {
namespace core {
namespace audio {

using AudioCallback = std::function<void(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample, const int16_t* samples, size_t sampleCount)>;

class AudioCapturer {
public:
    AudioCapturer();
    ~AudioCapturer();

    bool Start(AudioCallback callback);
    void Stop();
    bool IsRunning() const { return m_Running.load(); }

private:
    void CaptureLoop();

    std::atomic<bool> m_Running{false};
    std::thread m_CaptureThread;
    AudioCallback m_Callback;

    IAudioClient* m_AudioClient = nullptr;
    IAudioCaptureClient* m_CaptureClient = nullptr;
    WAVEFORMATEX* m_WaveFormat = nullptr;
    UINT32 m_BufferFrameCount = 0;
    int m_SilentStreak = 0;
};

} // namespace audio
} // namespace core
} // namespace gupt
