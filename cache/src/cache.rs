use crate::block::BlockLayout;
use crate::{AtIo, CacheConfig, CacheError};

#[derive(Debug)]
pub struct Cache<R> {
    config: CacheConfig,
    layout: BlockLayout,
    right: R,
}

impl<R: AtIo> Cache<R> {
    pub fn new(config: CacheConfig, right: R) -> Result<Self, CacheError> {
        config.validate()?;
        let layout = BlockLayout::new(config.block_size_bytes)?;
        Ok(Self {
            config,
            layout,
            right,
        })
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

    #[allow(dead_code)]
    pub(crate) fn layout(&self) -> &BlockLayout {
        &self.layout
    }

    #[allow(dead_code)]
    fn read_right_block(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        self.layout.validate_right_io(offset, buffer.len())?;
        self.right.read_at(offset, buffer)
    }

    #[allow(dead_code)]
    fn write_right_block(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        self.layout.validate_right_io(offset, data.len())?;
        self.right.write_at(offset, data)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;
    use std::time::Duration;

    use super::Cache;
    use crate::{AtIo, CacheConfig, CacheError};

    #[derive(Debug, PartialEq, Eq)]
    enum IoCall {
        Read { offset: u64, length: usize },
        Write { offset: u64, length: usize },
    }

    #[derive(Debug, Default)]
    struct MockAtIo {
        calls: Mutex<Vec<IoCall>>,
    }

    impl MockAtIo {
        fn take_calls(&self) -> Vec<IoCall> {
            self.calls.lock().unwrap().drain(..).collect()
        }
    }

    impl AtIo for MockAtIo {
        fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
            self.calls.lock().unwrap().push(IoCall::Read {
                offset,
                length: buffer.len(),
            });
            for (index, byte) in buffer.iter_mut().enumerate() {
                *byte = (index as u8).wrapping_add(1);
            }
            Ok(())
        }

        fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
            self.calls.lock().unwrap().push(IoCall::Write {
                offset,
                length: data.len(),
            });
            Ok(())
        }
    }

    fn test_config(block_size_bytes: u32) -> CacheConfig {
        CacheConfig {
            fifo_capacity_blocks: 2,
            lru_capacity_blocks: 2,
            block_size_bytes,
            dirty_scan_interval: Duration::from_secs(1),
            temp_max_files: 1,
            temp_dir: "tmp/cache-tests".into(),
        }
    }

    #[test]
    fn new_rejects_zero_block_size() {
        let error = Cache::new(test_config(0), MockAtIo::default()).unwrap_err();
        assert_eq!(
            error,
            CacheError::InvalidConfig("block_size_bytes must be greater than 0")
        );
    }

    #[test]
    fn read_right_block_rejects_misaligned_offset() {
        let cache = Cache::new(test_config(32), MockAtIo::default()).unwrap();
        let mut buffer = [0u8; 32];
        let error = cache.read_right_block(1, &mut buffer).unwrap_err();
        assert_eq!(
            error,
            CacheError::MisalignedRightIo {
                offset: 1,
                length: 32,
                block_size: 32,
            }
        );
    }

    #[test]
    fn read_right_block_rejects_wrong_length() {
        let cache = Cache::new(test_config(32), MockAtIo::default()).unwrap();
        let mut buffer = [0u8; 16];
        let error = cache.read_right_block(32, &mut buffer).unwrap_err();
        assert_eq!(
            error,
            CacheError::MisalignedRightIo {
                offset: 32,
                length: 16,
                block_size: 32,
            }
        );
    }

    #[test]
    fn aligned_right_io_reaches_backend_once() {
        let right = MockAtIo::default();
        let cache = Cache::new(test_config(32), right).unwrap();
        let mut read_buffer = [0u8; 32];
        let write_buffer = [7u8; 32];

        cache.read_right_block(64, &mut read_buffer).unwrap();
        cache.write_right_block(96, &write_buffer).unwrap();

        assert_eq!(read_buffer[0], 1);
        assert_eq!(read_buffer[31], 32);
        assert_eq!(
            cache.right().take_calls(),
            vec![
                IoCall::Read {
                    offset: 64,
                    length: 32
                },
                IoCall::Write {
                    offset: 96,
                    length: 32
                }
            ]
        );
    }
}
