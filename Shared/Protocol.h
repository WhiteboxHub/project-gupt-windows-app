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
    MonitorInfo = 18,    // Host reporting active monitor index and total display count
    WebRtcOffer = 19,    // WebRTC SDP Offer (cross-platform signaling)
    WebRtcAnswer = 20,   // WebRTC SDP Answer (cross-platform signaling)
    WebRtcCandidate = 21 // WebRTC ICE Candidate (cross-platform signaling)
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

struct WebRtcSdpHeader {
    char sessionId[32];
    char role[16];          // "host" or "controller"
    uint32_t sdpLen;        // Length of following UTF-8 SDP string
};

struct WebRtcCandidateHeader {
    char sessionId[32];
    char role[16];          // "host" or "controller"
    char sdpMid[32];        // Media stream ID (e.g., "0", "video", "data")
    int32_t sdpMLineIndex;  // Media line index in SDP (-1 if none)
    char ufrag[32];         // ICE username fragment if available
    uint32_t candidateLen;  // Length of following UTF-8 candidate string
};

#pragma pack(pop)

// WebRTC SDP Offer / Answer serialization
inline std::vector<uint8_t> SerializeWebRtcSdp(MessageType type, const std::string& sessionId, const std::string& role, const std::string& sdp) {
    if (sdp.size() > 65536) return {}; // Enforce 64KB max SDP limit
    uint32_t sdpLen = static_cast<uint32_t>(sdp.size());
    uint32_t payloadSize = sizeof(WebRtcSdpHeader) + sdpLen;
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + payloadSize);

    MessageHeader* hdr = reinterpret_cast<MessageHeader*>(buffer.data());
    hdr->type = type;
    hdr->payloadSize = payloadSize;

    WebRtcSdpHeader* sdpHdr = reinterpret_cast<WebRtcSdpHeader*>(buffer.data() + sizeof(MessageHeader));
    std::memset(sdpHdr, 0, sizeof(WebRtcSdpHeader));
    std::strncpy(sdpHdr->sessionId, sessionId.c_str(), sizeof(sdpHdr->sessionId) - 1);
    std::strncpy(sdpHdr->role, role.c_str(), sizeof(sdpHdr->role) - 1);
    sdpHdr->sdpLen = sdpLen;

    if (sdpLen > 0) {
        std::memcpy(buffer.data() + sizeof(MessageHeader) + sizeof(WebRtcSdpHeader), sdp.data(), sdpLen);
    }
    return buffer;
}

inline std::vector<uint8_t> SerializeWebRtcOffer(const std::string& sessionId, const std::string& role, const std::string& sdp) {
    return SerializeWebRtcSdp(MessageType::WebRtcOffer, sessionId, role, sdp);
}

inline std::vector<uint8_t> SerializeWebRtcAnswer(const std::string& sessionId, const std::string& role, const std::string& sdp) {
    return SerializeWebRtcSdp(MessageType::WebRtcAnswer, sessionId, role, sdp);
}

// WebRTC ICE Candidate serialization
inline std::vector<uint8_t> SerializeWebRtcCandidate(
    const std::string& sessionId,
    const std::string& role,
    const std::string& sdpMid,
    int32_t sdpMLineIndex,
    const std::string& ufrag,
    const std::string& candidate)
{
    if (candidate.size() > 8192) return {}; // Enforce 8KB max candidate limit
    uint32_t candLen = static_cast<uint32_t>(candidate.size());
    uint32_t payloadSize = sizeof(WebRtcCandidateHeader) + candLen;
    std::vector<uint8_t> buffer(sizeof(MessageHeader) + payloadSize);

    MessageHeader* hdr = reinterpret_cast<MessageHeader*>(buffer.data());
    hdr->type = MessageType::WebRtcCandidate;
    hdr->payloadSize = payloadSize;

    WebRtcCandidateHeader* candHdr = reinterpret_cast<WebRtcCandidateHeader*>(buffer.data() + sizeof(MessageHeader));
    std::memset(candHdr, 0, sizeof(WebRtcCandidateHeader));
    std::strncpy(candHdr->sessionId, sessionId.c_str(), sizeof(candHdr->sessionId) - 1);
    std::strncpy(candHdr->role, role.c_str(), sizeof(candHdr->role) - 1);
    std::strncpy(candHdr->sdpMid, sdpMid.c_str(), sizeof(candHdr->sdpMid) - 1);
    candHdr->sdpMLineIndex = sdpMLineIndex;
    std::strncpy(candHdr->ufrag, ufrag.c_str(), sizeof(candHdr->ufrag) - 1);
    candHdr->candidateLen = candLen;

    if (candLen > 0) {
        std::memcpy(buffer.data() + sizeof(MessageHeader) + sizeof(WebRtcCandidateHeader), candidate.data(), candLen);
    }
    return buffer;
}

