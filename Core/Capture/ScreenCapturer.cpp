#include "ScreenCapturer.h"
#include <iostream>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace gupt {
namespace core {
namespace capture {

ScreenCapturer::ScreenCapturer() {}
ScreenCapturer::~ScreenCapturer() {}

bool ScreenCapturer::Initialize() {
    D3D_FEATURE_LEVEL fl;
    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &m_Device, &fl, &m_DeviceContext);
    if (!m_Device) return false;
    ComPtr<IDXGIDevice> dxgiDevice; m_Device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetParent(IID_PPV_ARGS(&adapter));
    ComPtr<IDXGIOutput> output; adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1; output.As(&output1);
    output1->DuplicateOutput(m_Device.Get(), &m_DeskDupl);
    DXGI_OUTDUPL_DESC desc; m_DeskDupl->GetDesc(&desc);
    m_Width = desc.ModeDesc.Width; m_Height = desc.ModeDesc.Height;
    return true;
}

bool ScreenCapturer::CaptureNextFrame(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    if (!m_DeskDupl) return false;
    ComPtr<IDXGIResource> resource; DXGI_OUTDUPL_FRAME_INFO info;
    if (FAILED(m_DeskDupl->AcquireNextFrame(100, &info, &resource))) return false;
    ComPtr<ID3D11Texture2D> tex; resource.As(&tex);
    D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
    desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.Usage = D3D11_USAGE_STAGING; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging; m_Device->CreateTexture2D(&desc, NULL, &staging);
    m_DeviceContext->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map; m_DeviceContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
    outWidth = desc.Width; outHeight = desc.Height;
    outPixels.resize(outWidth * outHeight * 4);
    for (uint32_t y = 0; y < outHeight; ++y) {
        memcpy(outPixels.data() + y * outWidth * 4, (uint8_t*)map.pData + y * map.RowPitch, outWidth * 4);
    }
    m_DeviceContext->Unmap(staging.Get(), 0);
    m_DeskDupl->ReleaseFrame();

    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        HDC hdc = CreateCompatibleDC(NULL);
        HBITMAP hbm = CreateCompatibleBitmap(GetDC(NULL), outWidth, outHeight);
        SelectObject(hdc, hbm);
        BITMAPINFO bmi = { sizeof(BITMAPINFOHEADER), (int)outWidth, -(int)outHeight, 1, 32, BI_RGB };
        StretchDIBits(hdc, 0, 0, outWidth, outHeight, 0, 0, outWidth, outHeight, outPixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        ICONINFO ii; GetIconInfo(ci.hCursor, &ii);
        POINT pt = ci.ptScreenPos;
        DrawIconEx(hdc, pt.x - ii.xHotspot, pt.y - ii.yHotspot, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
        GetDIBits(hdc, hbm, 0, outHeight, outPixels.data(), &bmi, DIB_RGB_COLORS);
        if (ii.hbmMask) DeleteObject(ii.hbmMask); if (ii.hbmColor) DeleteObject(ii.hbmColor);
        DeleteObject(hbm); DeleteDC(hdc);
    }
    for (size_t i = 3; i < outPixels.size(); i += 4) outPixels[i] = 0xFF;
    return true;
}

bool ScreenCapturer::CaptureNextFrameJpeg(std::vector<uint8_t>& outJpeg, uint32_t& outWidth, uint32_t& outHeight, int quality) {
    std::vector<uint8_t> raw; if (!CaptureNextFrame(raw, outWidth, outHeight)) return false;
    return CaptureRegionJpeg(raw, outWidth, outHeight, 0, 0, outWidth, outHeight, outJpeg, quality);
}

bool ScreenCapturer::CaptureRegionJpeg(const std::vector<uint8_t>& rawPixels, uint32_t fullWidth, uint32_t fullHeight, uint32_t targetX, uint32_t targetY, uint32_t targetW, uint32_t targetH, std::vector<uint8_t>& outJpeg, int quality) {
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
    
    // 3. Force 4:4:4 High-Fidelity Color (Full Text Clarity)
    PROPBAG2 opts[2] = { 0 }; 
    VARIANT vals[2];
    opts[0].pstrName = (LPOLESTR)L"ImageQuality"; VariantInit(&vals[0]); vals[0].vt = VT_R4; vals[0].fltVal = quality / 100.0f;
    opts[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling"; VariantInit(&vals[1]); vals[1].vt = VT_UI1; vals[1].bVal = 0x02; // 4:4:4
    pProp->Write(2, opts, vals);
    
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
