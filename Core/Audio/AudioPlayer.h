#pragma once
#include <cstdint>
#include <vector>
#include <mutex>
#include <windows.h>
#include <mmsystem.h>

namespace gupt {
namespace core {
namespace audio {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    bool Initialize(uint32_t sampleRate = 48000, uint16_t channels = 2, uint16_t bits = 16);
    void PlaySamples(const int16_t* samples, size_t sampleCount, uint32_t sampleRate = 0, uint16_t channels = 0);
    void Stop();

private:
    void CleanupBuffers();

    HWAVEOUT m_hWaveOut = nullptr;
    uint32_t m_CurrentSampleRate = 0;
    uint16_t m_CurrentChannels = 0;
    std::mutex m_Mtx;

    static const int NUM_BUFFERS = 16;
    struct BufferSlot {
        WAVEHDR hdr;
        std::vector<uint8_t> data;
        bool inFlight = false;
    };
    BufferSlot m_Slots[NUM_BUFFERS];
    int m_CurrentSlot = 0;
};

} // namespace audio
} // namespace core
} // namespace gupt
