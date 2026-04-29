use enigo::{Axis, Button, Coordinate, Direction, Enigo, Key, Keyboard, Mouse, Settings};
use gupt_core::platform;
use lazy_static::lazy_static;
use serde::Serialize;
use std::sync::Mutex;

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

#[tauri::command]
fn inject_keyboard(keycode: u16, is_down: bool) -> Result<(), String> {
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

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            platform_capabilities,
            inject_input_preview,
            inject_mouse,
            inject_keyboard
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

fn display_size_for_input(window: &tauri::Window, input: &Enigo) -> Result<(i32, i32), String> {
    #[cfg(target_os = "macos")]
    {
        use core_graphics::display::CGDisplay;
        let bounds = CGDisplay::main().bounds();
        return Ok((bounds.size.width as i32, bounds.size.height as i32));
    }

    #[cfg(not(target_os = "macos"))]
    {
        if let Some(monitor) = window.current_monitor().map_err(|err| err.to_string())? {
            let size = monitor.size();
            let scale = monitor.scale_factor().max(1.0);
            return Ok((
                ((size.width as f64) / scale).round() as i32,
                ((size.height as f64) / scale).round() as i32,
            ));
        }
        input.main_display().map_err(|err| err.to_string())
    }
}

fn normalized_to_pixel(value: f32, size: i32) -> i32 {
    let max = size.saturating_sub(1).max(0) as f32;
    (value.clamp(0.0, 1.0) * max).round() as i32
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
