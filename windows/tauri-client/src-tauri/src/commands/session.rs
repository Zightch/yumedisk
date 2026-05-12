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

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InitializeClientResponse {
    pub session: SessionSnapshotDto,
}

#[tauri::command]
pub fn initialize_client(
    state: State<'_, ClientState>,
) -> Result<InitializeClientResponse, ApiError> {
    let session = session_service::initialize_client(&state)?;

    Ok(InitializeClientResponse {
        session: SessionSnapshotDto {
            ready: session.ready,
            state_text: session.state_text,
        },
    })
}
