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
#include "Shared/Protocol.h"
#include "Core/Network/TcpNetwork.h"
#include "Core/Input/InputInjector.h"
#include "Core/Capture/ScreenCapturer.h"
#include "Core/Audio/AudioCapturer.h"
#include "Core/Audio/AudioPlayer.h"
#include "Core/Video/H264Encoder.h"
#include "Core/Video/H264Decoder.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

using namespace gupt;

// ── Global State & Configurations ───────────────────────────────────────────
static std::string g_SignalingHost = "gupt-signal-server-560359652969.us-central1.run.app";
static int g_SignalingPort = 443;
static std::string g_CurrentSessionId = "";
static std::string g_HostIp = "";
static bool g_LaunchAsHost = false;
static int g_Launched = 0;
static bool g_ForceFullScan = true;
static bool g_IsConnected = false, g_UsingRelay = false, g_SidebarOpen = false, g_IsFullscreen = false, g_SessionActive = false, g_ClipboardSyncEnabled = true;
static std::atomic<bool> g_DeliveryBusy{false};
static std::atomic<uint32_t> g_HostLastSentFrameNum{0};
static std::atomic<uint32_t> g_HostLastAckedFrameNum{0};
static std::atomic<uint64_t> g_HostLastFrameSendTick{0};
static std::atomic<uint64_t> g_ClientLastFrameRecvTick{0};
static gupt::core::audio::AudioPlayer g_AudioPlayer;

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
        std::lock_guard<std::mutex> lk(stateMtx);
        CloseInternal();
    }

    void Abort() {
        connected = false;
        HINTERNET wsToClose = NULL;
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            wsToClose = hWS;
            hWS = NULL;
        }
        if(wsToClose) {
            WinHttpWebSocketShutdown(wsToClose, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle(wsToClose);
        }
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
        if(!connected || !hWS) return {};
        std::vector<uint8_t> buf;
        std::vector<uint8_t> frag(64*1024);
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD read = 0;
        do {
            HINTERNET wsLocal = hWS;
            if(!wsLocal || !connected) return {};
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
            WinHttpWebSocketShutdown(hWS, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
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
            // Audio queue cap: max 2 audio packets (~40ms) to prevent video starvation
            int audioCount = 0;
            for(auto& q : g_SendQueue) { if(q.size() > 400) audioCount++; }
            if(audioCount >= 2) {
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

static void ScaleTo1080p(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH, std::vector<uint8_t>& dst, uint32_t& dstW, uint32_t& dstH) {
    dstW = 1920;
    dstH = 1080;
    size_t targetSize = (size_t)dstW * dstH * 4;
    if (dst.size() != targetSize) {
        dst.resize(targetSize);
    }

    if (srcW == dstW && srcH == dstH) {
        memcpy(dst.data(), src.data(), targetSize);
        return;
    }

    // High precision fixed-point bilinear interpolation for clean 1080p output
    uint32_t xRatio = (srcW > 1) ? (((srcW - 1) << 16) / (dstW - 1)) : 0;
    uint32_t yRatio = (srcH > 1) ? (((srcH - 1) << 16) / (dstH - 1)) : 0;

    for (uint32_t y = 0; y < dstH; ++y) {
        uint32_t fpY = y * yRatio;
        uint32_t srcY = fpY >> 16;
        uint32_t yDiff = fpY & 0xFFFF;
        uint32_t yDiffInv = 0x10000 - yDiff;

        uint32_t nextY = (srcY + 1 < srcH) ? (srcY + 1) : srcY;
        const uint32_t* row0 = reinterpret_cast<const uint32_t*>(src.data() + srcY * srcW * 4);
        const uint32_t* row1 = reinterpret_cast<const uint32_t*>(src.data() + nextY * srcW * 4);
        uint32_t* dstRow = reinterpret_cast<uint32_t*>(dst.data() + y * dstW * 4);

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
            uint64_t now = GetTickCount64();
            bool isFullScan = g_ForceFullScan || (now - s_lastFullScanTick > 2500) || !hasDirty || (dw == 0 || dh == 0);
            
            uint32_t targetX = 0, targetY = 0, targetW = w, targetH = h;
            if (!isFullScan) {
                // 8-pixel alignment for optimal JPEG DCT block compression
                targetX = (dx / 8) * 8;
                targetY = (dy / 8) * 8;
                uint32_t right = ((dx + dw + 7) / 8) * 8;
                uint32_t bottom = ((dy + dh + 7) / 8) * 8;
                if (right > w) right = w;
                if (bottom > h) bottom = h;
                targetW = right - targetX;
                targetH = bottom - targetY;
                if ((size_t)targetW * targetH >= (size_t)w * h * 75 / 100) {
                    targetX = 0; targetY = 0; targetW = w; targetH = h;
                    isFullScan = true;
                }
            }

            if (isFullScan) {
                s_lastFullScanTick = now;
                g_ForceFullScan = false;
            }

            static gupt::core::video::H264Encoder s_h264Encoder;
            if(!s_h264Encoder.IsInitialized() || s_h264Encoder.GetWidth() != w || s_h264Encoder.GetHeight() != h) {
                s_h264Encoder.Initialize(w, h, 60, 2500000);
            }

            bool encodedH264 = false;
            if(s_h264Encoder.IsInitialized()) {
                std::vector<uint8_t> nalData;
                bool isKeyframe = isFullScan || g_ForceFullScan;
                if(s_h264Encoder.EncodeBgraFrame(raw.data(), w, h, nalData, isKeyframe)) {
                    if(isKeyframe) {
                        s_lastFullScanTick = now;
                        g_ForceFullScan = false;
                    }
                    uint32_t fn = ++s_frameCounter;
                    g_HostLastSentFrameNum = fn;
                    g_HostLastFrameSendTick = GetTickCount64();
                    gupt::shared::FrameDataHeader fHdr = {
                        fn, w, h, w, h, 0, 0, 264, !isKeyframe, now
                    };
                    PushOutbound(gupt::shared::SerializeFrame(fHdr, nalData), false);
                    encodedH264 = true;
                }
            }

            // Graceful fallback to JPEG if hardware encoder is unavailable
            if(!encodedH264) {
                std::vector<uint8_t> jpg;
                int quality = isFullScan ? 82 : 86;
                if(capturer->CaptureRegionJpeg(raw, w, h, targetX, targetY, targetW, targetH, jpg, quality, true)){
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
            if(srv->IsClientConnected()) srv->SendRaw(c);
            else if(g_Relay.connected) g_Relay.Send(c.data(), (uint32_t)c.size());
        }
        if(!frame.empty()) {
            if(srv->IsClientConnected()) srv->SendRaw(frame);
            else if(g_Relay.connected) g_Relay.Send(frame.data(), (uint32_t)frame.size());
        }
    }
}

static std::vector<uint8_t> g_LatestFrame, g_DrawingFrame;
static uint32_t g_FrameW = 0, g_FrameH = 0, g_DrawW = 0, g_DrawH = 0;
static int g_DestX = 0, g_DestY = 0, g_DestW = 800, g_DestH = 600, g_WinW = 0, g_WinH = 0, g_SidebarX = 0;
static HDC g_BackDC = NULL; static HBITMAP g_BackBmp = NULL;
static RECT g_SavedRect={0,0,800,600}, g_TabRect, g_CloseBtnRect, g_FullscreenBtnRect, g_ClipboardBtnRect;
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
        } else { 
            // All mouse clicks, releases, wheel events, keys, and clipboard are 100% preserved
            g_EvQueue.push_back(std::move(m)); 
        }
    }
    g_EvCv.notify_one();
}

static void EvSenderLoop() {
    while(true) {
        std::vector<uint8_t> m;
        {
            std::unique_lock<std::mutex> lk(g_EvMtx);
            g_EvCv.wait(lk, []{ return !g_EvQueue.empty(); });
            while(g_EvQueue.size() > 1 && IsPureMouseMove(g_EvQueue.front())) {
                g_EvQueue.erase(g_EvQueue.begin());
            }
            m = std::move(g_EvQueue.front()); g_EvQueue.erase(g_EvQueue.begin());
        }
        if(g_IsConnected) {
            if(g_UsingRelay) g_Relay.Send(m.data(), (uint32_t)m.size()); else g_Client.SendRaw(m);
        }
    }
}
static void TriggerSessionRefresh(HWND hWnd) {
    g_LastRefreshTime = GetTickCount64();
    if(hWnd) {
        SetWindowTextA(hWnd, "Gupt - Refreshing...");
        InvalidateRect(hWnd, NULL, FALSE);
    }
    if (g_UsingRelay) {
        g_Relay.Abort();
    } else if (g_Client.IsConnected()) {
        auto refMsg = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1);
        g_Client.SendRaw(refMsg);
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

std::string GetClipboardText() {
    std::string res; if (!OpenClipboard(NULL)) return "";
    HANDLE h = GetClipboardData(CF_UNICODETEXT); if (h) {
        wchar_t* w = (wchar_t*)GlobalLock(h); if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
            if (n > 1) { res.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &res[0], n, 0, 0); }
            GlobalUnlock(h); } }
    CloseClipboard(); return res;
}

static std::vector<uint8_t> MakeConnectRequest() {
    gupt::shared::ConnectRequest req = {};
    strncpy(req.sessionId, g_CurrentSessionId.empty() ? "GUPT_SESSION" : g_CurrentSessionId.c_str(), sizeof(req.sessionId) - 1);
    strncpy(req.authenticationToken, "GUPT_LOCAL_TOKEN", sizeof(req.authenticationToken) - 1);
    return gupt::shared::SerializeMessage(gupt::shared::MessageType::ConnectRequest, req);
}



static void RunHostMode() {
    std::thread(PowerKeepAwakeLoop).detach();

    core::input::InputInjector injector;
    injector.Initialize();
    core::capture::ScreenCapturer capturer;
    capturer.Initialize();
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

    // Auto-copy Session ID to clipboard for immediate convenience
    {
        int wn = MultiByteToWideChar(CP_UTF8, 0, g_CurrentSessionId.c_str(), -1, 0, 0);
        if (wn > 0) {
            HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, wn * sizeof(wchar_t));
            wchar_t* d = (wchar_t*)GlobalLock(m);
            MultiByteToWideChar(CP_UTF8, 0, g_CurrentSessionId.c_str(), -1, d, wn);
            GlobalUnlock(m);
            if (OpenClipboard(NULL)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, m); CloseClipboard(); }
        }
    }

    std::string rPath = "/relay?role=host&session=" + g_CurrentSessionId;
    g_Relay.Open(g_SignalingHost, g_SignalingPort, rPath, g_SignalingPort == 443);

    // Watchdog for idle/stalled relay socket
    static std::atomic<uint64_t> g_HostLastRecvTick{0};
    std::thread([](){
        while(true) {
            Sleep(1000);
            if(g_SessionActive && g_UsingRelay) {
                uint64_t last = g_HostLastRecvTick.load();
                if(last > 0 && (GetTickCount64() - last > 90000)) {
                    g_HostLastRecvTick = 0;
                    g_Relay.Abort();
                }
            }
        }
    }).detach();

    // Dedicated Relay Receiver Thread: Unattended, immediate acceptance like Chrome Remote Desktop
    std::thread([rPath, &injector, &capturer]() {
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
                    if (g_SessionActive) {
                        shared::ConnectResponse res = {true, "OK"};
                        auto f = shared::SerializeMessage(shared::MessageType::ConnectResponse, res);
                        g_Relay.Send(f.data(), (uint32_t)f.size());
                        g_ForceFullScan = true;
                    } else if (MessageBoxA(NULL, "Remote client is requesting access via Internet. Allow?", "Gupt Security", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES) {
                        g_SessionActive = true;
                        g_UsingRelay = true;
                        shared::ConnectResponse res = {true, "OK"};
                        auto f = shared::SerializeMessage(shared::MessageType::ConnectResponse, res);
                        g_Relay.Send(f.data(), (uint32_t)f.size());
                        g_ForceFullScan = true;
                    } else {
                        shared::ConnectResponse res = {false, "Denied"};
                        auto f = shared::SerializeMessage(shared::MessageType::ConnectResponse, res);
                        g_Relay.Send(f.data(), (uint32_t)f.size());
                    }
                }
                else if(t == shared::MessageType::Disconnect) {
                    g_SessionActive = false;
                }
                else if(t == shared::MessageType::MouseEvent) {
                    injector.IngestMouseEvent(*reinterpret_cast<const shared::MouseEvent*>(p.data()));
                }
                else if(t == shared::MessageType::KeyboardEvent) {
                    injector.IngestKeyboardEvent(*reinterpret_cast<const shared::KeyboardEvent*>(p.data()));
                }
                else if(t == shared::MessageType::RefreshRequest) {
                    g_ForceFullScan = true;
                }
                else if(t == shared::MessageType::FrameAck) {
                    if(p.size() >= sizeof(shared::FrameAck)) {
                        auto ack = reinterpret_cast<const shared::FrameAck*>(p.data());
                        g_HostLastAckedFrameNum = ack->frameNumber;
                    }
                }
                else if(g_ClipboardSyncEnabled && t == shared::MessageType::ClipboardText){
                    std::string s((char*)p.data(),p.size());
                    { std::lock_guard<std::mutex> lk(g_hostClipMtx); g_lastFromClient = s; }
                    int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, 0, 0);
                    if(wn > 0){
                        HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, wn * sizeof(wchar_t));
                        wchar_t* d = (wchar_t*)GlobalLock(m);
                        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, d, wn);
                        GlobalUnlock(m);
                        if(OpenClipboard(NULL)){ EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, m); CloseClipboard(); }
                    }
                }
            }
        }
    }).detach();

    // LAN TCP Server
    core::network::TcpServer server(8080);
    server.SetMessageCallback([&](shared::MessageType t, const std::vector<uint8_t>& p) {
        if(t == shared::MessageType::ConnectRequest) {
            if (g_SessionActive) {
                shared::ConnectResponse res={true, "OK"};
                server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
                g_ForceFullScan = true;
            } else if (MessageBoxA(NULL, "Remote client is requesting access on LAN. Allow?", "Gupt Security", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES) {
                g_SessionActive = true;
                shared::ConnectResponse res={true, "OK"};
                server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
                g_ForceFullScan = true;
            } else {
                shared::ConnectResponse res={false, "Denied"};
                server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse, res));
            }
        }
        else if(t == shared::MessageType::Disconnect) {
            g_SessionActive = false;
        }
        else if(t == shared::MessageType::MouseEvent) { injector.IngestMouseEvent(*reinterpret_cast<const shared::MouseEvent*>(p.data())); }
        else if(t == shared::MessageType::KeyboardEvent) { injector.IngestKeyboardEvent(*reinterpret_cast<const shared::KeyboardEvent*>(p.data())); }
        else if(t == shared::MessageType::RefreshRequest) { g_ForceFullScan = true; }
        else if(t == shared::MessageType::FrameAck) {
            if(p.size() >= sizeof(shared::FrameAck)) {
                auto ack = reinterpret_cast<const shared::FrameAck*>(p.data());
                g_HostLastAckedFrameNum = ack->frameNumber;
            }
        }
        else if(g_ClipboardSyncEnabled && t == shared::MessageType::ClipboardText){
            std::string s((char*)p.data(),p.size()); { std::lock_guard<std::mutex> lk(g_hostClipMtx); g_lastFromClient=s; }
            int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, 0, 0);
            if(wn > 0){
                HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, wn * sizeof(wchar_t));
                wchar_t* d = (wchar_t*)GlobalLock(m);
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, d, wn);
                GlobalUnlock(m);
                if(OpenClipboard(NULL)){ EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, m); CloseClipboard(); }
            }
        }
    });
    server.Start();

    // Audio Capturer
    core::audio::AudioCapturer audioCapturer;
    audioCapturer.Start([](uint32_t sampleRate, uint16_t channels, uint16_t bits, const int16_t* samples, size_t sampleCount) {
        if (g_SessionActive) {
            auto pkt = shared::SerializeAudio(sampleRate, channels, bits, samples, sampleCount);
            PushOutbound(std::move(pkt), true);
        }
    });

    // Start background Encoder and Delivery loops
    std::thread(HostEncoderLoop, &capturer).detach();
    std::thread(HostDeliveryLoop, &server).detach();
    std::thread([](){ while(true){ if(g_SessionActive){ auto hb=shared::SerializeMessage(shared::MessageType::Heartbeat,(uint8_t)1); PushOutbound(std::move(hb),true); } Sleep(2000); } }).detach();

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
                if(g_Relay.connected) g_Relay.Send(s.data(), (uint32_t)s.size());
            }
        }
        return DefWindowProcA(hw, m, w, l);
    };
    hwc.lpszClassName = "GuptH";
    hwc.hInstance = GetModuleHandle(NULL);
    RegisterClassExA(&hwc);
    HWND hHidden = CreateWindowExA(0, "GuptH", 0, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
    AddClipboardFormatListener(hHidden);

    // Dedicated 60 FPS Capture Thread (Runs decoupled in background immediately with sender-paced flow control)
    std::thread([&capturer, &injector]() {
        while(true) {
            if(g_SessionActive) {
                // Self-healing sender-paced flow control: paces up to 60ms, then advances ack to prevent deadlock
                uint32_t inFlight = g_HostLastSentFrameNum.load() - g_HostLastAckedFrameNum.load();
                if(inFlight >= 2) {
                    uint64_t diff = GetTickCount64() - g_HostLastFrameSendTick.load();
                    if(diff < 60) {
                        Sleep(8);
                        continue;
                    } else {
                        // Advance ack to keep live stream fluid if client dropped a packet
                        g_HostLastAckedFrameNum = g_HostLastSentFrameNum.load() - 1;
                    }
                }
                uint64_t loopStart = GetTickCount64();
                std::vector<uint8_t> raw; uint32_t w, h;
                if(capturer.CaptureNextFrame(raw, w, h)) {
                    injector.SetScreenDimensions(w, h);
                    uint32_t dx = 0, dy = 0, dw = 0, dh = 0;
                    bool hasDirty = capturer.GetLastDirtyBounds(dx, dy, dw, dh);
                    {
                        std::lock_guard<std::mutex> lk(g_EncMtx);
                        g_RawQueue = std::move(raw); g_RawW = w; g_RawH = h;
                        g_DirtyX = dx; g_DirtyY = dy; g_DirtyW = dw; g_DirtyH = dh;
                        g_HasDirtyBounds = hasDirty;
                    }
                    g_EncCv.notify_one();
                }
                uint64_t el = GetTickCount64() - loopStart;
                if(16 > el) Sleep((DWORD)(16 - el));
            } else {
                Sleep(50);
            }
        }
    }).detach();

    MessageBoxA(NULL, ("Gupt Host is active!\n\nSession ID: " + g_CurrentSessionId).c_str(), "Gupt", MB_OK | MB_ICONINFORMATION);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
void ProcessFrame(const std::vector<uint8_t>& p, HWND hWnd) {
    if(p.size() < sizeof(gupt::shared::FrameDataHeader)) return;
    gupt::shared::FrameDataHeader* hdr = (gupt::shared::FrameDataHeader*)p.data();
    uint32_t payloadSize = (UINT)(p.size() - sizeof(gupt::shared::FrameDataHeader));
    if(payloadSize == 0) return;

    // Fast-path Hardware H.264 Decoding
    if(hdr->bpp == 264) {
        static gupt::core::video::H264Decoder s_h264Decoder;
        if(!s_h264Decoder.IsInitialized()) {
            s_h264Decoder.Initialize(hdr->totalWidth, hdr->totalHeight);
        }
        std::vector<uint8_t> bgra;
        uint32_t decW = 0, decH = 0;
        if(s_h264Decoder.DecodeNal(p.data() + sizeof(gupt::shared::FrameDataHeader), payloadSize, bgra, decW, decH)) {
            {
                std::lock_guard<std::mutex> lk(g_FrameMutex);
                g_FrameW = decW;
                g_FrameH = decH;
                g_LatestFrame = std::move(bgra);
            }
            if(hWnd && g_LastRefreshTime > 0 && GetTickCount64() - g_LastRefreshTime < 4000){
                SetWindowTextA(hWnd, g_UsingRelay ? "Gupt - Connected (Relay)" : "Gupt - Connected (LAN)");
            }
            if(hWnd) PostMessage(hWnd, WM_GUPT_FRAME_ARRIVED, 0, 0);
            g_ClientLastFrameRecvTick = GetTickCount64();

            gupt::shared::FrameAck ack = { hdr->frameNumber };
            auto ackMsg = gupt::shared::SerializeMessage(gupt::shared::MessageType::FrameAck, ack);
            if(g_UsingRelay) g_Relay.Send(ackMsg.data(), (uint32_t)ackMsg.size());
            else g_Client.SendRaw(ackMsg);
        }
        return;
    }

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
                if(tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                    std::vector<uint8_t> pix(tw * th * 4);
                    HRESULT hr = conv->CopyPixels(NULL, tw * 4, (UINT)pix.size(), pix.data());
                    if(SUCCEEDED(hr)) {
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
                        if(hWnd && g_LastRefreshTime > 0 && GetTickCount64() - g_LastRefreshTime < 4000){
                            SetWindowTextA(hWnd, g_UsingRelay ? "Gupt - Connected (Relay)" : "Gupt - Connected (LAN)");
                        }
                        // Signal immediate repaint from any thread
                        if(hWnd) PostMessage(hWnd, WM_GUPT_FRAME_ARRIVED, 0, 0);
                        g_ClientLastFrameRecvTick = GetTickCount64();

                        // Sender-paced flow control: send FrameAck back to host
                        gupt::shared::FrameAck ack = { hdr->frameNumber };
                        auto ackMsg = gupt::shared::SerializeMessage(gupt::shared::MessageType::FrameAck, ack);
                        if(g_UsingRelay) g_Relay.Send(ackMsg.data(), (uint32_t)ackMsg.size());
                        else g_Client.SendRaw(ackMsg);
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
                    if(isLocalIp && g_Client.Connect(actualIp, 8080)){
                        g_UsingRelay = false;
                        SetWindowTextA(hWnd, "Gupt - Waiting for host approval (LAN)");
                        auto req = MakeConnectRequest();
                        g_Client.SendRaw(req);
                        while(g_Client.IsConnected()){Sleep(250);}
                        g_IsConnected = false;
                        SetWindowTextA(hWnd, "Gupt - Reconnecting...");
                        Sleep(1000);
                        continue;
                    }

                    if(g_Relay.Open(g_SignalingHost, g_SignalingPort, rp, g_SignalingPort==443)){
                        g_UsingRelay = true; 
                        SetWindowTextA(hWnd, "Gupt - Waiting for host approval...");
                        auto req = MakeConnectRequest();
                        g_Relay.Send(req.data(), (uint32_t)req.size());
                        bool verified = false; 
                        bool denied = false;
                        for(int i=0; i<300 && !verified && !denied; ++i){ 
                            auto d = g_Relay.Recv(); 
                            if(d.empty()){ Sleep(100); continue; } 
                            auto msgs = gupt::shared::DeserializeMessages(d); 
                            for(auto& m : msgs) {
                                if(m.first == gupt::shared::MessageType::ConnectResponse) {
                                    if(m.second.size() >= sizeof(gupt::shared::ConnectResponse)) {
                                        auto cr = reinterpret_cast<const gupt::shared::ConnectResponse*>(m.second.data());
                                        if(cr->accepted) verified = true;
                                        else denied = true;
                                    }
                                }
                            }
                        }
                        if(denied){
                            SetWindowTextA(hWnd, "Gupt - Connection denied by host");
                            g_Relay.Close();
                            Sleep(2500);
                            continue;
                        }
                        if(verified){
                            g_IsConnected = true; 
                            SetWindowTextA(hWnd, "Gupt - Connected (Relay)");
                            auto rInit = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1);
                            g_Relay.Send(rInit.data(), (uint32_t)rInit.size());
                            static std::atomic<uint64_t> g_ClientLastRecvTick{0};
                            g_ClientLastRecvTick = GetTickCount64();

                            // Dedicated Autonomous Watchdog: unblocks hanging sockets and frame stalls
                            std::thread([hWnd](){
                                while(g_IsConnected) {
                                    Sleep(1000);
                                    if(g_IsConnected && g_UsingRelay) {
                                        uint64_t last = g_ClientLastRecvTick.load();
                                        if(last > 0 && (GetTickCount64() - last > 90000)) {
                                            SetWindowTextA(hWnd, "Gupt - Stalled network, reconnecting...");
                                            g_ClientLastRecvTick = 0;
                                            g_Relay.Abort();
                                        }
                                    }
                                    // Auto frame-stall recovery: 3s=keyframe, 6s=hard reconnect
                                    if(g_IsConnected) {
                                        uint64_t lastFrame = g_ClientLastFrameRecvTick.load();
                                        uint64_t stalledMs = (lastFrame > 0) ? (GetTickCount64() - lastFrame) : 0;
                                        if(stalledMs >= 6000) {
                                            g_ClientLastFrameRecvTick = GetTickCount64();
                                            g_Relay.Abort();
                                        } else if(stalledMs >= 3000) {
                                            g_ClientLastFrameRecvTick = GetTickCount64();
                                            auto refMsg = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1);
                                            if(g_UsingRelay) g_Relay.Send(refMsg.data(), (uint32_t)refMsg.size());
                                            else g_Client.SendRaw(refMsg);
                                        }
                                    }
                                }
                            }).detach();

                            while(g_IsConnected){
                                if(g_UsingRelay && !g_Relay.connected){
                                    SetWindowTextA(hWnd, "Gupt - Reconnecting session...");
                                    g_Relay.Open(g_SignalingHost, g_SignalingPort, rp, g_SignalingPort==443);
                                    if(!g_Relay.connected){ Sleep(300); continue; }
                                    auto r1 = MakeConnectRequest();
                                    g_Relay.Send(r1.data(), (uint32_t)r1.size());
                                    auto r2 = gupt::shared::SerializeMessage(gupt::shared::MessageType::RefreshRequest, (uint8_t)1);
                                    g_Relay.Send(r2.data(), (uint32_t)r2.size());
                                    g_ClientLastRecvTick = GetTickCount64();
                                    SetWindowTextA(hWnd, "Gupt - Connected (Relay)");
                                }

                                auto d = g_Relay.Recv(); 
                                if(d.empty()){ Sleep(1); continue; }
                                g_ClientLastRecvTick = GetTickCount64();

                                auto msgs = gupt::shared::DeserializeMessages(d);
                                for(size_t i = 0; i < msgs.size(); ++i){ 
                                    if(msgs[i].first == gupt::shared::MessageType::FrameData) { 
                                        ProcessFrame(msgs[i].second, hWnd); 
                                    } else if(msgs[i].first == gupt::shared::MessageType::AudioData) {
                                        auto& p = msgs[i].second;
                                        if(p.size() >= sizeof(gupt::shared::AudioDataHeader)) {
                                            auto aHdr = reinterpret_cast<const gupt::shared::AudioDataHeader*>(p.data());
                                            if(aHdr->dataSize > 0 && p.size() >= sizeof(gupt::shared::AudioDataHeader) + aHdr->dataSize) {
                                                const int16_t* pcm = reinterpret_cast<const int16_t*>(p.data() + sizeof(gupt::shared::AudioDataHeader));
                                                size_t sampleCount = aHdr->dataSize / sizeof(int16_t);
                                                g_AudioPlayer.PlaySamples(pcm, sampleCount, aHdr->sampleRate, aHdr->channels);
                                            }
                                        }
                                    } else if(msgs[i].first == gupt::shared::MessageType::ClipboardText){ 
                                        auto& p = msgs[i].second; 
                                        std::string s((char*)p.data(),p.size()); 
                                        g_clientLastFromHost=s; 
                                        int wn=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,0,0); 
                                        if(wn>0){ 
                                            HGLOBAL m=GlobalAlloc(GMEM_MOVEABLE,wn*sizeof(wchar_t)); 
                                            wchar_t* d=(wchar_t*)GlobalLock(m); 
                                            MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,d,wn); 
                                            GlobalUnlock(m); 
                                            if(OpenClipboard(NULL)){
                                                EmptyClipboard();
                                                SetClipboardData(CF_UNICODETEXT,m);
                                                CloseClipboard();
                                            } 
                                        } 
                                    } 
                                }
                            }
                            g_IsConnected = false;
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
            SetStretchBltMode(g_BackDC, HALFTONE); SetBrushOrgEx(g_BackDC, 0, 0, NULL); 
            BITMAPINFO bmi = {0}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = (int)g_DrawW; 
            bmi.bmiHeader.biHeight = -(int)g_DrawH; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(g_BackDC,g_DestX,g_DestY,g_DestW,g_DestH,0,0,g_DrawW,g_DrawH,g_DrawingFrame.data(),&bmi,DIB_RGB_COLORS,SRCCOPY); 
        } else { SetTextColor(g_BackDC, CLR_SUBTEXT); SetBkMode(g_BackDC, TRANSPARENT); RECT tr={0,0,g_WinW,g_WinH}; DrawTextA(g_BackDC, g_IsConnected ? "Synchronizing..." : "Connecting...", -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        if(g_SidebarX<g_WinW){
            RECT sr={g_SidebarX,0,g_WinW,g_WinH}; DrawRoundedRect(g_BackDC,sr,0,CLR_CARD,CLR_BORDER,1);
            g_CloseBtnRect={g_SidebarX+20,20,g_WinW-20,65}; DrawRoundedRect(g_BackDC,g_CloseBtnRect,8,CLR_ACCENT,CLR_BORDER,1); DrawTextA(g_BackDC,"Disconnect",-1,&g_CloseBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            g_FullscreenBtnRect={g_SidebarX+20,80,g_WinW-20,125}; DrawRoundedRect(g_BackDC,g_FullscreenBtnRect,8,CLR_BG,CLR_BORDER,1); DrawTextA(g_BackDC,g_IsFullscreen?"Exit Fullscreen":"Fullscreen",-1,&g_FullscreenBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            g_ClipboardBtnRect={g_SidebarX+20,140,g_WinW-20,185}; DrawRoundedRect(g_BackDC,g_ClipboardBtnRect,8,g_ClipboardSyncEnabled?RGB(50,180,50):CLR_BG,CLR_BORDER,1); DrawTextA(g_BackDC,g_ClipboardSyncEnabled?"Sync Clipboard: ON":"Sync Clipboard: OFF",-1,&g_ClipboardBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        g_TabRect={g_SidebarX-30,g_WinH/2-40,g_SidebarX,g_WinH/2+40}; DrawRoundedRect(g_BackDC,g_TabRect,8,CLR_CARD,CLR_BORDER,1); SetTextColor(g_BackDC,CLR_SUBTEXT); DrawTextA(g_BackDC,g_SidebarOpen?">":"<",-1,&g_TabRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        BitBlt(hdc,0,0,g_WinW,g_WinH,g_BackDC,0,0,SRCCOPY); EndPaint(hWnd,&ps); break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: { if(((wp=='S' && (GetKeyState(VK_CONTROL)&0x8000)) || wp==VK_SNAPSHOT) && (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)){ SaveOrCopy1080pScreenshot(hWnd); return 0; } if((wp==VK_F5 || (wp=='R' && (GetKeyState(VK_CONTROL)&0x8000))) && (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)){ TriggerSessionRefresh(hWnd); return 0; } if(wp==VK_F11 && (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)){ g_IsFullscreen=!g_IsFullscreen; if(g_IsFullscreen){ GetWindowRect(hWnd,&g_SavedRect); SetWindowLongPtrA(hWnd,GWL_STYLE,WS_POPUP|WS_VISIBLE); SetWindowPos(hWnd,HWND_TOPMOST,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_FRAMECHANGED); } else { SetWindowLongPtrA(hWnd,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE); SetWindowPos(hWnd,HWND_NOTOPMOST,g_SavedRect.left,g_SavedRect.top,g_SavedRect.right-g_SavedRect.left,g_SavedRect.bottom-g_SavedRect.top,SWP_FRAMECHANGED); } InvalidateRect(hWnd, NULL, TRUE); return 0; } if(g_IsConnected){ gupt::shared::KeyboardEvent ke={(uint16_t)wp, (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyboardEvent,ke)); } break; }
    case WM_MOUSEWHEEL: { if(g_IsConnected){ POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt); if(!g_SidebarOpen || pt.x < g_SidebarX){ gupt::shared::MouseEvent me={(float)(pt.x-g_DestX)/g_DestW, (float)(pt.y-g_DestY)/g_DestH, 255, false, (int16_t)GET_WHEEL_DELTA_WPARAM(wp)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::MouseEvent,me)); } } break; }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: {
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
            }
            return 0;
        }
        bool isDown = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK);
        uint8_t btnId = (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK) ? 1 : 0;
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
        if(now - lastMove < 5) break;
        lastMove = now;
        int x=(short)LOWORD(lp),y=(short)HIWORD(lp);
        if(g_IsConnected && (!g_SidebarOpen || x < g_SidebarX) && g_DestW > 0 && g_DestH > 0){
            bool dragging = (wp & (MK_LBUTTON | MK_RBUTTON)) != 0;
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
    case WM_CLIPBOARDUPDATE: { if(g_IsConnected && OpenClipboard(hWnd)){ HANDLE h=GetClipboardData(CF_UNICODETEXT); if(h){ wchar_t* w=(wchar_t*)GlobalLock(h); if(w){ int n=WideCharToMultiByte(CP_UTF8,0,w,-1,0,0,0,0); if(n>1){ std::string s(n-1,'\0'); WideCharToMultiByte(CP_UTF8,0,w,-1,&s[0],n,0,0); if(g_ClipboardSyncEnabled && s!=g_clientLastFromHost && s!=g_clientLastSent){ g_clientLastSent=s; PushEv(gupt::shared::SerializeClipboardText(s)); } } GlobalUnlock(h); } } CloseClipboard(); } break; }
    case WM_SIZE: { g_WinW=LOWORD(lp); g_WinH=HIWORD(lp); g_SidebarX=g_SidebarOpen?g_WinW-260:g_WinW; if(g_BackDC){ DeleteDC(g_BackDC); g_BackDC=NULL; } if(g_BackBmp){ DeleteObject(g_BackBmp); g_BackBmp=NULL; } InvalidateRect(hWnd, NULL, TRUE); break; }
    case WM_CLOSE: PostQuitMessage(0); break;
    default: return DefWindowProcA(hWnd,msg,wp,lp);
    } return 0;
}

LRESULT CALLBACK KbdHookProc(int code, WPARAM wp, LPARAM lp) { if(code == HC_ACTION && g_IsFullscreen && g_IsConnected) { KBDLLHOOKSTRUCT* hs = (KBDLLHOOKSTRUCT*)lp; bool sysKey = (hs->vkCode == VK_TAB && (hs->flags & LLKHF_ALTDOWN)) || (hs->vkCode == VK_LWIN) || (hs->vkCode == VK_RWIN) || (hs->vkCode == VK_ESCAPE && (GetKeyState(VK_CONTROL)&0x8000)); if(sysKey || (GetKeyState(VK_LWIN)&0x8000) || (GetKeyState(VK_RWIN)&0x8000)) { gupt::shared::KeyboardEvent ke = {(uint16_t)hs->vkCode, (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyboardEvent, ke)); return 1; } } return CallNextHookEx(g_hKbdHook, code, wp, lp); }

static void RunClientMode(HINSTANCE hInst) {
    std::thread(PowerKeepAwakeLoop).detach();

    WNDCLASSEXA wc={sizeof(WNDCLASSEXA)}; wc.lpfnWndProc=ClientWndProc; wc.hInstance=hInst; wc.hIcon=(HICON)LoadImageA(hInst,MAKEINTRESOURCEA(101),IMAGE_ICON,32,32,0); wc.lpszClassName="GuptC"; wc.hCursor=LoadCursor(NULL,IDC_ARROW); RegisterClassExA(&wc);
    HWND hWnd=CreateWindowExA(WS_EX_APPWINDOW,"GuptC","Gupt Remote Desktop",WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,1280,720,NULL,NULL,hInst,NULL); SetFocus(hWnd); g_SidebarOpen = false; g_SidebarX = 1280;
    AddClipboardFormatListener(hWnd); g_AudioPlayer.Initialize(48000, 2, 16);
    g_hKbdHook = SetWindowsHookEx(WH_KEYBOARD_LL, KbdHookProc, hInst, 0);
    g_Client.SetMessageCallback([hWnd](gupt::shared::MessageType t,const std::vector<uint8_t>& p){
        if(t==gupt::shared::MessageType::ConnectResponse && p.size() >= sizeof(gupt::shared::ConnectResponse)){
            auto res = reinterpret_cast<const gupt::shared::ConnectResponse*>(p.data());
            g_IsConnected = res->accepted;
            SetWindowTextA(hWnd, res->accepted ? "Gupt - Connected (LAN)" : "Gupt - Connection denied");
        } else if(t==gupt::shared::MessageType::Disconnect) {
            g_IsConnected = false;
            SetWindowTextA(hWnd, "Gupt - Disconnected");
        } else if(t==gupt::shared::MessageType::FrameData)ProcessFrame(p,hWnd); else if(t==gupt::shared::MessageType::AudioData){ if(p.size() >= sizeof(gupt::shared::AudioDataHeader)) { auto aHdr = reinterpret_cast<const gupt::shared::AudioDataHeader*>(p.data()); if(aHdr->dataSize > 0 && p.size() >= sizeof(gupt::shared::AudioDataHeader) + aHdr->dataSize) { const int16_t* pcm = reinterpret_cast<const int16_t*>(p.data() + sizeof(gupt::shared::AudioDataHeader)); size_t sampleCount = aHdr->dataSize / sizeof(int16_t); g_AudioPlayer.PlaySamples(pcm, sampleCount, aHdr->sampleRate, aHdr->channels); } } } else if(t==gupt::shared::MessageType::ClipboardText){ std::string s((char*)p.data(),p.size()); g_clientLastFromHost=s; int wn=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,0,0); if(wn>0){ HGLOBAL m=GlobalAlloc(GMEM_MOVEABLE,wn*sizeof(wchar_t)); wchar_t* d=(wchar_t*)GlobalLock(m); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,d,wn); GlobalUnlock(m); if(OpenClipboard(NULL)){EmptyClipboard();SetClipboardData(CF_UNICODETEXT,m);CloseClipboard();} } } });
    
    // Independent Sovereign Heartbeat Thread
    std::thread([](){ while(true){ if(g_IsConnected){ auto hb=gupt::shared::SerializeMessage(gupt::shared::MessageType::Heartbeat,(uint8_t)1); PushEv(std::move(hb)); } Sleep(2000); } }).detach();

    g_ClientHwnd = hWnd;
    SetTimer(hWnd, 4, 33, 0);
    SetTimer(hWnd, 2, 100, 0); MSG m; while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
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
    case WM_COMMAND: { if(LOWORD(wp)==101)ShowWindow(g_hEditIp,0); if(LOWORD(wp)==102)ShowWindow(g_hEditIp,1); if(LOWORD(wp)==1){ g_LaunchAsHost=(SendMessage(g_hRadioHost,BM_GETCHECK,0,0)==BST_CHECKED); if(!g_LaunchAsHost){char b[64];GetWindowTextA(g_hEditIp,b,64);g_HostIp=b;} g_Launched=1; DestroyWindow(hWnd); } if(LOWORD(wp)==2)DestroyWindow(hWnd); break; }
    case WM_DRAWITEM: { LPDRAWITEMSTRUCT d=(LPDRAWITEMSTRUCT)lp; bool l=(d->CtlID==1); POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt); bool h = PtInRect(&d->rcItem, pt); bool s = (d->itemState & ODS_SELECTED); COLORREF bg = l ? (s ? RGB(100, 70, 220) : (h ? RGB(130, 100, 255) : CLR_ACCENT)) : (s ? RGB(30, 30, 45) : (h ? RGB(45, 45, 65) : CLR_CARD)); DrawRoundedRect(d->hDC, d->rcItem, 8, bg, CLR_BORDER, 1); SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, CLR_TEXT); SelectObject(d->hDC, g_FontBtn); DrawTextA(d->hDC, l?"Launch Gupt":"Cancel", -1, &d->rcItem, DT_CENTER|DT_VCENTER|DT_SINGLELINE); return 1; }
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProcA(hWnd,msg,wp,lp);
    } return 0;
}


int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
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
    WNDCLASSEXA wc={sizeof(WNDCLASSEXA)}; wc.lpfnWndProc=LauncherWndProc; wc.hInstance=h; wc.hIcon=(HICON)LoadImageA(h,MAKEINTRESOURCEA(101),IMAGE_ICON,32,32,0); wc.lpszClassName="GuptL"; wc.hbrBackground=CreateSolidBrush(CLR_BG); wc.hCursor=LoadCursor(0,IDC_ARROW); RegisterClassExA(&wc);
    DWORD style = WS_SYSMENU|WS_CAPTION|WS_MINIMIZEBOX|WS_VISIBLE; RECT wr = {0, 0, 380, 380}; AdjustWindowRectEx(&wr, style, FALSE, 0);
    HWND hW=CreateWindowExA(WS_EX_APPWINDOW,"GuptL","Gupt Remote Desktop",style,CW_USEDEFAULT,CW_USEDEFAULT,wr.right-wr.left,wr.bottom-wr.top,0,0,h,0);
    ShowWindow(g_hEditIp,SW_HIDE); MSG m; while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(!g_Launched) return 0;
    if(g_LaunchAsHost) RunHostMode(); else RunClientMode(h);
    return 0;
}
