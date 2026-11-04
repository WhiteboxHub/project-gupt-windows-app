#include "ScreenCapturer.h"
#include <iostream>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace gupt {
namespace core {
namespace capture {

static void UnionDirtyRect(RECT& bounds, bool& hasBounds, const RECT& r, uint32_t width, uint32_t height) {
    RECT clipped = r;
    if (clipped.left < 0) clipped.left = 0;
    if (clipped.top < 0) clipped.top = 0;
    if (clipped.right > static_cast<LONG>(width)) clipped.right = static_cast<LONG>(width);
    if (clipped.bottom > static_cast<LONG>(height)) clipped.bottom = static_cast<LONG>(height);
    if (clipped.left >= clipped.right || clipped.top >= clipped.bottom) return;

    if (!hasBounds) {
        bounds = clipped;
        hasBounds = true;
        return;
    }

    if (clipped.left < bounds.left) bounds.left = clipped.left;
    if (clipped.top < bounds.top) bounds.top = clipped.top;
    if (clipped.right > bounds.right) bounds.right = clipped.right;
    if (clipped.bottom > bounds.bottom) bounds.bottom = clipped.bottom;
}

static void DrawCursorOntoFrame(std::vector<uint8_t>& outPixels, uint32_t width, uint32_t height, RECT& dirtyBounds, bool& hasDirtyBounds) {
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    ICONINFO ii = { 0 };
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    int posX = ci.ptScreenPos.x - ii.xHotspot;
    int posY = ci.ptScreenPos.y - ii.yHotspot;

    int curW = 32, curH = 32;
    HDC hdcScreen = GetDC(NULL);
    if (hdcScreen) {
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        if (hdcMem) {
            BITMAPINFO bmi = {0};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = curW;
            bmi.bmiHeader.biHeight = -curH; // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* pBits = NULL;
            HBITMAP hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
            if (hbm && pBits) {
                HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);
                memset(pBits, 0, curW * curH * 4);

                DrawIconEx(hdcMem, 0, 0, ci.hCursor, curW, curH, 0, NULL, DI_NORMAL);

                const uint32_t* srcBits = reinterpret_cast<const uint32_t*>(pBits);
                for (int y = 0; y < curH; ++y) {
                    int dstY = posY + y;
                    if (dstY < 0 || dstY >= static_cast<int>(height)) continue;
                    for (int x = 0; x < curW; ++x) {
                        int dstX = posX + x;
                        if (dstX < 0 || dstX >= static_cast<int>(width)) continue;

                        uint32_t pixel = srcBits[y * curW + x];
                        uint8_t a = (pixel >> 24) & 0xFF;
                        uint8_t r = (pixel >> 16) & 0xFF;
                        uint8_t g = (pixel >> 8) & 0xFF;
                        uint8_t b = pixel & 0xFF;

                        if (a > 0 || r > 0 || g > 0 || b > 0) {
                            size_t dstIdx = (static_cast<size_t>(dstY) * width + dstX) * 4;
                            if (a == 255 || a == 0) {
                                outPixels[dstIdx] = b;
                                outPixels[dstIdx + 1] = g;
                                outPixels[dstIdx + 2] = r;
                                outPixels[dstIdx + 3] = 0xFF;
                            } else {
                                float alpha = a / 255.0f;
                                outPixels[dstIdx] = static_cast<uint8_t>(b * alpha + outPixels[dstIdx] * (1.0f - alpha));
                                outPixels[dstIdx + 1] = static_cast<uint8_t>(g * alpha + outPixels[dstIdx + 1] * (1.0f - alpha));
                                outPixels[dstIdx + 2] = static_cast<uint8_t>(r * alpha + outPixels[dstIdx + 2] * (1.0f - alpha));
                                outPixels[dstIdx + 3] = 0xFF;
                            }
                        }
                    }
                }

                RECT curRect = { posX, posY, posX + curW, posY + curH };
                UnionDirtyRect(dirtyBounds, hasDirtyBounds, curRect, width, height);

                SelectObject(hdcMem, hOld);
                DeleteObject(hbm);
            }
            DeleteDC(hdcMem);
        }
        ReleaseDC(NULL, hdcScreen);
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

ScreenCapturer::ScreenCapturer() {}
ScreenCapturer::~ScreenCapturer() {
    Reset();
    CleanupGdi();
}

void ScreenCapturer::CleanupGdi() {
    if (m_hGdiMemDC && m_hGdiOldBitmap) {
        SelectObject(m_hGdiMemDC, m_hGdiOldBitmap);
        m_hGdiOldBitmap = NULL;
    }
    if (m_hGdiBitmap) {
        DeleteObject(m_hGdiBitmap);
        m_hGdiBitmap = NULL;
    }
    if (m_hGdiMemDC) {
        DeleteDC(m_hGdiMemDC);
        m_hGdiMemDC = NULL;
    }
    if (m_hGdiScreenDC) {
        ReleaseDC(NULL, m_hGdiScreenDC);
        m_hGdiScreenDC = NULL;
    }
    m_pGdiBits = NULL;
    m_GdiWidth = 0;
    m_GdiHeight = 0;
}

void ScreenCapturer::Reset() {
    m_DeskDupl.Reset();
    m_StagingTexture.Reset();
    m_StagingWidth = 0;
    m_StagingHeight = 0;
    m_DeviceContext.Reset();
    m_Device.Reset();
    m_Width = 0;
    m_Height = 0;
    m_LastDirtyBounds = { 0, 0, 0, 0 };
    m_HasLastDirtyBounds = false;
    m_CachedFrame.clear();
    m_LastCursorPos = { -1, -1 };
    m_LastForceCaptureTick = 0;
    CleanupGdi();
}

bool ScreenCapturer::Initialize() {
    Reset();

    // Attach to current active input desktop (handles Task Manager, UAC, desktop switch)
    HDESK hInput = OpenInputDesktop(0, FALSE, DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL | DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD | DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS);
    if (!hInput) hInput = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
    if (hInput) {
        SetThreadDesktop(hInput);
        CloseDesktop(hInput);
    }

    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &m_Device, &fl, &m_DeviceContext))) return false;
    if (!m_Device) return false;
    ComPtr<IDXGIDevice> dxgiDevice; if (FAILED(m_Device.As(&dxgiDevice))) return false;
    ComPtr<IDXGIAdapter> adapter; if (FAILED(dxgiDevice->GetParent(IID_PPV_ARGS(&adapter)))) return false;
    ComPtr<IDXGIOutput> output; if (FAILED(adapter->EnumOutputs(0, &output))) return false;
    ComPtr<IDXGIOutput1> output1; if (FAILED(output.As(&output1))) return false;
    if (FAILED(output1->DuplicateOutput(m_Device.Get(), &m_DeskDupl))) return false;
    DXGI_OUTDUPL_DESC desc; m_DeskDupl->GetDesc(&desc);
    m_Width = desc.ModeDesc.Width; m_Height = desc.ModeDesc.Height;
    return true;
}

