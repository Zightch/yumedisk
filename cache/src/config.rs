use std::path::PathBuf;
use std::time::Duration;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CacheConfig {
    pub fifo_capacity_blocks: usize,
    pub lru_capacity_blocks: usize,
    pub block_size_bytes: u32,
    pub dirty_scan_interval: Duration,
    pub temp_max_files: usize,
    pub temp_dir: PathBuf,
}
