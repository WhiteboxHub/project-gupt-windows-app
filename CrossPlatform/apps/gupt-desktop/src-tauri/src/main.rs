#![allow(unused_imports, dead_code)]

use base64::{engine::general_purpose, Engine as _};
use enigo::{Axis, Button, Coordinate, Direction, Enigo, Key, Keyboard, Mouse, Settings};
use gupt_core::platform;
use lazy_static::lazy_static;
use serde::Serialize;
use std::{
    fs,
    path::PathBuf,
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

const MOUSE_BUTTON_MOVE_ONLY: u8 = 255;

lazy_static! {
    static ref INPUT: Mutex<Option<Enigo>> = Mutex::new(None);
}

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

#[tauri::command]
fn inject_mouse(
    window: tauri::Window,
    x: f32,
    y: f32,
    button: u8,
    is_down: bool,
    wheel_delta: i32,
) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    {
        use core_graphics::display::CGDisplay;
        use core_graphics::event::{
            CGEvent, CGEventTapLocation, CGEventType, CGMouseButton, EventField,
        };
        use core_graphics::event_source::{CGEventSource, CGEventSourceStateID};
        use core_graphics::geometry::CGPoint;

        let bounds = CGDisplay::main().bounds();
        let target_x = (x as f64 * bounds.size.width + bounds.origin.x)
            .clamp(bounds.origin.x, bounds.origin.x + bounds.size.width - 1.0);
        let target_y = (y as f64 * bounds.size.height + bounds.origin.y)
            .clamp(bounds.origin.y, bounds.origin.y + bounds.size.height - 1.0);
        let pt = CGPoint::new(target_x, target_y);

        if let Ok(source) = CGEventSource::new(CGEventSourceStateID::HIDSystemState) {
            if wheel_delta != 0 {
                let scroll_lines = (wheel_delta / 120).clamp(-10, 10);
                let scroll_lines = if scroll_lines == 0 {
                    wheel_delta.signum()
                } else {
                    scroll_lines
                };
                if let Ok(ev) = CGEvent::new_scroll_event(source.clone(), 1, 1, scroll_lines, 0, 0) {
                    ev.post(CGEventTapLocation::HID);
                }
            }

            if button == MOUSE_BUTTON_MOVE_ONLY {
                let move_type = if is_down {
                    CGEventType::LeftMouseDragged
                } else {
                    CGEventType::MouseMoved
                };
                if let Ok(ev) = CGEvent::new_mouse_event(source.clone(), move_type, pt, CGMouseButton::Left) {
                    ev.post(CGEventTapLocation::HID);
                }
            } else {
                let (ev_type, btn) = match button {
                    0 => (
                        if is_down { CGEventType::LeftMouseDown } else { CGEventType::LeftMouseUp },
                        CGMouseButton::Left,
                    ),
                    1 => (
                        if is_down { CGEventType::RightMouseDown } else { CGEventType::RightMouseUp },
                        CGMouseButton::Right,
                    ),
                    2 => (
                        if is_down { CGEventType::OtherMouseDown } else { CGEventType::OtherMouseUp },
                        CGMouseButton::Center,
                    ),
                    _ => (
                        if is_down { CGEventType::LeftMouseDown } else { CGEventType::LeftMouseUp },
                        CGMouseButton::Left,
                    ),
                };

                if let Ok(click_ev) = CGEvent::new_mouse_event(source.clone(), ev_type, pt, btn) {
                    if button == 2 {
                        click_ev.set_integer_value_field(EventField::MOUSE_EVENT_BUTTON_NUMBER, 2);
                    }
                    if is_down {
                        click_ev.set_integer_value_field(EventField::MOUSE_EVENT_CLICK_STATE, 1);
                    }
                    click_ev.post(CGEventTapLocation::HID);
                }
            }
        }
        let _ = window;
        return Ok(());
    }

    #[cfg(not(target_os = "macos"))]
    {
        let mut input_guard = input_controller()?;
        let input = input_guard
            .as_mut()
            .ok_or_else(|| "input controller was not initialized".to_string())?;

        let (width, height) = display_size_for_input(&window, input)?;
        let target_x = normalized_to_pixel(x, width);
        let target_y = normalized_to_pixel(y, height);

        input
            .move_mouse(target_x, target_y, Coordinate::Abs)
            .map_err(|err| err.to_string())?;

        if wheel_delta != 0 {
            let scroll_lines = (wheel_delta / 120).clamp(-10, 10);
            let scroll_lines = if scroll_lines == 0 {
                wheel_delta.signum()
            } else {
                scroll_lines
            };
            input
                .scroll(-scroll_lines, Axis::Vertical)
                .map_err(|err| err.to_string())?;
        }

        if button != MOUSE_BUTTON_MOVE_ONLY {
            input
                .button(mouse_button(button), key_direction(is_down))
                .map_err(|err| err.to_string())?;
        }

        Ok(())
    }
}

