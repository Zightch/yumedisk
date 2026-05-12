use backend_rust::BackendContext;
use backend_rust::SessionConfig;

use crate::api_error::ApiError;

#[derive(Debug, Clone)]
pub struct SessionSnapshot {
    pub ready: bool,
    pub state_text: String,
}

pub fn initialize_client(backend: &BackendContext) -> Result<SessionSnapshot, ApiError> {
    backend
        .set_session_config(SessionConfig::default())
        .map_err(|error| {
            ApiError::new(
                "invalid-session-config",
                "Session 配置无效",
                Some(error.as_code().to_string()),
            )
        })?;

    if !backend.open() {
        return Err(ApiError::new(
            "backend-session-open-failed",
            "打开 BackendRust session 失败",
            Some(backend.query_session_state_text()),
        ));
    }

    Ok(SessionSnapshot {
        ready: true,
        state_text: backend.query_session_state_text(),
    })
}
