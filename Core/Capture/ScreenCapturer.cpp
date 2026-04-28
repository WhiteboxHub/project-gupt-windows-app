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

ScreenCapturer::ScreenCapturer() {}
ScreenCapturer::~ScreenCapturer() { Reset(); }

void ScreenCapturer::Reset() {
    m_DeskDupl.Reset();
    m_DeviceContext.Reset();
    m_Device.Reset();
    m_Width = 0;
    m_Height = 0;
    m_LastDirtyBounds = { 0, 0, 0, 0 };
    m_HasLastDirtyBounds = false;
}

bool ScreenCapturer::Initialize() {
    Reset();
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

bool ScreenCapturer::CaptureNextFrame(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    if (!m_DeskDupl && !Initialize()) return false;
    ComPtr<IDXGIResource> resource; DXGI_OUTDUPL_FRAME_INFO info;
    HRESULT hr = m_DeskDupl->AcquireNextFrame(100, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_INVALID_CALL) {
            Reset();
            Sleep(250);
            Initialize();
        }
        return false;
    }

    ComPtr<ID3D11Texture2D> tex; if (FAILED(resource.As(&tex))) { m_DeskDupl->ReleaseFrame(); return false; }
    D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
    desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.Usage = D3D11_USAGE_STAGING; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging; if (FAILED(m_Device->CreateTexture2D(&desc, NULL, &staging))) { m_DeskDupl->ReleaseFrame(); return false; }
    m_DeviceContext->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map; if (FAILED(m_DeviceContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { m_DeskDupl->ReleaseFrame(); return false; }
    outWidth = desc.Width; outHeight = desc.Height;
    outPixels.resize(outWidth * outHeight * 4);
    for (uint32_t y = 0; y < outHeight; ++y) {
        memcpy(outPixels.data() + y * outWidth * 4, (uint8_t*)map.pData + y * map.RowPitch, outWidth * 4);
    }
    m_DeviceContext->Unmap(staging.Get(), 0);

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
    // 1. Contiguous 24bpp Extraction (Fixes RGB Lines & Row Bleeding)
    std::vector<uint8_t> bgr(targetW * targetH * 3);
    for (uint32_t y = 0; y < targetH; ++y) {
        uint32_t srcY = (targetY + y >= fullHeight) ? (fullHeight - 1) : (targetY + y);
        for (uint32_t x = 0; x < targetW; ++x) {
            uint32_t srcX = (targetX + x >= fullWidth) ? (fullWidth - 1) : (targetX + x);
            size_t srcIdx = (srcY * fullWidth + srcX) * 4;
            size_t dstIdx = (y * targetW + x) * 3;
            bgr[dstIdx + 0] = rawPixels[srcIdx + 0]; // B
            bgr[dstIdx + 1] = rawPixels[srcIdx + 1]; // G
            bgr[dstIdx + 2] = rawPixels[srcIdx + 2]; // R
        }
    }

    // 2. Initialize WIC
    IWICImagingFactory* pFactory = NULL; CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    IWICBitmapEncoder* pEncoder = NULL; pFactory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &pEncoder);
    IStream* pStream = NULL; CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* pFrame = NULL; IPropertyBag2* pProp = NULL; pEncoder->CreateNewFrame(&pFrame, &pProp);
    
    // 3. Use high-fidelity 4:4:4 for text/small UI, but let WIC use faster default
    // subsampling for large video-like regions to reduce latency and bandwidth.
    PROPBAG2 opts[2] = { 0 };
    VARIANT vals[2]; VariantInit(&vals[0]); VariantInit(&vals[1]);
    opts[0].pstrName = (LPOLESTR)L"ImageQuality"; VariantInit(&vals[0]); vals[0].vt = VT_R4; vals[0].fltVal = quality / 100.0f;
    if (force444) {
        opts[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling"; vals[1].vt = VT_UI1; vals[1].bVal = 0x02; // 4:4:4
        pProp->Write(2, opts, vals);
    } else {
        pProp->Write(1, opts, vals);
    }
    
    // 4. Encode Pure 24bpp Buffer
    pFrame->Initialize(pProp); pFrame->SetSize(targetW, targetH);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR; pFrame->SetPixelFormat(&fmt);
    pFrame->WritePixels(targetH, targetW * 3, (UINT)bgr.size(), bgr.data());
    
    pFrame->Commit(); pEncoder->Commit();
    STATSTG stg; pStream->Stat(&stg, STATFLAG_NONAME); outJpeg.resize(stg.cbSize.LowPart);
    LARGE_INTEGER li = { 0 }; pStream->Seek(li, STREAM_SEEK_SET, NULL); ULONG rb = 0; pStream->Read(outJpeg.data(), (ULONG)outJpeg.size(), &rb);
    pProp->Release(); pFrame->Release(); pStream->Release(); pEncoder->Release(); pFactory->Release();
    return true;
}

void ScreenCapturer::ReleaseFrame() {}

} } }
