use crate::types::{DesktopFrame, InputEvent};

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct SessionId(String);

impl SessionId {
    pub fn new(value: impl Into<String>) -> Self {
        Self(value.into())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    pub fn is_valid_short_code(&self) -> bool {
        let len = self.0.len();
        (6..=32).contains(&len)
            && self
                .0
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FramePacket {
    pub frame: DesktopFrame,
    pub sequence: u64,
    pub keyframe: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub enum ControlEvent {
    Input(InputEvent),
    ClipboardText(String),
    Disconnect,
    Heartbeat,
}

#[cfg(test)]
mod tests {
    use super::SessionId;

    #[test]
    fn validates_short_codes() {
        assert!(SessionId::new("ABC123").is_valid_short_code());
        assert!(SessionId::new("host_123-xyz").is_valid_short_code());
        assert!(!SessionId::new("abc").is_valid_short_code());
        assert!(!SessionId::new("has space").is_valid_short_code());
    }
}
