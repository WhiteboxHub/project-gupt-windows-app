#include "ScreenCapturer.h"
#include <iostream>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

namespace gupt {
namespace core {
namespace capture {

ScreenCapturer::ScreenCapturer() {}
ScreenCapturer::~ScreenCapturer() {}

bool ScreenCapturer::Initialize() {
    // With GDI, we don't need heavy graphics initializations.
    std::cout << "Screen capturer initialized using robust GDI backend." << std::endl;
    return true;
}

bool ScreenCapturer::CaptureNextFrame(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    HDC hScreenDC = GetDC(NULL);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);
    
    // Copy the entire screen into our memory device context
    BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, 0, 0, SRCCOPY);
    SelectObject(hMemoryDC, hOldBitmap);
    
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; // Negative means top-down row order
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    outWidth = width;
    outHeight = height;
    
    uint32_t stride = width * 4;
    outPixels.resize(stride * height);
    
    // Extract the raw pixel bits into our outgoing vector
    if (!GetDIBits(hScreenDC, hBitmap, 0, height, outPixels.data(), &bi, DIB_RGB_COLORS)) {
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);
    
    return true;
}

bool ScreenCapturer::CaptureNextFrameJpeg(std::vector<uint8_t>& outJpeg, uint32_t& outWidth, uint32_t& outHeight, int quality) {
    std::vector<uint8_t> rawPixels;
    if (!CaptureNextFrame(rawPixels, outWidth, outHeight)) return false;

    IWICImagingFactory* pFactory = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return false;

    IWICBitmapEncoder* pEncoder = NULL;
    hr = pFactory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &pEncoder);
    if (FAILED(hr)) { pFactory->Release(); return false; }

    IStream* pStream = NULL;
    hr = CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    if (FAILED(hr)) { pEncoder->Release(); pFactory->Release(); return false; }

    hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { pStream->Release(); pEncoder->Release(); pFactory->Release(); return false; }

    IWICBitmapFrameEncode* pFrame = NULL;
    IPropertyBag2* pPropertybag = NULL;
    hr = pEncoder->CreateNewFrame(&pFrame, &pPropertybag);
    if (FAILED(hr)) { pStream->Release(); pEncoder->Release(); pFactory->Release(); return false; }

    PROPBAG2 option = { 0 };
    option.pstrName = (LPOLESTR)L"ImageQuality";
    VARIANT varValue;
    VariantInit(&varValue);
    varValue.vt = VT_R4;
    varValue.fltVal = quality / 100.0f;
    hr = pPropertybag->Write(1, &option, &varValue);

    hr = pFrame->Initialize(pPropertybag);
    if (FAILED(hr)) { pPropertybag->Release(); pFrame->Release(); pStream->Release(); pEncoder->Release(); pFactory->Release(); return false; }

    hr = pFrame->SetSize(outWidth, outHeight);
    WICPixelFormatGUID formatGUID = GUID_WICPixelFormat32bppBGRA;
    hr = pFrame->SetPixelFormat(&formatGUID);

    UINT stride = outWidth * 4;
    UINT cbBufferSize = stride * outHeight;
    hr = pFrame->WritePixels(outHeight, stride, cbBufferSize, rawPixels.data());

    hr = pFrame->Commit();
    hr = pEncoder->Commit();

    STATSTG statstg;
    pStream->Stat(&statstg, STATFLAG_NONAME);
    UINT streamSize = statstg.cbSize.LowPart;

    outJpeg.resize(streamSize);
    LARGE_INTEGER liZero = { 0 };
    pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
    ULONG bytesRead = 0;
    pStream->Read(outJpeg.data(), streamSize, &bytesRead);

    pPropertybag->Release();
    pFrame->Release();
    pStream->Release();
    pEncoder->Release();
    pFactory->Release();

    return true;
}

void ScreenCapturer::ReleaseFrame() {
}

} // namespace capture
} // namespace core
} // namespace gupt
