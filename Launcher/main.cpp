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
#include <wincodec.h>
#include <condition_variable>
#include "Shared/Protocol.h"
#include "Core/Network/TcpNetwork.h"
#include "Core/Input/InputInjector.h"
#include "Core/Capture/ScreenCapturer.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comctl32.lib")

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

static std::mutex g_FrameMutex, g_hostClipMtx, g_EvMtx, g_SendMtx, g_EncMtx;
static std::vector<uint8_t> g_PendingFrame, g_RawQueue;
static uint32_t g_RawW=0, g_RawH=0;
static std::vector<std::vector<uint8_t>> g_SendQueue;
static std::vector<std::vector<uint8_t>> g_EvQueue;
static std::condition_variable g_EvCv, g_SendCv, g_EncCv;

// UI Components
static HFONT g_FontTitle, g_FontBody, g_FontBtn;
static HWND g_hRadioHost, g_hRadioClient, g_hEditIp;
static HHOOK g_hKbdHook = NULL;

class WsRelay {
public:
    HINTERNET hSession=NULL, hConnect=NULL, hRequest=NULL, hWS=NULL;
    bool connected=false;
    std::mutex mtx;
    bool Open(std::string host, int port, std::string path, bool secure) {
        if(connected) Close();
        hSession = WinHttpOpen(L"Gupt/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if(!hSession) return false;
        std::wstring wHost(host.begin(), host.end());
        hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
        if(!hConnect) return false;
        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        std::wstring wPath(path.begin(), path.end());
        hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
        if (WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(hRequest, NULL)) {
            hWS = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
            WinHttpCloseHandle(hRequest); hRequest = NULL;
            if(hWS) { connected = true; return true; }
        }
        return false;
    }
    void Close() { if(hWS) WinHttpWebSocketClose(hWS, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0); if(hWS) WinHttpCloseHandle(hWS); if(hConnect) WinHttpCloseHandle(hConnect); if(hSession) WinHttpCloseHandle(hSession); hWS=NULL; hConnect=NULL; hSession=NULL; connected=false; }
    bool Send(const void* data, uint32_t len) { 
        if(!connected) return false;
        std::lock_guard<std::mutex> lk(mtx); 
        DWORD r = WinHttpWebSocketSend(hWS, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (void*)data, len);
        if(r != 0) connected = false;
        return r == 0;
    }
    std::vector<uint8_t> Recv() {
        if(!connected) return {};
        std::vector<uint8_t> buf, frag(64*1024);
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type; DWORD read;
        do { 
            DWORD r; r = WinHttpWebSocketReceive(hWS, frag.data(), (DWORD)frag.size(), &read, &type);
            if(r != 0) { connected=false; return {}; }
            buf.insert(buf.end(), frag.begin(), frag.begin()+read);
        } while(type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);
        return buf;
    }
};
static gupt::core::network::TcpClient g_Client;
static WsRelay g_Relay;

static void PushOutbound(std::vector<uint8_t> f, bool isUrgent) {
    {
        std::lock_guard<std::mutex> lk(g_SendMtx);
        if(isUrgent || f.size() < 400) g_SendQueue.push_back(std::move(f));
        else g_PendingFrame = std::move(f);
    }
    g_SendCv.notify_one();
}

static void HostEncoderLoop(gupt::core::capture::ScreenCapturer* capturer) {
    std::vector<uint8_t> lastPixels;
    int frameCounter = 0;
    while(true) {
        std::vector<uint8_t> raw; uint32_t w, h;
        {
            std::unique_lock<std::mutex> lk(g_EncMtx);
            g_EncCv.wait(lk, []{ return !g_RawQueue.empty(); });
            raw = std::move(g_RawQueue); g_RawQueue.clear();
            w = g_RawW; h = g_RawH;
        }
        if(!raw.empty()) {
            uint32_t minX=w, minY=h, maxX=0, maxY=0; bool changed=false;
            frameCounter++; if(frameCounter > 120) { lastPixels.clear(); frameCounter = 0; }
            if(!lastPixels.empty() && lastPixels.size()==raw.size()){
                for(uint32_t y=0; y<h; y+=4){
                    for(uint32_t x=0; x<w; x+=4){
                        size_t idx=(y*w+x)*4;
                        if(raw[idx]!=lastPixels[idx]||raw[idx+1]!=lastPixels[idx+1]||raw[idx+2]!=lastPixels[idx+2]){
                            changed=true; minX=(x<minX)?x:minX; minY=(y<minY)?y:minY; maxX=(x+4>maxX)?(x+4):maxX; maxY=(y+4>maxY)?(y+4):maxY;
                        }
                    }
                }
            } else { changed=true; minX=0; minY=0; maxX=w; maxY=h; }
            if(changed){
                minX = (minX / 16) * 16; minY = (minY / 16) * 16;
                maxX = ((maxX + 15) / 16) * 16; maxY = ((maxY + 15) / 16) * 16;
                if(maxX > w) maxX = ((w + 15) / 16) * 16; 
                if(maxY > h) maxY = ((h + 15) / 16) * 16;
                uint32_t tw = maxX - minX; uint32_t th = maxY - minY;
                if(tw >= 16 && th >= 16) {
                    std::vector<uint8_t> jpg;
                    if(capturer->CaptureRegionJpeg(raw, w, h, minX, minY, tw, th, jpg, 95)){
                        PushOutbound(gupt::shared::SerializeFrame({0, w, h, tw, th, minX, minY, 32, (tw < w || th < h), 0}, jpg), false);
                    }
                }
                lastPixels = std::move(raw);
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
static RECT g_SavedRect={0,0,800,600}, g_TabRect, g_CloseBtnRect, g_FullscreenBtnRect;
static std::string g_lastFromClient, g_clientLastFromHost, g_clientLastSent;

static void PushEv(std::vector<uint8_t> m) {
    { 
        std::lock_guard<std::mutex> lk(g_EvMtx); 
        if(m.size()==1 && m[0]==0xFF){ g_EvQueue.insert(g_EvQueue.begin(), std::move(m)); }
        else if(m.size()>1 && m[0]==(uint8_t)gupt::shared::MessageType::MouseEvent){
            bool replaced=false;
            for(auto& old : g_EvQueue) if(old.size()>1 && old[0]==(uint8_t)gupt::shared::MessageType::MouseEvent){ old=std::move(m); replaced=true; break; }
            if(!replaced) g_EvQueue.push_back(std::move(m));
        } else { g_EvQueue.push_back(std::move(m)); }
    }
    g_EvCv.notify_one();
}

static void EvSenderLoop() {
    while(true) {
        std::vector<uint8_t> m;
        {
            std::unique_lock<std::mutex> lk(g_EvMtx);
            g_EvCv.wait(lk, []{ return !g_EvQueue.empty(); });
            m = std::move(g_EvQueue.front()); g_EvQueue.erase(g_EvQueue.begin());
        }
        if(g_IsConnected) {
            if(g_UsingRelay) g_Relay.Send(m.data(), (uint32_t)m.size()); else g_Client.SendRaw(m);
        }
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

static void RunHostMode() {
    core::input::InputInjector injector;
    std::string json = HttpGet("/register");
    size_t pos = json.find("sessionId\":\"");
    if(pos != std::string::npos){
        g_CurrentSessionId = json.substr(pos + 12);
        size_t end = g_CurrentSessionId.find("\"");
        if(end != std::string::npos) g_CurrentSessionId = g_CurrentSessionId.substr(0, end);
    } else g_CurrentSessionId = "ERROR";

    std::string rPath = "/relay?role=host&session=" + g_CurrentSessionId;
    g_Relay.Open(g_SignalingHost, g_SignalingPort, rPath, g_SignalingPort==443);

    WNDCLASSEXA hwc={sizeof(WNDCLASSEXA)}; hwc.lpfnWndProc=[](HWND hw,UINT m,WPARAM w,LPARAM l)->LRESULT{
        if(m==WM_CLIPBOARDUPDATE){ std::string c=GetClipboardText(); if(!c.empty() && c!=g_lastFromClient){ {std::lock_guard<std::mutex> lk(g_hostClipMtx); if(c==g_lastFromClient)return 0;} auto s=shared::SerializeClipboardText(c); if(g_Relay.connected)g_Relay.Send(s.data(),s.size()); } }
        return DefWindowProcA(hw,m,w,l);
    }; hwc.lpszClassName="GuptH"; hwc.hInstance=GetModuleHandle(NULL); RegisterClassExA(&hwc);
    HWND hHidden=CreateWindowExA(0,"GuptH",0,0,0,0,0,0,HWND_MESSAGE,0,0,0);
    AddClipboardFormatListener(hHidden);

    std::thread([rPath, &injector]() {
        while (true) {
            if(!g_Relay.connected) {
                g_Relay.Open(g_SignalingHost, g_SignalingPort, rPath, g_SignalingPort==443);
                if(!g_Relay.connected) { Sleep(1000); continue; }
            }
            auto d = g_Relay.Recv(); if(d.empty()){ continue; }
            auto msgs = shared::DeserializeMessages(d);
            for(auto& msg : msgs) {
                auto t = msg.first; auto& p = msg.second;
                if(t == shared::MessageType::ConnectRequest && !g_SessionActive) {
                    if(MessageBoxA(NULL, "Remote client is requesting access via Internet. Allow?", "Gupt Security", MB_YESNO|MB_ICONQUESTION|MB_SETFOREGROUND)==IDYES) {
                        shared::ConnectResponse res={true, "OK"}; auto f = shared::SerializeMessage(shared::MessageType::ConnectResponse, res);
                        g_Relay.Send(f.data(), f.size()); g_SessionActive = true; g_UsingRelay = true; 
                    } else { shared::ConnectResponse res={false, "Denied"}; auto f = shared::SerializeMessage(shared::MessageType::ConnectResponse, res); g_Relay.Send(f.data(), f.size()); }
                }
                else if(t == shared::MessageType::MouseEvent) { injector.IngestMouseEvent(*reinterpret_cast<const shared::MouseEvent*>(p.data())); }
                else if(t == shared::MessageType::KeyboardEvent) { injector.IngestKeyboardEvent(*reinterpret_cast<const shared::KeyboardEvent*>(p.data())); }
                else if(g_ClipboardSyncEnabled && t == shared::MessageType::ClipboardText){ 
                    std::string s((char*)p.data(),p.size()); {std::lock_guard<std::mutex> lk(g_hostClipMtx); g_lastFromClient=s;} 
                    int wn=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,0,0); 
                    if(wn>0){ HGLOBAL m=GlobalAlloc(GMEM_MOVEABLE,wn*sizeof(wchar_t)); wchar_t* d=(wchar_t*)GlobalLock(m); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,d,wn); GlobalUnlock(m); if(OpenClipboard(NULL)){EmptyClipboard();SetClipboardData(CF_UNICODETEXT,m);CloseClipboard();} } 
                }
            }
        }
    }).detach();

    MessageBoxA(NULL, ("Gupt Host is active!\n\nSession ID: " + g_CurrentSessionId).c_str(), "Gupt", MB_OK|MB_ICONINFORMATION);

    core::network::TcpServer server(8080); 
    injector.Initialize();
    core::capture::ScreenCapturer capturer; capturer.Initialize();

    server.SetMessageCallback([&](shared::MessageType t, const std::vector<uint8_t>& p) {
        if(t==shared::MessageType::ConnectRequest) { g_SessionActive=true; shared::ConnectResponse res={true,"OK"}; server.SendRaw(shared::SerializeMessage(shared::MessageType::ConnectResponse,res)); }
        else if(t==shared::MessageType::MouseEvent) { injector.IngestMouseEvent(*reinterpret_cast<const shared::MouseEvent*>(p.data())); }
        else if(t==shared::MessageType::KeyboardEvent) { injector.IngestKeyboardEvent(*reinterpret_cast<const shared::KeyboardEvent*>(p.data())); }
        else if(g_ClipboardSyncEnabled && t==shared::MessageType::ClipboardText){ std::string s((char*)p.data(),p.size()); {std::lock_guard<std::mutex> lk(g_hostClipMtx); g_lastFromClient=s;} int wn=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,0,0); if(wn>0){ HGLOBAL m=GlobalAlloc(GMEM_MOVEABLE,wn*sizeof(wchar_t)); wchar_t* d=(wchar_t*)GlobalLock(m); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,d,wn); GlobalUnlock(m); if(OpenClipboard(NULL)){EmptyClipboard();SetClipboardData(CF_UNICODETEXT,m);CloseClipboard();} } }
    });
    
    std::thread(HostEncoderLoop, &capturer).detach();
    std::thread(HostDeliveryLoop, &server).detach();
    std::thread([](){ while(true){ if(g_SessionActive) PushOutbound({0},true); Sleep(2000); } }).detach();

    server.Start();

    while(true) {
        if(g_SessionActive) {
            uint64_t loopStart = GetTickCount64();
            std::vector<uint8_t> raw; uint32_t w, h;
            if(capturer.CaptureNextFrame(raw, w, h)) {
                { std::lock_guard<std::mutex> lk(g_EncMtx); g_RawQueue = std::move(raw); g_RawW = w; g_RawH = h; }
                g_EncCv.notify_one();
            }
            uint64_t el = GetTickCount64() - loopStart;
            if(16 > el) Sleep((DWORD)(16 - el)); 
        } else { Sleep(100); }
        MSG msg; while(PeekMessage(&msg,hHidden,0,0,PM_REMOVE)){ TranslateMessage(&msg); DispatchMessage(&msg); }
    }
}

static IWICImagingFactory* g_WICFactory=NULL;
void ProcessFrame(const std::vector<uint8_t>& p, HWND hWnd) {
    if(p.size()<sizeof(gupt::shared::FrameDataHeader)) return;
    std::unique_lock<std::mutex> lk(g_FrameMutex, std::defer_lock);
    if(!lk.try_lock()) return; 

    gupt::shared::FrameDataHeader* hdr = (gupt::shared::FrameDataHeader*)p.data();
    IStream* stream = SHCreateMemStream(p.data()+sizeof(gupt::shared::FrameDataHeader), (UINT)(p.size()-sizeof(gupt::shared::FrameDataHeader)));
    if(stream) {
        if(!g_WICFactory) CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_WICFactory));
        IWICBitmapDecoder* dec=NULL; g_WICFactory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand, &dec);
        if(dec){
            IWICBitmapFrameDecode* frm=NULL; dec->GetFrame(0, &frm);
            if(frm){
                IWICFormatConverter* conv=NULL; g_WICFactory->CreateFormatConverter(&conv);
                if(conv){
                    conv->Initialize(frm, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0.f, WICBitmapPaletteTypeCustom);
                    UINT tw, th; conv->GetSize(&tw, &th); 
                    if(tw > 4096 || th > 4096 || tw == 0 || th == 0) { conv->Release(); frm->Release(); dec->Release(); stream->Release(); return; } 
                    static std::vector<uint8_t> pix; if(pix.size() < tw*th*4) pix.resize(tw*th*4);
                    HRESULT hr = conv->CopyPixels(0, tw*4, (UINT)pix.size(), pix.data());
                    if(SUCCEEDED(hr)) {
                        if(g_LatestFrame.empty() || g_FrameW != hdr->totalWidth || g_FrameH != hdr->totalHeight) { g_FrameW = hdr->totalWidth; g_FrameH = hdr->totalHeight; g_LatestFrame.assign(g_FrameW * g_FrameH * 4, 0); }
                        if((hdr->targetX + tw <= g_FrameW) && (hdr->targetY + th <= g_FrameH)) { for(uint32_t row=0; row<th; ++row) { memcpy(&g_LatestFrame[((hdr->targetY + row)*g_FrameW + hdr->targetX)*4], &pix[row*tw*4], tw*4); } }
                    }
                    conv->Release();
                }
                frm->Release();
            }
            dec->Release();
        }
        stream->Release();
    }
}

