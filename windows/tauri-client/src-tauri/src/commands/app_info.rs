use serde::Serialize;
use tauri::State;

use crate::backend::app_info_service;
use crate::state::client_state::ClientState;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ComponentVersionSnapshotDto {
    pub appkernel_version_text: String,
    pub kmdf_version_text: String,
    pub scsi_version_text: String,
}

#[tauri::command]
pub fn query_component_versions(state: State<'_, ClientState>) -> ComponentVersionSnapshotDto {
    let snapshot = app_info_service::query_component_versions(&state.backend);

    ComponentVersionSnapshotDto {
        appkernel_version_text: snapshot.appkernel_version_text,
        kmdf_version_text: snapshot.kmdf_version_text,
        scsi_version_text: snapshot.scsi_version_text,
    }
}