#[tauri::command]
fn inject_keyboard(keycode: u16, is_down: bool) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    {
        use core_graphics::event::{CGEvent, CGEventTapLocation, CGKeyCode};
        use core_graphics::event_source::{CGEventSource, CGEventSourceStateID};

        if let Some(mac_code) = windows_virtual_key_to_mac_key(keycode) {
            if let Ok(source) = CGEventSource::new(CGEventSourceStateID::HIDSystemState) {
                if let Ok(ev) = CGEvent::new_keyboard_event(source, mac_code as CGKeyCode, is_down) {
                    ev.post(CGEventTapLocation::HID);
                    return Ok(());
                }
            }
        }
        Ok(())
    }

    #[cfg(not(target_os = "macos"))]
    {
        let mut input_guard = input_controller()?;
        let input = input_guard
            .as_mut()
            .ok_or_else(|| "input controller was not initialized".to_string())?;
        let key = windows_virtual_key_to_enigo_key(keycode)
            .ok_or_else(|| format!("unsupported keyboard virtual key: {keycode}"))?;

        input
            .key(key, key_direction(is_down))
            .map_err(|err| err.to_string())
    }
}

#[tauri::command]
fn save_error_report(
    role: String,
    context: String,
    message: String,
    screenshot_data_url: Option<String>,
) -> Result<String, String> {
    let report_dir = gupt_report_dir()?;
    fs::create_dir_all(&report_dir).map_err(|err| err.to_string())?;

    let timestamp = unix_millis();
    let safe_context = sanitize_filename(&context);
    let base_name = format!("{timestamp}-{}-{safe_context}", sanitize_filename(&role));
    let log_path = report_dir.join(format!("{base_name}.log"));
    let screenshot_path = report_dir.join(format!("{base_name}.png"));

    let mut body = String::new();
    body.push_str("Gupt error report\n");
    body.push_str(&format!("timestamp_ms: {timestamp}\n"));
    body.push_str(&format!("role: {role}\n"));
    body.push_str(&format!("context: {context}\n"));
    body.push_str("message:\n");
    body.push_str(&message);
    body.push('\n');

    if let Some(data_url) = screenshot_data_url {
        if let Some(png) = decode_png_data_url(&data_url)? {
            fs::write(&screenshot_path, png).map_err(|err| err.to_string())?;
            body.push_str(&format!("screenshot: {}\n", screenshot_path.display()));
        }
    }

    fs::write(&log_path, body).map_err(|err| err.to_string())?;
    Ok(log_path.display().to_string())
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            platform_capabilities,
            inject_input_preview,
            inject_mouse,
            inject_keyboard,
            save_error_report
        ])
        .run(tauri::generate_context!())
        .expect("failed to run Gupt desktop app");
}

fn input_controller() -> Result<std::sync::MutexGuard<'static, Option<Enigo>>, String> {
    let mut input = INPUT
        .lock()
        .map_err(|_| "input controller lock was poisoned".to_string())?;

    if input.is_none() {
        *input = Some(
            Enigo::new(&Settings::default())
                .map_err(|err| format!("input injection is unavailable: {err}"))?,
        );
    }

    Ok(input)
}

