use crate::appkernel;
use crate::error::BackendError;
use crate::media::Media;
use crate::types::DiskConfig;
use crate::types::SECTOR_ALIGNMENT_BYTES;
use crate::types::SessionConfig;
use crate::types::YUMEDISK_MAX_TARGETS;
use crate::types::YUMEDISK_MAX_USABLE_TARGET_ID;

pub fn validate_session_config(session_config: &SessionConfig) -> Result<(), BackendError> {
    if session_config.heartbeat_interval_ms == 0 {
        return Err(BackendError::InvalidHeartbeatIntervalMs);
    }
    if session_config.initial_response_queue_capacity == 0 {
        return Err(BackendError::InvalidInitialResponseQueueCapacity);
    }
    if session_config.initial_session_notice_queue_capacity == 0 {
        return Err(BackendError::InvalidInitialSessionNoticeQueueCapacity);
    }
    Ok(())
}

pub fn validate_disk_config(disk_config: &DiskConfig) -> Result<(), BackendError> {
    if disk_config.target_id != YUMEDISK_MAX_TARGETS
        && disk_config.target_id > YUMEDISK_MAX_USABLE_TARGET_ID
    {
        return Err(BackendError::InvalidTargetId);
    }
    if disk_config.sector_size == 0 || disk_config.sector_size % SECTOR_ALIGNMENT_BYTES != 0 {
        return Err(BackendError::InvalidSectorSize);
    }
    if disk_config.disk_size_bytes == 0
        || disk_config.disk_size_bytes % u64::from(disk_config.sector_size) != 0
    {
        return Err(BackendError::InvalidDiskSizeBytes);
    }
    if disk_config.queue_depth == 0 {
        return Err(BackendError::InvalidQueueDepth);
    }
    if disk_config.write_slot_bytes == 0 {
        return Err(BackendError::InvalidWriteSlotBytes);
    }
    if disk_config.read_worker_count == 0 {
        return Err(BackendError::InvalidReadWorkerCount);
    }
    if disk_config.write_worker_count == 0 {
        return Err(BackendError::InvalidWriteWorkerCount);
    }
    if disk_config.ack_batch_max_ranges == 0 {
        return Err(BackendError::InvalidAckBatchMaxRanges);
    }
    Ok(())
}

pub fn validate_create_disk_inputs(
    disk_config: &DiskConfig,
    media: Option<&dyn Media>,
) -> Result<(), BackendError> {
    let media = media.ok_or(BackendError::InvalidMediaInstance)?;
    validate_disk_config(disk_config)?;
    if media.size_bytes() != disk_config.disk_size_bytes {
        return Err(BackendError::MediaSizeMismatch);
    }
    Ok(())
}

pub(crate) fn build_ak_open_params(
    session_config: &SessionConfig,
    log_fn: Option<appkernel::AkLogFn>,
    log_ctx: *mut core::ffi::c_void,
) -> appkernel::AkOpenParams {
    appkernel::AkOpenParams {
        heartbeat_interval_ms: session_config.heartbeat_interval_ms,
        initial_response_queue_capacity: session_config.initial_response_queue_capacity,
        initial_session_notice_queue_capacity: session_config.initial_session_notice_queue_capacity,
        log_fn,
        log_ctx,
    }
}

pub(crate) fn build_ak_disk_params(disk_config: &DiskConfig) -> appkernel::AkDiskParams {
    appkernel::AkDiskParams {
        target_id: disk_config.target_id,
        sector_size: disk_config.sector_size,
        disk_size_bytes: disk_config.disk_size_bytes,
        queue_depth: disk_config.queue_depth,
        write_slot_bytes: disk_config.write_slot_bytes,
        read_worker_count: disk_config.read_worker_count,
        write_worker_count: disk_config.write_worker_count,
        ack_batch_max_ranges: disk_config.ack_batch_max_ranges,
        read_only: u32::from(disk_config.read_only),
    }
}

#[cfg(test)]
mod tests {
    use super::validate_disk_config;
    use crate::types::DiskConfig;

    #[test]
    fn default_disk_config_uses_new_queue_defaults() {
        let config = DiskConfig {
            disk_size_bytes: 1024 * 1024,
            ..DiskConfig::default()
        };

        assert_eq!(config.sector_size, 512);
        assert_eq!(config.queue_depth, 96);
        assert_eq!(config.write_slot_bytes, 1024 * 1024);
        assert_eq!(config.read_worker_count, 12);
        assert_eq!(config.write_worker_count, 12);
        assert_eq!(config.ack_batch_max_ranges, 96);
        assert!(validate_disk_config(&config).is_ok());
    }

    #[test]
    fn disk_config_rejects_sector_size_without_512_alignment() {
        let config = DiskConfig {
            disk_size_bytes: 3 * 1024,
            sector_size: 513,
            ..DiskConfig::default()
        };

        assert!(validate_disk_config(&config).is_err());
    }
}
