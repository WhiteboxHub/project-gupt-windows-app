#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace gupt {
namespace shared {

enum class MessageType : uint8_t {
    ConnectRequest = 1,
    ConnectResponse = 2,
    FrameData = 3,
    MouseEvent = 4,
    KeyboardEvent = 5,
    Heartbeat = 6,
    Disconnect = 7,
    ClipboardText = 8,   // UTF-8 clipboard text sync
    ClipboardImage = 9,  // Binary clipboard Image (BMP/DIB) sync
    RefreshRequest = 10, // Client request to reload / force full keyframe refresh
    FrameAck = 11,       // Client ACK to pace frame delivery and prevent bufferbloat
    AudioData = 12,      // High-fidelity PCM stereo/mono audio stream
    P2PCandidate = 13,   // Direct P2P NAT candidate endpoint exchange
    SwitchMonitor = 14,  // Request host to switch active capture monitor
    SecureDesktopNotification = 15, // Host notifying client that UAC / Secure Desktop is active
    KeyExchange = 16,    // Ephemeral ECDH P-256 X9.63 public key exchange for E2EE
    EncryptedPayload = 17, // AES-256-GCM authenticated encrypted payload wrapper
    MonitorInfo = 18     // Host reporting active monitor index and total display count
};

#pragma pack(push, 1) // Ensure no padding for raw struct serialization

struct MessageHeader {
    MessageType type;
    uint32_t payloadSize;
};

struct ConnectRequest {
    char sessionId[32];
    char authenticationToken[64];
};

struct ConnectResponse {
    bool accepted;
    char reason[128];
};

struct FrameDataHeader {
    uint32_t frameNumber;
    uint32_t totalWidth;
    uint32_t totalHeight;
    uint32_t updateWidth;
    uint32_t updateHeight;
    uint32_t targetX;
    uint32_t targetY;
    uint32_t bpp;
    bool isDelta;
    uint64_t timestampMs;
    // Followed by raw pixel data or H.264 NAL units
};

struct MouseEvent {
    float normalizedX; // 0.0 to 1.0 (Mapped to monitor)
    float normalizedY; // 0.0 to 1.0
    uint8_t buttonId;  // 0: left, 1: right, 2: middle
    bool isDown;
    int wheelDelta;
};

struct KeyboardEvent {
    uint16_t virtualKey;
    bool isDown;
};

struct AudioDataHeader {
    uint32_t sampleRate;    // e.g. 48000
    uint16_t channels;      // 1 or 2
    uint16_t bitsPerSample; // 16
    uint32_t dataSize;      // byte length of following PCM data
};

struct FrameAck {
    uint32_t frameNumber;
};

struct P2PCandidateMsg {
    char publicIp[64];
    uint16_t publicPort;
    char localIp[64];
    uint16_t localPort;
};

struct SwitchMonitorMsg {
    uint32_t monitorIndex;
};

struct SecureDesktopMsg {
    bool isSecureDesktopActive;
};

struct KeyExchangeMsg {
    uint8_t publicKeyLen;
    uint8_t publicKey[96];
};

struct MonitorInfoMsg {
    uint32_t activeIndex;
    uint32_t monitorCount;
};

#pragma pack(pop)

// Clipboard: variable-length UTF-8 text — no fixed struct, raw bytes follow the header
inline std::vector<uint8_t> SerializeClipboardText(const std::string& utf8Text) {
    uint32_t textLen = static_cast<uint32_t>(utf8Text.size());
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + textLen);
    MessageHeader* hdr = reinterpret_cast<MessageHeader*>(buffer.data());
    hdr->type = MessageType::ClipboardText;
    hdr->payloadSize = textLen;
    if (textLen > 0)
        std::memcpy(buffer.data() + sizeof(MessageHeader), utf8Text.data(), textLen);
    return buffer;
}

