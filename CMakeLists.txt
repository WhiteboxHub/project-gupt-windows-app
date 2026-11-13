cmake_minimum_required(VERSION 3.20)
project(GuptRemoteDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "This project is strictly for Windows as it relies on Win32, DXGI, and WIC APIs.")
endif()

# ── Static runtime: bundle C++ runtime inside the .exe (no DLL dependencies) ──
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Group core dependencies
set(WINDOWS_LIBS d3d11 dxgi ws2_32 user32 gdi32 msimg32 windowscodecs shlwapi comctl32 winhttp winmm ole32 dbghelp advapi32 bcrypt)

# Include directories
set(CORE_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# ════════════════════════════════════════════════════════
#   Gupt.exe  —  Unified single executable (Host + Client)
#   Portable: statically linked, no MSVC runtime DLLs needed
# ════════════════════════════════════════════════════════
add_executable(Gupt WIN32
    Launcher/main.cpp
    Shared/Protocol.h
    Core/Capture/ScreenCapturer.h
    Core/Capture/ScreenCapturer.cpp
    Core/Input/InputInjector.h
    Core/Input/InputInjector.cpp
    Core/Network/TcpNetwork.h
    Core/Network/TcpNetwork.cpp
    Core/Network/CryptoSession.h
    Core/Network/CryptoSession.cpp
    Core/Network/StunClient.h
    Core/Network/StunClient.cpp
    Core/Network/UdpP2PTransport.h
    Core/Network/UdpP2PTransport.cpp
    Core/Audio/AudioCapturer.h
    Core/Audio/AudioCapturer.cpp
    Core/Audio/AudioPlayer.h
    Core/Audio/AudioPlayer.cpp
    resources.rc
)
target_include_directories(Gupt PRIVATE ${CORE_INCLUDES})
target_link_libraries(Gupt PRIVATE ${WINDOWS_LIBS})
set_property(TARGET Gupt PROPERTY FOLDER "Applications")
set_target_properties(Gupt PROPERTIES
    LINK_FLAGS "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\""
)

# ════════════════════════════════════════════════════════
#   Legacy split executables (kept for development only)
# ════════════════════════════════════════════════════════

# ==== Add Host Agent ====
add_executable(HostAgent
    HostAgent/main.cpp
    Shared/Protocol.h
    Core/Capture/ScreenCapturer.h
    Core/Capture/ScreenCapturer.cpp
    Core/Input/InputInjector.h
    Core/Input/InputInjector.cpp
    Core/Network/TcpNetwork.h
    Core/Network/TcpNetwork.cpp
    resources.rc
)
target_include_directories(HostAgent PRIVATE ${CORE_INCLUDES})
target_link_libraries(HostAgent PRIVATE ${WINDOWS_LIBS})
set_property(TARGET HostAgent PROPERTY FOLDER "Applications")
set_target_properties(HostAgent PROPERTIES
    LINK_FLAGS "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\""
)

# ==== Add Client Viewer ====
add_executable(ClientViewer WIN32
    ClientViewer/main.cpp
    Shared/Protocol.h
    Core/Network/TcpNetwork.h
    Core/Network/TcpNetwork.cpp
    resources.rc
)
target_include_directories(ClientViewer PRIVATE ${CORE_INCLUDES})
target_link_libraries(ClientViewer PRIVATE ${WINDOWS_LIBS})
set_property(TARGET ClientViewer PROPERTY FOLDER "Applications")
set_target_properties(ClientViewer PROPERTIES
    LINK_FLAGS "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\""
)

# ==== Endurance Multi-App Stress Test ====
add_executable(GuptEnduranceTest
    test/endurance_test.cpp
    Shared/Protocol.h
    Core/Capture/ScreenCapturer.h
    Core/Capture/ScreenCapturer.cpp
    Core/Input/InputInjector.h
    Core/Input/InputInjector.cpp
    Core/Network/CryptoSession.h
    Core/Network/CryptoSession.cpp
    Core/Audio/AudioCapturer.h
    Core/Audio/AudioCapturer.cpp
    Core/Audio/AudioPlayer.h
    Core/Audio/AudioPlayer.cpp
)
target_include_directories(GuptEnduranceTest PRIVATE ${CORE_INCLUDES})
target_link_libraries(GuptEnduranceTest PRIVATE ${WINDOWS_LIBS} psapi)

# ==== Cross-WiFi Endurance Test ====
add_executable(GuptCrossWifiTest
    test/test_cross_wifi_endurance.cpp
    Shared/Protocol.h
    Core/Capture/ScreenCapturer.h
    Core/Capture/ScreenCapturer.cpp
    Core/Input/InputInjector.h
    Core/Input/InputInjector.cpp
    Core/Network/CryptoSession.h
    Core/Network/CryptoSession.cpp
    Core/Audio/AudioCapturer.h
    Core/Audio/AudioCapturer.cpp
    Core/Audio/AudioPlayer.h
    Core/Audio/AudioPlayer.cpp
)
target_include_directories(GuptCrossWifiTest PRIVATE ${CORE_INCLUDES})
target_link_libraries(GuptCrossWifiTest PRIVATE ${WINDOWS_LIBS} psapi)

# ==== 10-Tab Dynamic Multi-App & Typing Cross-WiFi Test ====
add_executable(GuptTabsTypingTest
    test/test_tabs_typing_cross_wifi.cpp
    Shared/Protocol.h
    Core/Capture/ScreenCapturer.h
    Core/Capture/ScreenCapturer.cpp
    Core/Input/InputInjector.h
    Core/Input/InputInjector.cpp
    Core/Network/CryptoSession.h
    Core/Network/CryptoSession.cpp
    Core/Audio/AudioCapturer.h
    Core/Audio/AudioCapturer.cpp
    Core/Audio/AudioPlayer.h
    Core/Audio/AudioPlayer.cpp
)
target_include_directories(GuptTabsTypingTest PRIVATE ${CORE_INCLUDES})
target_link_libraries(GuptTabsTypingTest PRIVATE ${WINDOWS_LIBS} psapi)




# Primary startup project is now the unified launcher
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Gupt)
