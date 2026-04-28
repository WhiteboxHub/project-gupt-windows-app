#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    Bgra8,
    Rgba8,
    Nv12,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DirtyRegion {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

impl DirtyRegion {
    pub fn full(width: u32, height: u32) -> Self {
        Self {
            x: 0,
            y: 0,
            width,
            height,
        }
    }

    pub fn is_empty(self) -> bool {
        self.width == 0 || self.height == 0
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DesktopFrame {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    pub dirty: DirtyRegion,
    pub bytes: Vec<u8>,
    pub timestamp_ms: u64,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum InputEvent {
    MouseMove {
        normalized_x: f32,
        normalized_y: f32,
    },
    MouseButton {
        button: MouseButton,
        down: bool,
    },
    MouseWheel {
        delta: i32,
    },
    Key {
        virtual_key: u32,
        down: bool,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButton {
    Left,
    Right,
    Middle,
    Other(u8),
}
