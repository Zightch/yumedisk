use crate::api_error::ApiError;
use crate::backend::persistence_service;
use crate::state::client_state::ClientState;

#[derive(Debug, Clone)]
pub struct SessionSnapshot {
    pub ready: bool,
    pub state_text: String,
}

pub fn restore_client_state(state: &ClientState) -> Result<(), ApiError> {
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

    {
        let mut disk_catalog = state
            .disk_catalog
            .lock()
            .expect("disk catalog mutex should not be poisoned");
        disk_catalog.replace_runtime_store(restored_state.disk_runtime_store);
    }

    Ok(())
}

pub fn open_session(state: &ClientState) -> Result<SessionSnapshot, ApiError> {
    if !state.backend.open() {
        return Err(ApiError::new(
            "backend-session-open-failed",
            "打开 BackendRust session 失败",
            Some(state.backend.query_session_state_text()),
        ));
    }

    Ok(SessionSnapshot {
        ready: true,
        state_text: state.backend.query_session_state_text(),
    })
}
