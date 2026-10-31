# Gupt Cross-Platform Implementation Plan

## Phase 1: Shared Engine

- Keep `gupt-core` dependency-light and testable.
- Model session state, frame metadata, input events, clipboard events, and
  platform capabilities.
- Add transport traits before integrating WebRTC/TURN.

## Phase 2: Tauri Shell

- Add a Tauri desktop shell that calls `gupt-core`.
- Keep setup screens, permissions, connection status, and viewer controls in UI.
- Do not put capture or input logic in the frontend.

## Phase 3: Windows Backend

- Bridge the existing DXGI capture and `SendInput` behavior into Rust.
- Prefer Windows Graphics Capture for modern Windows where it improves recovery.
- Keep the current C++ `Gupt.exe` stable until the Rust backend reaches parity.

## Phase 4: macOS Viewer

- Build receive/decode/render first, because it does not need host permissions.
- Use this to prove Windows host to macOS client before macOS host support.

## Phase 5: macOS Host

- Implement ScreenCaptureKit capture.
- Implement Quartz/Accessibility input injection.
- Add explicit Screen Recording and Accessibility permission UX.

## Phase 6: Low-Latency Streaming

- Add hardware H.264 path for video-like regions.
- Keep JPEG fallback for compatibility and simple relay mode.
- Move internet traversal toward WebRTC with STUN/TURN.
