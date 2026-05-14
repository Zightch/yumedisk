use serde::Serialize;
use tauri::State;

use crate::api_error::ApiError;
use crate::backend::session_service;
use crate::state::client_state::ClientState;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionSnapshotDto {
    pub ready: bool,
    pub state_text: String,
}

#[tauri::command]
pub fn restore_client_state(state: State<'_, ClientState>) -> Result<(), ApiError> {
    session_service::restore_client_state(&state)
}

#[tauri::command]
pub fn open_session(state: State<'_, ClientState>) -> Result<SessionSnapshotDto, ApiError> {
    let session = session_service::open_session(&state)?;

    Ok(SessionSnapshotDto {
        ready: session.ready,
        state_text: session.state_text,
    })
}
