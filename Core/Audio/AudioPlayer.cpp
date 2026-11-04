#include "AudioPlayer.h"
#include <cstring>

namespace gupt {
namespace core {
namespace audio {

AudioPlayer::AudioPlayer() {}

AudioPlayer::~AudioPlayer() {
    Stop();
}

bool AudioPlayer::Initialize(uint32_t sampleRate, uint16_t channels, uint16_t bits) {
    std::lock_guard<std::mutex> lk(m_Mtx);

    if (m_hWaveOut && m_CurrentSampleRate == sampleRate && m_CurrentChannels == channels) {
        return true;
    }

    if (m_hWaveOut) {
        CleanupBuffers();
        waveOutReset(m_hWaveOut);
        waveOutClose(m_hWaveOut);
        m_hWaveOut = nullptr;
    }

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = bits;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    MMRESULT res = waveOutOpen(&m_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR) {
        m_hWaveOut = nullptr;
        return false;
    }

    m_CurrentSampleRate = sampleRate;
    m_CurrentChannels = channels;

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        std::memset(&m_Slots[i].hdr, 0, sizeof(WAVEHDR));
        m_Slots[i].data.resize(16384);
        m_Slots[i].inFlight = false;
    }
    m_CurrentSlot = 0;

    return true;
}

void AudioPlayer::PlaySamples(const int16_t* samples, size_t sampleCount, uint32_t sampleRate, uint16_t channels) {
    if (!samples || sampleCount == 0) return;

    if (sampleRate > 0 && channels > 0 &&
        (sampleRate != m_CurrentSampleRate || channels != m_CurrentChannels)) {
        Initialize(sampleRate, channels, 16);
    }

    std::lock_guard<std::mutex> lk(m_Mtx);
    if (!m_hWaveOut) {
        if (!Initialize(m_CurrentSampleRate ? m_CurrentSampleRate : 48000,
                        m_CurrentChannels ? m_CurrentChannels : 2, 16)) {
            return;
        }
    }

    // Reclaim finished buffers
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (m_Slots[i].inFlight && (m_Slots[i].hdr.dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(m_hWaveOut, &m_Slots[i].hdr, sizeof(WAVEHDR));
            m_Slots[i].inFlight = false;
        }
    }

    // Find the next available non-in-flight slot (never force-unprepare an active buffer!)
    int targetSlot = -1;
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        int idx = (m_CurrentSlot + i) % NUM_BUFFERS;
        if (!m_Slots[idx].inFlight) {
            targetSlot = idx;
            break;
        }
    }

    if (targetSlot == -1) {
        // Driver buffer queue is full (>400ms): skip to prevent pop/click noise
        return;
    }

    m_CurrentSlot = (targetSlot + 1) % NUM_BUFFERS;
    BufferSlot& slot = m_Slots[targetSlot];

    size_t byteCount = sampleCount * sizeof(int16_t);
    if (slot.data.size() < byteCount) {
        slot.data.resize(byteCount);
    }
    std::memcpy(slot.data.data(), samples, byteCount);

    std::memset(&slot.hdr, 0, sizeof(WAVEHDR));
    slot.hdr.lpData = reinterpret_cast<LPSTR>(slot.data.data());
    slot.hdr.dwBufferLength = static_cast<DWORD>(byteCount);

    MMRESULT rPrep = waveOutPrepareHeader(m_hWaveOut, &slot.hdr, sizeof(WAVEHDR));
    if (rPrep == MMSYSERR_NOERROR) {
        MMRESULT rWrite = waveOutWrite(m_hWaveOut, &slot.hdr, sizeof(WAVEHDR));
        if (rWrite == MMSYSERR_NOERROR) {
            slot.inFlight = true;
        } else {
            waveOutUnprepareHeader(m_hWaveOut, &slot.hdr, sizeof(WAVEHDR));
        }
    }
}

void AudioPlayer::CleanupBuffers() {
    if (!m_hWaveOut) return;
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (m_Slots[i].inFlight) {
            waveOutUnprepareHeader(m_hWaveOut, &m_Slots[i].hdr, sizeof(WAVEHDR));
            m_Slots[i].inFlight = false;
        }
    }
}

void AudioPlayer::Stop() {
    std::lock_guard<std::mutex> lk(m_Mtx);
    if (m_hWaveOut) {
        waveOutReset(m_hWaveOut);
        CleanupBuffers();
        waveOutClose(m_hWaveOut);
        m_hWaveOut = nullptr;
    }
}

} // namespace audio
} // namespace core
} // namespace gupt
