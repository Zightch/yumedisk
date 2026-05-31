use std::thread;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};

use crate::api_error::ApiError;
use crate::backend::persistence_service;
use crate::network::runtime_flow;
use crate::state::client_state::ClientState;
use crate::workflow::runtime_disk;

pub const RUNTIME_DISKS_CHANGED_EVENT: &str = "network-runtime-changed";
pub const RUNTIME_RESCAN_STATE_CHANGED_EVENT: &str = "runtime-rescan-state-changed";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeRescanStateSnapshot {
    pub running: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeRescanStartResult {
    pub started: bool,
    pub running: bool,
}

#[derive(Debug, Clone, Copy, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum RuntimeRescanPhase {
    Started,
    Finished,
    Failed,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeRescanLifecycleEvent {
    pub phase: RuntimeRescanPhase,
    pub error_text: Option<String>,
}

pub fn query_runtime_rescan_state(state: &ClientState) -> RuntimeRescanStateSnapshot {
    RuntimeRescanStateSnapshot {
        running: state.is_runtime_rescan_running(),
    }
}

pub fn ensure_runtime_rescan_idle(state: &ClientState) -> Result<(), ApiError> {
    if !state.is_runtime_rescan_running() {
        return Ok(());
    }

    Err(ApiError::new(
        "runtime-rescan-running",
        "重扫进行中，暂不允许该操作",
        None,
    ))
}

pub fn start_runtime_rescan(app: AppHandle) -> Result<RuntimeRescanStartResult, ApiError> {
    let Some(state) = app.try_state::<ClientState>() else {
        return Err(ApiError::new(
            "client-state-missing",
            "客户端状态不可用",
            None,
        ));
    };

    if !state.try_begin_runtime_rescan() {
        return Ok(RuntimeRescanStartResult {
            started: false,
            running: true,
        });
    }

    emit_runtime_rescan_state(&app, RuntimeRescanPhase::Started, None);

    let worker_app = app.clone();
    thread::spawn(move || {
        let result = execute_runtime_rescan(&worker_app);
        finish_runtime_rescan(&worker_app, result);
    });

    Ok(RuntimeRescanStartResult {
        started: true,
        running: true,
    })
}

fn execute_runtime_rescan(app: &AppHandle) -> Result<(), ApiError> {
    let Some(state) = app.try_state::<ClientState>() else {
        return Err(ApiError::new(
            "client-state-missing",
            "客户端状态不可用",
            None,
        ));
    };

    let (plans, local_stage_save_result) = {
        let mut disk_catalog = state
            .disk_catalog
            .lock()
            .expect("disk catalog mutex should not be poisoned");
        runtime_disk::rescan_local_runtime_disks(
            &state.backend,
            disk_catalog.runtime_store_mut(),
            &state.network_client,
        );
        let plans = runtime_flow::collect_network_rescan_plans(disk_catalog.runtime_store());
        let save_result =
            persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store());
        (plans, save_result)
    };
    emit_runtime_disks_changed(app);
    local_stage_save_result?;

    for plan in plans {
        let resolution = runtime_flow::resolve_network_rescan_plan(&state.network_client, &plan);
        let batch_save_result = {
            let mut disk_catalog = state
                .disk_catalog
                .lock()
                .expect("disk catalog mutex should not be poisoned");
            runtime_flow::commit_network_rescan_resolution(
                &state.backend,
                disk_catalog.runtime_store_mut(),
                &state.network_client,
                resolution,
            );
            persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store())
        };
        emit_runtime_disks_changed(app);
        batch_save_result?;
    }

    Ok(())
}

fn finish_runtime_rescan(app: &AppHandle, result: Result<(), ApiError>) {
    let Some(state) = app.try_state::<ClientState>() else {
        return;
    };

    state.finish_runtime_rescan();

    match result {
        Ok(()) => emit_runtime_rescan_state(app, RuntimeRescanPhase::Finished, None),
        Err(error) => emit_runtime_rescan_state(
            app,
            RuntimeRescanPhase::Failed,
            Some(resolve_api_error_text(error)),
        ),
    }
}

fn emit_runtime_disks_changed(app: &AppHandle) {
    let _ = app.emit(RUNTIME_DISKS_CHANGED_EVENT, ());
}

fn emit_runtime_rescan_state(
    app: &AppHandle,
    phase: RuntimeRescanPhase,
    error_text: Option<String>,
) {
    let _ = app.emit(
        RUNTIME_RESCAN_STATE_CHANGED_EVENT,
        RuntimeRescanLifecycleEvent { phase, error_text },
    );
}

fn resolve_api_error_text(error: ApiError) -> String {
    error.detail.unwrap_or(error.message)
}
