# Gupt Remote Desktop (Windows App)

Gupt Remote Desktop is a minimalist, MVP-stage remote desktop and screen-sharing application designed exclusively for Windows. It utilizes native Win32 APIs (GDI, SendInput) to provide reliable screen capture and input injection capabilities across any environment.

## Architecture

This project consists of three primary components:

1. **HostAgent (C++):** A Windows executable running on the machine to be controlled. It captures the screen using GDI BitBlt, injects simulated keyboard and mouse events using Win32 `SendInput`, and ensures security by prompting the user for consent before allowing remote control.
2. **ClientViewer (C++):** A Windows executable used by the controller to connect to the HostAgent, view its screen stream via a native Win32 window, and capture local inputs to send to the host.
3. **Signaling Server (Node.js):** A WebSocket server designed for future WebRTC signaling. (Currently, the C++ applications use a direct raw TCP connection for MVP).

## Prerequisites

- **Windows OS**: This project uses native Windows APIs and will not compile on macOS or Linux.
- **CMake**: Version 3.20 or newer.
- **C++17 Compiler**: MSVC (Visual Studio) is recommended.

## How to Build

You can build the `HostAgent` and `ClientViewer` using CMake from the root of the Windows app project.

```cmd
# Create a build directory
mkdir build
cd build

# Configure the project
cmake ..

# Build both applications (Release or Debug)
cmake --build . --config Release
```

## How to Test on a Single Computer (Localhost)

If you have built both applications on the same computer, you can test them together locally.

1. **Start the Host:** 
   Open a terminal and run `.\build\Release\HostAgent.exe`. It will begin listening on port 8080.
2. **Start the Client:** 
   Open a second terminal and run `.\build\Release\ClientViewer.exe`. It will connect to localhost `127.0.0.1` by default.
3. **Accept Connection:** 
   The Host will display a security dialog prompting you to grant permission. Click "Yes".
4. **Testing Inputs:** 
   *Note: Testing on a single monitor creates an "infinite mirror" effect.* To successfully test remote control, drag the ClientViewer window to the side so you can see your physical desktop behind it. Click deeply inside the ClientViewer's video feed, and watch your physical mouse cursor teleport and click the real item!

## How to Control Another Computer over the Network

To cleanly control a secondary computer over your local network:

1. **Host Setup (The controlled PC):**
   - Copy `HostAgent.exe` to the other computer.
   - Run `ipconfig` in PowerShell on that machine to find its `IPv4 Address` (e.g. `192.168.1.15`).
   - Start `HostAgent.exe`. **Important:** If Windows Defender Firewall prompts you, make sure to click **Allow Access** or the incoming connection will be blocked.
2. **Client Setup (The controlling PC):**
   - Open PowerShell on your main PC.
   - Run the Client application and pass the Host's IP address as an argument:
     ```cmd
     .\build\Release\ClientViewer.exe 192.168.1.15
     ```
3. Once the person at the Host PC clicks "Yes" to accept your connection, their desktop will appear in your Client window. Any keystrokes or mouse clicks you perform inside the window will instantly happen on their machine!

## Security
The `HostAgent` forces a user consent prompt before any peer can connect, fulfilling the security constraint that a remote session cannot be silently initiated. A console indicator remains active during the active session.