bool ScreenCapturer::CaptureNextFrameGdi(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return false;

    if (!m_hGdiMemDC || !m_hGdiBitmap || m_GdiWidth != w || m_GdiHeight != h) {
        CleanupGdi();
        m_hGdiScreenDC = GetDC(NULL);
        if (!m_hGdiScreenDC) return false;
        m_hGdiMemDC = CreateCompatibleDC(m_hGdiScreenDC);
        if (!m_hGdiMemDC) {
            ReleaseDC(NULL, m_hGdiScreenDC);
            m_hGdiScreenDC = NULL;
            return false;
        }

        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        m_hGdiBitmap = CreateDIBSection(m_hGdiMemDC, &bmi, DIB_RGB_COLORS, &m_pGdiBits, NULL, 0);
        if (!m_hGdiBitmap || !m_pGdiBits) {
            CleanupGdi();
            return false;
        }
        m_hGdiOldBitmap = (HBITMAP)SelectObject(m_hGdiMemDC, m_hGdiBitmap);
        m_GdiWidth = w;
        m_GdiHeight = h;
    }

    if (!BitBlt(m_hGdiMemDC, 0, 0, w, h, m_hGdiScreenDC, 0, 0, SRCCOPY)) {
        CleanupGdi();
        return false;
    }

    outWidth = static_cast<uint32_t>(w);
    outHeight = static_cast<uint32_t>(h);
    size_t byteCount = static_cast<size_t>(outWidth) * outHeight * 4;
    outPixels.resize(byteCount);
    memcpy(outPixels.data(), m_pGdiBits, byteCount);

    for (size_t i = 3; i < byteCount; i += 4) {
        outPixels[i] = 0xFF;
    }

    return true;
}

