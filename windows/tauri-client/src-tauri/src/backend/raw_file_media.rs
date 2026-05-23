use std::fs::File;
use std::fs::OpenOptions;
use std::io;
use std::os::windows::fs::FileExt;
use std::path::Path;
use std::sync::Mutex;

use backend_rust::BackendError;
use backend_rust::Media;
use backend_rust::SECTOR_ALIGNMENT_BYTES;

#[derive(Debug)]
pub enum RawFileMediaError {
    OpenFailed {
        read_write: io::Error,
        read_only: io::Error,
    },
    MetadataFailed(io::Error),
    NoUsableCapacity {
        actual_size_bytes: u64,
    },
}

pub struct RawFileMedia {
    file: Mutex<File>,
    size_bytes: u64,
    read_only: bool,
}

impl RawFileMedia {
    pub fn open(path: &Path, force_read_only: bool) -> Result<Self, RawFileMediaError> {
        let (file, source_read_only) = match OpenOptions::new().read(true).write(true).open(path) {
            Ok(file) => (file, false),
            Err(read_write_error) => match OpenOptions::new().read(true).open(path) {
                Ok(file) => (file, true),
                Err(read_only_error) => {
                    return Err(RawFileMediaError::OpenFailed {
                        read_write: read_write_error,
                        read_only: read_only_error,
                    });
                }
            },
        };

        let actual_size_bytes = file
            .metadata()
            .map_err(RawFileMediaError::MetadataFailed)?
            .len();
        let size_bytes =
            align_down_capacity_bytes(actual_size_bytes, u64::from(SECTOR_ALIGNMENT_BYTES));

        if size_bytes == 0 {
            return Err(RawFileMediaError::NoUsableCapacity { actual_size_bytes });
        }

        Ok(Self {
            file: Mutex::new(file),
            size_bytes,
            read_only: force_read_only || source_read_only,
        })
    }

    pub fn size_bytes(&self) -> u64 {
        self.size_bytes
    }

    pub fn read_only(&self) -> bool {
        self.read_only
    }
}

impl Media for RawFileMedia {
    fn size_bytes(&self) -> u64 {
        self.size_bytes
    }

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
        if buffer.is_empty() {
            return Ok(());
        }
        validate_range(self.size_bytes, offset, buffer.len())?;

        let file = self
            .file
            .lock()
            .expect("raw file media mutex should not be poisoned");
        read_all_at(&file, offset, buffer).map_err(|_| BackendError::InvalidParameter)
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        if data.is_empty() {
            return Ok(());
        }
        validate_range(self.size_bytes, offset, data.len())?;

        if self.read_only {
            return Err(BackendError::InvalidParameter);
        }

        let file = self
            .file
            .lock()
            .expect("raw file media mutex should not be poisoned");
        write_all_at(&file, offset, data).map_err(|_| BackendError::InvalidParameter)
    }
}

fn align_down_capacity_bytes(size_bytes: u64, alignment_bytes: u64) -> u64 {
    if alignment_bytes == 0 {
        return size_bytes;
    }

    size_bytes / alignment_bytes * alignment_bytes
}

fn validate_range(size_bytes: u64, offset: u64, length: usize) -> Result<(), BackendError> {
    let end = offset
        .checked_add(length as u64)
        .ok_or(BackendError::InvalidParameter)?;
    if end > size_bytes {
        return Err(BackendError::InvalidParameter);
    }
    Ok(())
}

fn read_all_at(file: &File, mut offset: u64, mut buffer: &mut [u8]) -> io::Result<()> {
    while !buffer.is_empty() {
        let bytes_read = file.seek_read(buffer, offset)?;
        if bytes_read == 0 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "raw file read reached eof",
            ));
        }

        let (_, remaining) = buffer.split_at_mut(bytes_read);
        buffer = remaining;
        offset += bytes_read as u64;
    }

    Ok(())
}

fn write_all_at(file: &File, mut offset: u64, mut data: &[u8]) -> io::Result<()> {
    while !data.is_empty() {
        let bytes_written = file.seek_write(data, offset)?;
        if bytes_written == 0 {
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "raw file write returned zero bytes",
            ));
        }

        data = &data[bytes_written..];
        offset += bytes_written as u64;
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::RawFileMedia;
    use super::RawFileMediaError;
    use backend_rust::BackendError;
    use backend_rust::Media;
    use std::fs;
    use std::fs::OpenOptions;
    use std::io::Write;
    use std::path::PathBuf;
    use std::time::SystemTime;
    use std::time::UNIX_EPOCH;

    fn temp_file_path(name: &str) -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock before epoch")
            .as_nanos();
        std::env::temp_dir().join(format!("{}_{}_raw.bin", name, unique))
    }

    #[test]
    fn open_aligns_capacity_down_to_512_boundary() {
        let path = temp_file_path("raw_file_align_down");
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&vec![0u8; 1025])
            .expect("write temp raw file should succeed");
        drop(file);

        let media = RawFileMedia::open(&path, false).expect("open raw file media should succeed");
        assert_eq!(media.size_bytes(), 1024);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }

    #[test]
    fn open_rejects_file_without_complete_512_block() {
        let path = temp_file_path("raw_file_too_small");
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&vec![0u8; 511])
            .expect("write temp raw file should succeed");
        drop(file);

        match RawFileMedia::open(&path, false) {
            Err(RawFileMediaError::NoUsableCapacity { actual_size_bytes }) => {
                assert_eq!(actual_size_bytes, 511);
            }
            Err(other) => panic!("unexpected error: {:?}", other),
            Ok(_) => panic!("open raw file media should fail"),
        }

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }

    #[test]
    fn force_read_only_rejects_write_even_when_file_is_writable() {
        let path = temp_file_path("raw_file_force_read_only");
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&vec![0u8; 1024])
            .expect("write temp raw file should succeed");
        drop(file);

        let media = RawFileMedia::open(&path, true).expect("open raw file media should succeed");
        let error = media
            .write_locked(0, &[1, 2, 3, 4])
            .expect_err("write should fail");
        assert_eq!(error, BackendError::InvalidParameter);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }
}
