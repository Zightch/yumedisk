use std::fs::{self, File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use crate::{AtIo, CacheError};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoOperation {
    Read,
    Write,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IoLogEntry {
    pub sequence: u64,
    pub operation: IoOperation,
    pub offset: u64,
    pub length: usize,
    pub block_index: u64,
    pub result: Result<(), CacheError>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct IoTimings {
    pub read_delay: Duration,
    pub write_delay: Duration,
}

pub trait TestAtIo: AtIo {
    fn log_snapshot(&self) -> Vec<IoLogEntry>;

    fn take_log(&self) -> Vec<IoLogEntry>;

    fn in_flight_count(&self) -> usize;
}

#[derive(Debug)]
struct IoRecorder {
    block_size_bytes: u32,
    next_sequence: AtomicU64,
    in_flight: AtomicUsize,
    log: Mutex<Vec<IoLogEntry>>,
}

impl IoRecorder {
    fn new(block_size_bytes: u32) -> Self {
        assert!(block_size_bytes > 0);
        Self {
            block_size_bytes,
            next_sequence: AtomicU64::new(1),
            in_flight: AtomicUsize::new(0),
            log: Mutex::new(Vec::new()),
        }
    }

    fn begin_call(&self) -> u64 {
        self.in_flight.fetch_add(1, Ordering::SeqCst);
        self.next_sequence.fetch_add(1, Ordering::SeqCst)
    }

    fn finish_call(&self, entry: IoLogEntry) {
        self.log.lock().unwrap().push(entry);
        self.in_flight.fetch_sub(1, Ordering::SeqCst);
    }

    fn log_snapshot(&self) -> Vec<IoLogEntry> {
        let mut log = self.log.lock().unwrap().clone();
        log.sort_by_key(|entry| entry.sequence);
        log
    }

    fn take_log(&self) -> Vec<IoLogEntry> {
        let mut log = self.log.lock().unwrap().drain(..).collect::<Vec<_>>();
        log.sort_by_key(|entry| entry.sequence);
        log
    }

    fn in_flight_count(&self) -> usize {
        self.in_flight.load(Ordering::SeqCst)
    }

    fn block_index(&self, offset: u64) -> u64 {
        offset / u64::from(self.block_size_bytes)
    }
}

#[derive(Debug)]
struct MemoryAtIoInner {
    recorder: IoRecorder,
    storage: Mutex<Vec<u8>>,
    timings: IoTimings,
}

#[derive(Debug, Clone)]
pub struct MemoryAtIo {
    inner: Arc<MemoryAtIoInner>,
}

impl MemoryAtIo {
    pub fn from_bytes(block_size_bytes: u32, storage: Vec<u8>) -> Self {
        Self::with_timings(block_size_bytes, storage, IoTimings::default())
    }

    pub fn with_timings(block_size_bytes: u32, storage: Vec<u8>, timings: IoTimings) -> Self {
        Self {
            inner: Arc::new(MemoryAtIoInner {
                recorder: IoRecorder::new(block_size_bytes),
                storage: Mutex::new(storage),
                timings,
            }),
        }
    }

    pub fn log_snapshot(&self) -> Vec<IoLogEntry> {
        self.inner.recorder.log_snapshot()
    }

    pub fn take_log(&self) -> Vec<IoLogEntry> {
        self.inner.recorder.take_log()
    }

    pub fn in_flight_count(&self) -> usize {
        self.inner.recorder.in_flight_count()
    }

    pub fn storage_slice(&self, offset: usize, length: usize) -> Vec<u8> {
        self.inner.storage.lock().unwrap()[offset..offset + length].to_vec()
    }
}

impl TestAtIo for MemoryAtIo {
    fn log_snapshot(&self) -> Vec<IoLogEntry> {
        self.log_snapshot()
    }

    fn take_log(&self) -> Vec<IoLogEntry> {
        self.take_log()
    }

    fn in_flight_count(&self) -> usize {
        self.in_flight_count()
    }
}

impl AtIo for MemoryAtIo {
    fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        self.run_call(IoOperation::Read, offset, buffer.len(), |storage| {
            let (start, end) = checked_range(offset, buffer.len(), storage.len())?;
            buffer.copy_from_slice(&storage[start..end]);
            Ok(())
        })
    }

    fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        self.run_call(IoOperation::Write, offset, data.len(), |storage| {
            let (start, end) = checked_range(offset, data.len(), storage.len())?;
            storage[start..end].copy_from_slice(data);
            Ok(())
        })
    }
}

impl MemoryAtIo {
    fn run_call<F>(
        &self,
        operation: IoOperation,
        offset: u64,
        length: usize,
        f: F,
    ) -> Result<(), CacheError>
    where
        F: FnOnce(&mut Vec<u8>) -> Result<(), CacheError>,
    {
        let sequence = self.inner.recorder.begin_call();
        let delay = match operation {
            IoOperation::Read => self.inner.timings.read_delay,
            IoOperation::Write => self.inner.timings.write_delay,
        };
        if delay != Duration::ZERO {
            thread::sleep(delay);
        }

        let result = {
            let mut storage = self.inner.storage.lock().unwrap();
            f(&mut storage)
        };
        self.inner.recorder.finish_call(IoLogEntry {
            sequence,
            operation,
            offset,
            length,
            block_index: self.inner.recorder.block_index(offset),
            result: result.clone(),
        });
        result
    }
}

#[derive(Debug)]
struct FileBackedAtIoInner {
    recorder: IoRecorder,
    path: PathBuf,
    timings: IoTimings,
}

#[derive(Debug, Clone)]
pub struct FileBackedAtIo {
    inner: Arc<FileBackedAtIoInner>,
}

impl FileBackedAtIo {
    pub fn create(
        path: impl AsRef<Path>,
        block_size_bytes: u32,
        storage: &[u8],
    ) -> std::io::Result<Self> {
        Self::create_with_timings(path, block_size_bytes, storage, IoTimings::default())
    }

    pub fn create_with_timings(
        path: impl AsRef<Path>,
        block_size_bytes: u32,
        storage: &[u8],
        timings: IoTimings,
    ) -> std::io::Result<Self> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(&path, storage)?;
        Ok(Self {
            inner: Arc::new(FileBackedAtIoInner {
                recorder: IoRecorder::new(block_size_bytes),
                path,
                timings,
            }),
        })
    }

    pub fn path(&self) -> &Path {
        &self.inner.path
    }

    pub fn log_snapshot(&self) -> Vec<IoLogEntry> {
        self.inner.recorder.log_snapshot()
    }

    pub fn take_log(&self) -> Vec<IoLogEntry> {
        self.inner.recorder.take_log()
    }

    pub fn in_flight_count(&self) -> usize {
        self.inner.recorder.in_flight_count()
    }

    pub fn storage_slice(&self, offset: u64, length: usize) -> std::io::Result<Vec<u8>> {
        let mut buffer = vec![0u8; length];
        let mut file = File::open(&self.inner.path)?;
        file.seek(SeekFrom::Start(offset))?;
        file.read_exact(&mut buffer)?;
        Ok(buffer)
    }
}

