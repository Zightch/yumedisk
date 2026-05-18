use backend_rust::BackendContext;
use backend_rust::DiskConfig;
use backend_rust::SessionConfig;
use backend_rust::YUMEDISK_MAX_TARGETS;
use backend_rust::YUMEDISK_MAX_USABLE_TARGET_ID;
use backend_rust::YUMEDISK_MIN_TARGET_ID;

#[derive(Debug, Clone)]
pub struct BackendDefaultsSnapshot {
    pub session_config: SessionConfig,
    pub disk_config_template: DiskConfig,
    pub target_id_auto: u32,
    pub target_id_min: u32,
    pub target_id_max: u32,
}

pub fn query_backend_defaults() -> BackendDefaultsSnapshot {
    BackendDefaultsSnapshot {
        session_config: SessionConfig::default(),
        disk_config_template: DiskConfig::default(),
        target_id_auto: YUMEDISK_MAX_TARGETS,
        target_id_min: YUMEDISK_MIN_TARGET_ID,
        target_id_max: YUMEDISK_MAX_USABLE_TARGET_ID,
    }
}

pub fn query_app_session_config(backend: &BackendContext) -> SessionConfig {
    backend.session_config()
}