// Clipboard Image: variable-length DIB/pixels
inline std::vector<uint8_t> SerializeClipboardImage(const std::vector<uint8_t>& dibData) {
    uint32_t imgLen = static_cast<uint32_t>(dibData.size());
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + imgLen);
    MessageHeader* hdr = reinterpret_cast<MessageHeader*>(buffer.data());
    hdr->type = MessageType::ClipboardImage;
    hdr->payloadSize = imgLen;
    if (imgLen > 0)
        std::memcpy(buffer.data() + sizeof(MessageHeader), dibData.data(), imgLen);
    return buffer;
}

// Audio: variable-length PCM samples
inline std::vector<uint8_t> SerializeAudio(uint32_t sampleRate, uint16_t channels, uint16_t bits, const int16_t* samples, size_t sampleCount) {
    uint32_t pcmBytes = static_cast<uint32_t>(sampleCount * sizeof(int16_t));
    uint32_t payloadSize = sizeof(AudioDataHeader) + pcmBytes;
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + payloadSize);
    MessageHeader* hdr = reinterpret_cast<MessageHeader*>(buffer.data());
    hdr->type = MessageType::AudioData;
    hdr->payloadSize = payloadSize;

    AudioDataHeader* aHdr = reinterpret_cast<AudioDataHeader*>(buffer.data() + sizeof(MessageHeader));
    aHdr->sampleRate = sampleRate;
    aHdr->channels = channels;
    aHdr->bitsPerSample = bits;
    aHdr->dataSize = pcmBytes;

    if (pcmBytes > 0 && samples) {
        std::memcpy(buffer.data() + sizeof(MessageHeader) + sizeof(AudioDataHeader), samples, pcmBytes);
    }
    return buffer;
}

// Utility function to serialize structs (MVP binary serialization)
template<typename T>
std::vector<uint8_t> SerializeMessage(MessageType type, const T& payload) {
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + sizeof(T));
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer.data());
    header->type = type;
    header->payloadSize = sizeof(T);
    
    std::memcpy(buffer.data() + sizeof(MessageHeader), &payload, sizeof(T));
    return buffer;
}

inline std::vector<uint8_t> SerializeRawMessage(MessageType type, const uint8_t* data, uint32_t size) {
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + size);
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer.data());
    header->type = type;
    header->payloadSize = size;
    if (size > 0 && data) {
        std::memcpy(buffer.data() + sizeof(MessageHeader), data, size);
    }
    return buffer;
}

// Overload for FrameData where payload comprises a header and arbitrary byte array
inline std::vector<uint8_t> SerializeFrame(const FrameDataHeader& frameInfo, const std::vector<uint8_t>& frameData) {
    uint32_t payloadSize = sizeof(FrameDataHeader) + frameData.size();
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + payloadSize);
    
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer.data());
    header->type = MessageType::FrameData;
    header->payloadSize = payloadSize;
    
    std::memcpy(buffer.data() + sizeof(MessageHeader), &frameInfo, sizeof(FrameDataHeader));
    std::memcpy(buffer.data() + sizeof(MessageHeader) + sizeof(FrameDataHeader), frameData.data(), frameData.size());
    
    return buffer;
}

// Parse a raw byte buffer into a list of (MessageType, payload) pairs
inline std::vector<std::pair<MessageType, std::vector<uint8_t>>>
DeserializeMessages(const std::vector<uint8_t>& data) {
    std::vector<std::pair<MessageType, std::vector<uint8_t>>> result;
    size_t offset = 0;
    while (offset + sizeof(MessageHeader) <= data.size()) {
        const MessageHeader* hdr = reinterpret_cast<const MessageHeader*>(data.data() + offset);
        uint8_t t = static_cast<uint8_t>(hdr->type);
        if (t < 1 || t > 18) {
            offset++;
            continue;
        }
        if (hdr->payloadSize > 64 * 1024 * 1024 || offset + sizeof(MessageHeader) + hdr->payloadSize > data.size()) {
            break;
        }
        size_t total = sizeof(MessageHeader) + hdr->payloadSize;
        std::vector<uint8_t> payload(data.begin() + offset + sizeof(MessageHeader),
                                     data.begin() + offset + total);
        result.emplace_back(hdr->type, std::move(payload));
        offset += total;
    }
    return result;
}

} // namespace shared
} // namespace gupt