impl TestAtIo for FileBackedAtIo {
    fn log_snapshot(&self) -> Vec<IoLogEntry> {
        self.log_snapshot()
    }

    fn take_log(&self) -> Vec<IoLogEntry> {
        self.take_log()
    }

    fn in_flight_count(&self) -> usize {
        self.in_flight_count()
    }
}

impl AtIo for FileBackedAtIo {
    fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        self.run_call(IoOperation::Read, offset, buffer.len(), |path| {
            checked_file_range(path, offset, buffer.len())?;
            let mut file = File::open(path).map_err(|error| {
                map_file_error("open file-backed test device for read", path, error)
            })?;
            file.seek(SeekFrom::Start(offset)).map_err(|error| {
                map_file_error("seek file-backed test device for read", path, error)
            })?;
            file.read_exact(buffer)
                .map_err(|error| map_file_error("read file-backed test device", path, error))
        })
    }

    fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        self.run_call(IoOperation::Write, offset, data.len(), |path| {
            checked_file_range(path, offset, data.len())?;
            let mut file = OpenOptions::new().write(true).open(path).map_err(|error| {
                map_file_error("open file-backed test device for write", path, error)
            })?;
            file.seek(SeekFrom::Start(offset)).map_err(|error| {
                map_file_error("seek file-backed test device for write", path, error)
            })?;
            file.write_all(data)
                .map_err(|error| map_file_error("write file-backed test device", path, error))?;
            file.sync_all()
                .map_err(|error| map_file_error("sync file-backed test device", path, error))
        })
    }
}

