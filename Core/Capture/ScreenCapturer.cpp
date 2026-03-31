#include "ScreenCapturer.h"
#include <iostream>

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
    
    // We captured the screen! Sleep slightly to throttle to ~30 FPS for this MVP.
    Sleep(33); 
    return true;
}

void ScreenCapturer::ReleaseFrame() {
}

} // namespace capture
} // namespace core
} // namespace gupt
