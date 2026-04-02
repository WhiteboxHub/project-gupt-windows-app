# Gupt Remote Desktop (Windows App)

Gupt Remote Desktop is a lightweight, self-contained remote desktop application for Windows. It uses native Win32 APIs (GDI, SendInput) for screen capture and input injection — **no third-party libraries, no runtimes**. The entire application ships as a single portable `.exe`.

---

## Architecture

```
project-gupt-windows-app/
├── Launcher/         ← Unified GUI launcher (Host + Client in one .exe)
├── HostAgent/        ← Legacy standalone host (dev/testing only)
├── ClientViewer/     ← Legacy standalone client (dev/testing only)
├── Core/
│   ├── Capture/      ← GDI-based JPEG screen capturer
│   ├── Input/        ← Win32 SendInput mouse/keyboard injector
│   └── Network/      ← Raw TCP framing layer
├── Shared/           ← Protocol definitions (message types, serialization)
└── SignalingServer/  ← Node.js WebSocket server (reserved for WebRTC)
```

### Components

| Target | Description |
|---|---|
| **`Gupt.exe`** | ✅ **Recommended.** Single portable executable. Launches a GUI dialog to choose Host or Client mode. Statically linked — runs on any Windows PC with no dependencies. |
| `HostAgent.exe` | Legacy dev binary. Runs host-only in a console window (no launcher UI). |
| `ClientViewer.exe` | Legacy dev binary. Runs client-only; IP is hardcoded (for dev use). |

---

## Prerequisites

- **Windows 10/11** (Win32 APIs used: GDI, SendInput, WIC, DirectX)
- **CMake** 3.20 or newer
- **MSVC** (Visual Studio 2019 or later with "Desktop development with C++" workload)

> **No external libraries required.** The build uses only Windows SDK APIs.

---

## How to Build

Run from the `project-gupt-windows-app/` directory:

```powershell
# Configure (auto-detects installed Visual Studio generator)
cmake -S . -B build

# Build the unified launcher (recommended)
cmake --build build --target Gupt --config Release

# Or build everything (Gupt + legacy HostAgent + ClientViewer)
cmake --build build --config Release
```

Output binaries are written to:
```
build/Release/Gupt.exe          ← Main distributable (~240 KB, self-contained)
build/Release/HostAgent.exe     ← Legacy host binary
build/Release/ClientViewer.exe  ← Legacy client binary
```

> **Static linking** is enabled via `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded"`.  
> `Gupt.exe` does **not** require the Visual C++ Redistributable to be installed on the target machine.

---

## How to Run — Using Gupt.exe (Recommended)

Just copy `Gupt.exe` to any Windows PC and double-click it.

A launcher dialog will appear:

```
┌──────────────────────────────────────────┐
│  Gupt Remote Desktop                     │
│  Secure, lightweight screen sharing      │
│                                          │
│  SELECT MODE                             │
│  ● Host (Share my screen)                │
│  ○ Client (View remote screen)           │
│                                          │
│  [Host IP Address input — Client only]   │
│                                          │
│  [  Launch Gupt  ]     [  Cancel  ]      │
└──────────────────────────────────────────┘
```

### Host Machine (the PC being shared)

1. Run `Gupt.exe`
2. Select **"Host (Share my screen)"**
3. Click **Launch Gupt**
4. Find the machine's IP address:
   ```powershell
   ipconfig
   # Look for: IPv4 Address . . . . : 192.168.x.x
   ```
5. Share that IP with the person who will connect.
6. When a client connects, a consent dialog will appear — click **Yes** to allow the session.
7. A **Windows Firewall prompt** may appear on first run — click **Allow Access**.

### Client Machine (the PC doing the viewing/controlling)

1. Run `Gupt.exe`
2. Select **"Client (View remote screen)"**
3. Enter the **Host's IP address** in the input field (e.g. `192.168.1.15`)
4. Click **Launch Gupt**
5. Once the host accepts, their desktop streams into a fullscreen window.

#### Client Controls

| Key / Action | Function |
|---|---|
| Click the `<` tab on the right edge | Open/close sidebar |
| **Disconnect** (sidebar card) | End session and close |
| **Full-screen / Exit Full-screen** (sidebar card) | Toggle fullscreen |
| `F11` | Toggle fullscreen |
| Mouse move / click / scroll inside the frame | Injected on host machine |
| Keyboard input | Injected on host machine |

---

## How to Test on a Single Computer (Localhost)

You can run both host and client on the same machine for testing:

1. Run `Gupt.exe` → select **Host** → Launch
2. Run a **second instance** of `Gupt.exe` → select **Client** → enter `127.0.0.1` → Launch
3. Accept the consent dialog on the Host.

> ⚠️ On a single monitor this creates an "infinite mirror" effect. Drag the client window to the side to see your real desktop behind it. Mouse clicks inside the client feed will land on the real desktop.

---

## Distributing the Application

To share the app with another machine:

- Copy **only `Gupt.exe`** — nothing else is needed.
- No installer, no runtime setup, no Visual C++ Redistributable.
- Works on: Windows 10, Windows 11 (x64).

---

## Security

- The **Host always shows a consent dialog** before any remote session begins. A session cannot be silently started.
- An active session indicator remains visible while a remote peer is connected.
- All input is injected only after explicit user approval.

---

## Legacy Build Targets (Dev Only)

If you need to build the old split binaries separately:

```powershell
# Host only
cmake --build build --target HostAgent --config Release

# Client only
cmake --build build --target ClientViewer --config Release
```

> The `ClientViewer` legacy binary has the host IP hardcoded inside `ClientViewer/main.cpp` (line ~504). Edit `g_Client.Connect(...)` before building if you need a specific IP.
