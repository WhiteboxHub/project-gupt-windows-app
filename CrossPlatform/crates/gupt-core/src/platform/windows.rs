use crate::platform::{PlatformBackend, PlatformCapabilities};
use crate::types::{DesktopFrame, InputEvent};

pub struct WindowsBackend {
    capture_started: bool,
}

impl WindowsBackend {
    pub fn new() -> Self {
        Self {
            capture_started: false,
        }
    }
}

impl PlatformBackend for WindowsBackend {
    fn capabilities(&self) -> PlatformCapabilities {
        PlatformCapabilities {
            os_name: "windows",
            can_capture_screen: true,
            can_inject_input: true,
            requires_screen_permission: false,
            requires_accessibility_permission: false,
            supports_hardware_video: true,
        }
    }

    fn start_capture(&mut self) -> Result<(), String> {
        self.capture_started = true;
        Ok(())
    }

    fn next_frame(&mut self) -> Result<Option<DesktopFrame>, String> {
        if !self.capture_started {
            return Err("capture has not been started".to_string());
        }

        // The native implementation will bridge to DXGI or Windows Graphics Capture.
        Ok(None)
    }

    fn inject_input(&mut self, _event: InputEvent) -> Result<(), String> {
        // The native implementation will bridge to SendInput.
        Ok(())
    }
}