fn display_size_for_input(_window: &tauri::Window, _input: &Enigo) -> Result<(i32, i32), String> {
    #[cfg(target_os = "macos")]
    {
        use core_graphics::display::CGDisplay;
        let bounds = CGDisplay::main().bounds();
        return Ok((bounds.size.width as i32, bounds.size.height as i32));
    }

    #[cfg(not(target_os = "macos"))]
    {
        if let Some(monitor) = _window.current_monitor().map_err(|err| err.to_string())? {
            let size = monitor.size();
            let scale = monitor.scale_factor().max(1.0);
            return Ok((
                ((size.width as f64) / scale).round() as i32,
                ((size.height as f64) / scale).round() as i32,
            ));
        }
        _input.main_display().map_err(|err| err.to_string())
    }
}

fn normalized_to_pixel(value: f32, size: i32) -> i32 {
    let max = size.saturating_sub(1).max(0) as f32;
    (value.clamp(0.0, 1.0) * max).round() as i32
}

fn gupt_report_dir() -> Result<PathBuf, String> {
    #[cfg(target_os = "windows")]
    {
        if let Some(appdata) = std::env::var_os("APPDATA") {
            return Ok(PathBuf::from(appdata).join("Gupt").join("reports"));
        }
    }

    if let Some(home) = std::env::var_os("HOME") {
        return Ok(PathBuf::from(home)
            .join("Library")
            .join("Logs")
            .join("Gupt")
            .join("reports"));
    }

    std::env::current_dir()
        .map(|path| path.join("gupt-reports"))
        .map_err(|err| err.to_string())
}

fn unix_millis() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis())
        .unwrap_or_default()
}

fn sanitize_filename(value: &str) -> String {
    let sanitized: String = value
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
                ch
            } else {
                '-'
            }
        })
        .collect();

    sanitized.trim_matches('-').chars().take(48).collect()
}

fn decode_png_data_url(data_url: &str) -> Result<Option<Vec<u8>>, String> {
    let Some(payload) = data_url.strip_prefix("data:image/png;base64,") else {
        return Ok(None);
    };
    general_purpose::STANDARD
        .decode(payload)
        .map(Some)
        .map_err(|err| err.to_string())
}

fn key_direction(is_down: bool) -> Direction {
    if is_down {
        Direction::Press
    } else {
        Direction::Release
    }
}

fn mouse_button(button: u8) -> Button {
    match button {
        1 => Button::Right,
        2 => Button::Middle,
        _ => Button::Left,
    }
}