LRESULT CALLBACK ClientWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_TIMER: {
        if(wp==4) { InvalidateRect(hWnd, 0, FALSE); } 
        if(wp==1){ int target=g_SidebarOpen?g_WinW-260:g_WinW; if(g_SidebarX!=target){ g_SidebarX+=(target>g_SidebarX?60:-60); if(abs(g_SidebarX-target)<60)g_SidebarX=target; InvalidateRect(hWnd,0,FALSE); } else KillTimer(hWnd,1); }
        if(wp==3){ if(g_IsConnected){ std::vector<uint8_t> hb; hb.push_back(0); PushEv(hb); } }
        if(wp==2){ KillTimer(hWnd,2);
            std::thread([hWnd](){
                CoInitializeEx(0, COINIT_MULTITHREADED);
                SetWindowTextA(hWnd, "Gupt - Connecting...");
                std::string session = g_HostIp;
                std::string rp="/relay?role=client&session=" + session;
                std::string actualIp = DecodeIP(session);
                if(actualIp.empty() || actualIp.find('.') == std::string::npos){
                    std::string j = HttpGet("/join?id=" + session);
                    size_t pos = j.find("hostIp\":\"");
                    if(pos != std::string::npos){ actualIp = j.substr(pos + 9); size_t end = actualIp.find("\""); if(end != std::string::npos) actualIp = actualIp.substr(0, end); }
                }
                if(!actualIp.empty() && g_Client.Connect(actualIp, 8080)){ g_IsConnected = true; SetWindowTextA(hWnd, "Gupt - Connected (LAN)"); while(g_IsConnected){Sleep(100);} } 
                else {
                    if(g_Relay.Open(g_SignalingHost, g_SignalingPort, rp, g_SignalingPort==443)){
                        g_UsingRelay = true; 
                        auto req = gupt::shared::SerializeMessage(gupt::shared::MessageType::ConnectRequest, std::vector<uint8_t>{});
                        g_Relay.Send(req.data(), (uint32_t)req.size());
                        bool verified = false; for(int i=0; i<100 && !verified; ++i){ auto d = g_Relay.Recv(); if(d.empty()){ Sleep(100); continue; } auto msgs = gupt::shared::DeserializeMessages(d); for(auto& m : msgs) if(m.first == gupt::shared::MessageType::ConnectResponse) verified = true; }
                        if(verified){
                            g_IsConnected = true; SetWindowTextA(hWnd, "Gupt - Connected (Relay)");
                            while(g_IsConnected){
                                if(g_UsingRelay && !g_Relay.connected){ g_Relay.Open(g_SignalingHost, g_SignalingPort, rp, g_SignalingPort==443); if(!g_Relay.connected){ Sleep(2000); continue; } }
                                auto d = g_Relay.Recv(); if(d.empty()){ Sleep(5); continue; }
                                auto msgs = gupt::shared::DeserializeMessages(d);
                                int lastVid = -1; for(int i=(int)msgs.size()-1; i>=0; --i) if(msgs[i].first == gupt::shared::MessageType::FrameData) { lastVid = i; break; }
                                for(int i=0; i<(int)msgs.size(); ++i){ if(i == lastVid) { ProcessFrame(msgs[i].second, hWnd); PushEv({0xFF}); } else if(msgs[i].first == gupt::shared::MessageType::ClipboardText){ auto& p = msgs[i].second; std::string s((char*)p.data(),p.size()); g_clientLastFromHost=s; int wn=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,0,0); if(wn>0){ HGLOBAL m=GlobalAlloc(GMEM_MOVEABLE,wn*sizeof(wchar_t)); wchar_t* d=(wchar_t*)GlobalLock(m); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,d,wn); GlobalUnlock(m); if(OpenClipboard(NULL)){EmptyClipboard();SetClipboardData(CF_UNICODETEXT,m);CloseClipboard();} } } }
                            }
                        } else { MessageBoxA(hWnd, "Handshake failed.", "Gupt", MB_OK); }
                    } else { MessageBoxA(hWnd, "Connection failed.", "Gupt", MB_OK); }
                }
            }).detach();
        } break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps); 
        if(!g_BackDC || !g_BackBmp){ g_BackDC=CreateCompatibleDC(hdc); g_BackBmp=CreateCompatibleBitmap(hdc,g_WinW,g_WinH); SelectObject(g_BackDC,g_BackBmp); }
        HBRUSH hb=CreateSolidBrush(CLR_BG); RECT wr={0,0,g_WinW,g_WinH}; FillRect(g_BackDC,&wr,hb); DeleteObject(hb);
        { std::unique_lock<std::mutex> lk(g_FrameMutex, std::defer_lock); if(lk.try_lock()){ if(!g_LatestFrame.empty()){ if(g_DrawingFrame.size() != g_LatestFrame.size()) g_DrawingFrame.resize(g_LatestFrame.size()); memcpy(g_DrawingFrame.data(), g_LatestFrame.data(), g_LatestFrame.size()); g_DrawW = g_FrameW; g_DrawH = g_FrameH; } } }
        if(!g_DrawingFrame.empty()){
            float ha=(float)g_DrawW/g_DrawH, ca=(float)g_WinW/g_WinH; if(ca>ha){ g_DestH=g_WinH; g_DestW=(int)(g_WinH*ha); g_DestX=(g_WinW-g_DestW)/2; g_DestY=0; } else { g_DestW=g_WinW; g_DestH=(int)(g_WinW/ha); g_DestX=0; g_DestY=(g_WinH-g_DestH)/2; }
            SetStretchBltMode(g_BackDC, HALFTONE); SetBrushOrgEx(g_BackDC, 0, 0, NULL); 
            BITMAPINFO bmi = {0}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = (int)g_DrawW; 
            bmi.bmiHeader.biHeight = -(int)g_DrawH; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(g_BackDC,g_DestX,g_DestY,g_DestW,g_DestH,0,0,g_DrawW,g_DrawH,g_DrawingFrame.data(),&bmi,DIB_RGB_COLORS,SRCCOPY); 
        } else { SetTextColor(g_BackDC, CLR_SUBTEXT); SetBkMode(g_BackDC, TRANSPARENT); RECT tr={0,0,g_WinW,g_WinH}; DrawTextA(g_BackDC, g_IsConnected ? "Synchronizing..." : "Connecting...", -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        if(g_SidebarX<g_WinW){ RECT sr={g_SidebarX,0,g_WinW,g_WinH}; DrawRoundedRect(g_BackDC,sr,0,CLR_CARD,CLR_BORDER,1); g_CloseBtnRect={g_SidebarX+20,80,g_WinW-20,130}; DrawRoundedRect(g_BackDC,g_CloseBtnRect,8,CLR_ACCENT,CLR_BORDER,1); DrawTextA(g_BackDC,"Disconnect",-1,&g_CloseBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE); g_FullscreenBtnRect={g_SidebarX+20,150,g_WinW-20,200}; DrawRoundedRect(g_BackDC,g_FullscreenBtnRect,8,CLR_BG,CLR_BORDER,1); DrawTextA(g_BackDC,g_IsFullscreen?"Exit Fullscreen":"Fullscreen",-1,&g_FullscreenBtnRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE); RECT cr={g_SidebarX+20,220,g_WinW-20,270}; DrawRoundedRect(g_BackDC,cr,8,g_ClipboardSyncEnabled?RGB(50,180,50):CLR_BG,CLR_BORDER,1); DrawTextA(g_BackDC,g_ClipboardSyncEnabled?"Sync Clipboard: ON":"Sync Clipboard: OFF",-1,&cr,DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        g_TabRect={g_SidebarX-30,g_WinH/2-40,g_SidebarX,g_WinH/2+40}; DrawRoundedRect(g_BackDC,g_TabRect,8,CLR_CARD,CLR_BORDER,1); SetTextColor(g_BackDC,CLR_SUBTEXT); DrawTextA(g_BackDC,g_SidebarOpen?">":"<",-1,&g_TabRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        BitBlt(hdc,0,0,g_WinW,g_WinH,g_BackDC,0,0,SRCCOPY); EndPaint(hWnd,&ps); break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: { if(wp==VK_F11 && (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)){ g_IsFullscreen=!g_IsFullscreen; if(g_IsFullscreen){ GetWindowRect(hWnd,&g_SavedRect); SetWindowLongPtrA(hWnd,GWL_STYLE,WS_POPUP|WS_VISIBLE); SetWindowPos(hWnd,HWND_TOPMOST,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),SWP_FRAMECHANGED); } else { SetWindowLongPtrA(hWnd,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE); SetWindowPos(hWnd,HWND_NOTOPMOST,g_SavedRect.left,g_SavedRect.top,g_SavedRect.right-g_SavedRect.left,g_SavedRect.bottom-g_SavedRect.top,SWP_FRAMECHANGED); } InvalidateRect(hWnd, NULL, TRUE); return 0; } if(g_IsConnected && !g_SidebarOpen){ gupt::shared::KeyboardEvent ke={(uint16_t)wp, (msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyboardEvent,ke)); } break; }
    case WM_MOUSEWHEEL: { if(g_IsConnected && !g_SidebarOpen){ POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt); gupt::shared::MouseEvent me={(float)(pt.x-g_DestX)/g_DestW, (float)(pt.y-g_DestY)/g_DestH, 255, false, (int16_t)GET_WHEEL_DELTA_WPARAM(wp)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::MouseEvent,me)); } break; }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: { POINT pt={(short)LOWORD(lp),(short)HIWORD(lp)}; if(PtInRect(&g_TabRect,pt)){ if(msg==WM_LBUTTONDOWN){g_SidebarOpen=!g_SidebarOpen;SetTimer(hWnd,1,10,0);} return 0; } if(g_SidebarOpen){ if(msg==WM_LBUTTONDOWN){ if(PtInRect(&g_CloseBtnRect,pt))PostQuitMessage(0); if(PtInRect(&g_FullscreenBtnRect,pt))SendMessage(hWnd,WM_KEYDOWN,VK_F11,0); RECT cr={g_SidebarX+20, 220, g_WinW-20, 270}; if(PtInRect(&cr,pt)) { g_ClipboardSyncEnabled = !g_ClipboardSyncEnabled; InvalidateRect(hWnd,0,FALSE); } } return 0; } if(g_IsConnected && g_DestW > 0 && g_DestH > 0 && pt.x >= g_DestX && pt.x < g_DestX+g_DestW && pt.y >= g_DestY && pt.y < g_DestY+g_DestH){ gupt::shared::MouseEvent me={(float)(pt.x-g_DestX)/g_DestW, (float)(pt.y-g_DestY)/g_DestH, (uint8_t)((msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP)?0:1), (bool)(msg==WM_LBUTTONDOWN||msg==WM_RBUTTONDOWN), 0}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::MouseEvent,me)); } break; }
    case WM_MOUSEMOVE: { static uint64_t lastMove = 0; uint64_t now = GetTickCount64(); if(now - lastMove < 4) break; lastMove = now; int x=(short)LOWORD(lp),y=(short)HIWORD(lp); if(g_IsConnected && !g_SidebarOpen && g_DestW > 0 && g_DestH > 0 && x>=g_DestX && x<g_DestX+g_DestW && y>=g_DestY && y<g_DestY+g_DestH){ gupt::shared::MouseEvent me={(float)(x-g_DestX)/g_DestW, (float)(y-g_DestY)/g_DestH, 255, (bool)(wp&MK_LBUTTON), 0}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::MouseEvent,me)); } break; }
    case WM_CLIPBOARDUPDATE: { if(g_IsConnected && OpenClipboard(hWnd)){ HANDLE h=GetClipboardData(CF_UNICODETEXT); if(h){ wchar_t* w=(wchar_t*)GlobalLock(h); if(w){ int n=WideCharToMultiByte(CP_UTF8,0,w,-1,0,0,0,0); if(n>1){ std::string s(n-1,'\0'); WideCharToMultiByte(CP_UTF8,0,w,-1,&s[0],n,0,0); if(g_ClipboardSyncEnabled && s!=g_clientLastFromHost && s!=g_clientLastSent){ g_clientLastSent=s; PushEv(gupt::shared::SerializeClipboardText(s)); } } GlobalUnlock(h); } } CloseClipboard(); } break; }
    case WM_SIZE: { g_WinW=LOWORD(lp); g_WinH=HIWORD(lp); g_SidebarX=g_SidebarOpen?g_WinW-260:g_WinW; if(g_BackDC){ DeleteDC(g_BackDC); g_BackDC=NULL; } if(g_BackBmp){ DeleteObject(g_BackBmp); g_BackBmp=NULL; } InvalidateRect(hWnd, NULL, TRUE); break; }
    case WM_CLOSE: PostQuitMessage(0); break;
    default: return DefWindowProcA(hWnd,msg,wp,lp);
    } return 0;
}