// WebRTC SDP parsing helper
inline bool ParseWebRtcSdp(const std::vector<uint8_t>& payload, std::string& outSessionId, std::string& outRole, std::string& outSdp) {
    if (payload.size() < sizeof(WebRtcSdpHeader)) return false;
    const WebRtcSdpHeader* hdr = reinterpret_cast<const WebRtcSdpHeader*>(payload.data());
    if (hdr->sdpLen > 65536 || sizeof(WebRtcSdpHeader) + hdr->sdpLen > payload.size()) return false;

    char sessBuf[sizeof(hdr->sessionId) + 1] = {0};
    std::memcpy(sessBuf, hdr->sessionId, sizeof(hdr->sessionId));
    outSessionId = sessBuf;

    char roleBuf[sizeof(hdr->role) + 1] = {0};
    std::memcpy(roleBuf, hdr->role, sizeof(hdr->role));
    outRole = roleBuf;

    if (hdr->sdpLen > 0) {
        outSdp.assign(reinterpret_cast<const char*>(payload.data() + sizeof(WebRtcSdpHeader)), hdr->sdpLen);
    } else {
        outSdp.clear();
    }
    return true;
}

// WebRTC Candidate parsing helper
inline bool ParseWebRtcCandidate(
    const std::vector<uint8_t>& payload,
    std::string& outSessionId,
    std::string& outRole,
    std::string& outSdpMid,
    int32_t& outSdpMLineIndex,
    std::string& outUfrag,
    std::string& outCandidate)
{
    if (payload.size() < sizeof(WebRtcCandidateHeader)) return false;
    const WebRtcCandidateHeader* hdr = reinterpret_cast<const WebRtcCandidateHeader*>(payload.data());
    if (hdr->candidateLen > 8192 || sizeof(WebRtcCandidateHeader) + hdr->candidateLen > payload.size()) return false;

    char sessBuf[sizeof(hdr->sessionId) + 1] = {0};
    std::memcpy(sessBuf, hdr->sessionId, sizeof(hdr->sessionId));
    outSessionId = sessBuf;

    char roleBuf[sizeof(hdr->role) + 1] = {0};
    std::memcpy(roleBuf, hdr->role, sizeof(hdr->role));
    outRole = roleBuf;

    char midBuf[sizeof(hdr->sdpMid) + 1] = {0};
    std::memcpy(midBuf, hdr->sdpMid, sizeof(hdr->sdpMid));
    outSdpMid = midBuf;

    outSdpMLineIndex = hdr->sdpMLineIndex;

    char ufragBuf[sizeof(hdr->ufrag) + 1] = {0};
    std::memcpy(ufragBuf, hdr->ufrag, sizeof(hdr->ufrag));
    outUfrag = ufragBuf;

    if (hdr->candidateLen > 0) {
        outCandidate.assign(reinterpret_cast<const char*>(payload.data() + sizeof(WebRtcCandidateHeader)), hdr->candidateLen);
    } else {
        outCandidate.clear();
    }
    return true;
}

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
        if (t < 1 || t > 21) {
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