use std::sync::{Condvar, Mutex};

use crate::block::{BlockLayout, TouchedBlock};
use crate::resident::{BeginLoadResult, LoadState, TwoQueueResident};
use crate::{AtIo, CacheConfig, CacheError};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ReadBlockAction {
    Copied,
    LoadRemote { block_index: u64, block_base: u64 },
    Wait,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WriteBlockAction {
    Patched,
    LoadRemote { block_index: u64, block_base: u64 },
    Wait,
}

#[derive(Debug)]
pub struct Cache<R> {
    config: CacheConfig,
    layout: BlockLayout,
    resident: Mutex<TwoQueueResident>,
    resident_changed: Condvar,
    right: R,
}

impl<R: AtIo> Cache<R> {
    pub fn new(config: CacheConfig, right: R) -> Result<Self, CacheError> {
        let layout = BlockLayout::new(config.block_size_bytes)?;
        let resident = Mutex::new(TwoQueueResident::new(
            config.fifo_capacity_blocks,
            config.lru_capacity_blocks,
            layout.block_size_usize()?,
        )?);
        Ok(Self {
            config,
            layout,
            resident,
            resident_changed: Condvar::new(),
            right,
        })
    }

    pub fn config(&self) -> &CacheConfig {
        &self.config
    }

    pub fn right(&self) -> &R {
        &self.right
    }

    pub fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        let touched_blocks = self.layout.touched_blocks(offset, buffer.len())?;
        let block_size = self.layout.block_size_usize()?;

        for touched in touched_blocks {
            self.read_touched_block(&touched, buffer, block_size)?;
        }

        Ok(())
    }

    pub fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        let touched_blocks = self.layout.touched_blocks(offset, data.len())?;
        let block_size = self.layout.block_size_usize()?;

        for touched in touched_blocks {
            self.write_touched_block(&touched, data, block_size)?;
        }

        Ok(())
    }

    #[allow(dead_code)]
    pub(crate) fn layout(&self) -> &BlockLayout {
        &self.layout
    }

    fn read_touched_block(
        &self,
        touched: &TouchedBlock,
        buffer: &mut [u8],
        valid_len: usize,
    ) -> Result<(), CacheError> {
        loop {
            let mut resident = self.resident.lock().unwrap();
            match self.plan_read_touched_block(&mut resident, touched, buffer, valid_len)? {
                ReadBlockAction::Copied => return Ok(()),
                ReadBlockAction::Wait => {
                    drop(self.resident_changed.wait(resident).unwrap());
                }
                ReadBlockAction::LoadRemote {
                    block_index,
                    block_base,
                } => {
                    drop(resident);
                    let mut loaded_block = vec![0u8; valid_len];
                    let read_result = self.read_right_block(block_base, &mut loaded_block);

                    let mut resident = self.resident.lock().unwrap();
                    match read_result {
                        Ok(()) => {
                            resident.finish_remote_load(block_index, loaded_block)?;
                            self.copy_ready_block(&resident, touched, buffer)?;
                            drop(resident);
                            self.resident_changed.notify_all();
                            return Ok(());
                        }
                        Err(read_error) => match resident.abort_remote_load(block_index) {
                            Ok(()) => {
                                drop(resident);
                                self.resident_changed.notify_all();
                                return Err(read_error);
                            }
                            Err(rollback_error) => {
                                drop(resident);
                                self.resident_changed.notify_all();
                                return Err(rollback_error);
                            }
                        },
                    }
                }
            }
        }
    }

    fn write_touched_block(
        &self,
        touched: &TouchedBlock,
        data: &[u8],
        valid_len: usize,
    ) -> Result<(), CacheError> {
        loop {
            let mut resident = self.resident.lock().unwrap();
            match self.plan_write_touched_block(&mut resident, touched, data, valid_len)? {
                WriteBlockAction::Patched => return Ok(()),
                WriteBlockAction::Wait => {
                    drop(self.resident_changed.wait(resident).unwrap());
                }
                WriteBlockAction::LoadRemote {
                    block_index,
                    block_base,
                } => {
                    drop(resident);
                    let mut loaded_block = vec![0u8; valid_len];
                    let read_result = self.read_right_block(block_base, &mut loaded_block);

                    let mut resident = self.resident.lock().unwrap();
                    match read_result {
                        Ok(()) => {
                            resident.finish_remote_load(block_index, loaded_block)?;
                            drop(resident);
                            self.resident_changed.notify_all();
                            return Ok(());
                        }
                        Err(read_error) => match resident.abort_remote_load(block_index) {
                            Ok(()) => {
                                drop(resident);
                                self.resident_changed.notify_all();
                                return Err(read_error);
                            }
                            Err(rollback_error) => {
                                drop(resident);
                                self.resident_changed.notify_all();
                                return Err(rollback_error);
                            }
                        },
                    }
                }
            }
        }
    }

    fn plan_read_touched_block(
        &self,
        resident: &mut TwoQueueResident,
        touched: &TouchedBlock,
        buffer: &mut [u8],
        valid_len: usize,
    ) -> Result<ReadBlockAction, CacheError> {
        match resident
            .get(touched.block_index)
            .map(|entry| entry.state.load_state)
        {
            Some(LoadState::Ready) => {
                self.copy_ready_block(resident, touched, buffer)?;
                let _ = resident.record_hit(touched.block_index)?;
                Ok(ReadBlockAction::Copied)
            }
            Some(LoadState::LoadingRemote | LoadState::Rehydrating) => Ok(ReadBlockAction::Wait),
            None => match resident.begin_remote_load(touched.block_index, valid_len)? {
                BeginLoadResult::Started => Ok(ReadBlockAction::LoadRemote {
                    block_index: touched.block_index,
                    block_base: touched.block_base,
                }),
                BeginLoadResult::Wait => Ok(ReadBlockAction::Wait),
            },
        }
    }

    fn plan_write_touched_block(
        &self,
        resident: &mut TwoQueueResident,
        touched: &TouchedBlock,
        data: &[u8],
        valid_len: usize,
    ) -> Result<WriteBlockAction, CacheError> {
        let patch_slice = self.patch_slice(touched, data)?;
        match resident
            .get(touched.block_index)
            .map(|entry| entry.state.load_state)
        {
            Some(LoadState::Ready) => {
                resident.patch_ready_block(
                    touched.block_index,
                    touched.block_range(),
                    patch_slice,
                )?;
                let _ = resident.record_hit(touched.block_index)?;
                Ok(WriteBlockAction::Patched)
            }
            Some(LoadState::LoadingRemote | LoadState::Rehydrating) => Ok(WriteBlockAction::Wait),
            None => match resident.begin_remote_load(touched.block_index, valid_len)? {
                BeginLoadResult::Started => {
                    resident.enqueue_pending_patch(
                        touched.block_index,
                        touched.block_range(),
                        patch_slice,
                    )?;
                    Ok(WriteBlockAction::LoadRemote {
                        block_index: touched.block_index,
                        block_base: touched.block_base,
                    })
                }
                BeginLoadResult::Wait => Ok(WriteBlockAction::Wait),
            },
        }
    }

    fn copy_ready_block(
        &self,
        resident: &TwoQueueResident,
        touched: &TouchedBlock,
        buffer: &mut [u8],
    ) -> Result<(), CacheError> {
        let entry = resident
            .get(touched.block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing during read copy",
            ))?;
        if entry.state.load_state != LoadState::Ready {
            return Err(CacheError::InvariantViolation(
                "resident read copy requires ready block",
            ));
        }

        touched.copy_from_block(&entry.data, buffer)
    }

    fn patch_slice<'a>(
        &self,
        touched: &TouchedBlock,
        data: &'a [u8],
    ) -> Result<&'a [u8], CacheError> {
        let range = touched.buffer_range();
        if data.len() < range.end {
            return Err(CacheError::BufferTooSmall {
                context: "request write buffer",
                expected: range.end,
                actual: data.len(),
            });
        }

        Ok(&data[range])
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
    use std::sync::{Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    use super::Cache;
    use crate::{AtIo, CacheConfig, CacheError};

    #[derive(Debug, Clone, PartialEq, Eq)]
    enum IoCall {
        Read { offset: u64, length: usize },
        Write { offset: u64, length: usize },
    }

    #[derive(Debug)]
    struct MockAtIoInner {
        calls: Mutex<Vec<IoCall>>,
        storage: Mutex<Vec<u8>>,
        read_delay: Duration,
    }

    #[derive(Debug, Clone)]
    struct MockAtIo {
        inner: Arc<MockAtIoInner>,
    }

    impl MockAtIo {
        fn new(storage: Vec<u8>, read_delay: Duration) -> Self {
            Self {
                inner: Arc::new(MockAtIoInner {
                    calls: Mutex::new(Vec::new()),
                    storage: Mutex::new(storage),
                    read_delay,
                }),
            }
        }

        fn with_len(length: usize) -> Self {
            Self::new(test_bytes(length), Duration::ZERO)
        }

        fn with_delay(length: usize, read_delay: Duration) -> Self {
            Self::new(test_bytes(length), read_delay)
        }

        fn take_calls(&self) -> Vec<IoCall> {
            self.inner.calls.lock().unwrap().drain(..).collect()
        }
    }

    impl AtIo for MockAtIo {
        fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
            self.inner.calls.lock().unwrap().push(IoCall::Read {
                offset,
                length: buffer.len(),
            });
            if self.inner.read_delay != Duration::ZERO {
                thread::sleep(self.inner.read_delay);
            }

            let start = usize::try_from(offset)
                .map_err(|_| CacheError::ArithmeticOverflow("mock read offset"))?;
            let end = start
                .checked_add(buffer.len())
                .ok_or(CacheError::ArithmeticOverflow("mock read end"))?;
            let storage = self.inner.storage.lock().unwrap();
            buffer.copy_from_slice(&storage[start..end]);
            Ok(())
        }

        fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
            self.inner.calls.lock().unwrap().push(IoCall::Write {
                offset,
                length: data.len(),
            });
            let start = usize::try_from(offset)
                .map_err(|_| CacheError::ArithmeticOverflow("mock write offset"))?;
            let end = start
                .checked_add(data.len())
                .ok_or(CacheError::ArithmeticOverflow("mock write end"))?;
            let mut storage = self.inner.storage.lock().unwrap();
            storage[start..end].copy_from_slice(data);
            Ok(())
        }
    }

    impl Default for MockAtIo {
        fn default() -> Self {
            Self::with_len(256)
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

    fn test_bytes(length: usize) -> Vec<u8> {
        (0..length).map(|index| index as u8).collect()
    }

    fn expected_bytes(offset: usize, length: usize) -> Vec<u8> {
        (offset..offset + length).map(|index| index as u8).collect()
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
    fn new_rejects_zero_fifo_capacity() {
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 0;

        let error = Cache::new(config, MockAtIo::default()).unwrap_err();
        assert_eq!(
            error,
            CacheError::InvalidConfig("fifo_capacity_blocks must be greater than 0")
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
        let right = MockAtIo::with_len(160);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut read_buffer = [0u8; 32];
        let write_buffer = [7u8; 32];

        cache.read_right_block(64, &mut read_buffer).unwrap();
        cache.write_right_block(96, &write_buffer).unwrap();

        assert_eq!(read_buffer[0], 64);
        assert_eq!(read_buffer[31], 95);
        assert_eq!(
            right.take_calls(),
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

    #[test]
    fn read_locked_hits_resident_after_first_miss() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut first = [0u8; 7];
        let mut second = [0u8; 7];

        cache.read_locked(5, &mut first).unwrap();
        cache.read_locked(5, &mut second).unwrap();

        assert_eq!(&first[..], &expected_bytes(5, 7));
        assert_eq!(&second[..], &expected_bytes(5, 7));
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn read_locked_spans_multiple_blocks_with_aligned_backend_reads() {
        let right = MockAtIo::with_len(160);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut buffer = [0u8; 40];

        cache.read_locked(28, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &expected_bytes(28, 40));
        assert_eq!(
            right.take_calls(),
            vec![
                IoCall::Read {
                    offset: 0,
                    length: 32
                },
                IoCall::Read {
                    offset: 32,
                    length: 32
                },
                IoCall::Read {
                    offset: 64,
                    length: 32
                }
            ]
        );
    }

    #[test]
    fn read_locked_reads_tail_slice_from_single_loaded_block() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut buffer = [0u8; 4];

        cache.read_locked(60, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &expected_bytes(60, 4));
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 32,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_patches_resident_hit_without_backend_write() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut warm = [0u8; 1];
        let mut buffer = [0u8; 8];

        cache.read_locked(0, &mut warm).unwrap();
        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 200, 201, 202, 7]);
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_miss_reads_full_block_once_and_patches_resident() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut buffer = [0u8; 10];

        cache.write_locked(5, &[90, 91, 92]).unwrap();
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 4, 90, 91, 92, 8, 9]);
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_spans_multiple_blocks_without_backend_write() {
        let right = MockAtIo::with_len(160);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 4;
        let cache = Cache::new(config, right.clone()).unwrap();
        let patch = vec![0xAB; 40];
        let mut buffer = [0u8; 40];

        cache.write_locked(28, &patch).unwrap();
        cache.read_locked(28, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &patch[..]);
        assert_eq!(
            right.take_calls(),
            vec![
                IoCall::Read {
                    offset: 0,
                    length: 32
                },
                IoCall::Read {
                    offset: 32,
                    length: 32
                },
                IoCall::Read {
                    offset: 64,
                    length: 32
                }
            ]
        );
    }

    #[test]
    fn concurrent_same_block_miss_loads_backend_once() {
        let right = MockAtIo::with_delay(128, Duration::from_millis(100));
        let cache = Arc::new(Cache::new(test_config(32), right.clone()).unwrap());

        let first_cache = Arc::clone(&cache);
        let first = thread::spawn(move || {
            let mut buffer = vec![0u8; 16];
            first_cache.read_locked(3, &mut buffer).unwrap();
            buffer
        });

        thread::sleep(Duration::from_millis(20));

        let second_cache = Arc::clone(&cache);
        let second = thread::spawn(move || {
            let mut buffer = vec![0u8; 8];
            second_cache.read_locked(5, &mut buffer).unwrap();
            buffer
        });

        assert_eq!(first.join().unwrap(), expected_bytes(3, 16));
        assert_eq!(second.join().unwrap(), expected_bytes(5, 8));
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn concurrent_same_block_write_miss_loads_backend_once() {
        let right = MockAtIo::with_delay(128, Duration::from_millis(100));
        let cache = Arc::new(Cache::new(test_config(32), right.clone()).unwrap());

        let first_cache = Arc::clone(&cache);
        let first = thread::spawn(move || {
            first_cache.write_locked(3, &[200, 201, 202, 203]).unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let second_cache = Arc::clone(&cache);
        let second = thread::spawn(move || {
            second_cache.write_locked(10, &[150, 151, 152]).unwrap();
        });

        first.join().unwrap();
        second.join().unwrap();

        let mut buffer = [0u8; 16];
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(
            &buffer[..],
            &[
                0, 1, 2, 200, 201, 202, 203, 7, 8, 9, 150, 151, 152, 13, 14, 15
            ]
        );
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_waits_for_read_load_without_second_backend_read() {
        let right = MockAtIo::with_delay(128, Duration::from_millis(100));
        let cache = Arc::new(Cache::new(test_config(32), right.clone()).unwrap());

        let first_cache = Arc::clone(&cache);
        let first = thread::spawn(move || {
            let mut buffer = vec![0u8; 8];
            first_cache.read_locked(2, &mut buffer).unwrap();
            buffer
        });

        thread::sleep(Duration::from_millis(20));

        let second_cache = Arc::clone(&cache);
        let second = thread::spawn(move || {
            second_cache.write_locked(6, &[222, 223, 224]).unwrap();
        });

        assert_eq!(first.join().unwrap(), expected_bytes(2, 8));
        second.join().unwrap();

        let mut buffer = [0u8; 12];
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 4, 5, 222, 223, 224, 9, 10, 11]);
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }
}
