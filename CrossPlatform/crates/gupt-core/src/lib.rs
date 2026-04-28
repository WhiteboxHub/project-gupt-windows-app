//! Shared Gupt engine contracts for cross-platform desktop control.
//!
//! The current production Windows app is still the native C++ implementation.
//! This crate provides the portable model that a future Tauri shell can call
//! from Windows and macOS.

pub mod engine;
pub mod platform;
pub mod protocol;
pub mod types;

pub use engine::{GuptEngine, SessionState};
pub use platform::{BackendFactory, PlatformBackend, PlatformCapabilities};
pub use protocol::{ControlEvent, FramePacket, SessionId};
pub use types::{DesktopFrame, DirtyRegion, InputEvent, PixelFormat};