fn windows_virtual_key_to_mac_key(keycode: u16) -> Option<u16> {
    match keycode {
        // Letters (VK_A 0x41 ..= VK_Z 0x5A)
        0x41 => Some(0x00), // A
        0x42 => Some(0x0B), // B
        0x43 => Some(0x08), // C
        0x44 => Some(0x02), // D
        0x45 => Some(0x0E), // E
        0x46 => Some(0x03), // F
        0x47 => Some(0x05), // G
        0x48 => Some(0x04), // H
        0x49 => Some(0x22), // I
        0x4A => Some(0x26), // J
        0x4B => Some(0x28), // K
        0x4C => Some(0x25), // L
        0x4D => Some(0x2E), // M
        0x4E => Some(0x2D), // N
        0x4F => Some(0x1F), // O
        0x50 => Some(0x23), // P
        0x51 => Some(0x0C), // Q
        0x52 => Some(0x0F), // R
        0x53 => Some(0x01), // S
        0x54 => Some(0x11), // T
        0x55 => Some(0x20), // U
        0x56 => Some(0x09), // V
        0x57 => Some(0x0D), // W
        0x58 => Some(0x07), // X
        0x59 => Some(0x10), // Y
        0x5A => Some(0x06), // Z

        // Digits (0x30 ..= 0x39)
        0x30 => Some(0x1D), // 0
        0x31 => Some(0x12), // 1
        0x32 => Some(0x13), // 2
        0x33 => Some(0x14), // 3
        0x34 => Some(0x15), // 4
        0x35 => Some(0x17), // 5
        0x36 => Some(0x16), // 6
        0x37 => Some(0x1A), // 7
        0x38 => Some(0x1C), // 8
        0x39 => Some(0x19), // 9

        // Navigation & Control
        0x08 => Some(0x33), // Backspace / Delete
        0x09 => Some(0x30), // Tab
        0x0D => Some(0x24), // Return / Enter
        0x10 => Some(0x38), // Shift
        0x11 => Some(0x3B), // Control
        0x12 => Some(0x3A), // Option / Alt
        0x14 => Some(0x39), // CapsLock
        0x1B => Some(0x35), // Escape
        0x20 => Some(0x31), // Space
        0x21 => Some(0x74), // PageUp
        0x22 => Some(0x79), // PageDown
        0x23 => Some(0x77), // End
        0x24 => Some(0x73), // Home
        0x25 => Some(0x7B), // Left Arrow
        0x26 => Some(0x7E), // Up Arrow
        0x27 => Some(0x7C), // Right Arrow
        0x28 => Some(0x7D), // Down Arrow
        0x2E => Some(0x75), // Forward Delete
        0x5B | 0x5C | 0xE0 | 0xE1 => Some(0x37), // Command / Meta (Win)

        // Punctuation & Symbols
        0xBA => Some(0x29), // ;
        0xBB => Some(0x18), // =
        0xBC => Some(0x2B), // ,
        0xBD => Some(0x1B), // -
        0xBE => Some(0x2F), // .
        0xBF => Some(0x2C), // /
        0xC0 => Some(0x32), // `
        0xDB => Some(0x21), // [
        0xDC => Some(0x2A), // \
        0xDD => Some(0x1E), // ]
        0xDE => Some(0x27), // '

        // Function keys F1 - F12
        0x70 => Some(0x7A), // F1
        0x71 => Some(0x78), // F2
        0x72 => Some(0x63), // F3
        0x73 => Some(0x76), // F4
        0x74 => Some(0x60), // F5
        0x75 => Some(0x61), // F6
        0x76 => Some(0x62), // F7
        0x77 => Some(0x64), // F8
        0x78 => Some(0x65), // F9
        0x79 => Some(0x6D), // F10
        0x7A => Some(0x67), // F11
        0x7B => Some(0x6F), // F12

        // Numpad (0x60 ..= 0x69)
        0x60 => Some(0x52), // Num 0
        0x61 => Some(0x53), // Num 1
        0x62 => Some(0x54), // Num 2
        0x63 => Some(0x55), // Num 3
        0x64 => Some(0x56), // Num 4
        0x65 => Some(0x57), // Num 5
        0x66 => Some(0x58), // Num 6
        0x67 => Some(0x59), // Num 7
        0x68 => Some(0x5B), // Num 8
        0x69 => Some(0x5C), // Num 9
        0x6A => Some(0x43), // Num *
        0x6B => Some(0x45), // Num +
        0x6D => Some(0x4E), // Num -
        0x6E => Some(0x41), // Num .
        0x6F => Some(0x4B), // Num /

        _ => None,
    }
}

