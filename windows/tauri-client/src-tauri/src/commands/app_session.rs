use serde::Serialize;
use tauri::State;

use crate::api_error::ApiError;
use crate::backend::app_session_service;
use crate::state::client_state::ClientState;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSessionSnapshotDto {
    pub ready: bool,
    pub state_text: String,
}

#[tauri::command]
pub fn restore_client_state(state: State<'_, ClientState>) -> Result<(), ApiError> {
    app_session_service::restore_client_state(&state)
}

#[tauri::command]
pub fn open_app_session(state: State<'_, ClientState>) -> Result<AppSessionSnapshotDto, ApiError> {
    let session = app_session_service::open_app_session(&state)?;

    Ok(AppSessionSnapshotDto {
        ready: session.ready,
        state_text: session.state_text,
    })
}
