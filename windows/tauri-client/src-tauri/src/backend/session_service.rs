use crate::api_error::ApiError;
use crate::backend::persistence_service;
use crate::state::client_state::ClientState;

#[derive(Debug, Clone)]
pub struct SessionSnapshot {
    pub ready: bool,
    pub state_text: String,
}

pub fn initialize_client(state: &ClientState) -> Result<SessionSnapshot, ApiError> {
    state.backend.close();

    let restored_state = persistence_service::load_client_state()?;

    state
        .backend
        .set_session_config(restored_state.session_config)
        .map_err(|error| {
            ApiError::new(
                "invalid-session-config",
                "Session 配置无效",
                Some(error.as_code().to_string()),
            )
        })?;

    if !state.backend.open() {
        return Err(ApiError::new(
            "backend-session-open-failed",
            "打开 BackendRust session 失败",
            Some(state.backend.query_session_state_text()),
        ));
    }

    {
        let mut disk_store = state
            .disk_store
            .lock()
            .expect("disk store mutex should not be poisoned");
        *disk_store = restored_state.disk_store;
    }

    Ok(SessionSnapshot {
        ready: true,
        state_text: state.backend.query_session_state_text(),
    })
}
