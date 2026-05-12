use serde::Serialize;
use tauri::State;

use crate::backend::config_service;
use crate::state::client_state::ClientState;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionConfigDto {
    pub heartbeat_interval_ms: u32,
    pub initial_event_queue_capacity: u32,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiskConfigDto {
    pub target_id: u32,
    pub sector_size: u32,
    pub disk_size_bytes: u64,
    pub queue_depth: u32,
    pub write_slot_bytes: u32,
    pub read_worker_count: u16,
    pub write_worker_count: u16,
    pub ack_batch_max_ranges: u32,
    pub read_only: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BackendDefaultsDto {
    pub session_config: SessionConfigDto,
    pub disk_config_template: DiskConfigDto,
    pub target_id_auto: u32,
    pub target_id_min: u32,
    pub target_id_max: u32,
}

fn map_session_config_dto(config: backend_rust::SessionConfig) -> SessionConfigDto {
    SessionConfigDto {
        heartbeat_interval_ms: config.heartbeat_interval_ms,
        initial_event_queue_capacity: config.initial_event_queue_capacity,
    }
}

fn map_disk_config_dto(config: backend_rust::DiskConfig) -> DiskConfigDto {
    DiskConfigDto {
        target_id: config.target_id,
        sector_size: config.sector_size,
        disk_size_bytes: config.disk_size_bytes,
        queue_depth: config.queue_depth,
        write_slot_bytes: config.write_slot_bytes,
        read_worker_count: config.read_worker_count,
        write_worker_count: config.write_worker_count,
        ack_batch_max_ranges: config.ack_batch_max_ranges,
        read_only: config.read_only,
    }
}

#[tauri::command]
pub fn query_backend_defaults() -> BackendDefaultsDto {
    let snapshot = config_service::query_backend_defaults();

    BackendDefaultsDto {
        session_config: map_session_config_dto(snapshot.session_config),
        disk_config_template: map_disk_config_dto(snapshot.disk_config_template),
        target_id_auto: snapshot.target_id_auto,
        target_id_min: snapshot.target_id_min,
        target_id_max: snapshot.target_id_max,
    }
}

#[tauri::command]
pub fn query_session_config(state: State<'_, ClientState>) -> SessionConfigDto {
    let config = config_service::query_session_config(&state.backend);
    map_session_config_dto(config)
}
