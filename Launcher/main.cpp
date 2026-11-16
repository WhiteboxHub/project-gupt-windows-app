/*
 * Gupt Remote Desktop - Professional Master Build
 * ===============================================
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <winhttp.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <condition_variable>
#include <functional>
#include <random>
#include <cstdarg>
#include "Shared/Protocol.h"
#include "Core/Network/TcpNetwork.h"
#include "Core/Network/CryptoSession.h"
#include "Core/Network/StunClient.h"
#include "Core/Network/UdpP2PTransport.h"
#include "Core/Input/InputInjector.h"
#include "Core/Capture/ScreenCapturer.h"
#include "Core/Audio/AudioCapturer.h"
#include "Core/Audio/AudioPlayer.h"
#include <dbghelp.h>
#include <wincrypt.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

using namespace gupt;

// ── Enterprise Diagnostic Logger (2 MB Circular Rotation) ─────────────────────
static void GuptLog(const char* fmt, ...) {
    static std::mutex s_logMtx;
    std::lock_guard<std::mutex> lk(s_logMtx);

    char path[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path));
    if (len > 0 && len < sizeof(path)) {
        strcat_s(path, "\\Gupt");
        CreateDirectoryA(path, NULL);
        strcat_s(path, "\\session.log");
    }

    auto tryOpenRotating = [](const char* logPath) -> FILE* {
        if (!logPath || !logPath[0]) return NULL;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(logPath, GetFileExInfoStandard, &fad)) {
            uint64_t fsize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            if (fsize >= 2ULL * 1024ULL * 1024ULL) {
                std::string bakPath = std::string(logPath) + ".1";
                MoveFileExA(logPath, bakPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        FILE* f = NULL;
        if (fopen_s(&f, logPath, "a") == 0 && f) return f;
        return NULL;
    };

    FILE* fp = tryOpenRotating(path);
    if (!fp) {
        if (GetTempPathA(sizeof(path), path) > 0) {
            strcat_s(path, "Gupt_session.log");
            fp = tryOpenRotating(path);
        }
    }

    if (fp) {
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        SYSTEMTIME st; GetLocalTime(&st);
        char timeStr[64];
        sprintf_s(timeStr, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        fputs(timeStr, fp);
        fputs(buffer, fp);
        fputc('\n', fp);
        fclose(fp);
    }
}

// ── Top-Level Production Crash Handler ────────────────────────────────────────
static LONG WINAPI GuptCrashHandler(EXCEPTION_POINTERS* pEx) {
    char path[MAX_PATH];
    if (GetTempPathA(MAX_PATH, path) > 0) {
        strcat_s(path, "Gupt_Crash.dmp");
        HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = pEx;
            mei.ClientPointers = TRUE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei, NULL, NULL);
            CloseHandle(hFile);
            GuptLog("FATAL: Unhandled exception caught! MiniDump written to %s", path);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── Dynamic Configurations (Zero Hardcoding) ─────────────────────────────────
static std::string GetConfiguredSignalingHost() {
    char envBuf[256] = {0};
    DWORD len = GetEnvironmentVariableA("GUPT_SIGNAL_SERVER", envBuf, sizeof(envBuf));
    if (len > 0 && len < sizeof(envBuf)) {
        return std::string(envBuf);
    }
    return "gupt-signal-server-560359652969.us-central1.run.app";
}

static int GetConfiguredSignalingPort() {
    char envBuf[32] = {0};
    DWORD len = GetEnvironmentVariableA("GUPT_SIGNAL_PORT", envBuf, sizeof(envBuf));
    if (len > 0 && len < sizeof(envBuf)) {
        int p = atoi(envBuf);
        if (p > 0 && p <= 65535) return p;
    }
    return 443;
}

static uint16_t GetConfiguredLanPort() {
    char envBuf[32] = {0};
    DWORD len = GetEnvironmentVariableA("GUPT_LAN_PORT", envBuf, sizeof(envBuf));
    if (len > 0 && len < sizeof(envBuf)) {
        int p = atoi(envBuf);
        if (p > 0 && p <= 65535) return static_cast<uint16_t>(p);
    }
    return 8080;
}

static uint32_t GetConfiguredUint(const char* envName, uint32_t defaultVal, uint32_t minVal, uint32_t maxVal) {
    char envBuf[32] = {0};
    DWORD len = GetEnvironmentVariableA(envName, envBuf, sizeof(envBuf));
    if (len > 0 && len < sizeof(envBuf)) {
        long val = atol(envBuf);
        if (val >= static_cast<long>(minVal) && val <= static_cast<long>(maxVal)) {
            return static_cast<uint32_t>(val);
        }
    }
    return defaultVal;
}

static const uint32_t g_CaptureFps        = GetConfiguredUint("GUPT_CAPTURE_FPS", 60, 15, 120);
static const uint64_t g_P2PFallbackMs     = GetConfiguredUint("GUPT_P2P_FALLBACK_MS", 4000, 1500, 30000);
static const uint64_t g_P2PRepunchMs      = GetConfiguredUint("GUPT_P2P_REPUNCH_MS", 20000, 5000, 120000);
static const uint64_t g_StallTimeoutMs    = GetConfiguredUint("GUPT_STALL_TIMEOUT_MS", 45000, 10000, 180000);
static const size_t   g_MaxClipboardBytes = GetConfiguredUint("GUPT_MAX_CLIPBOARD_BYTES", 65536, 4096, 1048576);
static std::atomic<bool> g_AppRunning{true};

// ── Global State & Configurations ───────────────────────────────────────────
static std::string g_SignalingHost = GetConfiguredSignalingHost();
static int g_SignalingPort = GetConfiguredSignalingPort();
static uint16_t g_LanPort = GetConfiguredLanPort();
static std::atomic<uint32_t> g_HostMonitorCount{1};
static gupt::core::network::TcpServer* g_ActiveLanServer = nullptr;
static std::string g_CurrentSessionId = "";
static std::string g_HostIp = "";
static bool g_LaunchAsHost = false;
static int g_Launched = 0;
static std::atomic<bool> g_ForceFullScan{true};
static std::atomic<bool> g_IsConnected{false};
static std::atomic<bool> g_UsingRelay{false};
static std::atomic<bool> g_UsingP2P{false};
static std::atomic<bool> g_SidebarOpen{false};
static std::atomic<bool> g_IsFullscreen{false};
static std::atomic<bool> g_SessionActive{false};
static std::atomic<bool> g_ClipboardSyncEnabled{true};
static core::network::UdpP2PTransport g_P2PTransport;
static std::atomic<bool> g_DeliveryBusy{false};
static std::atomic<uint32_t> g_HostLastSentFrameNum{0};
static std::atomic<uint32_t> g_HostLastAckedFrameNum{0};
static std::atomic<uint32_t> g_HostLastSendDurationMs{0};
static std::atomic<uint64_t> g_HostLastFrameSendTick{0};
static std::atomic<uint64_t> g_HostLastRecvTick{0};
static std::atomic<uint64_t> g_ClientLastFrameRecvTick{0};
static std::atomic<uint64_t> g_ClientLastRecvTick{0};

static gupt::core::audio::AudioPlayer g_AudioPlayer;

static gupt::core::network::CryptoSession g_CryptoSession;
static std::string g_ClientInstanceToken;
static std::vector<std::string> g_SignalingCandidates;
static bool g_Headless = false;

static std::string GenerateClientInstanceToken() {
    uint8_t rnd[8]{};
    if (!gupt::core::network::CryptoSession::GenerateRandomBytes(rnd, sizeof(rnd))) {
        std::random_device rd;
        uint32_t a = rd(), b = rd();
        memcpy(rnd, &a, 4);
        memcpy(rnd + 4, &b, 4);
    }
    char hex[24]{};
    sprintf_s(hex, "C_%02X%02X%02X%02X%02X%02X%02X%02X",
              rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
    return std::string(hex);
}

static bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    size_t maxLen = (a.size() > b.size()) ? a.size() : b.size();
    volatile unsigned char diff = (a.size() != b.size()) ? 1 : 0;
    for (size_t i = 0; i < maxLen; ++i) {
        unsigned char ca = (i < a.size()) ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0;
        diff |= (ca ^ cb);
    }
    return diff == 0;
}

static std::vector<uint8_t> MakeKeyExchangePacket() {
    auto pub = g_CryptoSession.GetLocalPublicKey();
    gupt::shared::KeyExchangeMsg msg{};
    if (!pub.empty() && pub.size() <= sizeof(msg.publicKey)) {
        msg.publicKeyLen = static_cast<uint8_t>(pub.size());
        memcpy(msg.publicKey, pub.data(), pub.size());
    }
    return gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyExchange, msg);
}

// Wraps sensitive application payloads into EncryptedPayload once ECDH+AES-256-GCM is active.
// Control/handshake packets pass through unmodified so session recovery and key negotiation never deadlock.
static std::vector<uint8_t> MaybeEncryptOutbound(std::vector<uint8_t> packet) {
    if (!g_CryptoSession.IsEstablished() || packet.size() < sizeof(gupt::shared::MessageHeader)) {
        return packet;
    }
    const auto* hdr = reinterpret_cast<const gupt::shared::MessageHeader*>(packet.data());
    switch (hdr->type) {
        case gupt::shared::MessageType::ConnectRequest:
        case gupt::shared::MessageType::ConnectResponse:
        case gupt::shared::MessageType::Disconnect:
        case gupt::shared::MessageType::Heartbeat:
        case gupt::shared::MessageType::P2PCandidate:
        case gupt::shared::MessageType::KeyExchange:
        case gupt::shared::MessageType::EncryptedPayload:
            return packet;
        default:
            break;
    }
    std::vector<uint8_t> cipher;
    if (g_CryptoSession.Encrypt(packet.data(), packet.size(), cipher)) {
        return gupt::shared::SerializeRawMessage(gupt::shared::MessageType::EncryptedPayload, cipher.data(), static_cast<uint32_t>(cipher.size()));
    }
    return packet;
}

static std::vector<std::string> ParseRelayList(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : raw) {
        if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool ParseRelayHostPort(const std::string& entry, int defaultPort, std::string& outHost, int& outPort) {
    if (entry.empty()) return false;
    size_t colon = entry.rfind(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < entry.size()) {
        int parsed = atoi(entry.substr(colon + 1).c_str());
        if (parsed > 0 && parsed <= 65535) {
            outHost = entry.substr(0, colon);
            outPort = parsed;
            return true;
        }
    }
    outHost = entry;
    outPort = defaultPort;
    return true;
}

static std::vector<std::string> GetConfiguredSignalingCandidates() {
    char envBuf[1024] = {0};
    DWORD len = GetEnvironmentVariableA("GUPT_SIGNAL_SERVERS", envBuf, sizeof(envBuf));
    if (len > 0 && len < sizeof(envBuf)) {
        auto parsed = ParseRelayList(std::string(envBuf));
        if (!parsed.empty()) return parsed;
    }
    return { GetConfiguredSignalingHost() };
}

static std::mutex g_FrameMutex, g_hostClipMtx, g_EvMtx, g_SendMtx, g_EncMtx;
static std::vector<uint8_t> g_PendingFrame, g_RawQueue;
static uint32_t g_RawW=0, g_RawH=0;
static uint32_t g_DirtyX=0, g_DirtyY=0, g_DirtyW=0, g_DirtyH=0;
static bool g_HasDirtyBounds=false;
static std::vector<std::vector<uint8_t>> g_SendQueue;
static std::vector<std::vector<uint8_t>> g_EvQueue;
static std::condition_variable g_EvCv, g_SendCv, g_EncCv;

// UI Components
static HFONT g_FontTitle, g_FontBody, g_FontBtn;
static HWND g_hRadioHost, g_hRadioClient, g_hEditIp;
static HHOOK g_hKbdHook = NULL;

static void PowerKeepAwakeLoop() {
    while(true) {
        if(g_SessionActive || g_IsConnected) {
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED | ES_AWAYMODE_REQUIRED);
            // Send synthetic hardware zero-delta input pulse every 15s to keep Windows display & idle timer awake 24/7
            INPUT ip = {0};
            ip.type = INPUT_MOUSE;
            ip.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &ip, sizeof(INPUT));
        } else {
            SetThreadExecutionState(ES_CONTINUOUS);
        }
        Sleep(15000);
    }
}

class WsRelay {
public:
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    HINTERNET hWS = NULL;
    std::atomic<bool> connected{false};
    std::mutex sendMtx;
    std::mutex stateMtx;

    bool Open(const std::string& host, int port, const std::string& path, bool secure) {
        Close();

        std::lock_guard<std::mutex> lkSend(sendMtx);
        std::lock_guard<std::mutex> lk(stateMtx);
        hSession = WinHttpOpen(L"Gupt/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if(!hSession) return false;

        WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);

        std::wstring wHost(host.begin(), host.end());
        hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
        if(!hConnect) { CloseInternal(); return false; }

        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        std::wstring wPath(path.begin(), path.end());
        hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if(!hRequest) { CloseInternal(); return false; }

        if(!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
            CloseInternal(); return false;
        }

        if(WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(hRequest, NULL)) {
            hWS = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
            WinHttpCloseHandle(hRequest);
            hRequest = NULL;

            if(hWS) {
                DWORD keepAlive = 15000;
                WinHttpSetOption(hWS, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAlive, sizeof(keepAlive));
                connected = true;
                return true;
            }
        }
        CloseInternal();
        return false;
    }

    void Close() {
        connected = false;
        std::lock_guard<std::mutex> lkSend(sendMtx);
        std::lock_guard<std::mutex> lk(stateMtx);
        CloseInternal();
    }

    void Abort() {
        connected = false;
        std::lock_guard<std::mutex> lkSend(sendMtx);
        std::lock_guard<std::mutex> lk(stateMtx);
        CloseInternal();
    }

    bool Send(const void* data, uint32_t len) {
        if(!connected) return false;
        std::lock_guard<std::mutex> lk(sendMtx);
        if(!connected || !hWS) return false;
        DWORD r = WinHttpWebSocketSend(hWS, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, const_cast<void*>(data), len);
        if(r != 0) {
            connected = false;
            return false;
        }
        return true;
    }

    std::vector<uint8_t> Recv() {
        if(!connected) return {};
        HINTERNET wsLocal = NULL;
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            if(!connected || !hWS) return {};
            wsLocal = hWS;
        }
        std::vector<uint8_t> buf;
        std::vector<uint8_t> frag(64*1024);
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD read = 0;
        do {
            if(!connected) return {};
            DWORD r = WinHttpWebSocketReceive(wsLocal, frag.data(), (DWORD)frag.size(), &read, &type);
            if(r != 0 || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                connected = false;
                return {};
            }
            if(read > 0) {
                buf.insert(buf.end(), frag.begin(), frag.begin() + read);
            }
        } while(type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);
        return buf;
    }

private:
    void CloseInternal() {
        connected = false;
        if(hWS) {
            WinHttpCloseHandle(hWS);
            hWS = NULL;
        }
        if(hRequest) { WinHttpCloseHandle(hRequest); hRequest = NULL; }
        if(hConnect) { WinHttpCloseHandle(hConnect); hConnect = NULL; }
        if(hSession) { WinHttpCloseHandle(hSession); hSession = NULL; }
    }
};
static gupt::core::network::TcpClient g_Client;
static WsRelay g_Relay;

static void PushOutbound(std::vector<uint8_t> f, bool isUrgent) {
    {
        std::lock_guard<std::mutex> lk(g_SendMtx);
        if(isUrgent && f.size() > 400) {
            // Audio queue headroom: 15 packets (~300ms) preserves 100% of audio continuity without pops/clicks
            int audioCount = 0;
            for(auto& q : g_SendQueue) { if(q.size() > 400) audioCount++; }
            if(audioCount >= 15) {
                for(auto it = g_SendQueue.begin(); it != g_SendQueue.end(); ++it) {
                    if(it->size() > 400) { g_SendQueue.erase(it); break; }
                }
            }
            g_SendQueue.push_back(std::move(f));
        } else if(isUrgent || f.size() < 400) {
            g_SendQueue.push_back(std::move(f));
        } else {
            g_PendingFrame = std::move(f);
        }
    }
    g_SendCv.notify_one();
}

static void DownsampleBgra(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint8_t* dst, uint32_t dstW, uint32_t dstH) {
    if (srcW == dstW && srcH == dstH) {
        memcpy(dst, src, (size_t)dstW * dstH * 4);
        return;
    }

    // High precision fixed-point bilinear interpolation for clean, anti-aliased output
    uint32_t xRatio = (srcW > 1) ? (((srcW - 1) << 16) / (dstW - 1)) : 0;
    uint32_t yRatio = (srcH > 1) ? (((srcH - 1) << 16) / (dstH - 1)) : 0;

    for (uint32_t y = 0; y < dstH; ++y) {
        uint32_t fpY = y * yRatio;
        uint32_t srcY = fpY >> 16;
        uint32_t yDiff = fpY & 0xFFFF;
        uint32_t yDiffInv = 0x10000 - yDiff;

        uint32_t nextY = (srcY + 1 < srcH) ? (srcY + 1) : srcY;
        const uint32_t* row0 = reinterpret_cast<const uint32_t*>(src + (size_t)srcY * srcW * 4);
        const uint32_t* row1 = reinterpret_cast<const uint32_t*>(src + (size_t)nextY * srcW * 4);
        uint32_t* dstRow = reinterpret_cast<uint32_t*>(dst + (size_t)y * dstW * 4);

        for (uint32_t x = 0; x < dstW; ++x) {
            uint32_t fpX = x * xRatio;
            uint32_t srcX = fpX >> 16;
            uint32_t xDiff = fpX & 0xFFFF;
            uint32_t xDiffInv = 0x10000 - xDiff;
            uint32_t nextX = (srcX + 1 < srcW) ? (srcX + 1) : srcX;

            uint32_t p00 = row0[srcX];
            uint32_t p01 = row0[nextX];
            uint32_t p10 = row1[srcX];
            uint32_t p11 = row1[nextX];

            uint32_t w00 = (xDiffInv * yDiffInv) >> 16;
            uint32_t w01 = (xDiff * yDiffInv) >> 16;
            uint32_t w10 = (xDiffInv * yDiff) >> 16;
            uint32_t w11 = (xDiff * yDiff) >> 16;

            uint32_t b = (((p00 & 0xFF) * w00) + ((p01 & 0xFF) * w01) + ((p10 & 0xFF) * w10) + ((p11 & 0xFF) * w11)) >> 16;
            uint32_t g = ((((p00 >> 8) & 0xFF) * w00) + (((p01 >> 8) & 0xFF) * w01) + (((p10 >> 8) & 0xFF) * w10) + (((p11 >> 8) & 0xFF) * w11)) >> 16;
            uint32_t r = ((((p00 >> 16) & 0xFF) * w00) + (((p01 >> 16) & 0xFF) * w01) + (((p10 >> 16) & 0xFF) * w10) + (((p11 >> 16) & 0xFF) * w11)) >> 16;

            dstRow[x] = (b & 0xFF) | ((g & 0xFF) << 8) | ((r & 0xFF) << 16) | 0xFF000000;
        }
    }
}

static void ScaleTo1080p(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH, std::vector<uint8_t>& dst, uint32_t& dstW, uint32_t& dstH) {
    dstW = 1920;
    dstH = 1080;
    size_t targetSize = (size_t)dstW * dstH * 4;
    if (dst.size() != targetSize) {
        dst.resize(targetSize);
    }
    DownsampleBgra(src.data(), srcW, srcH, dst.data(), dstW, dstH);
}

static uint32_t CalculateOptimalBitrate(uint32_t width, uint32_t height) {
    uint64_t pixels = (uint64_t)width * height;
    if (pixels >= 7680 * 4320) {
        return 60000000; // 60 Mbps for 8K
    } else if (pixels >= 3840 * 2160) {
        return 35000000; // 35 Mbps for 4K UHD
    } else if (pixels >= 2560 * 1440) {
        return 22000000; // 22 Mbps for 1440p (2K)
    } else if (pixels >= 1920 * 1080) {
        return 14000000; // 14 Mbps for 1080p
    } else {
        return 7000000;  // 7 Mbps for 720p
    }
}

// Adaptive network resolution and bitrate scaling: dynamically scales between 4K and 720p,
// strictly enforcing 720p (1280x720) as the minimum floor so the screen NEVER blurs below 720p.
static void GetAdaptiveResolutionAndBitrate(uint32_t srcW, uint32_t srcH,
                                           uint32_t& outW, uint32_t& outH, uint32_t& outBitrate) {
    // 720p is the hard floor (unless the host monitor itself is smaller than 720p)
    uint32_t minFloorW = (srcW >= 1280) ? 1280 : srcW;
    uint32_t minFloorH = (srcH >= 720) ? 720 : srcH;

    // Direct LAN connection: always stream at native resolution (up to 4K) with full clarity
    if (!g_UsingRelay) {
        outW = srcW;
        outH = srcH;
        outBitrate = CalculateOptimalBitrate(srcW, srcH);
        return;
    }

    uint32_t inFlight = g_HostLastSentFrameNum.load() - g_HostLastAckedFrameNum.load();
    uint32_t sendMs = g_HostLastSendDurationMs.load();

    // Hysteresis-governed tier adaptation (0: Native/4K/2K, 1: 1080p, 2: 720p Floor)
    static int s_activeTier = 0;
    static uint64_t s_lastTierShiftTick = 0;
    uint64_t now = GetTickCount64();

    if (now - s_lastTierShiftTick > 2500) {
        if (inFlight > 18 || sendMs > 70) {
            // High queuing / bottleneck -> scale to 720p floor to clear queue immediately
            if (s_activeTier < 2) { s_activeTier = 2; s_lastTierShiftTick = now; }
        } else if (inFlight > 7 || sendMs > 35) {
            // Moderate latency -> scale to 1080p
            if (s_activeTier != 1) { s_activeTier = 1; s_lastTierShiftTick = now; }
        } else if (inFlight <= 3 && sendMs < 20) {
            // Low latency and fast network -> upgrade to native (4K / 2K / 1080p)
            if (s_activeTier > 0) { s_activeTier = 0; s_lastTierShiftTick = now; }
        }
    }

    if (s_activeTier == 0) {
        // High Tier: Native resolution (up to 4K)
        outW = srcW;
        outH = srcH;
        outBitrate = CalculateOptimalBitrate(srcW, srcH);
    } else if (s_activeTier == 1) {
        // Medium Tier: 1080p (or native if native is <= 1080p)
        if (srcW > 1920 && srcH > 1080) {
            outW = 1920;
            outH = 1080;
            outBitrate = 12000000; // 12 Mbps
        } else {
            outW = srcW;
            outH = srcH;
            outBitrate = 9000000;
        }
    } else {
        // Floor Tier: 720p minimum (NEVER below 720p)
        if (srcW > minFloorW && srcH > minFloorH) {
            outW = minFloorW;
            outH = minFloorH;
            outBitrate = 5000000; // 5 Mbps
        } else {
            outW = srcW;
            outH = srcH;
            outBitrate = 4000000;
        }
    }
}

static void HostEncoderLoop(gupt::core::capture::ScreenCapturer* capturer) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    static uint32_t s_frameCounter = 0;
    static uint64_t s_lastFullScanTick = 0;

    while(true) {
        std::vector<uint8_t> raw; uint32_t w = 0, h = 0;
        uint32_t dx = 0, dy = 0, dw = 0, dh = 0;
        bool hasDirty = false;
        {
            std::unique_lock<std::mutex> lk(g_EncMtx);
            g_EncCv.wait(lk, []{ return !g_RawQueue.empty(); });
            raw = std::move(g_RawQueue); g_RawQueue.clear();
            w = g_RawW; h = g_RawH;
            dx = g_DirtyX; dy = g_DirtyY; dw = g_DirtyW; dh = g_DirtyH;
            hasDirty = g_HasDirtyBounds;
        }
        if(!raw.empty() && w > 0 && h > 0) {
            if (g_UsingRelay || g_UsingP2P) {
                uint32_t sent = g_HostLastSentFrameNum.load();
                uint32_t acked = g_HostLastAckedFrameNum.load();
                uint32_t maxInFlight = g_UsingP2P ? 3 : 2;
                uint64_t nowTick = GetTickCount64();
                if (sent > acked + maxInFlight && (nowTick - g_HostLastFrameSendTick.load() < 120)) {
                    Sleep(4);
                    continue;
                }
            }
            uint64_t now = GetTickCount64();
            bool isFullScan = g_ForceFullScan || (now - s_lastFullScanTick > 10000) || !hasDirty || (dw == 0 || dh == 0);

            uint32_t targetX = 0, targetY = 0, targetW = w, targetH = h;
            int quality = 92;
            bool force444 = true; // True 4K 4:4:4 Chroma Subsampling: eliminates text blur and color bleeding

            if (!isFullScan) {
                // 8-pixel macroblock alignment for optimal JPEG 8x8 DCT blocks
                targetX = (dx / 8) * 8;
                targetY = (dy / 8) * 8;
                uint32_t right = ((dx + dw + 7) / 8) * 8;
                uint32_t bottom = ((dy + dh + 7) / 8) * 8;
                if (right > w) right = w;
                if (bottom > h) bottom = h;
                targetW = right - targetX;
                targetH = bottom - targetY;

                // When switching tabs, opening windows, or maximizing:
                // If more than 80% of the screen changed, promote to full-screen keyframe so client
                // instantaneously renders the new tab with zero lag, dropping stale intermediate frames.
                float dirtyRatio = static_cast<float>((size_t)targetW * targetH) / static_cast<float>((size_t)w * h);
                if (dirtyRatio >= 0.80f) {
                    targetX = 0; targetY = 0; targetW = w; targetH = h;
                    isFullScan = true;
                    s_lastFullScanTick = now;
                    g_ForceFullScan = false;
                    quality = (g_UsingRelay && !g_UsingP2P) ? 88 : 92;
                } else {
                    quality = 92;
                }
                force444 = true;
            } else {
                s_lastFullScanTick = now;
                g_ForceFullScan = false;
                quality = (g_UsingRelay && !g_UsingP2P) ? 88 : 92;
                force444 = true;
            }

            std::vector<uint8_t> jpg;
            if(capturer->CaptureRegionJpeg(raw, w, h, targetX, targetY, targetW, targetH, jpg, quality, force444)) {
                uint32_t fn = ++s_frameCounter;
                g_HostLastSentFrameNum = fn;
                g_HostLastFrameSendTick = GetTickCount64();
                gupt::shared::FrameDataHeader fHdr = {
                    fn, w, h, targetW, targetH, targetX, targetY, 32, !isFullScan, now
                };
                PushOutbound(gupt::shared::SerializeFrame(fHdr, jpg), false);
            }
        }
    }
}
static void HostDeliveryLoop(gupt::core::network::TcpServer* srv) {
    while(true) {
        std::vector<std::vector<uint8_t>> cmds;
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(g_SendMtx);
            g_SendCv.wait(lk, []{ return !g_SendQueue.empty() || !g_PendingFrame.empty(); });
            cmds = std::move(g_SendQueue); g_SendQueue.clear();
            frame = std::move(g_PendingFrame); g_PendingFrame.clear();
        }
        for(auto& c : cmds) {
            auto pkt = MaybeEncryptOutbound(std::move(c));
            if(srv->IsClientConnected()) srv->SendRaw(pkt);
            else if(g_UsingP2P && g_P2PTransport.IsConnected()) {
                if(!g_P2PTransport.Send(pkt.data(), pkt.size()) && g_Relay.connected) {
                    g_Relay.Send(pkt.data(), (uint32_t)pkt.size());
                }
            }
            else if(g_Relay.connected) g_Relay.Send(pkt.data(), (uint32_t)pkt.size());
        }
        if(!frame.empty()) {
            ULONGLONG t0 = GetTickCount64();
            auto pkt = MaybeEncryptOutbound(std::move(frame));
            if(srv->IsClientConnected()) srv->SendRaw(pkt);
            else if(g_UsingP2P && g_P2PTransport.IsConnected()) {
                if(!g_P2PTransport.Send(pkt.data(), pkt.size()) && g_Relay.connected) {
                    g_Relay.Send(pkt.data(), (uint32_t)pkt.size());
                }
            }
            else if(g_Relay.connected) g_Relay.Send(pkt.data(), (uint32_t)pkt.size());
            g_HostLastSendDurationMs = static_cast<uint32_t>(GetTickCount64() - t0);
        }
    }
}

static std::vector<uint8_t> g_LatestFrame, g_DrawingFrame;
static uint32_t g_FrameW = 0, g_FrameH = 0, g_DrawW = 0, g_DrawH = 0;
static int g_DestX = 0, g_DestY = 0, g_DestW = 800, g_DestH = 600, g_WinW = 0, g_WinH = 0, g_SidebarX = 0;
static HDC g_BackDC = NULL; static HBITMAP g_BackBmp = NULL;
static RECT g_SavedRect={0,0,800,600}, g_TabRect, g_CloseBtnRect, g_FullscreenBtnRect, g_ClipboardBtnRect, g_DisplayBtnRect;
static std::atomic<uint32_t> g_ClientActiveMonitorIndex{0};
static std::atomic<uint32_t> g_ClientMonitorCount{1};
static std::atomic<bool> g_HostSecureDesktopActive{false};
static uint64_t g_LastRefreshTime = 0;
static HWND g_ClientHwnd = NULL;  // Cross-thread repaint signaling
#define WM_GUPT_FRAME_ARRIVED (WM_APP + 1)
static std::string g_lastFromClient, g_clientLastFromHost, g_clientLastSent;

static bool IsPureMouseMove(const std::vector<uint8_t>& m) {
    if (m.size() < sizeof(gupt::shared::MessageHeader) + sizeof(gupt::shared::MouseEvent)) return false;
    const gupt::shared::MessageHeader* hdr = reinterpret_cast<const gupt::shared::MessageHeader*>(m.data());
    if (hdr->type != gupt::shared::MessageType::MouseEvent) return false;
    const gupt::shared::MouseEvent* me = reinterpret_cast<const gupt::shared::MouseEvent*>(m.data() + sizeof(gupt::shared::MessageHeader));
    return (me->buttonId == 255 && me->wheelDelta == 0);
}

static bool IsWheelEvent(const std::vector<uint8_t>& m) {
    if (m.size() < sizeof(gupt::shared::MessageHeader) + sizeof(gupt::shared::MouseEvent)) return false;
    const gupt::shared::MessageHeader* hdr = reinterpret_cast<const gupt::shared::MessageHeader*>(m.data());
    if (hdr->type != gupt::shared::MessageType::MouseEvent) return false;
    const gupt::shared::MouseEvent* me = reinterpret_cast<const gupt::shared::MouseEvent*>(m.data() + sizeof(gupt::shared::MessageHeader));
    return (me->wheelDelta != 0);
}

static void PushEv(std::vector<uint8_t> m) {
    { 
        std::lock_guard<std::mutex> lk(g_EvMtx); 
        if(m.size()==1 && m[0]==0xFF){ 
            g_EvQueue.insert(g_EvQueue.begin(), std::move(m)); 
        } else if (IsPureMouseMove(m)) {
            // Only coalesce consecutive pure mouse moves so clicks and drags are NEVER lost
            if (!g_EvQueue.empty() && IsPureMouseMove(g_EvQueue.back())) {
                g_EvQueue.back() = std::move(m);
            } else {
                g_EvQueue.push_back(std::move(m));
            }
        } else if (IsWheelEvent(m)) {
            // Drop any preceding stale pure mouse moves so scroll events execute immediately without delay
            if (!g_EvQueue.empty() && IsPureMouseMove(g_EvQueue.back())) {
                g_EvQueue.pop_back();
            }
            // Coalesce rapid consecutive wheel events so scroll delta is smoothly batched without machine-gun jumps
            if (!g_EvQueue.empty() && IsWheelEvent(g_EvQueue.back())) {
                gupt::shared::MouseEvent* backMe = reinterpret_cast<gupt::shared::MouseEvent*>(
                    g_EvQueue.back().data() + sizeof(gupt::shared::MessageHeader));
                const gupt::shared::MouseEvent* newMe = reinterpret_cast<const gupt::shared::MouseEvent*>(
                    m.data() + sizeof(gupt::shared::MessageHeader));
                backMe->wheelDelta += newMe->wheelDelta;
                backMe->normalizedX = newMe->normalizedX;
                backMe->normalizedY = newMe->normalizedY;
            } else {
                g_EvQueue.push_back(std::move(m));
            }
        } else { 
            // All mouse clicks, releases, keys, and clipboard are 100% preserved
            g_EvQueue.push_back(std::move(m)); 
        }
    }
    g_EvCv.notify_one();
}

static void SendFrameAck(uint32_t frameNum) {
    gupt::shared::FrameAck ack = { frameNum };
    auto ackMsg = gupt::shared::SerializeMessage(gupt::shared::MessageType::FrameAck, ack);
    auto pkt = MaybeEncryptOutbound(std::move(ackMsg));
    if (g_UsingP2P && g_P2PTransport.IsConnected()) {
        g_P2PTransport.Send(pkt.data(), pkt.size());
    } else if (g_UsingRelay && g_Relay.connected) {
        g_Relay.Send(pkt.data(), static_cast<uint32_t>(pkt.size()));
    } else if (g_Client.IsConnected()) {
        g_Client.SendRaw(pkt);
    }
}

static void EvSenderLoop() {
    while(true) {
        std::vector<uint8_t> m;
        {
            std::unique_lock<std::mutex> lk(g_EvMtx);
            g_EvCv.wait(lk, []{ return !g_EvQueue.empty(); });
            while(g_EvQueue.size() > 1 && IsPureMouseMove(g_EvQueue[0]) && (IsPureMouseMove(g_EvQueue[1]) || IsWheelEvent(g_EvQueue[1]))) {
                g_EvQueue.erase(g_EvQueue.begin());
            }
            m = std::move(g_EvQueue.front()); g_EvQueue.erase(g_EvQueue.begin());
        }
        if(g_IsConnected) {
            auto pkt = MaybeEncryptOutbound(std::move(m));
            if(g_UsingP2P && g_P2PTransport.IsConnected()) g_P2PTransport.Send(pkt.data(), pkt.size());
            else if(g_UsingRelay) g_Relay.Send(pkt.data(), (uint32_t)pkt.size()); else g_Client.SendRaw(pkt);
        }
    }
}
static void TriggerSessionRefresh(HWND hWnd) {
    g_LastRefreshTime = GetTickCount64();
    if(hWnd) {
        SetWindowTextA(hWnd, "Gupt - Refreshing...");
        InvalidateRect(hWnd, NULL, FALSE);
    }
    auto refMsg = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1);
    PushEv(refMsg);
    auto wireRef = MaybeEncryptOutbound(refMsg);
    if (g_Relay.connected) {
        g_Relay.Send(wireRef.data(), (uint32_t)wireRef.size());
    }
    if (g_Client.IsConnected()) {
        g_Client.SendRaw(wireRef);
    }
}

static std::string HttpGet(const std::string& path) {
    std::string res; HINTERNET hS = WinHttpOpen(L"Gupt/1.0", 0, 0, 0, 0);
    if(hS){
        std::wstring wH(g_SignalingHost.begin(), g_SignalingHost.end()), wP(path.begin(), path.end());
        HINTERNET hC = WinHttpConnect(hS, wH.c_str(), g_SignalingPort, 0);
        HINTERNET hR = WinHttpOpenRequest(hC, L"GET", wP.c_str(), 0, 0, 0, g_SignalingPort==443?WINHTTP_FLAG_SECURE:0);
        if(WinHttpSendRequest(hR, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hR, 0)){
            DWORD sz=0; do{ WinHttpQueryDataAvailable(hR, &sz); if(!sz)break; char* b=new char[sz+1]; DWORD r=0;
            WinHttpReadData(hR, b, sz, &r); b[r]=0; res+=b; delete[] b; }while(sz>0);
        }
        WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    }
    return res;
}

static uint64_t ProbeHealthLatencyMs(const std::string& host, int port) {
    HINTERNET hS = WinHttpOpen(L"Gupt-Probe/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return UINT64_MAX;
    WinHttpSetTimeouts(hS, 2000, 2000, 2000, 2000);

    std::wstring wH(host.begin(), host.end());
    HINTERNET hC = WinHttpConnect(hS, wH.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!hC) { WinHttpCloseHandle(hS); return UINT64_MAX; }

    DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", L"/health", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return UINT64_MAX; }

    uint64_t rtt = UINT64_MAX;
    uint64_t t0 = GetTickCount64();
    if (WinHttpSendRequest(hR, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hR, 0)) {
        DWORD statusCode = 0;
        DWORD sz = sizeof(statusCode);
        if (WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX) &&
            statusCode == 200) {
            rtt = GetTickCount64() - t0;
        }
    }

    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return rtt;
}

// Probes configured regional relays in parallel and updates g_SignalingHost/Port to the fastest healthy region.
static void SelectFastestSignalingRelay(const std::vector<std::string>& candidates, int defaultPort) {
    if (candidates.empty()) return;
    if (candidates.size() == 1) {
        ParseRelayHostPort(candidates[0], defaultPort, g_SignalingHost, g_SignalingPort);
        return;
    }

    struct ProbeResult {
        std::string host;
        int port = 443;
        uint64_t rttMs = UINT64_MAX;
    };

    std::vector<ProbeResult> results(candidates.size());
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());

    for (size_t i = 0; i < candidates.size(); ++i) {
        ParseRelayHostPort(candidates[i], defaultPort, results[i].host, results[i].port);
        workers.emplace_back([&, i]() {
            results[i].rttMs = ProbeHealthLatencyMs(results[i].host, results[i].port);
        });
    }
    for (auto& w : workers) w.join();

    size_t bestIdx = 0;
    uint64_t bestRtt = UINT64_MAX;
    for (size_t i = 0; i < results.size(); ++i) {
        GuptLog("Relay Probe [%zu]: %s:%d -> %s", i, results[i].host.c_str(), results[i].port,
                (results[i].rttMs == UINT64_MAX) ? "UNREACHABLE" : (std::to_string(results[i].rttMs) + "ms").c_str());
        if (results[i].rttMs < bestRtt) {
            bestRtt = results[i].rttMs;
            bestIdx = i;
        }
    }

    g_SignalingHost = results[bestIdx].host;
    g_SignalingPort = results[bestIdx].port;
    GuptLog("Selected Signaling Relay: %s:%d (RTT: %s)", g_SignalingHost.c_str(), g_SignalingPort,
            (bestRtt == UINT64_MAX) ? "fallback" : (std::to_string(bestRtt) + "ms").c_str());
}
static std::string DecodeIP(const std::string& id) {
    unsigned int a,b,c,d; if(sscanf(id.c_str(), "%02X%02X%02X%02X", &a,&b,&c,&d)!=4) return "";
    char buf[16]; sprintf(buf, "%u.%u.%u.%u", a,b,c,d); return buf;
}

#define CLR_BG       RGB(24, 24, 34)
#define CLR_HEADER   RGB(110, 80, 240)
#define CLR_CARD     RGB(34, 34, 50)
#define CLR_BORDER   RGB(45, 45, 70)
#define CLR_ACCENT   RGB(120, 100, 255)
#define CLR_TEXT     RGB(240, 240, 255)
#define CLR_SUBTEXT  RGB(160, 160, 190)

void DrawRoundedRect(HDC hdc, RECT r, int rad, COLORREF bg, COLORREF brd, int penW) {
    HBRUSH b = CreateSolidBrush(bg), ob = (HBRUSH)SelectObject(hdc, b);
    HPEN p = CreatePen(PS_SOLID, penW, brd), op = (HPEN)SelectObject(hdc, p);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, ob); DeleteObject(b);
    SelectObject(hdc, op); DeleteObject(p);
}

class ScopedClipboard {
public:
    explicit ScopedClipboard(HWND hWnd = NULL) : m_open(OpenClipboard(hWnd) != FALSE) {}
    ~ScopedClipboard() { if (m_open) CloseClipboard(); }
    bool IsOpen() const { return m_open; }
    ScopedClipboard(const ScopedClipboard&) = delete;
    ScopedClipboard& operator=(const ScopedClipboard&) = delete;
private:
    bool m_open = false;
};

static bool SafeSetClipboardText(const std::string& s, HWND hWnd = NULL) {
    if (s.empty()) return false;
    if (s.size() > g_MaxClipboardBytes) return false;
    int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (wn <= 0) return false;
    HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (size_t)wn * sizeof(wchar_t));
    if (!m) return false;
    wchar_t* d = static_cast<wchar_t*>(GlobalLock(m));
    if (!d) {
        GlobalFree(m);
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, d, wn);
    GlobalUnlock(m);

    ScopedClipboard clip(hWnd);
    if (clip.IsOpen()) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, m) != NULL) {
            return true;
        }
    }
    GlobalFree(m);
    return false;
}

// ── Privacy & Sensitive Data Clipboard Protection ────────────────────────────
static bool IsClipboardContentSensitive() {
    static UINT s_cfExclude1 = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static UINT s_cfExclude2 = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    if ((s_cfExclude1 != 0 && IsClipboardFormatAvailable(s_cfExclude1)) ||
        (s_cfExclude2 != 0 && IsClipboardFormatAvailable(s_cfExclude2))) {
        return true;
    }
    return false;
}

static std::string SafeGetClipboardText(HWND hWnd = NULL) {
    if (IsClipboardContentSensitive()) {
        return "";
    }
    ScopedClipboard clip(hWnd);
    if (!clip.IsOpen()) return "";
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) return "";
    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string res;
    if (n > 1) {
        res.resize(n - 1);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &res[0], n, nullptr, nullptr);
    }
    GlobalUnlock(h);
    return res;
}

std::string GetClipboardText() {
    return SafeGetClipboardText();
}

static std::vector<uint8_t> MakeConnectRequest() {
    if (g_ClientInstanceToken.empty()) {
        g_ClientInstanceToken = GenerateClientInstanceToken();
    }
    gupt::shared::ConnectRequest req = {};
    strncpy(req.sessionId, g_ClientInstanceToken.c_str(), sizeof(req.sessionId) - 1);
    return gupt::shared::SerializeMessage(gupt::shared::MessageType::ConnectRequest, req);
}

static void SyncHostMonitorState(
    core::capture::ScreenCapturer& capturer,
    core::input::InputInjector& injector)
{
    int32_t ox = 0, oy = 0, w = 0, h = 0;
    capturer.GetMonitorBounds(ox, oy, w, h);
    injector.SetMonitorBounds(ox, oy, w, h);
    uint32_t count = capturer.GetMonitorCount();
    g_HostMonitorCount = count;
    shared::MonitorInfoMsg mi{ capturer.GetCurrentMonitorIndex(), count };
    if (g_SessionActive.load()) {
        PushOutbound(shared::SerializeMessage(shared::MessageType::MonitorInfo, mi), true);
    }
}

static void DispatchHostDataMessage(
    shared::MessageType t,
    const std::vector<uint8_t>& p,
    core::input::InputInjector& injector,
    core::capture::ScreenCapturer& capturer)
{
    if (t == shared::MessageType::EncryptedPayload) {
        std::vector<uint8_t> plain;
        if (!g_CryptoSession.Decrypt(p.data(), p.size(), plain)) {
            return;
        }
        auto inner = shared::DeserializeMessages(plain);
        for (const auto& m : inner) {
            if (m.first != shared::MessageType::EncryptedPayload) {
                DispatchHostDataMessage(m.first, m.second, injector, capturer);
            }
        }
        return;
    }

    if (t == shared::MessageType::KeyExchange) {
        if (p.size() >= sizeof(shared::KeyExchangeMsg)) {
            const auto* kx = reinterpret_cast<const shared::KeyExchangeMsg*>(p.data());
            if (g_CryptoSession.DeriveSharedSecret(kx->publicKey, kx->publicKeyLen)) {
                GuptLog("E2EE session established on Host (ECDH P-256 + AES-256-GCM)");
                SyncHostMonitorState(capturer, injector);
                g_ForceFullScan = true;
                capturer.RequestKeyframe();
            }
        }
    } else if (t == shared::MessageType::MouseEvent) {
        if (p.size() >= sizeof(shared::MouseEvent)) {
            injector.IngestMouseEvent(*reinterpret_cast<const shared::MouseEvent*>(p.data()));
        }
    } else if (t == shared::MessageType::KeyboardEvent) {
        if (p.size() >= sizeof(shared::KeyboardEvent)) {
            injector.IngestKeyboardEvent(*reinterpret_cast<const shared::KeyboardEvent*>(p.data()));
        }
    } else if (t == shared::MessageType::RefreshRequest) {
        SyncHostMonitorState(capturer, injector);
        g_ForceFullScan = true;
        capturer.RequestKeyframe();
    } else if (t == shared::MessageType::SwitchMonitor) {
        if (p.size() >= sizeof(shared::SwitchMonitorMsg)) {
            const auto* sm = reinterpret_cast<const shared::SwitchMonitorMsg*>(p.data());
            capturer.SetMonitorIndex(sm->monitorIndex);
            SyncHostMonitorState(capturer, injector);
            g_ForceFullScan = true;
            capturer.RequestKeyframe();
            GuptLog("Host switched active display to Monitor %u", sm->monitorIndex);
        }
    } else if (t == shared::MessageType::FrameAck) {
        if (p.size() >= sizeof(shared::FrameAck)) {
            const auto* ack = reinterpret_cast<const shared::FrameAck*>(p.data());
            g_HostLastAckedFrameNum = ack->frameNumber;
        }
    } else if (t == shared::MessageType::Heartbeat) {
        // Activity tick refreshed by transport wrapper
    } else if (g_ClipboardSyncEnabled && t == shared::MessageType::ClipboardText) {
        std::string s(reinterpret_cast<const char*>(p.data()), p.size());
        { std::lock_guard<std::mutex> lk(g_hostClipMtx); g_lastFromClient = s; }
        SafeSetClipboardText(s);
    }
}

static void SendRelayP2PCandidateAsync() {
    std::thread([]() {
        for (int w = 0; w < 30 && !g_P2PTransport.GetEndpoint().isDiscovered; ++w) Sleep(50);
        const auto& myEp = g_P2PTransport.GetEndpoint();
        shared::P2PCandidateMsg candMsg{};
        strncpy_s(candMsg.publicIp, myEp.publicIp.c_str(), sizeof(candMsg.publicIp));
        candMsg.publicPort = myEp.publicPort;
        strncpy_s(candMsg.localIp, myEp.localIp.c_str(), sizeof(candMsg.localIp));
        candMsg.localPort = myEp.localPort;
        auto candBuf = shared::SerializeMessage(shared::MessageType::P2PCandidate, candMsg);
        g_Relay.Send(candBuf.data(), static_cast<uint32_t>(candBuf.size()));
    }).detach();
}

static void HandleHostConnectRequest(
    const std::vector<uint8_t>& p,
    bool isRelay,
    core::capture::ScreenCapturer& capturer,
    std::function<void(const std::vector<uint8_t>&)> sendFn)
{
    g_SessionActive = true;
    if (isRelay) g_UsingRelay = true;
    g_HostLastRecvTick = GetTickCount64();
    shared::ConnectResponse res = {true, "OK"};
    sendFn(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
    sendFn(MakeKeyExchangePacket());
    g_ForceFullScan = true;
    capturer.RequestKeyframe();
    if (isRelay) SendRelayP2PCandidateAsync();
    GuptLog("Host accepted client connection for session %s (immediate access)", g_CurrentSessionId.c_str());
}

static void RunHostMode() {
    GuptLog("Gupt Host Mode initialized. Signaling Host: %s:%d", g_SignalingHost.c_str(), g_SignalingPort);
    g_CryptoSession.InitializeLocalKeyPair();
    std::thread(PowerKeepAwakeLoop).detach();

    core::input::InputInjector injector;
    injector.Initialize();
    core::capture::ScreenCapturer capturer;
    capturer.Initialize();
    std::thread([]() { g_P2PTransport.Initialize(); }).detach();

    g_P2PTransport.SetMessageCallback([&injector, &capturer](const std::vector<uint8_t>& d) {
        g_HostLastRecvTick = GetTickCount64();
        auto msgs = shared::DeserializeMessages(d);
        for(auto& msg : msgs) {
            DispatchHostDataMessage(msg.first, msg.second, injector, capturer);
        }
    });
    std::string json = HttpGet("/register");
    size_t pos = json.find("sessionId\":\"");
    if(pos != std::string::npos){
        g_CurrentSessionId = json.substr(pos + 12);
        size_t end = g_CurrentSessionId.find("\"");
        if(end != std::string::npos) g_CurrentSessionId = g_CurrentSessionId.substr(0, end);
    } else g_CurrentSessionId = "ERROR";

    if(g_CurrentSessionId == "ERROR" || g_CurrentSessionId.empty()){
        MessageBoxA(NULL, "Could not reach Cloud Relay. Check Internet connection.", "Gupt", MB_ICONERROR);
        return;
    }

    GuptLog("Host Session Registered. ID: %s", g_CurrentSessionId.c_str());

    // Auto-copy Session ID to clipboard for immediate convenience
    SafeSetClipboardText(g_CurrentSessionId);

    std::string rPath = "/relay?role=host&session=" + g_CurrentSessionId;
    g_Relay.Open(g_SignalingHost, g_SignalingPort, rPath, g_SignalingPort == 443);

    // Watchdog for idle/stalled relay socket
    std::thread([](){
        while(true) {
            Sleep(1000);
            if(g_SessionActive) {
                uint64_t last = g_HostLastRecvTick.load();
                // If P2P is actively connected and receiving packets, do not abort relay!
                if(g_UsingP2P && g_P2PTransport.IsConnected() && (GetTickCount64() - g_P2PTransport.GetLastPeerActivityTick() < 15000)) {
                    g_HostLastRecvTick = GetTickCount64();
                } else if(g_UsingRelay && last > 0 && (GetTickCount64() - last > 90000)) {
                    g_HostLastRecvTick = 0;
                    g_Relay.Abort();
                }
            }
        }
    }).detach();

    // Dedicated Relay Receiver Thread: Secure authenticated session management
    std::thread([rPath, &injector, &capturer]() {
        
        auto relaySend = [](const std::vector<uint8_t>& buf) {
            g_Relay.Send(buf.data(), static_cast<uint32_t>(buf.size()));
        };
        while (true) {
            if(!g_Relay.connected) {
                g_Relay.Open(g_SignalingHost, g_SignalingPort, rPath, g_SignalingPort == 443);
                if(!g_Relay.connected) { Sleep(500); continue; }
                g_HostLastRecvTick = GetTickCount64();
            }
            auto d = g_Relay.Recv(); if(d.empty()){ continue; }
            g_HostLastRecvTick = GetTickCount64();
            auto msgs = shared::DeserializeMessages(d);
            for(auto& msg : msgs) {
                auto t = msg.first; auto& p = msg.second;
                if(t == shared::MessageType::ConnectRequest) {
                    HandleHostConnectRequest(p, true, capturer, relaySend);
                }
                else if(t == shared::MessageType::P2PCandidate) {
                    if(p.size() >= sizeof(shared::P2PCandidateMsg)) {
                        auto c = reinterpret_cast<const shared::P2PCandidateMsg*>(p.data());
                        core::network::NatEndpoint peerEp;
                        peerEp.publicIp = c->publicIp;
                        peerEp.publicPort = c->publicPort;
                        peerEp.localIp = c->localIp;
                        peerEp.localPort = c->localPort;
                        std::thread([peerEp, &capturer]() {
                            if(g_P2PTransport.PunchHole(peerEp, 8000)) {
                                g_UsingP2P = true;
                                g_ForceFullScan = true;
                                capturer.RequestKeyframe();
                            }
                        }).detach();
                    }
                }
                else if(t == shared::MessageType::Disconnect) {
                    g_SessionActive = false;
                    g_UsingP2P = false;
                    g_CryptoSession.Reset();
                }
                else {
                    DispatchHostDataMessage(t, p, injector, capturer);
                }
            }
        }
    }).detach();

    // LAN TCP Server
    core::network::TcpServer server(g_LanPort);
    g_ActiveLanServer = &server;
    server.SetMessageCallback([&](shared::MessageType t, const std::vector<uint8_t>& p) {
        g_HostLastRecvTick = GetTickCount64();
        
        if(t == shared::MessageType::ConnectRequest) {
            HandleHostConnectRequest(p, false, capturer, [&server](const std::vector<uint8_t>& buf) {
                server.SendRaw(buf);
            });
        }
        else if(t == shared::MessageType::Disconnect) {
            g_SessionActive = false;
            g_CryptoSession.Reset();
        }
        else {
            DispatchHostDataMessage(t, p, injector, capturer);
        }
    });
    server.Start();

    // Audio Capturer
    static core::audio::AudioCapturer s_audioCapturer;
    s_audioCapturer.Start([](uint32_t sampleRate, uint16_t channels, uint16_t bits, const int16_t* samples, size_t sampleCount) {
        if (g_SessionActive) {
            auto pkt = shared::SerializeAudio(sampleRate, channels, bits, samples, sampleCount);
            PushOutbound(std::move(pkt), true);
        }
    });

    // Start background Encoder and Delivery loops
    std::thread(HostEncoderLoop, &capturer).detach();
    std::thread(HostDeliveryLoop, &server).detach();
    std::thread([](){
        uint32_t ticks = 0;
        while(true){
            if(g_SessionActive){
                auto hb=shared::SerializeMessage(shared::MessageType::Heartbeat,(uint8_t)1);
                PushOutbound(hb, true);
                if(++ticks % 5 == 0 && g_Relay.connected) {
                    g_Relay.Send(hb.data(), (uint32_t)hb.size());
                }
            }
            Sleep(2000);
        }
    }).detach();

    WNDCLASSEXA hwc = {sizeof(WNDCLASSEXA)};
    hwc.lpfnWndProc = [](HWND hw, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if(m == WM_CLIPBOARDUPDATE) {
            std::string c = GetClipboardText();
            if(!c.empty() && c != g_lastFromClient) {
                {
                    std::lock_guard<std::mutex> lk(g_hostClipMtx);
                    if(c == g_lastFromClient) return 0;
                }
                auto s = shared::SerializeClipboardText(c);
                PushOutbound(std::move(s), true);
            }
        }
        return DefWindowProcA(hw, m, w, l);
    };
    hwc.lpszClassName = "GuptH";
    hwc.hInstance = GetModuleHandle(NULL);
    RegisterClassExA(&hwc);
    HWND hHidden = CreateWindowExA(0, "GuptH", 0, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
    AddClipboardFormatListener(hHidden);

    // Dedicated Dynamic-FPS Capture Thread (Runs decoupled in background immediately with sender-paced flow control)
    const uint64_t defaultFrameBudgetMs = (g_CaptureFps > 0) ? (1000 / g_CaptureFps) : 16;
    std::thread([&capturer, &injector, defaultFrameBudgetMs]() {
        while(g_AppRunning.load()) {
            if(g_SessionActive) {
                if(g_ForceFullScan) {
                    capturer.RequestKeyframe();
                }
                uint64_t loopStart = GetTickCount64();
                std::vector<uint8_t> raw; uint32_t w, h;
                if(capturer.CaptureNextFrame(raw, w, h)) {
                    int32_t ox = 0, oy = 0, bw = 0, bh = 0;
                    capturer.GetMonitorBounds(ox, oy, bw, bh);
                    injector.SetMonitorBounds(ox, oy, bw > 0 ? bw : static_cast<int>(w), bh > 0 ? bh : static_cast<int>(h));
                    uint32_t dx = 0, dy = 0, dw = 0, dh = 0;
                    bool hasDirty = capturer.GetLastDirtyBounds(dx, dy, dw, dh);
                    {
                        std::lock_guard<std::mutex> lk(g_EncMtx);
                        if (!g_RawQueue.empty() && g_HasDirtyBounds && hasDirty) {
                            uint32_t r1 = g_DirtyX + g_DirtyW;
                            uint32_t b1 = g_DirtyY + g_DirtyH;
                            uint32_t r2 = dx + dw;
                            uint32_t b2 = dy + dh;
                            g_DirtyX = (g_DirtyX < dx) ? g_DirtyX : dx;
                            g_DirtyY = (g_DirtyY < dy) ? g_DirtyY : dy;
                            g_DirtyW = ((r1 > r2) ? r1 : r2) - g_DirtyX;
                            g_DirtyH = ((b1 > b2) ? b1 : b2) - g_DirtyY;
                        } else {
                            g_DirtyX = dx; g_DirtyY = dy; g_DirtyW = dw; g_DirtyH = dh;
                            g_HasDirtyBounds = hasDirty;
                        }
                        g_RawQueue = std::move(raw); g_RawW = w; g_RawH = h;
                    }
                    g_EncCv.notify_one();
                }
                static uint64_t s_lastUacCheck = 0;
                static bool s_lastUacActive = false;
                if (loopStart - s_lastUacCheck > 600) {
                    s_lastUacCheck = loopStart;
                    HDESK hDesk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
                    bool uacActive = (hDesk == NULL);
                    if (hDesk) CloseDesktop(hDesk);
                    if (uacActive != s_lastUacActive) {
                        s_lastUacActive = uacActive;
                        shared::SecureDesktopMsg sdm = { uacActive };
                        PushOutbound(shared::SerializeMessage(shared::MessageType::SecureDesktopNotification, sdm), true);
                    }
                }
                uint64_t frameBudgetMs = defaultFrameBudgetMs;
                uint64_t el = GetTickCount64() - loopStart;
                if(frameBudgetMs > el) Sleep(static_cast<DWORD>(frameBudgetMs - el));
            } else {
                Sleep(50);
            }
        }
    }).detach();

    if (!g_Headless) {
        MessageBoxA(NULL, ("Gupt Host is active!\n\nSession ID: " + g_CurrentSessionId).c_str(), "Gupt", MB_OK | MB_ICONINFORMATION);
    }

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_ActiveLanServer = nullptr;
}
struct ClientFrameTask {
    std::vector<uint8_t> data;
    HWND hWnd;
};

static std::mutex g_ClientDecodeMtx;
static std::condition_variable g_ClientDecodeCv;
static std::deque<ClientFrameTask> g_ClientDecodeQueue;
static std::atomic<bool> g_ClientDecoderRunning{true};

static void DecodeAndApplyJpeg(const std::vector<uint8_t>& p, HWND hWnd) {
    if(p.size() < sizeof(gupt::shared::FrameDataHeader)) return;
    gupt::shared::FrameDataHeader* hdr = (gupt::shared::FrameDataHeader*)p.data();
    uint32_t payloadSize = (UINT)(p.size() - sizeof(gupt::shared::FrameDataHeader));
    if(payloadSize == 0) return;

    IStream* stream = SHCreateMemStream(p.data() + sizeof(gupt::shared::FrameDataHeader), payloadSize);
    if(!stream) return;

    static thread_local IWICImagingFactory* s_clientWicFactory = NULL;
    if(!s_clientWicFactory) CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_clientWicFactory));
    if(!s_clientWicFactory) { stream->Release(); return; }

    IWICBitmapDecoder* dec = NULL;
    s_clientWicFactory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand, &dec);
    if(dec) {
        IWICBitmapFrameDecode* frm = NULL;
        dec->GetFrame(0, &frm);
        if(frm) {
            IWICFormatConverter* conv = NULL;
            s_clientWicFactory->CreateFormatConverter(&conv);
            if(conv) {
                conv->Initialize(frm, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0.f, WICBitmapPaletteTypeCustom);
                UINT tw = 0, th = 0;
                conv->GetSize(&tw, &th);
                if(tw > 0 && th > 0 && tw <= 8192 && th <= 8192) {
                    std::vector<uint8_t> pix(tw * th * 4);
                    HRESULT hr = conv->CopyPixels(NULL, tw * 4, (UINT)pix.size(), pix.data());
                    if(SUCCEEDED(hr)) {
                        {
                            std::lock_guard<std::mutex> lk(g_FrameMutex);
                            if(hdr->updateWidth >= hdr->totalWidth && hdr->updateHeight >= hdr->totalHeight) {
                                g_FrameW = tw;
                                g_FrameH = th;
                                g_LatestFrame = std::move(pix);
                            } else {
                                if(g_LatestFrame.empty() || g_FrameW != hdr->totalWidth || g_FrameH != hdr->totalHeight) {
                                    g_FrameW = hdr->totalWidth;
                                    g_FrameH = hdr->totalHeight;
                                    g_LatestFrame.assign(g_FrameW * g_FrameH * 4, 0);
                                }
                                if((hdr->targetX + tw <= g_FrameW) && (hdr->targetY + th <= g_FrameH)) {
                                    for(uint32_t row = 0; row < th; ++row) {
                                        memcpy(&g_LatestFrame[((hdr->targetY + row) * g_FrameW + hdr->targetX) * 4], &pix[row * tw * 4], tw * 4);
                                    }
                                }
                            }
                        }
                        if(hWnd && g_LastRefreshTime > 0 && GetTickCount64() - g_LastRefreshTime < 4000){
                            SetWindowTextA(hWnd, g_UsingRelay ? "Gupt - Connected (Relay)" : "Gupt - Connected (LAN)");
                        }
                        // Non-blocking asynchronous paint signal
                        if(hWnd) PostMessage(hWnd, WM_GUPT_FRAME_ARRIVED, 0, 0);
                        g_ClientLastFrameRecvTick = GetTickCount64();

                        // Send FrameAck back to host for paced flow control directly (without polluting input event queue)
                        SendFrameAck(hdr->frameNumber);
                    }
                }
                conv->Release();
            }
            frm->Release();
        }
        dec->Release();
    }
    stream->Release();
}

static void ClientDecoderWorkerLoop() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (g_ClientDecoderRunning.load()) {
        ClientFrameTask task;
        {
            std::unique_lock<std::mutex> lk(g_ClientDecodeMtx);
            g_ClientDecodeCv.wait(lk, []{ return !g_ClientDecodeQueue.empty() || !g_ClientDecoderRunning.load(); });
            if (!g_ClientDecoderRunning.load()) break;

            // Frame drop logic for socket/latency protection:
            // If more than 2 frames backed up, and the newest frame is a full-frame,
            // drop the stale intermediate frames so the client renders in absolute real-time!
            while (g_ClientDecodeQueue.size() > 1) {
                const auto& nextTask = g_ClientDecodeQueue.at(1);
                if (nextTask.data.size() >= sizeof(gupt::shared::FrameDataHeader)) {
                    auto nextHdr = reinterpret_cast<const gupt::shared::FrameDataHeader*>(nextTask.data.data());
                    if (!nextHdr->isDelta) {
                        g_ClientDecodeQueue.pop_front();
                        continue;
                    }
                }
                break;
            }

            task = std::move(g_ClientDecodeQueue.front());
            g_ClientDecodeQueue.pop_front();
        }

        if (!task.data.empty()) {
            DecodeAndApplyJpeg(task.data, task.hWnd);
        }
    }
    CoUninitialize();
}

void ProcessFrame(const std::vector<uint8_t>& p, HWND hWnd) {
    if(p.size() < sizeof(gupt::shared::FrameDataHeader)) return;
    {
        std::lock_guard<std::mutex> lk(g_ClientDecodeMtx);
        // Cap client decode queue size at 4 frames: prevents memory growth during heavy workloads
        if (g_ClientDecodeQueue.size() >= 4) {
            g_ClientDecodeQueue.pop_front();
        }
        g_ClientDecodeQueue.push_back({p, hWnd});
    }
    g_ClientDecodeCv.notify_one();
}

static void SaveOrCopy1080pScreenshot(HWND hWnd) {
    std::vector<uint8_t> frame;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(g_FrameMutex);
        if (g_LatestFrame.empty() || g_FrameW == 0 || g_FrameH == 0) return;
        frame = g_LatestFrame;
        w = g_FrameW;
        h = g_FrameH;
    }

    // Scale to clean 1080p (1920x1080) on demand for screenshot
    std::vector<uint8_t> frame1080;
    uint32_t outW = 1920, outH = 1080;
    if (w != 1920 || h != 1080) {
        ScaleTo1080p(frame, w, h, frame1080, outW, outH);
    } else {
        frame1080 = std::move(frame);
    }
    w = outW;
    h = outH;
    frame = std::move(frame1080);

    // 1. Copy 1080p DIB to Windows Clipboard
    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        size_t dibSize = sizeof(BITMAPINFOHEADER) + (size_t)w * h * 4;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
        if (hMem) {
            uint8_t* pMem = (uint8_t*)GlobalLock(hMem);
            BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)pMem;
            bih->biSize = sizeof(BITMAPINFOHEADER);
            bih->biWidth = w;
            bih->biHeight = h;
            bih->biPlanes = 1;
            bih->biBitCount = 32;
            bih->biCompression = BI_RGB;
            bih->biSizeImage = w * h * 4;

            uint8_t* pDest = pMem + sizeof(BITMAPINFOHEADER);
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* srcRow = frame.data() + (h - 1 - y) * w * 4;
                memcpy(pDest + y * w * 4, srcRow, w * 4);
            }
            GlobalUnlock(hMem);
            SetClipboardData(CF_DIB, hMem);
        }
        CloseClipboard();
    }

    // 2. Save pristine PNG to Desktop
    char desktopPath[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktopPath))) {
        SYSTEMTIME st; GetLocalTime(&st);
        char filename[MAX_PATH];
        sprintf_s(filename, "%s\\Gupt_1080p_%04d%02d%02d_%02d%02d%02d.png", desktopPath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        
        IWICImagingFactory* pFactory = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
            IWICBitmapEncoder* pEncoder = NULL;
            if (SUCCEEDED(pFactory->CreateEncoder(GUID_ContainerFormatPng, NULL, &pEncoder))) {
                wchar_t wFilename[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, filename, -1, wFilename, MAX_PATH);
                IWICStream* pWicStream = NULL;
                if (SUCCEEDED(pFactory->CreateStream(&pWicStream)) &&
                    SUCCEEDED(pWicStream->InitializeFromFilename(wFilename, GENERIC_WRITE)) &&
                    SUCCEEDED(pEncoder->Initialize(pWicStream, WICBitmapEncoderNoCache))) {
                    IWICBitmapFrameEncode* pFrame = NULL;
                    if (SUCCEEDED(pEncoder->CreateNewFrame(&pFrame, NULL)) &&
                        SUCCEEDED(pFrame->Initialize(NULL))) {
                        pFrame->SetSize(w, h);
                        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                        pFrame->SetPixelFormat(&fmt);
                        pFrame->WritePixels(h, w * 4, (UINT)frame.size(), frame.data());
                        pFrame->Commit();
                        pEncoder->Commit();
                    }
                    if (pFrame) pFrame->Release();
                }
                if (pWicStream) pWicStream->Release();
                pEncoder->Release();
            }
            pFactory->Release();
        }
    }

    SetWindowTextA(hWnd, "Gupt - 1080p Screenshot Saved to Desktop & Clipboard!");
    SetTimer(hWnd, 3, 3000, 0);
}

static void SendClientP2PCandidateAsync() {
    std::thread([]() {
        for (int w = 0; w < 30 && !g_P2PTransport.GetEndpoint().isDiscovered; ++w) Sleep(50);
        const auto& myEp = g_P2PTransport.GetEndpoint();
        shared::P2PCandidateMsg candMsg{};
        strncpy_s(candMsg.publicIp, myEp.publicIp.c_str(), sizeof(candMsg.publicIp));
        candMsg.publicPort = myEp.publicPort;
        strncpy_s(candMsg.localIp, myEp.localIp.c_str(), sizeof(candMsg.localIp));
        candMsg.localPort = myEp.localPort;
        auto candBuf = shared::SerializeMessage(shared::MessageType::P2PCandidate, candMsg);
        g_Relay.Send(candBuf.data(), static_cast<uint32_t>(candBuf.size()));
    }).detach();
}

static void DispatchClientMessage(gupt::shared::MessageType t, const std::vector<uint8_t>& p, HWND hWnd) {
    if (t == gupt::shared::MessageType::EncryptedPayload) {
        std::vector<uint8_t> plain;
        if (!g_CryptoSession.Decrypt(p.data(), p.size(), plain)) {
            return;
        }
        auto inner = gupt::shared::DeserializeMessages(plain);
        for (const auto& m : inner) {
            if (m.first != gupt::shared::MessageType::EncryptedPayload) {
                DispatchClientMessage(m.first, m.second, hWnd);
            }
        }
        return;
    }

    if (t == gupt::shared::MessageType::KeyExchange) {
        if (p.size() >= sizeof(gupt::shared::KeyExchangeMsg)) {
            const auto* kx = reinterpret_cast<const gupt::shared::KeyExchangeMsg*>(p.data());
            if (g_CryptoSession.DeriveSharedSecret(kx->publicKey, kx->publicKeyLen)) {
                GuptLog("E2EE session established on Client (ECDH P-256 + AES-256-GCM)");
                auto ref = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, static_cast<uint8_t>(1));
                PushEv(std::move(ref));
            }
        }
    } else if (t == gupt::shared::MessageType::FrameData) {
        ProcessFrame(p, hWnd);
    } else if (t == gupt::shared::MessageType::AudioData) {
        if (p.size() >= sizeof(gupt::shared::AudioDataHeader)) {
            const auto* aHdr = reinterpret_cast<const gupt::shared::AudioDataHeader*>(p.data());
            if (aHdr->dataSize > 0 &&
                aHdr->sampleRate >= 8000 && aHdr->sampleRate <= 192000 &&
                (aHdr->channels == 1 || aHdr->channels == 2) &&
                p.size() >= sizeof(gupt::shared::AudioDataHeader) + aHdr->dataSize)
            {
                const int16_t* pcm = reinterpret_cast<const int16_t*>(p.data() + sizeof(gupt::shared::AudioDataHeader));
                size_t sampleCount = aHdr->dataSize / sizeof(int16_t);
                g_AudioPlayer.PlaySamples(pcm, sampleCount, aHdr->sampleRate, aHdr->channels);
            }
        }
    } else if (t == gupt::shared::MessageType::ClipboardText) {
        std::string s(reinterpret_cast<const char*>(p.data()), p.size());
        g_clientLastFromHost = s;
        SafeSetClipboardText(s, hWnd);
    } else if (t == gupt::shared::MessageType::SecureDesktopNotification) {
        if (p.size() >= sizeof(gupt::shared::SecureDesktopMsg)) {
            const auto* sdm = reinterpret_cast<const gupt::shared::SecureDesktopMsg*>(p.data());
            g_HostSecureDesktopActive = sdm->isSecureDesktopActive;
            InvalidateRect(hWnd, NULL, FALSE);
        }
    } else if (t == gupt::shared::MessageType::MonitorInfo) {
        if (p.size() >= sizeof(gupt::shared::MonitorInfoMsg)) {
            const auto* mi = reinterpret_cast<const gupt::shared::MonitorInfoMsg*>(p.data());
            g_ClientActiveMonitorIndex = mi->activeIndex;
            g_ClientMonitorCount = (mi->monitorCount > 0) ? mi->monitorCount : 1;
            InvalidateRect(hWnd, NULL, FALSE);
        }
    } else if (t == gupt::shared::MessageType::Heartbeat) {
        g_ClientLastRecvTick = GetTickCount64();
    } else if (t == gupt::shared::MessageType::P2PCandidate) {
        if (p.size() >= sizeof(gupt::shared::P2PCandidateMsg)) {
            const auto* c = reinterpret_cast<const gupt::shared::P2PCandidateMsg*>(p.data());
            core::network::NatEndpoint hostPeer;
            hostPeer.publicIp = c->publicIp;
            hostPeer.publicPort = c->publicPort;
            hostPeer.localIp = c->localIp;
            hostPeer.localPort = c->localPort;
            std::thread([hWnd, hostPeer]() {
                if (g_P2PTransport.PunchHole(hostPeer, 8000)) {
                    g_UsingP2P = true;
                    g_ClientLastFrameRecvTick = GetTickCount64();
                    SetWindowTextA(hWnd, "Gupt - Connected (Direct P2P)");
                    auto rInit = MaybeEncryptOutbound(gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, static_cast<uint8_t>(1)));
                    PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, static_cast<uint8_t>(1)));
                    if (g_Relay.connected) {
                        g_Relay.Send(rInit.data(), static_cast<uint32_t>(rInit.size()));
                    }
                }
            }).detach();
        }
    }
}

static void SetupClientP2PCallback(HWND hWnd) {
    g_P2PTransport.SetMessageCallback([hWnd](const std::vector<uint8_t>& d) {
        g_ClientLastRecvTick = GetTickCount64();
        if (!g_UsingP2P) {
            g_UsingP2P = true;
            SetWindowTextA(hWnd, "Gupt - Connected (Direct P2P)");
        }
        auto msgs = gupt::shared::DeserializeMessages(d);
        for (const auto& m : msgs) {
            DispatchClientMessage(m.first, m.second, hWnd);
        }
    });
}

LRESULT CALLBACK ClientWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_TIMER: {
        if(wp==4){ if(g_IsConnected) InvalidateRect(hWnd, NULL, FALSE); }
        if(wp==3){ KillTimer(hWnd, 3); SetWindowTextA(hWnd, g_UsingRelay ? "Gupt - Connected (Relay)" : "Gupt - Connected (LAN)"); }
        if(wp==1){ int target=g_SidebarOpen?g_WinW-260:g_WinW; if(g_SidebarX!=target){ g_SidebarX+=(target>g_SidebarX?60:-60); if(abs(g_SidebarX-target)<60)g_SidebarX=target; InvalidateRect(hWnd,0,FALSE); } else KillTimer(hWnd,1); }
        if(wp==2){ KillTimer(hWnd,2);
            std::thread([hWnd](){
                CoInitializeEx(0, COINIT_MULTITHREADED);
                SetWindowTextA(hWnd, "Gupt - Connecting...");
                std::string session = g_HostIp;
                std::string rp="/relay?role=client&session=" + session;
                bool isRelaySession = (session.length() == 8 && session.find('.') == std::string::npos);
                std::string actualIp = isRelaySession ? "" : DecodeIP(session);
                bool isLocalIp = (!actualIp.empty() && (actualIp.rfind("192.168.", 0) == 0 || actualIp.rfind("10.", 0) == 0 || actualIp.rfind("172.", 0) == 0));
                while(true) {
                    if(isLocalIp && g_Client.Connect(actualIp, g_LanPort)){
                        g_UsingRelay = false;
                        g_CryptoSession.InitializeLocalKeyPair();
                        SetWindowTextA(hWnd, "Gupt - Waiting for host approval (LAN)");
                        auto req = MakeConnectRequest();
                        g_Client.SendRaw(req);
                        while(g_Client.IsConnected()){Sleep(250);}
                        g_IsConnected = false;
                        g_CryptoSession.Reset();
                        SetWindowTextA(hWnd, "Gupt - Reconnecting...");
                        Sleep(1000);
                        continue;
                    }

                    if(g_Relay.Open(g_SignalingHost, g_SignalingPort, rp, g_SignalingPort==443)){
                        g_UsingRelay = true;
                        g_CryptoSession.InitializeLocalKeyPair();
                        SetWindowTextA(hWnd, "Gupt - Waiting for host approval...");
                        auto req = MakeConnectRequest();
                        g_Relay.Send(req.data(), (uint32_t)req.size());
                        bool verified = false;
                        bool denied = false;
                        std::string denyReason = "Connection denied by host";
                        for(int i=0; i<300 && !verified && !denied; ++i){
                            auto d = g_Relay.Recv();
                            if(d.empty()){ Sleep(100); continue; }
                            auto msgs = gupt::shared::DeserializeMessages(d);
                            for(auto& m : msgs) {
                                if(m.first == gupt::shared::MessageType::ConnectResponse) {
                                    if(m.second.size() >= sizeof(gupt::shared::ConnectResponse)) {
                                        auto cr = reinterpret_cast<const gupt::shared::ConnectResponse*>(m.second.data());
                                        if(cr->accepted) {
                                            verified = true;
                                        } else {
                                            denied = true;
                                            if (cr->reason[0]) {
                                                denyReason = std::string(cr->reason, strnlen(cr->reason, sizeof(cr->reason)));
                                            }
                                        }
                                    }
                                } else {
                                    DispatchClientMessage(m.first, m.second, hWnd);
                                }
                            }
                        }
                        if(denied){
                            SetWindowTextA(hWnd, ("Gupt - " + denyReason).c_str());
                            GuptLog("Connection denied by host: %s", denyReason.c_str());
                            g_Relay.Close();
                            Sleep(2500);
                            continue;
                        }
                        if(verified){
                            g_IsConnected = true;
                            SetWindowTextA(hWnd, "Gupt - Connected (Relay)");
                            auto kx = MakeKeyExchangePacket();
                            g_Relay.Send(kx.data(), static_cast<uint32_t>(kx.size()));

                            SendClientP2PCandidateAsync();
                            SetupClientP2PCallback(hWnd);

                            auto rInit = MaybeEncryptOutbound(gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1));
                            g_Relay.Send(rInit.data(), (uint32_t)rInit.size());
                            
                            g_ClientLastRecvTick = GetTickCount64();
                            g_ClientLastFrameRecvTick = GetTickCount64();

                            // Dedicated Autonomous Watchdog: unblocks hanging sockets and handles seamless P2P <-> Relay failover & upgrade
                            std::thread([hWnd](){
                                uint64_t lastP2PRepunchTick = GetTickCount64();
                                while(g_IsConnected && g_AppRunning.load()) {
                                    Sleep(1000);
                                    uint64_t now = GetTickCount64();

                                    // 1. Maintain background Relay WebSocket with a keepalive ping every 10s
                                    static uint64_t s_lastRelayPing = 0;
                                    if(now - s_lastRelayPing > 10000 && g_Relay.connected) {
                                        s_lastRelayPing = now;
                                        auto hb = gupt::shared::SerializeMessage(gupt::shared::MessageType::Heartbeat, (uint8_t)1);
                                        g_Relay.Send(hb.data(), (uint32_t)hb.size());
                                    }

                                    // 2. If using Relay only, check for completely dead connection (no packets for 90s)
                                    if(g_IsConnected && g_UsingRelay && !g_UsingP2P) {
                                        uint64_t last = g_ClientLastRecvTick.load();
                                        if(last > 0 && (now - last > 90000)) {
                                            SetWindowTextA(hWnd, "Gupt - Stalled network, reconnecting...");
                                            g_ClientLastRecvTick = 0;
                                            g_Relay.Abort();
                                        }
                                        // Periodic P2P re-punch upgrade attempt after Wi-Fi switch or transient NAT drop
                                        if (g_Relay.connected && (now - lastP2PRepunchTick >= g_P2PRepunchMs)) {
                                            lastP2PRepunchTick = now;
                                            std::thread([]() {
                                                g_P2PTransport.Initialize();
                                                SendClientP2PCandidateAsync();
                                            }).detach();
                                        }
                                    }

                                    // 3. P2P Health Check & Seamless Relay Fallback:
                                    // If P2P was active but peer activity stopped for g_P2PFallbackMs,
                                    // fall back immediately to Relay without disconnecting the session!
                                    if(g_IsConnected && g_UsingP2P) {
                                        uint64_t p2pLast = g_P2PTransport.GetLastPeerActivityTick();
                                        uint64_t p2pSilence = (p2pLast > 0) ? (now - p2pLast) : 0;
                                        if(p2pSilence >= g_P2PFallbackMs) {
                                            g_UsingP2P = false;
                                            lastP2PRepunchTick = now;
                                            SetWindowTextA(hWnd, "Gupt - Connected (Relay Fallback)");
                                            GuptLog("P2P silent for %llu ms; seamless fallback to Relay", static_cast<unsigned long long>(p2pSilence));
                                            auto refMsg = MaybeEncryptOutbound(gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1));
                                            if(g_Relay.connected) {
                                                g_Relay.Send(refMsg.data(), (uint32_t)refMsg.size());
                                            }
                                        }
                                    }

                                    // 4. Frame stall recovery & dead connection detection
                                    if(g_IsConnected) {
                                        uint64_t lastRecv = g_ClientLastRecvTick.load();
                                        uint64_t lastFrame = g_ClientLastFrameRecvTick.load();
                                        uint64_t frameStalledMs = (lastFrame > 0) ? (now - lastFrame) : 3000;
                                        uint64_t totalStalledMs = (lastRecv > 0) ? (now - lastRecv) : 30000;

                                        // If frames haven't arrived for 3s, ask for a keyframe refresh
                                        if(frameStalledMs >= 3000 && frameStalledMs < 4000) {
                                            auto refMsg = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1);
                                            PushEv(refMsg);
                                            auto wireRef = MaybeEncryptOutbound(refMsg);
                                            if(g_Relay.connected) g_Relay.Send(wireRef.data(), (uint32_t)wireRef.size());
                                        }

                                        // ONLY perform hard reconnect if BOTH frames AND heartbeats have stopped for g_StallTimeoutMs
                                        if(totalStalledMs >= g_StallTimeoutMs) {
                                            g_ClientLastRecvTick = now;
                                            g_ClientLastFrameRecvTick = now;
                                            g_Relay.Abort();
                                        }
                                    }
                                }
                            }).detach();

                            while(g_IsConnected){
                                if(g_UsingRelay && !g_Relay.connected){
                                    SetWindowTextA(hWnd, "Gupt - Reconnecting session...");
                                    g_Relay.Open(g_SignalingHost, g_SignalingPort, rp, g_SignalingPort==443);
                                    if(!g_Relay.connected){ Sleep(300); continue; }
                                    g_CryptoSession.InitializeLocalKeyPair();
                                    auto r1 = MakeConnectRequest();
                                    g_Relay.Send(r1.data(), (uint32_t)r1.size());
                                    auto r2 = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1);
                                    g_Relay.Send(r2.data(), (uint32_t)r2.size());
                                    g_ClientLastRecvTick = GetTickCount64();
                                    SetWindowTextA(hWnd, "Gupt - Connected (Relay)");
                                    SendClientP2PCandidateAsync();
                                    SetupClientP2PCallback(hWnd);
                                }

                                auto d = g_Relay.Recv();
                                if(d.empty()){ Sleep(1); continue; }
                                g_ClientLastRecvTick = GetTickCount64();

                                auto msgs = gupt::shared::DeserializeMessages(d);
                                for(size_t i = 0; i < msgs.size(); ++i){
                                    if (msgs[i].first == gupt::shared::MessageType::ConnectResponse) {
                                        if (msgs[i].second.size() >= sizeof(gupt::shared::ConnectResponse)) {
                                            auto cr = reinterpret_cast<const gupt::shared::ConnectResponse*>(msgs[i].second.data());
                                            if (cr->accepted) {
                                                auto kx = MakeKeyExchangePacket();
                                                g_Relay.Send(kx.data(), static_cast<uint32_t>(kx.size()));
                                            }
                                        }
                                    } else {
                                        DispatchClientMessage(msgs[i].first, msgs[i].second, hWnd);
                                    }
                                }
                            }
                            g_IsConnected = false;
                            g_UsingP2P = false;
                            g_P2PTransport.Close();
                        } else { SetWindowTextA(hWnd, "Gupt - Handshake failed, retrying..."); g_Relay.Close(); Sleep(1500); }
                    } else { SetWindowTextA(hWnd, "Gupt - Connection failed, retrying..."); Sleep(1500); }
                }
            }).detach();
        } break;
    }
    case WM_GUPT_FRAME_ARRIVED: {
        InvalidateRect(hWnd, NULL, FALSE);
        UpdateWindow(hWnd);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps); 
        if(!g_BackDC || !g_BackBmp){ g_BackDC=CreateCompatibleDC(hdc); g_BackBmp=CreateCompatibleBitmap(hdc,g_WinW,g_WinH); SelectObject(g_BackDC,g_BackBmp); }
        HBRUSH hb=CreateSolidBrush(CLR_BG); RECT wr={0,0,g_WinW,g_WinH}; FillRect(g_BackDC,&wr,hb); DeleteObject(hb);
        { std::lock_guard<std::mutex> lk(g_FrameMutex); if(!g_LatestFrame.empty()){ if(g_DrawingFrame.size() != g_LatestFrame.size()) g_DrawingFrame.resize(g_LatestFrame.size()); memcpy(g_DrawingFrame.data(), g_LatestFrame.data(), g_LatestFrame.size()); g_DrawW = g_FrameW; g_DrawH = g_FrameH; } }
        if(!g_DrawingFrame.empty()){
            float ha=(float)g_DrawW/g_DrawH, ca=(float)g_WinW/g_WinH; if(ca>ha){ g_DestH=g_WinH; g_DestW=(int)(g_WinH*ha); g_DestX=(g_WinW-g_DestW)/2; g_DestY=0; } else { g_DestW=g_WinW; g_DestH=(int)(g_WinW/ha); g_DestX=0; g_DestY=(g_WinH-g_DestH)/2; }
            if(g_DestW == (int)g_DrawW && g_DestH == (int)g_DrawH) {
                SetStretchBltMode(g_BackDC, COLORONCOLOR);
            } else {
                SetStretchBltMode(g_BackDC, HALFTONE);
                SetBrushOrgEx(g_BackDC, 0, 0, NULL);
            }
            BITMAPINFO bmi = {0}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = (int)g_DrawW; 
            bmi.bmiHeader.biHeight = -(int)g_DrawH; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(g_BackDC,g_DestX,g_DestY,g_DestW,g_DestH,0,0,g_DrawW,g_DrawH,g_DrawingFrame.data(),&bmi,DIB_RGB_COLORS,SRCCOPY); 
        } else { SetTextColor(g_BackDC, CLR_SUBTEXT); SetBkMode(g_BackDC, TRANSPARENT); RECT tr={0,0,g_WinW,g_WinH}; DrawTextA(g_BackDC, g_IsConnected ? "Synchronizing..." : "Connecting...", -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        if(g_SidebarX<g_WinW){
            RECT sr={g_SidebarX,0,g_WinW,g_WinH}; DrawRoundedRect(g_BackDC,sr,0,CLR_CARD,CLR_BORDER,1);
            g_CloseBtnRect={g_SidebarX+20,20,g_WinW-20,65}; DrawRoundedRect(g_BackDC,g_CloseBtnRect,8,CLR_ACCENT,CLR_BORDER,1); DrawTextA(g_BackDC,"Disconnect",-1,&g_CloseBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            g_FullscreenBtnRect={g_SidebarX+20,80,g_WinW-20,125}; DrawRoundedRect(g_BackDC,g_FullscreenBtnRect,8,CLR_BG,CLR_BORDER,1); DrawTextA(g_BackDC,g_IsFullscreen?"Exit Fullscreen":"Fullscreen",-1,&g_FullscreenBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            g_ClipboardBtnRect={g_SidebarX+20,140,g_WinW-20,185}; DrawRoundedRect(g_BackDC,g_ClipboardBtnRect,8,g_ClipboardSyncEnabled?RGB(50,180,50):CLR_BG,CLR_BORDER,1); DrawTextA(g_BackDC,g_ClipboardSyncEnabled?"Sync Clipboard: ON":"Sync Clipboard: OFF",-1,&g_ClipboardBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            g_DisplayBtnRect={g_SidebarX+20,200,g_WinW-20,245}; DrawRoundedRect(g_BackDC,g_DisplayBtnRect,8,CLR_BG,CLR_BORDER,1);
            char dispBuf[48];
            uint32_t monCount = g_ClientMonitorCount.load();
            if (monCount > 1) sprintf_s(dispBuf, "Display: Screen %u/%u", g_ClientActiveMonitorIndex.load() + 1, monCount);
            else sprintf_s(dispBuf, "Display: Screen %u", g_ClientActiveMonitorIndex.load() + 1);
            DrawTextA(g_BackDC,dispBuf,-1,&g_DisplayBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        if (g_HostSecureDesktopActive.load()) {
            RECT uacR = {0, 0, g_WinW, 36};
            HBRUSH ub = CreateSolidBrush(RGB(220, 130, 20));
            FillRect(g_BackDC, &uacR, ub);
            DeleteObject(ub);
            SetTextColor(g_BackDC, RGB(255, 255, 255));
            SetBkMode(g_BackDC, TRANSPARENT);
            DrawTextA(g_BackDC, "⚠️ Host is interacting with a Windows UAC / Secure Desktop prompt (Input paused)", -1, &uacR, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        g_TabRect={g_SidebarX-30,g_WinH/2-40,g_SidebarX,g_WinH/2+40}; DrawRoundedRect(g_BackDC,g_TabRect,8,CLR_CARD,CLR_BORDER,1); SetTextColor(g_BackDC,CLR_SUBTEXT); DrawTextA(g_BackDC,g_SidebarOpen?">":"<",-1,&g_TabRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        BitBlt(hdc,0,0,g_WinW,g_WinH,g_BackDC,0,0,SRCCOPY); EndPaint(hWnd,&ps); break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: { if(((wp=='S' && (GetKeyState(VK_CONTROL)&0x8000)) || wp==VK_SNAPSHOT) && (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)){ SaveOrCopy1080pScreenshot(hWnd); return 0; } if((wp==VK_F5 || (wp=='R' && (GetKeyState(VK_CONTROL)&0x8000))) && (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)){ TriggerSessionRefresh(hWnd); return 0; } if(wp==VK_F11 && (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)){ g_IsFullscreen=!g_IsFullscreen; if(g_IsFullscreen){ GetWindowRect(hWnd,&g_SavedRect); SetWindowLongPtrA(hWnd,GWL_STYLE,WS_POPUP|WS_VISIBLE); SetWindowPos(hWnd,HWND_TOPMOST,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_FRAMECHANGED); } else { SetWindowLongPtrA(hWnd,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE); SetWindowPos(hWnd,HWND_NOTOPMOST,g_SavedRect.left,g_SavedRect.top,g_SavedRect.right-g_SavedRect.left,g_SavedRect.bottom-g_SavedRect.top,SWP_FRAMECHANGED); } InvalidateRect(hWnd, NULL, TRUE); return 0; } if(g_IsConnected){ gupt::shared::KeyboardEvent ke={(uint16_t)wp, (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyboardEvent,ke)); } break; }
    case WM_MOUSEHWHEEL:
    case WM_MOUSEWHEEL: {
        if(g_IsConnected && g_DestW > 0 && g_DestH > 0){
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            if(!g_SidebarOpen || pt.x < g_SidebarX){
                float nx = (float)(pt.x - g_DestX) / (float)g_DestW;
                float ny = (float)(pt.y - g_DestY) / (float)g_DestH;
                if(nx < 0.0f) nx = 0.0f; else if(nx > 1.0f) nx = 1.0f;
                if(ny < 0.0f) ny = 0.0f; else if(ny > 1.0f) ny = 1.0f;
                int16_t delta = (int16_t)GET_WHEEL_DELTA_WPARAM(wp);
                if(delta != 0){
                    uint8_t btn = (msg == WM_MOUSEHWHEEL) ? 254 : 255;
                    gupt::shared::MouseEvent me = { nx, ny, btn, false, (int)delta };
                    PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::MouseEvent, me));
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: {
        POINT pt={(short)LOWORD(lp),(short)HIWORD(lp)};
        if(PtInRect(&g_TabRect,pt)){
            if(msg==WM_LBUTTONDOWN){g_SidebarOpen=!g_SidebarOpen;SetTimer(hWnd,1,10,0);}
            return 0;
        }
        if(g_SidebarOpen && pt.x >= g_SidebarX){
            if(msg==WM_LBUTTONDOWN){
                if(PtInRect(&g_CloseBtnRect,pt))PostQuitMessage(0);
                if(PtInRect(&g_FullscreenBtnRect,pt))SendMessage(hWnd,WM_KEYDOWN,VK_F11,0);
                if(PtInRect(&g_ClipboardBtnRect,pt)) { g_ClipboardSyncEnabled = !g_ClipboardSyncEnabled; InvalidateRect(hWnd,0,FALSE); }
                if(PtInRect(&g_DisplayBtnRect,pt)) {
                    uint32_t monCount = g_ClientMonitorCount.load();
                    uint32_t nextMon = (monCount > 1) ? ((g_ClientActiveMonitorIndex.load() + 1) % monCount) : 0;
                    g_ClientActiveMonitorIndex = nextMon;
                    gupt::shared::SwitchMonitorMsg sm = { nextMon };
                    PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::SwitchMonitor, sm));
                    InvalidateRect(hWnd, 0, FALSE);
                }
            }
            return 0;
        }
        bool isDown = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK);
        uint8_t btnId = 0;
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK) btnId = 1;
        else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP || msg == WM_MBUTTONDBLCLK) btnId = 2;
        if(isDown) SetCapture(hWnd); else ReleaseCapture();
        if(g_IsConnected && g_DestW > 0 && g_DestH > 0){
            float nx = (float)(pt.x-g_DestX)/(float)g_DestW;
            float ny = (float)(pt.y-g_DestY)/(float)g_DestH;
            if(nx < 0.0f) nx = 0.0f; else if(nx > 1.0f) nx = 1.0f;
            if(ny < 0.0f) ny = 0.0f; else if(ny > 1.0f) ny = 1.0f;
            if(isDown && (pt.x < g_DestX || pt.x >= g_DestX+g_DestW || pt.y < g_DestY || pt.y >= g_DestY+g_DestH)){
                // outside remote area
            } else {
                SetFocus(hWnd);
                gupt::shared::MouseEvent me={nx, ny, btnId, isDown, 0};
                PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::MouseEvent,me));
            }
        }
        break;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        return DefWindowProc(hWnd, msg, wp, lp);
    }
    case WM_MOUSEMOVE: {
        static uint64_t lastMove = 0; uint64_t now = GetTickCount64();
        bool dragging = (wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0;
        uint64_t minInterval = dragging ? 12 : 16;
        if(now - lastMove < minInterval) break;
        lastMove = now;
        int x=(short)LOWORD(lp),y=(short)HIWORD(lp);
        if(g_IsConnected && (!g_SidebarOpen || x < g_SidebarX) && g_DestW > 0 && g_DestH > 0){
            if(dragging || (x>=g_DestX && x<g_DestX+g_DestW && y>=g_DestY && y<g_DestY+g_DestH)){
                float nx = (float)(x-g_DestX)/(float)g_DestW;
                float ny = (float)(y-g_DestY)/(float)g_DestH;
                if(nx < 0.0f) nx = 0.0f; else if(nx > 1.0f) nx = 1.0f;
                if(ny < 0.0f) ny = 0.0f; else if(ny > 1.0f) ny = 1.0f;
                gupt::shared::MouseEvent me={nx, ny, 255, dragging, 0};
                PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::MouseEvent,me));
            }
        }
        break;
    }
    case WM_CLIPBOARDUPDATE: {
        if(g_IsConnected && g_ClipboardSyncEnabled) {
            std::string text = SafeGetClipboardText(hWnd);
            if(!text.empty() && text != g_clientLastFromHost && text != g_clientLastSent) {
                g_clientLastSent = text;
                PushEv(gupt::shared::SerializeClipboardText(text));
            }
        }
        break;
    }
    case WM_KILLFOCUS: {
        ReleaseCapture();
        if (g_IsConnected.load()) {
            static constexpr uint16_t kModifiers[] = { VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN };
            for (uint16_t vk : kModifiers) {
                gupt::shared::KeyboardEvent ke = { vk, false };
                PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyboardEvent, ke));
            }
        }
        break;
    }
    case WM_SIZE: { g_WinW=LOWORD(lp); g_WinH=HIWORD(lp); g_SidebarX=g_SidebarOpen?g_WinW-260:g_WinW; if(g_BackDC){ DeleteDC(g_BackDC); g_BackDC=NULL; } if(g_BackBmp){ DeleteObject(g_BackBmp); g_BackBmp=NULL; } InvalidateRect(hWnd, NULL, TRUE); break; }
    case WM_CLOSE:
    case WM_DESTROY: {
        if(g_hKbdHook) {
            UnhookWindowsHookEx(g_hKbdHook);
            g_hKbdHook = NULL;
        }
        if(g_IsConnected.load()) {
            auto disc = gupt::shared::SerializeMessage(gupt::shared::MessageType::Disconnect, static_cast<uint8_t>(1));
            if(g_UsingP2P && g_P2PTransport.IsConnected()) g_P2PTransport.Send(disc.data(), disc.size());
            if(g_Relay.connected) g_Relay.Send(disc.data(), static_cast<uint32_t>(disc.size()));
            if(g_Client.IsConnected()) g_Client.SendRaw(disc);
        }
        g_IsConnected = false;
        g_UsingP2P = false;
        g_HostSecureDesktopActive = false;
        g_ClientMonitorCount = 1;
        g_AppRunning = false;
        g_CryptoSession.Reset();
        g_ClientDecoderRunning = false;
        g_ClientDecodeCv.notify_all();
        g_EvCv.notify_all();
        PostQuitMessage(0);
        break;
    }
    default: return DefWindowProcA(hWnd,msg,wp,lp);
    } return 0;
}

LRESULT CALLBACK KbdHookProc(int code, WPARAM wp, LPARAM lp) { if(code == HC_ACTION && g_IsFullscreen && g_IsConnected) { KBDLLHOOKSTRUCT* hs = (KBDLLHOOKSTRUCT*)lp; bool sysKey = (hs->vkCode == VK_TAB && (hs->flags & LLKHF_ALTDOWN)) || (hs->vkCode == VK_LWIN) || (hs->vkCode == VK_RWIN) || (hs->vkCode == VK_ESCAPE && (GetKeyState(VK_CONTROL)&0x8000)); if(sysKey || (GetKeyState(VK_LWIN)&0x8000) || (GetKeyState(VK_RWIN)&0x8000)) { gupt::shared::KeyboardEvent ke = {(uint16_t)hs->vkCode, (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyboardEvent, ke)); return 1; } } return CallNextHookEx(g_hKbdHook, code, wp, lp); }

static void RunClientMode(HINSTANCE hInst) {
    GuptLog("Gupt Client Mode initialized. Target: %s, Signaling Host: %s:%d", g_HostIp.c_str(), g_SignalingHost.c_str(), g_SignalingPort);
    std::thread(PowerKeepAwakeLoop).detach();

    WNDCLASSEXA wc={sizeof(WNDCLASSEXA)}; wc.lpfnWndProc=ClientWndProc; wc.hInstance=hInst; wc.hIcon=(HICON)LoadImageA(hInst,MAKEINTRESOURCEA(101),IMAGE_ICON,32,32,0); wc.lpszClassName="GuptC"; wc.hCursor=LoadCursor(NULL,IDC_ARROW); RegisterClassExA(&wc);
    HWND hWnd=CreateWindowExA(WS_EX_APPWINDOW,"GuptC","Gupt Remote Desktop",WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,1280,720,NULL,NULL,hInst,NULL); SetFocus(hWnd); g_SidebarOpen = false; g_SidebarX = 1280;
    AddClipboardFormatListener(hWnd); g_AudioPlayer.Initialize(48000, 2, 16);
    std::thread([]() { g_P2PTransport.Initialize(); }).detach();
    g_hKbdHook = SetWindowsHookEx(WH_KEYBOARD_LL, KbdHookProc, hInst, 0);
    g_Client.SetMessageCallback([hWnd](gupt::shared::MessageType t, const std::vector<uint8_t>& p){
        if(t == gupt::shared::MessageType::ConnectResponse && p.size() >= sizeof(gupt::shared::ConnectResponse)){
            auto res = reinterpret_cast<const gupt::shared::ConnectResponse*>(p.data());
            g_IsConnected = res->accepted;
            if(res->accepted) {
                SetWindowTextA(hWnd, "Gupt - Connected (LAN)");
                g_Client.SendRaw(MakeKeyExchangePacket());
            } else {
                std::string msgText = res->reason[0]
                    ? std::string(res->reason, strnlen(res->reason, sizeof(res->reason)))
                    : std::string("Connection denied");
                SetWindowTextA(hWnd, ("Gupt - " + msgText).c_str());
                GuptLog("LAN Connection denied: %s", msgText.c_str());
            }
        } else if(t == gupt::shared::MessageType::Disconnect) {
            g_IsConnected = false;
            g_CryptoSession.Reset();
            SetWindowTextA(hWnd, "Gupt - Disconnected");
        } else {
            DispatchClientMessage(t, p, hWnd);
        }
    });
    
    // Independent Sovereign Heartbeat Thread
    std::thread([](){ while(true){ if(g_IsConnected){ auto hb=gupt::shared::SerializeMessage(gupt::shared::MessageType::Heartbeat,(uint8_t)1); PushEv(std::move(hb)); } Sleep(2000); } }).detach();

    g_ClientHwnd = hWnd;
    std::thread(ClientDecoderWorkerLoop).detach();
    SetTimer(hWnd, 4, 33, 0);
    SetTimer(hWnd, 2, 100, 0); MSG m; while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(g_hKbdHook) {
        UnhookWindowsHookEx(g_hKbdHook);
        g_hKbdHook = NULL;
    }
    g_ClientDecoderRunning = false;
    g_ClientDecodeCv.notify_all();
}

LRESULT CALLBACK LauncherWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        g_FontTitle=CreateFontA(24,0,0,0,900,0,0,0,0,0,0,0,0,"Segoe UI"); g_FontBody=CreateFontA(16,0,0,0,400,0,0,0,0,0,0,0,0,"Segoe UI"); g_FontBtn=CreateFontA(18,0,0,0,700,0,0,0,0,0,0,0,0,"Segoe UI");
        HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance;
        g_hRadioHost=CreateWindowExA(0,"BUTTON","Host (Share Screen)",WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,60,140,250,30,hWnd,(HMENU)101,hi,0);
        g_hRadioClient=CreateWindowExA(0,"BUTTON","Client (View Screen)",WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,60,180,250,30,hWnd,(HMENU)102,hi,0);
        SendMessage(g_hRadioHost,BM_SETCHECK,BST_CHECKED,0);
        g_hEditIp=CreateWindowExA(0,"EDIT","",WS_CHILD|WS_BORDER,60,225,300,32,hWnd,(HMENU)103,hi,0);
        CreateWindowExA(0,"BUTTON","Launch Gupt",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,60,290,140,42,hWnd,(HMENU)1,hi,0);
        CreateWindowExA(0,"BUTTON","Cancel",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,220,290,140,42,hWnd,(HMENU)2,hi,0);
        SendMessage(g_hRadioHost,WM_SETFONT,(WPARAM)g_FontBody,1); SendMessage(g_hRadioClient,WM_SETFONT,(WPARAM)g_FontBody,1); SendMessage(g_hEditIp,WM_SETFONT,(WPARAM)g_FontBody,1);
        break;
    }
    case WM_CTLCOLORSTATIC: SetTextColor((HDC)wp,CLR_TEXT);SetBkColor((HDC)wp,CLR_CARD);return(LRESULT)CreateSolidBrush(CLR_CARD);
    case WM_PAINT: { 
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps); RECT r; GetClientRect(hWnd,&r);
        HDC hdcBack = CreateCompatibleDC(hdc); HBITMAP hbmBack = CreateCompatibleBitmap(hdc, r.right, r.bottom); HBITMAP hOld = (HBITMAP)SelectObject(hdcBack, hbmBack);
        RECT hr={0,0,r.right,100}; HBRUSH hhb=CreateSolidBrush(CLR_HEADER); FillRect(hdcBack,&hr,hhb); DeleteObject(hhb);
        RECT bbr={0,100,r.right,r.bottom}; HBRUSH bbb=CreateSolidBrush(CLR_BG); FillRect(hdcBack,&bbr,bbb); DeleteObject(bbb);
        SetBkMode(hdcBack,TRANSPARENT); SelectObject(hdcBack,g_FontTitle); SetTextColor(hdcBack,RGB(255,255,255));
        RECT tr={25,25,r.right,65}; DrawTextA(hdcBack,"Gupt Remote Desktop",-1,&tr,0);
        SelectObject(hdcBack,g_FontBody); SetTextColor(hdcBack,RGB(210,210,255));
        RECT sr={25,55,r.right,85}; DrawTextA(hdcBack,"Secure, lightweight screen sharing",-1,&sr,0);
        RECT gr={30,120,r.right-30,240}; DrawRoundedRect(hdcBack,gr,10,CLR_CARD,CLR_BORDER,1);
        BitBlt(hdc, 0, 0, r.right, r.bottom, hdcBack, 0, 0, SRCCOPY);
        SelectObject(hdcBack, hOld); DeleteObject(hbmBack); DeleteDC(hdcBack); EndPaint(hWnd,&ps); break;
    }
    case WM_COMMAND: {
        if(LOWORD(wp)==101) ShowWindow(g_hEditIp, SW_HIDE);
        if(LOWORD(wp)==102) ShowWindow(g_hEditIp, SW_SHOW);
        if(LOWORD(wp)==1){
            g_LaunchAsHost = (SendMessage(g_hRadioHost, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if(!g_LaunchAsHost){
                char b[128] = {0};
                GetWindowTextA(g_hEditIp, b, sizeof(b)-1);
                g_HostIp = b;
            }
            g_Launched = 1;
            DestroyWindow(hWnd);
        }
        if(LOWORD(wp)==2) DestroyWindow(hWnd);
        break;
    }
    case WM_DRAWITEM: { LPDRAWITEMSTRUCT d=(LPDRAWITEMSTRUCT)lp; bool l=(d->CtlID==1); POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt); bool h = PtInRect(&d->rcItem, pt); bool s = (d->itemState & ODS_SELECTED); COLORREF bg = l ? (s ? RGB(100, 70, 220) : (h ? RGB(130, 100, 255) : CLR_ACCENT)) : (s ? RGB(30, 30, 45) : (h ? RGB(45, 45, 65) : CLR_CARD)); DrawRoundedRect(d->hDC, d->rcItem, 8, bg, CLR_BORDER, 1); SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, CLR_TEXT); SelectObject(d->hDC, g_FontBtn); DrawTextA(d->hDC, l?"Launch Gupt":"Cancel", -1, &d->rcItem, DT_CENTER|DT_VCENTER|DT_SINGLELINE); return 1; }
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProcA(hWnd,msg,wp,lp);
    } return 0;
}


int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    SetUnhandledExceptionFilter(GuptCrashHandler);
    GuptLog("Gupt process started. Command line: %s", GetCommandLineA());

    typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        PFN_SetProcessDpiAwarenessContext pfn = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pfn) pfn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        else SetProcessDPIAware();
    } else {
        SetProcessDPIAware();
    }
    CoInitializeEx(0, COINIT_MULTITHREADED);
    std::thread(EvSenderLoop).detach();

    // CLI overrides support (Zero Hardcoding)
    g_SignalingCandidates = GetConfiguredSignalingCandidates();
    for (int i = 1; i < __argc; ++i) {
        std::string arg = __argv[i];
        if ((arg == "--relay" || arg == "-r") && i + 1 < __argc) {
            g_SignalingCandidates = { __argv[++i] };
        } else if (arg == "--relays" && i + 1 < __argc) {
            auto parsed = ParseRelayList(__argv[++i]);
            if (!parsed.empty()) g_SignalingCandidates = parsed;
        } else if ((arg == "--port" || arg == "-p") && i + 1 < __argc) {
            g_SignalingPort = atoi(__argv[++i]);
        } else if (arg == "--lan-port" && i + 1 < __argc) {
            int p = atoi(__argv[++i]);
            if (p > 0 && p <= 65535) g_LanPort = static_cast<uint16_t>(p);
        } else if (arg == "--headless" || arg == "--test") {
            g_Headless = true;
        } else if (arg == "--host") {
            g_LaunchAsHost = true;
            g_Launched = 1;
        } else if ((arg == "--client" || arg == "-c") && i + 1 < __argc) {
            g_LaunchAsHost = false;
            g_Launched = 1;
            g_HostIp = __argv[++i];
        }
    }

    if (!g_SignalingCandidates.empty()) {
        SelectFastestSignalingRelay(g_SignalingCandidates, g_SignalingPort);
    }

    if (!g_Launched) {
        WNDCLASSEXA wc={sizeof(WNDCLASSEXA)}; wc.lpfnWndProc=LauncherWndProc; wc.hInstance=h; wc.hIcon=(HICON)LoadImageA(h,MAKEINTRESOURCEA(101),IMAGE_ICON,32,32,0); wc.lpszClassName="GuptL"; wc.hbrBackground=CreateSolidBrush(CLR_BG); wc.hCursor=LoadCursor(0,IDC_ARROW); RegisterClassExA(&wc);
        DWORD style = WS_SYSMENU|WS_CAPTION|WS_MINIMIZEBOX|WS_VISIBLE; RECT wr = {0, 0, 380, 380}; AdjustWindowRectEx(&wr, style, FALSE, 0);
        HWND hW=CreateWindowExA(WS_EX_APPWINDOW,"GuptL","Gupt Remote Desktop",style,CW_USEDEFAULT,CW_USEDEFAULT,wr.right-wr.left,wr.bottom-wr.top,0,0,h,0);
        ShowWindow(g_hEditIp,SW_HIDE); MSG m; while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    }

    if(!g_Launched) return 0;
    if(g_LaunchAsHost) RunHostMode(); else RunClientMode(h);
    return 0;
}



