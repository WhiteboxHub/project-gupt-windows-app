#pragma once
#include <cstdint>
#include <vector>
#include <string>

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
    ClipboardData = 8   // UTF-8 clipboard text sync (client ↔ host)
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
    uint32_t width;
    uint32_t height;
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

#pragma pack(pop)

// Clipboard: variable-length UTF-8 text — no fixed struct, raw bytes follow the header
inline std::vector<uint8_t> SerializeClipboard(const std::string& utf8Text) {
    uint32_t textLen = static_cast<uint32_t>(utf8Text.size());
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + textLen);
    MessageHeader* hdr = reinterpret_cast<MessageHeader*>(buffer.data());
    hdr->type = MessageType::ClipboardData;
    hdr->payloadSize = textLen;
    if (textLen > 0)
        std::memcpy(buffer.data() + sizeof(MessageHeader), utf8Text.data(), textLen);
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

} // namespace shared
} // namespace gupt