LRESULT CALLBACK KbdHookProc(int code, WPARAM wp, LPARAM lp) { if(code == HC_ACTION && g_IsFullscreen && g_IsConnected) { KBDLLHOOKSTRUCT* hs = (KBDLLHOOKSTRUCT*)lp; bool sysKey = (hs->vkCode == VK_TAB && (hs->flags & LLKHF_ALTDOWN)) || (hs->vkCode == VK_LWIN) || (hs->vkCode == VK_RWIN) || (hs->vkCode == VK_ESCAPE && (GetKeyState(VK_CONTROL)&0x8000)); if(sysKey || (GetKeyState(VK_LWIN)&0x8000) || (GetKeyState(VK_RWIN)&0x8000)) { gupt::shared::KeyboardEvent ke = {(uint16_t)hs->vkCode, (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)}; PushEv(gupt::shared::SerializeMessage(gupt::shared::MessageType::KeyboardEvent, ke)); return 1; } } return CallNextHookEx(g_hKbdHook, code, wp, lp); }

static void RunClientMode(HINSTANCE hInst) {
    WNDCLASSEXA wc={sizeof(WNDCLASSEXA)}; wc.lpfnWndProc=ClientWndProc; wc.hInstance=hInst; wc.hIcon=(HICON)LoadImageA(hInst,MAKEINTRESOURCEA(101),IMAGE_ICON,32,32,0); wc.lpszClassName="GuptC"; wc.hCursor=LoadCursor(NULL,IDC_ARROW); RegisterClassExA(&wc);
    HWND hWnd=CreateWindowExA(WS_EX_APPWINDOW,"GuptC","Gupt Remote Desktop",WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,1280,720,NULL,NULL,hInst,NULL);
    AddClipboardFormatListener(hWnd); SetTimer(hWnd,3,8000,0); SetTimer(hWnd,4,16,0);
    g_hKbdHook = SetWindowsHookEx(WH_KEYBOARD_LL, KbdHookProc, hInst, 0);
    g_Client.SetMessageCallback([hWnd](gupt::shared::MessageType t,const std::vector<uint8_t>& p){ if(t==gupt::shared::MessageType::FrameData)ProcessFrame(p,hWnd); else if(t==gupt::shared::MessageType::ClipboardText){ std::string s((char*)p.data(),p.size()); g_clientLastFromHost=s; int wn=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,0,0); if(wn>0){ HGLOBAL m=GlobalAlloc(GMEM_MOVEABLE,wn*sizeof(wchar_t)); wchar_t* d=(wchar_t*)GlobalLock(m); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,d,wn); GlobalUnlock(m); if(OpenClipboard(NULL)){EmptyClipboard();SetClipboardData(CF_UNICODETEXT,m);CloseClipboard();} } } });
    SetTimer(hWnd,2,100,0); MSG m; while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
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
    SetProcessDPIAware(); CoInitializeEx(0, COINIT_MULTITHREADED);
    std::thread(EvSenderLoop).detach();
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_WICFactory));
    WNDCLASSEXA wc={sizeof(WNDCLASSEXA)}; wc.lpfnWndProc=LauncherWndProc; wc.hInstance=h; wc.hIcon=(HICON)LoadImageA(h,MAKEINTRESOURCEA(101),IMAGE_ICON,32,32,0); wc.lpszClassName="GuptL"; wc.hbrBackground=CreateSolidBrush(CLR_BG); wc.hCursor=LoadCursor(0,IDC_ARROW); RegisterClassExA(&wc);
    DWORD style = WS_SYSMENU|WS_CAPTION|WS_MINIMIZEBOX|WS_VISIBLE; RECT wr = {0, 0, 380, 380}; AdjustWindowRectEx(&wr, style, FALSE, 0);
    HWND hW=CreateWindowExA(WS_EX_APPWINDOW,"GuptL","Gupt Remote Desktop",style,CW_USEDEFAULT,CW_USEDEFAULT,wr.right-wr.left,wr.bottom-wr.top,0,0,h,0);
    ShowWindow(g_hEditIp,SW_HIDE); MSG m; while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(!g_Launched) return 0;
    if(g_LaunchAsHost) RunHostMode(); else RunClientMode(h);
    return 0;
}
