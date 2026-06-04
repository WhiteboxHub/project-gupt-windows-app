# Gupt Cross-Platform Foundation

This workspace is the starting point for the next-generation Gupt architecture.
The existing Win32/C++ app remains intact; this Rust workspace is intended to
grow into the shared engine used by future Tauri desktop shells.

## Direction

- Tauri handles UI, packaging, tray/menu integration, and permissions UX.
- `gupt-core` owns session state, platform abstraction, input/capture contracts,
  frame metadata, and transport-independent protocol concepts.
- Platform backends implement host capabilities:
  - Windows: DXGI or Windows Graphics Capture plus `SendInput`.
  - macOS: ScreenCaptureKit plus Quartz/Accessibility event injection.
- Streaming should move toward hardware H.264/WebRTC for low latency, with JPEG
  retained as a fallback path.

## Current Status

This is a buildable foundation, not a complete replacement for the Windows app.
It gives us stable interfaces and capability reporting for Windows/macOS work
without breaking the existing `Gupt.exe`.

```powershell
cd CrossPlatform
cargo test
```

## Desktop Shell Scaffold

`apps/gupt-desktop` is a Tauri-style desktop shell scaffold. Its frontend speaks
the existing Gupt binary protocol over the relay WebSocket so a future macOS app
can view/control a Windows host and later act as a macOS host.

Builds that produce `.dmg` must run on macOS with Tauri installed:

```bash
cd CrossPlatform/apps/gupt-desktop
npm install
npm run tauri build
```
