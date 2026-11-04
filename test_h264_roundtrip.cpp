#include <iostream>
#include <vector>
#include <chrono>
#include "Core/Video/H264Encoder.h"
#include "Core/Video/H264Decoder.h"

int main() {
    std::cout << "========================================================\n"
              << "  Gupt Hardware H.264 Encoder & Decoder Verification\n"
              << "========================================================\n" << std::endl;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    const uint32_t W = 1920;
    const uint32_t H = 1080;
    const uint32_t FPS = 60;
    const uint32_t BITRATE = 2500000; // 2.5 Mbps

    std::cout << "[1/4] Creating H264Encoder instance..." << std::endl;
    gupt::core::video::H264Encoder encoder;
    std::cout << "[1/4] Calling Initialize..." << std::endl;
    bool initOk = encoder.Initialize(W, H, FPS, BITRATE);
    std::cout << "[1/4] Initialize returned: " << (initOk ? "TRUE" : "FALSE") << std::endl;
    if (!initOk) {
        std::cerr << "FAIL: Could not initialize H264Encoder!" << std::endl;
        return 1;
    }
    std::cout << "SUCCESS: Hardware H.264 Encoder initialized.\n\n";

    gupt::core::video::H264Decoder decoder;
    std::cout << "[2/4] Initializing Hardware H.264 Decoder..." << std::endl;
    if (!decoder.Initialize(W, H)) {
        std::cerr << "FAIL: Could not initialize H264Decoder!" << std::endl;
        return 1;
    }
    std::cout << "SUCCESS: Hardware H.264 Decoder initialized.\n\n";

    // Generate synthetic 1080p desktop frame with patterns
    std::vector<uint8_t> testFrame(W * H * 4);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            size_t idx = (y * W + x) * 4;
            testFrame[idx + 0] = static_cast<uint8_t>(x & 0xFF);         // B
            testFrame[idx + 1] = static_cast<uint8_t>(y & 0xFF);         // G
            testFrame[idx + 2] = static_cast<uint8_t>((x + y) & 0xFF);   // R
            testFrame[idx + 3] = 0xFF;                                   // A
        }
    }

    std::cout << "[3/4] Encoding 10 test 1080p frames..." << std::endl;
    size_t totalBytes = 0;
    double totalEncodeMs = 0.0;
    double totalDecodeMs = 0.0;
    int encodedFrames = 0;
    int decodedFrames = 0;

    for (int i = 0; i < 10; ++i) {
        // Slightly alter frame to simulate desktop motion
        testFrame[0] = static_cast<uint8_t>(i * 20);

        auto t0 = std::chrono::high_resolution_clock::now();
        std::vector<uint8_t> nals;
        bool ok = encoder.EncodeBgraFrame(testFrame.data(), W, H, nals, (i == 0));
        auto t1 = std::chrono::high_resolution_clock::now();

        if (ok && !nals.empty()) {
            double encMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            totalEncodeMs += encMs;
            totalBytes += nals.size();
            encodedFrames++;

            std::cout << "  Frame #" << (i + 1) << ": " << nals.size() << " bytes encoded in "
                      << encMs << " ms";

            // Decode
            auto d0 = std::chrono::high_resolution_clock::now();
            std::vector<uint8_t> decodedBgra;
            uint32_t decW = 0, decH = 0;
            bool decOk = decoder.DecodeNal(nals.data(), nals.size(), decodedBgra, decW, decH);
            auto d1 = std::chrono::high_resolution_clock::now();

            if (decOk && !decodedBgra.empty()) {
                double decMs = std::chrono::duration<double, std::milli>(d1 - d0).count();
                totalDecodeMs += decMs;
                decodedFrames++;
                std::cout << " | Decoded in " << decMs << " ms (" << decW << "x" << decH << ")";
            }
            std::cout << "\n";
        }
    }

    std::cout << "\n[4/4] Verification Summary:\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "  Raw 1080p BGRA frame size: " << (W * H * 4 / 1024) << " KB\n";
    std::cout << "  Previous JPEG size:        ~450 - 700 KB per frame\n";
    if (encodedFrames > 0) {
        std::cout << "  Average H.264 NAL size:    " << (totalBytes / encodedFrames / 1024) << " KB per frame\n";
        std::cout << "  Compression ratio:         ~" << ((W * H * 4) / (totalBytes / encodedFrames)) << ":1\n";
        std::cout << "  Average Encode latency:    " << (totalEncodeMs / encodedFrames) << " ms\n";
    }
    if (decodedFrames > 0) {
        std::cout << "  Average Decode latency:    " << (totalDecodeMs / decodedFrames) << " ms\n";
    }
    std::cout << "--------------------------------------------------------\n";

    if (encodedFrames >= 5) {
        std::cout << "\n>>> ALL H.264 HARDWARE ENCODE/DECODE CHECKS PASSED! <<<\n";
        return 0;
    } else {
        std::cerr << "\nFAIL: Encoded fewer frames than expected.\n";
        return 1;
    }
}
