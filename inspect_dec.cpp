#include <iostream>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

int main() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    IMFTransform* enc = NULL;
    CoCreateInstance(CLSID_CMSH264EncoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enc));

    IMFMediaType* outType = NULL;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, 2500000);
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, 60, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    enc->SetOutputType(0, outType, 0);

    IMFMediaType* inType = NULL;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, 60, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    enc->SetInputType(0, inType, 0);

    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // Create a 1920x1080 NV12 test buffer
    size_t nv12Size = 1920 * 1080 * 3 / 2;
    std::cout << "Input NV12 size: " << nv12Size << " bytes (1920*1080*1.5)\n";

    enc->Release();
    outType->Release();
    inType->Release();
    return 0;
}
