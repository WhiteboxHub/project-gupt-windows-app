use crate::platform::PlatformBackend;
use crate::protocol::SessionId;
use crate::types::{DesktopFrame, InputEvent};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionState {
    Idle,
    Hosting,
    Viewing,
    Reconnecting,
    Stopped,
}

pub struct GuptEngine<B: PlatformBackend> {
    backend: B,
    state: SessionState,
    session_id: Option<SessionId>,
}

impl<B: PlatformBackend> GuptEngine<B> {
    pub fn new(backend: B) -> Self {
        Self {
            backend,
            state: SessionState::Idle,
            session_id: None,
        }
    }

    pub fn state(&self) -> SessionState {
        self.state
    }

    pub fn session_id(&self) -> Option<&SessionId> {
        self.session_id.as_ref()
    }

    pub fn start_hosting(&mut self, session_id: SessionId) -> Result<(), String> {
        if !session_id.is_valid_short_code() {
            return Err("invalid session id".to_string());
        }

        self.backend.start_capture()?;
        self.session_id = Some(session_id);
        self.state = SessionState::Hosting;
        Ok(())
    }

    pub fn next_host_frame(&mut self) -> Result<Option<DesktopFrame>, String> {
        match self.state {
            SessionState::Hosting => self.backend.next_frame(),
            _ => Err("engine is not hosting".to_string()),
        }
    }

    pub fn apply_remote_input(&mut self, event: InputEvent) -> Result<(), String> {
        match self.state {
            SessionState::Hosting => self.backend.inject_input(event),
            _ => Err("engine is not hosting".to_string()),
        }
    }

    pub fn stop(&mut self) {
        self.state = SessionState::Stopped;
        self.session_id = None;
    }
}

#[cfg(test)]
mod tests {
    use super::{GuptEngine, SessionState};
    use crate::platform::{PlatformBackend, PlatformCapabilities};
    use crate::protocol::SessionId;
    use crate::types::{DesktopFrame, InputEvent};

    struct FakeBackend {
        started: bool,
    }

    impl PlatformBackend for FakeBackend {
        fn capabilities(&self) -> PlatformCapabilities {
            PlatformCapabilities {
                os_name: "test",
                can_capture_screen: true,
                can_inject_input: true,
                requires_screen_permission: false,
                requires_accessibility_permission: false,
                supports_hardware_video: false,
            }
        }

        fn start_capture(&mut self) -> Result<(), String> {
            self.started = true;
            Ok(())
        }

        fn next_frame(&mut self) -> Result<Option<DesktopFrame>, String> {
            assert!(self.started);
            Ok(None)
        }

        fn inject_input(&mut self, _event: InputEvent) -> Result<(), String> {
            Ok(())
        }
    }

    #[test]
    fn starts_host_session() {
        let mut engine = GuptEngine::new(FakeBackend { started: false });
        engine.start_hosting(SessionId::new("ABC123")).unwrap();
        assert_eq!(engine.state(), SessionState::Hosting);
        assert_eq!(engine.session_id().unwrap().as_str(), "ABC123");
    }
}