impl FileBackedAtIo {
    fn run_call<F>(
        &self,
        operation: IoOperation,
        offset: u64,
        length: usize,
        f: F,
    ) -> Result<(), CacheError>
    where
        F: FnOnce(&Path) -> Result<(), CacheError>,
    {
        let sequence = self.inner.recorder.begin_call();
        let delay = match operation {
            IoOperation::Read => self.inner.timings.read_delay,
            IoOperation::Write => self.inner.timings.write_delay,
        };
        if delay != Duration::ZERO {
            thread::sleep(delay);
        }

        let result = f(&self.inner.path);
        self.inner.recorder.finish_call(IoLogEntry {
            sequence,
            operation,
            offset,
            length,
            block_index: self.inner.recorder.block_index(offset),
            result: result.clone(),
        });
        result
    }
}

fn checked_range(
    offset: u64,
    length: usize,
    total_len: usize,
) -> Result<(usize, usize), CacheError> {
    let start =
        usize::try_from(offset).map_err(|_| CacheError::ArithmeticOverflow("test io offset"))?;
    let end = start
        .checked_add(length)
        .ok_or(CacheError::ArithmeticOverflow("test io end"))?;
    if end > total_len {
        return Err(CacheError::InvalidRange { offset, length });
    }

    Ok((start, end))
}

fn checked_file_range(path: &Path, offset: u64, length: usize) -> Result<(), CacheError> {
    let metadata = fs::metadata(path)
        .map_err(|error| map_file_error("stat file-backed test device", path, error))?;
    let total_len = usize::try_from(metadata.len())
        .map_err(|_| CacheError::ArithmeticOverflow("file-backed test device length"))?;
    let _ = checked_range(offset, length, total_len)?;
    Ok(())
}

fn map_file_error(operation: &'static str, path: &Path, error: std::io::Error) -> CacheError {
    CacheError::TempIo {
        operation,
        path: path.to_path_buf(),
        kind: error.kind(),
    }
}

#[cfg(test)]
mod tests {
    use super::{FileBackedAtIo, IoLogEntry, IoOperation, MemoryAtIo};
    use crate::AtIo;
    use crate::test_support::TestTempDir;

    fn test_bytes(length: usize) -> Vec<u8> {
        (0..length).map(|index| index as u8).collect()
    }

    #[test]
    fn memory_at_io_reads_writes_and_records_structured_log() {
        let io = MemoryAtIo::from_bytes(32, test_bytes(128));
        let mut buffer = [0u8; 32];

        io.read_at(32, &mut buffer).unwrap();
        io.write_at(64, &[200, 201, 202, 203]).unwrap();

        assert_eq!(buffer.to_vec(), test_bytes(128)[32..64].to_vec());
        assert_eq!(
            &io.storage_slice(64, 8),
            &[200, 201, 202, 203, 68, 69, 70, 71]
        );
        assert_eq!(
            io.take_log(),
            vec![
                IoLogEntry {
                    sequence: 1,
                    operation: IoOperation::Read,
                    offset: 32,
                    length: 32,
                    block_index: 1,
                    result: Ok(()),
                },
                IoLogEntry {
                    sequence: 2,
                    operation: IoOperation::Write,
                    offset: 64,
                    length: 4,
                    block_index: 2,
                    result: Ok(()),
                },
            ]
        );
        assert_eq!(io.in_flight_count(), 0);
    }

    #[test]
    fn file_backed_at_io_reads_writes_and_records_structured_log() {
        let dir = TestTempDir::with_prefix("cache-file-io");
        let path = dir.child("backing.bin");
        let io = FileBackedAtIo::create(&path, 32, &test_bytes(128)).unwrap();
        let mut buffer = [0u8; 16];

        io.read_at(96, &mut buffer).unwrap();
        io.write_at(32, &[210, 211, 212]).unwrap();

        assert_eq!(
            buffer.to_vec(),
            vec![
                96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111
            ]
        );
        assert_eq!(
            io.storage_slice(32, 8).unwrap(),
            vec![210, 211, 212, 35, 36, 37, 38, 39]
        );
        assert_eq!(io.path(), path.as_path());
        assert_eq!(
            io.log_snapshot(),
            vec![
                IoLogEntry {
                    sequence: 1,
                    operation: IoOperation::Read,
                    offset: 96,
                    length: 16,
                    block_index: 3,
                    result: Ok(()),
                },
                IoLogEntry {
                    sequence: 2,
                    operation: IoOperation::Write,
                    offset: 32,
                    length: 3,
                    block_index: 1,
                    result: Ok(()),
                },
            ]
        );
        assert_eq!(io.in_flight_count(), 0);
    }
}
