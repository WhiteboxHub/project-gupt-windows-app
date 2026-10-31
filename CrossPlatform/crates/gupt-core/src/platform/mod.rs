use crate::types::{DesktopFrame, InputEvent};

#[cfg(target_os = "macos")]
mod macos;
#[cfg(target_os = "windows")]
mod windows;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlatformCapabilities {
    pub os_name: &'static str,
    pub can_capture_screen: bool,
    pub can_inject_input: bool,
    pub requires_screen_permission: bool,
    pub requires_accessibility_permission: bool,
    pub supports_hardware_video: bool,
}

pub trait PlatformBackend: Send {
    fn capabilities(&self) -> PlatformCapabilities;
    fn start_capture(&mut self) -> Result<(), String>;
    fn next_frame(&mut self) -> Result<Option<DesktopFrame>, String>;
    fn inject_input(&mut self, event: InputEvent) -> Result<(), String>;
}

pub trait BackendFactory {
    type Backend: PlatformBackend;

    fn create_backend() -> Self::Backend;
}

pub fn current_platform_capabilities() -> PlatformCapabilities {
    create_default_backend().capabilities()
}

pub fn create_default_backend() -> Box<dyn PlatformBackend> {
    #[cfg(target_os = "windows")]
    {
        return Box::new(windows::WindowsBackend::new());
    }

    #[cfg(target_os = "macos")]
    {
        return Box::new(macos::MacOsBackend::new());
    }

    #[allow(unreachable_code)]
    Box::new(UnsupportedBackend)
}

struct UnsupportedBackend;

impl PlatformBackend for UnsupportedBackend {
    fn capabilities(&self) -> PlatformCapabilities {
        PlatformCapabilities {
            os_name: "unsupported",
            can_capture_screen: false,
            can_inject_input: false,
            requires_screen_permission: false,
            requires_accessibility_permission: false,
            supports_hardware_video: false,
        }
    }

    fn start_capture(&mut self) -> Result<(), String> {
        Err("Gupt does not support this operating system yet".to_string())
    }

    fn next_frame(&mut self) -> Result<Option<DesktopFrame>, String> {
        Ok(None)
    }

    fn inject_input(&mut self, _event: InputEvent) -> Result<(), String> {
        Err("input injection is unavailable on this operating system".to_string())
    }
}