bool ScreenCapturer::CaptureNextFrame(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    uint64_t now = GetTickCount64();

    if (!m_DeskDupl && !Initialize()) {
        // Fallback to GDI only if hardware DXGI is completely unavailable on this machine
        std::vector<uint8_t> gdiFrame;
        uint32_t gw = 0, gh = 0;
        if (CaptureNextFrameGdi(gdiFrame, gw, gh)) {
            POINT curPt = {0, 0};
            GetCursorPos(&curPt);
            bool cursorMoved = (curPt.x != m_LastCursorPos.x || curPt.y != m_LastCursorPos.y);
            bool changed = true;
            if (!m_CachedFrame.empty() && m_CachedFrame.size() == gdiFrame.size()) {
                changed = (memcmp(m_CachedFrame.data(), gdiFrame.data(), gdiFrame.size()) != 0);
            }
            if (changed || cursorMoved || (now - m_LastForceCaptureTick > 1000)) {
                m_LastCursorPos = curPt;
                m_LastForceCaptureTick = now;
                m_CachedFrame = gdiFrame;
                m_Width = gw;
                m_Height = gh;
                outPixels = std::move(gdiFrame);
                outWidth = gw;
                outHeight = gh;
                m_LastDirtyBounds = { 0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight) };
                m_HasLastDirtyBounds = true;
                DrawCursorOntoFrame(outPixels, outWidth, outHeight, m_LastDirtyBounds, m_HasLastDirtyBounds);
                return true;
            }
        }
        return false;
    }

    ComPtr<IDXGIResource> resource;
    DXGI_OUTDUPL_FRAME_INFO info;
    HRESULT hr = m_DeskDupl->AcquireNextFrame(16, &info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // Screen is static (no pixel updates in the last 16ms)
        POINT curPt = {0, 0};
        GetCursorPos(&curPt);
        bool cursorMoved = (curPt.x != m_LastCursorPos.x || curPt.y != m_LastCursorPos.y);

        if (cursorMoved && !m_CachedFrame.empty() && m_Width > 0 && m_Height > 0) {
            m_LastCursorPos = curPt;
            m_LastForceCaptureTick = now;
            outWidth = m_Width;
            outHeight = m_Height;
            outPixels = m_CachedFrame;
            m_HasLastDirtyBounds = true;
            m_LastDirtyBounds = { 0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight) };
            DrawCursorOntoFrame(outPixels, outWidth, outHeight, m_LastDirtyBounds, m_HasLastDirtyBounds);
            return true;
        }

        // Periodic keep-alive frame for static screens (ensures client always receives a crisp frame)
        if (now - m_LastForceCaptureTick > 1500 && !m_CachedFrame.empty() && m_Width > 0 && m_Height > 0) {
            m_LastForceCaptureTick = now;
            outWidth = m_Width;
            outHeight = m_Height;
            outPixels = m_CachedFrame;
            m_HasLastDirtyBounds = true;
            m_LastDirtyBounds = { 0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight) };
            DrawCursorOntoFrame(outPixels, outWidth, outHeight, m_LastDirtyBounds, m_HasLastDirtyBounds);
            return true;
        }

        return false;
    }

    if (FAILED(hr)) {
        // Desktop switch, resolution change, or lock screen
        Reset();
        Sleep(50);
        Initialize();
        return false;
    }

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(resource.As(&tex))) {
        m_DeskDupl->ReleaseFrame();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);

    if (!m_StagingTexture || m_StagingWidth != desc.Width || m_StagingHeight != desc.Height) {
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.MiscFlags = 0;
        m_StagingTexture.Reset();
        if (FAILED(m_Device->CreateTexture2D(&stagingDesc, NULL, &m_StagingTexture))) {
            m_DeskDupl->ReleaseFrame();
            return false;
        }
        m_StagingWidth = desc.Width;
        m_StagingHeight = desc.Height;
    }

    m_DeviceContext->CopyResource(m_StagingTexture.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_DeviceContext->Map(m_StagingTexture.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        m_DeskDupl->ReleaseFrame();
        return false;
    }
    outWidth = desc.Width;
    outHeight = desc.Height;
    outPixels.resize(outWidth * outHeight * 4);
    for (uint32_t y = 0; y < outHeight; ++y) {
        memcpy(outPixels.data() + y * outWidth * 4, (uint8_t*)map.pData + y * map.RowPitch, outWidth * 4);
    }
    m_DeviceContext->Unmap(m_StagingTexture.Get(), 0);

    m_HasLastDirtyBounds = false;
    m_LastDirtyBounds = { 0, 0, 0, 0 };

    if (info.TotalMetadataBufferSize > 0) {
        std::vector<uint8_t> metadata(info.TotalMetadataBufferSize);
        UINT requiredSize = 0;
        if (SUCCEEDED(m_DeskDupl->GetFrameMoveRects(info.TotalMetadataBufferSize, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata.data()), &requiredSize))) {
            UINT moveCount = requiredSize / sizeof(DXGI_OUTDUPL_MOVE_RECT);
            auto moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(metadata.data());
            for (UINT i = 0; i < moveCount; ++i) {
                UnionDirtyRect(m_LastDirtyBounds, m_HasLastDirtyBounds, moves[i].DestinationRect, outWidth, outHeight);
                RECT source = {
                    moves[i].SourcePoint.x,
                    moves[i].SourcePoint.y,
                    moves[i].SourcePoint.x + (moves[i].DestinationRect.right - moves[i].DestinationRect.left),
                    moves[i].SourcePoint.y + (moves[i].DestinationRect.bottom - moves[i].DestinationRect.top)
                };
                UnionDirtyRect(m_LastDirtyBounds, m_HasLastDirtyBounds, source, outWidth, outHeight);
            }
        }

        UINT dirtyBytes = 0;
        if (SUCCEEDED(m_DeskDupl->GetFrameDirtyRects(info.TotalMetadataBufferSize, reinterpret_cast<RECT*>(metadata.data()), &dirtyBytes))) {
            UINT dirtyCount = dirtyBytes / sizeof(RECT);
            auto dirtyRects = reinterpret_cast<const RECT*>(metadata.data());
            for (UINT i = 0; i < dirtyCount; ++i) {
                UnionDirtyRect(m_LastDirtyBounds, m_HasLastDirtyBounds, dirtyRects[i], outWidth, outHeight);
            }
        }
    }

    if (!m_HasLastDirtyBounds) {
        m_LastDirtyBounds = { 0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight) };
        m_HasLastDirtyBounds = true;
    }

    m_DeskDupl->ReleaseFrame();

    for (size_t i = 3; i < outPixels.size(); i += 4) outPixels[i] = 0xFF;
    m_CachedFrame = outPixels;
    m_Width = outWidth;
    m_Height = outHeight;
    m_LastForceCaptureTick = GetTickCount64();

    DrawCursorOntoFrame(outPixels, outWidth, outHeight, m_LastDirtyBounds, m_HasLastDirtyBounds);

    return true;
}

