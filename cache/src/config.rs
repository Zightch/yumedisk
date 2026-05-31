use std::path::PathBuf;
use std::time::Duration;

use crate::CacheError;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CacheConfig {
    pub fifo_capacity_blocks: usize,
    pub lru_capacity_blocks: usize,
    pub block_size_bytes: u32,
    pub dirty_scan_interval: Duration,
    pub temp_max_files: usize,
    pub temp_dir: PathBuf,
}

impl CacheConfig {
    pub(crate) fn validate(&self) -> Result<(), CacheError> {
        if self.block_size_bytes == 0 {
            return Err(CacheError::InvalidConfig(
                "block_size_bytes must be greater than 0",
            ));
        }

        Ok(())
    }
}
