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

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CacheError {
    NotImplemented,
}

pub trait AtIo: Send + Sync {
    fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError>;

    fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError>;
}

#[derive(Debug)]
pub struct Cache<R> {
    config: CacheConfig,
    right: R,
}

impl<R: AtIo> Cache<R> {
    pub fn new(config: CacheConfig, right: R) -> Self {
        Self { config, right }
    }

    pub fn config(&self) -> &CacheConfig {
        &self.config
    }

    pub fn right(&self) -> &R {
        &self.right
    }

    pub fn read_locked(&self, _offset: u64, _buffer: &mut [u8]) -> Result<(), CacheError> {
        Err(CacheError::NotImplemented)
    }

    pub fn write_locked(&self, _offset: u64, _data: &[u8]) -> Result<(), CacheError> {
        Err(CacheError::NotImplemented)
    }
}