fn windows_virtual_key_to_enigo_key(keycode: u16) -> Option<Key> {
    match keycode {
        0x08 => Some(Key::Backspace),
        0x09 => Some(Key::Tab),
        0x0D => Some(Key::Return),
        0x10 => Some(Key::Shift),
        0x11 => Some(Key::Control),
        0x12 => Some(Key::Option),
        0x14 => Some(Key::CapsLock),
        0x1B => Some(Key::Escape),
        0x20 => Some(Key::Space),
        0x21 => Some(Key::PageUp),
        0x22 => Some(Key::PageDown),
        0x23 => Some(Key::End),
        0x24 => Some(Key::Home),
        0x25 => Some(Key::LeftArrow),
        0x26 => Some(Key::UpArrow),
        0x27 => Some(Key::RightArrow),
        0x28 => Some(Key::DownArrow),
        0x2E => Some(Key::Delete),
        0x30..=0x39 => char::from_u32(keycode as u32).map(Key::Unicode),
        0x41..=0x5A => char::from_u32((keycode + 32) as u32).map(Key::Unicode),
        0x5B | 0x5C | 0xE0 | 0xE1 => Some(Key::Meta),
        0x60..=0x69 => Some(numpad_key(keycode - 0x60)),
        0x6A => Some(Key::Multiply),
        0x6B => Some(Key::Add),
        0x6D => Some(Key::Subtract),
        0x6E => Some(Key::Decimal),
        0x6F => Some(Key::Divide),
        0x70..=0x83 => function_key(keycode - 0x70 + 1),
        0xBA => Some(Key::Unicode(';')),
        0xBB => Some(Key::Unicode('=')),
        0xBC => Some(Key::Unicode(',')),
        0xBD => Some(Key::Unicode('-')),
        0xBE => Some(Key::Unicode('.')),
        0xBF => Some(Key::Unicode('/')),
        0xC0 => Some(Key::Unicode('`')),
        0xDB => Some(Key::Unicode('[')),
        0xDC => Some(Key::Unicode('\\')),
        0xDD => Some(Key::Unicode(']')),
        0xDE => Some(Key::Unicode('\'')),
        _ => None,
    }
}

fn numpad_key(index: u16) -> Key {
    match index {
        0 => Key::Numpad0,
        1 => Key::Numpad1,
        2 => Key::Numpad2,
        3 => Key::Numpad3,
        4 => Key::Numpad4,
        5 => Key::Numpad5,
        6 => Key::Numpad6,
        7 => Key::Numpad7,
        8 => Key::Numpad8,
        _ => Key::Numpad9,
    }
}

fn function_key(index: u16) -> Option<Key> {
    match index {
        1 => Some(Key::F1),
        2 => Some(Key::F2),
        3 => Some(Key::F3),
        4 => Some(Key::F4),
        5 => Some(Key::F5),
        6 => Some(Key::F6),
        7 => Some(Key::F7),
        8 => Some(Key::F8),
        9 => Some(Key::F9),
        10 => Some(Key::F10),
        11 => Some(Key::F11),
        12 => Some(Key::F12),
        13 => Some(Key::F13),
        14 => Some(Key::F14),
        15 => Some(Key::F15),
        16 => Some(Key::F16),
        17 => Some(Key::F17),
        18 => Some(Key::F18),
        19 => Some(Key::F19),
        20 => Some(Key::F20),
        _ => None,
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cg_keyboard() {
        use core_graphics::event::{CGEvent, CGKeyCode};
        use core_graphics::event_source::{CGEventSource, CGEventSourceStateID};

        let source = CGEventSource::new(CGEventSourceStateID::HIDSystemState).unwrap();
        let key_ev = CGEvent::new_keyboard_event(source, 0x00 as CGKeyCode, true);
        println!("CGEvent keyboard event created: {:?}", key_ev.is_ok());
        assert!(key_ev.is_ok());
    }

    #[test]
    fn test_cg_mouse_fields() {
        use core_graphics::event::{CGEvent, CGEventType, CGMouseButton, EventField};
        use core_graphics::event_source::{CGEventSource, CGEventSourceStateID};
        use core_graphics::geometry::CGPoint;

        let source = CGEventSource::new(CGEventSourceStateID::HIDSystemState).unwrap();
        let pt = CGPoint::new(500.0, 500.0);
        let left_ev = CGEvent::new_mouse_event(source.clone(), CGEventType::LeftMouseDown, pt, CGMouseButton::Left).unwrap();
        println!("LeftMouseDown default button number: {}", left_ev.get_integer_value_field(EventField::MOUSE_EVENT_BUTTON_NUMBER));
        println!("LeftMouseDown default click state: {}", left_ev.get_integer_value_field(EventField::MOUSE_EVENT_CLICK_STATE));

        let right_ev = CGEvent::new_mouse_event(source.clone(), CGEventType::RightMouseDown, pt, CGMouseButton::Right).unwrap();
        println!("RightMouseDown default button number: {}", right_ev.get_integer_value_field(EventField::MOUSE_EVENT_BUTTON_NUMBER));
    }
}