bool ScreenCapturer::GetLastDirtyBounds(uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) const {
    if (!m_HasLastDirtyBounds) return false;
    x = static_cast<uint32_t>(m_LastDirtyBounds.left);
    y = static_cast<uint32_t>(m_LastDirtyBounds.top);
    w = static_cast<uint32_t>(m_LastDirtyBounds.right - m_LastDirtyBounds.left);
    h = static_cast<uint32_t>(m_LastDirtyBounds.bottom - m_LastDirtyBounds.top);
    return w > 0 && h > 0;
}

bool ScreenCapturer::CaptureNextFrameJpeg(std::vector<uint8_t>& outJpeg, uint32_t& outWidth, uint32_t& outHeight, int quality) {
    std::vector<uint8_t> raw; if (!CaptureNextFrame(raw, outWidth, outHeight)) return false;
    return CaptureRegionJpeg(raw, outWidth, outHeight, 0, 0, outWidth, outHeight, outJpeg, quality);
}

bool ScreenCapturer::CaptureRegionJpeg(const std::vector<uint8_t>& rawPixels, uint32_t fullWidth, uint32_t fullHeight, uint32_t targetX, uint32_t targetY, uint32_t targetW, uint32_t targetH, std::vector<uint8_t>& outJpeg, int quality, bool force444) {
    if (targetW == 0 || targetH == 0 || rawPixels.empty()) return false;
    
    // Thread-local reusable buffer: eliminates 6MB memory allocation on every frame
    static thread_local std::vector<uint8_t> s_bgr;
    size_t requiredBgrSize = static_cast<size_t>(targetW) * targetH * 3;
    if (s_bgr.size() < requiredBgrSize) {
        s_bgr.resize(requiredBgrSize);
    }

    if (targetX == 0 && targetY == 0 && targetW == fullWidth && targetH == fullHeight) {
        // Fast linear vectorization for full screen frames
        const uint8_t* pSrc = rawPixels.data();
        uint8_t* pDst = s_bgr.data();
        size_t count = static_cast<size_t>(targetW) * targetH;
        for (size_t i = 0; i < count; ++i) {
            pDst[0] = pSrc[0];
            pDst[1] = pSrc[1];
            pDst[2] = pSrc[2];
            pSrc += 4;
            pDst += 3;
        }
    } else {
        // Fast row-pointer copy for sub-regions
        uint8_t* pDst = s_bgr.data();
        for (uint32_t y = 0; y < targetH; ++y) {
            uint32_t srcY = targetY + y;
            if (srcY >= fullHeight) break;
            const uint8_t* pSrcRow = rawPixels.data() + (srcY * fullWidth + targetX) * 4;
            for (uint32_t x = 0; x < targetW; ++x) {
                pDst[0] = pSrcRow[0];
                pDst[1] = pSrcRow[1];
                pDst[2] = pSrcRow[2];
                pSrcRow += 4;
                pDst += 3;
            }
        }
    }

    static thread_local IWICImagingFactory* s_encWicFactory = NULL;
    if (!s_encWicFactory) {
        CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_encWicFactory));
    }
    if (!s_encWicFactory) return false;

    IWICBitmapEncoder* pEncoder = NULL;
    if (FAILED(s_encWicFactory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &pEncoder)) || !pEncoder) return false;

    IStream* pStream = NULL;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pStream)) || !pStream) {
        pEncoder->Release();
        return false;
    }

    if (FAILED(pEncoder->Initialize(pStream, WICBitmapEncoderNoCache))) {
        pStream->Release();
        pEncoder->Release();
        return false;
    }

    IWICBitmapFrameEncode* pFrame = NULL;
    IPropertyBag2* pProp = NULL;
    if (FAILED(pEncoder->CreateNewFrame(&pFrame, &pProp)) || !pFrame) {
        if (pProp) pProp->Release();
        pStream->Release();
        pEncoder->Release();
        return false;
    }

    PROPBAG2 opts[2] = { 0 };
    VARIANT vals[2];
    VariantInit(&vals[0]);
    VariantInit(&vals[1]);
    opts[0].pstrName = (LPOLESTR)L"ImageQuality";
    vals[0].vt = VT_R4;
    vals[0].fltVal = quality / 100.0f;

    if (force444) {
        opts[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";
        vals[1].vt = VT_UI1;
        vals[1].bVal = 0x02; // 4:4:4
        if (pProp) pProp->Write(2, opts, vals);
    } else {
        if (pProp) pProp->Write(1, opts, vals);
    }

    if (FAILED(pFrame->Initialize(pProp))) {
        if (pProp) pProp->Release();
        pFrame->Release();
        pStream->Release();
        pEncoder->Release();
        return false;
    }

    pFrame->SetSize(targetW, targetH);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    pFrame->SetPixelFormat(&fmt);
    pFrame->WritePixels(targetH, targetW * 3, (UINT)requiredBgrSize, s_bgr.data());

    pFrame->Commit();
    pEncoder->Commit();

    STATSTG stg;
    pStream->Stat(&stg, STATFLAG_NONAME);
    outJpeg.resize(stg.cbSize.LowPart);
    LARGE_INTEGER li = { 0 };
    pStream->Seek(li, STREAM_SEEK_SET, NULL);
    ULONG rb = 0;
    pStream->Read(outJpeg.data(), (ULONG)outJpeg.size(), &rb);

    if (pProp) pProp->Release();
    pFrame->Release();
    pStream->Release();
    pEncoder->Release();
    return !outJpeg.empty();
}

void ScreenCapturer::ReleaseFrame() {}

} // namespace capture
} // namespace core
} // namespace gupt
