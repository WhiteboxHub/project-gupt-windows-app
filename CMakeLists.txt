cmake_minimum_required(VERSION 3.20)
project(GuptRemoteDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "This project is strictly for Windows as it relies on Win32, DXGI, and Media Foundation APIs.")
endif()

# Group core dependencies
set(WINDOWS_LIBS d3d11 dxgi ws2_32 user32)

# Include directories
set(CORE_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# ==== Add Host Agent ====
add_executable(HostAgent 
    HostAgent/main.cpp
    Shared/Protocol.h
    Core/Capture/ScreenCapturer.h
    Core/Input/InputInjector.h
    Core/Network/TcpNetwork.h
)
target_include_directories(HostAgent PRIVATE ${CORE_INCLUDES})
target_link_libraries(HostAgent PRIVATE ${WINDOWS_LIBS})
set_property(TARGET HostAgent PROPERTY FOLDER "Applications")

# ==== Add Client Viewer ====
add_executable(ClientViewer 
    ClientViewer/main.cpp
    Shared/Protocol.h
    Core/Network/TcpNetwork.h
)
target_include_directories(ClientViewer PRIVATE ${CORE_INCLUDES})
target_link_libraries(ClientViewer PRIVATE ${WINDOWS_LIBS})
set_property(TARGET ClientViewer PROPERTY FOLDER "Applications")

# Configure VS Startup project
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HostAgent)
