use gupt_core::platform;
use serde::Serialize;

#[tauri::command]
fn platform_capabilities() -> PlatformCapabilitiesDto {
    platform::current_platform_capabilities().into()
}

#[tauri::command]
fn inject_input_preview(_event_json: String) -> Result<(), String> {
    // The frontend can run in a normal browser for viewer testing.
    // Packaged host builds will replace this preview command with platform-native
    // input injection calls.
    Ok(())
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            platform_capabilities,
            inject_input_preview
        ])
        .run(tauri::generate_context!())
        .expect("failed to run Gupt desktop app");
}

#[derive(Serialize)]
struct PlatformCapabilitiesDto {
    os_name: &'static str,
    can_capture_screen: bool,
    can_inject_input: bool,
    requires_screen_permission: bool,
    requires_accessibility_permission: bool,
    supports_hardware_video: bool,
}

impl From<platform::PlatformCapabilities> for PlatformCapabilitiesDto {
    fn from(value: platform::PlatformCapabilities) -> Self {
        Self {
            os_name: value.os_name,
            can_capture_screen: value.can_capture_screen,
            can_inject_input: value.can_inject_input,
            requires_screen_permission: value.requires_screen_permission,
            requires_accessibility_permission: value.requires_accessibility_permission,
            supports_hardware_video: value.supports_hardware_video,
        }
    }
}
