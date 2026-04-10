# Gupt Remote Desktop (Windows App)

Gupt Remote Desktop is a professional-grade, high-fidelity, and industrial-stable remote desktop utility for Windows. Built for performance and resilience, it features an "Eternal Sovereign" architecture that ensures session persistence over both LAN and the Internet.

The entire application is self-contained, high-performance, and portable — **no third-party libraries, no runtimes, no bloat**.

---

## 🚀 Key Features

*   **Eternal Sovereign Architecture**: Implements a self-healing "Infinite Relink" loop. If an Internet connection drops, the client automatically restores the session without closing the window.
*   **Studio 95 Clarity**: High-fidelity video streaming using 95% Quality JPEG encoding with forced **4:4:4 color subsampling** for razor-sharp text and zero color smearing.
*   **Pixel-Perfect Scaling**: Uses **Halftone pixel-blending** in the viewer, ensuring that text remains crisp and readable even when the remote window is resized.
*   **Bidirectional Clipboard Sync**: Seamlessly share text between the host and client machines in real-time.
*   **Pro-Gamer Responsiveness**: Features a **250Hz (4ms) Hardware-Level Mouse Throttle** for smooth, low-latency control while reducing network congestion.
*   **Industrial Stability**: Grid-locked 16x16 MCU-aligned padding and contiguous 24bpp pixel extraction eliminate "RGB lines" and vertical artifacts on all screen resolutions.

---

## 📂 Architecture

```
project-gupt-windows-app/
├── Launcher/         ← Elite Unified GUI (Eternal Re-relink Logic + UI)
├── Core/
│   ├── Capture/      ← Studio 95 / 4:4:4 Screen Engine
│   ├── Input/        ← High-Precision Input Injector
│   └── Network/      ← Sovereign Network Layer (LAN & Relay)
├── Shared/           ← Protocol & Session Definitions
├── SignalingServer/  ← Node.js Global Relay (Internet Traversal)
├── logo.ico          ← Application Branding
└── resources.rc      ← Performance Manifests & Resources
```

### 📦 Distribution Components

| Target | Description |
|---|---|
| **`Gupt.exe`** | ✅ **Recommended.** Single portable executable. Statically linked — runs on any Windows PC (Win 10/11) with zero dependencies. |
| `SignalingServer` | The global relay required for connecting machines across the Internet. |

---

## 🛠️ Prerequisites

- **Windows 10/11** (Uses native GDI, SendInput, WIC, and WinHTTP APIs)
- **CMake** 3.20 or newer
- **MSVC** (Visual Studio 2022 recommended)

> **No external libraries required.** The project is 100% native Windows SDK.

---

## 🏗️ How to Build

Run from the root directory:

```powershell
# 1. Configure
cmake -S . -B build

# 2. Build the high-performance binary
cmake --build build --target Gupt --config Release
```

Output: `build/Release/Gupt.exe` (Self-contained, ~250 KB)

---

## 🎮 How to Run

### Internet Mode (Over the Web)
1.  **Host**: Launch `Gupt.exe`, select **Host**, and click Launch. Copy your **Session ID**.
2.  **Client**: Launch `Gupt.exe`, select **Client**, enter the **Session ID**, and click Launch.
3.  **Persistence**: The session will stay active indefinitely. If your ISP blips, Gupt will auto-restore the connection.

### LAN Mode (Extreme Latency)
Gupt automatically detects if the machines are on the same network. If so, it bypasses the internet relay for **near-zero latency** performance.

---

## ⌨️ Client Controls

| Key / Action | Function |
|---|---|
| `<` Tab (Right edge) | Open sidebar for Control Dashboard |
| **Disconnect** | Cleanly terminate the session |
| **Fullscreen** | Toggle pixel-perfect fullscreen (`F11`) |
| **Clipboard Sync** | Toggle real-time clipboard sharing |
| **Alt+Tab / Win Keys** | Passed to remote host in Fullscreen mode |

---

## 🛡️ Security
*   **Explicit Consent**: The Host must manually approve every connection request via a secure popup.
*   **Privacy-First**: No background data tracking. All screen data travels directly via your signaling server/relay.
*   **Static Manifest**: Built using hardened Win32 primitives to prevent buffer overflows and memory leaks.

---

## ✨ Design Aesthetics
The application features a modern, branded Win32 UI with:
- Custom **Glassmorphism-inspired** Sidebar
- **Smooth Animations** for UI transitions
- **High-contrast, eye-friendly** dark mode palette
