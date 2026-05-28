use std::fs::File;
use std::fs::OpenOptions;
use std::io;
use std::os::windows::fs::FileExt;
use std::path::Path;
use std::sync::Mutex;

use backend_rust::BackendError;
use backend_rust::Media;

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
    actual_size_bytes: u64,
    size_bytes: u64,
    read_only: bool,
}

impl RawFileMedia {
    pub fn open(
        path: &Path,
        force_read_only: bool,
        sector_size_bytes: u32,
    ) -> Result<Self, RawFileMediaError> {
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
        if actual_size_bytes == 0 {
            return Err(RawFileMediaError::NoUsableCapacity { actual_size_bytes });
        }

        let alignment_bytes = u64::from(sector_size_bytes);
        let size_bytes =
            align_up_capacity_bytes(actual_size_bytes, alignment_bytes).ok_or_else(|| {
                RawFileMediaError::MetadataFailed(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "raw file aligned capacity overflow",
                ))
            })?;

        Ok(Self {
            file: Mutex::new(file),
            actual_size_bytes,
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

        let file_backed_length =
            visible_file_backed_length(self.actual_size_bytes, offset, buffer.len());
        if file_backed_length == 0 {
            buffer.fill(0);
            return Ok(());
        }

        let file = self
            .file
            .lock()
            .expect("raw file media mutex should not be poisoned");
        let (file_backed_buffer, zero_fill_buffer) = buffer.split_at_mut(file_backed_length);
        read_all_at(&file, offset, file_backed_buffer)
            .map_err(|_| BackendError::InvalidParameter)?;
        zero_fill_buffer.fill(0);
        Ok(())
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        if data.is_empty() {
            return Ok(());
        }
        validate_range(self.size_bytes, offset, data.len())?;

        if self.read_only {
            return Err(BackendError::InvalidParameter);
        }

        let file_backed_length =
            visible_file_backed_length(self.actual_size_bytes, offset, data.len());
        if file_backed_length == 0 {
            return Ok(());
        }

        let file = self
            .file
            .lock()
            .expect("raw file media mutex should not be poisoned");
        write_all_at(&file, offset, &data[..file_backed_length])
            .map_err(|_| BackendError::InvalidParameter)
    }
}

fn align_up_capacity_bytes(size_bytes: u64, alignment_bytes: u64) -> Option<u64> {
    if alignment_bytes == 0 || size_bytes == 0 {
        return Some(size_bytes);
    }

    let remainder = size_bytes % alignment_bytes;
    if remainder == 0 {
        return Some(size_bytes);
    }

    size_bytes.checked_add(alignment_bytes - remainder)
}

fn visible_file_backed_length(actual_size_bytes: u64, offset: u64, request_length: usize) -> usize {
    if request_length == 0 || offset >= actual_size_bytes {
        return 0;
    }

    (actual_size_bytes - offset).min(request_length as u64) as usize
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
    fn open_aligns_capacity_up_to_512_boundary() {
        let path = temp_file_path("raw_file_align_up");
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&vec![0u8; 1025])
            .expect("write temp raw file should succeed");
        drop(file);

        let media =
            RawFileMedia::open(&path, false, 512).expect("open raw file media should succeed");
        assert_eq!(media.size_bytes(), 1536);
        drop(media);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }

    #[test]
    fn open_accepts_file_without_complete_512_block() {
        let path = temp_file_path("raw_file_partial_tail");
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&vec![0u8; 511])
            .expect("write temp raw file should succeed");
        drop(file);

        let media =
            RawFileMedia::open(&path, false, 512).expect("open raw file media should succeed");
        assert_eq!(media.size_bytes(), 512);
        drop(media);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }

    #[test]
    fn open_rejects_empty_file() {
        let path = temp_file_path("raw_file_empty");
        let file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        drop(file);

        match RawFileMedia::open(&path, false, 512) {
            Err(RawFileMediaError::NoUsableCapacity { actual_size_bytes }) => {
                assert_eq!(actual_size_bytes, 0);
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

        let media =
            RawFileMedia::open(&path, true, 512).expect("open raw file media should succeed");
        let error = media
            .write_locked(0, &[1, 2, 3, 4])
            .expect_err("write should fail");
        assert_eq!(error, BackendError::InvalidParameter);
        drop(media);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }

    #[test]
    fn read_locked_zero_fills_bytes_past_actual_eof() {
        let path = temp_file_path("raw_file_zero_fill_read");
        let mut data = vec![0u8; 513];
        data[512] = 0xA5;

        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&data)
            .expect("write temp raw file should succeed");
        drop(file);

        let media =
            RawFileMedia::open(&path, false, 512).expect("open raw file media should succeed");
        let mut buffer = vec![0xFF; 512];
        media
            .read_locked(512, &mut buffer)
            .expect("read should succeed");
        assert_eq!(buffer[0], 0xA5);
        assert!(buffer[1..].iter().all(|byte| *byte == 0));
        drop(media);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }

    #[test]
    fn write_locked_drops_bytes_past_actual_eof() {
        let path = temp_file_path("raw_file_truncate_write");
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&vec![0u8; 513])
            .expect("write temp raw file should succeed");
        drop(file);

        let media =
            RawFileMedia::open(&path, false, 512).expect("open raw file media should succeed");
        let write_data = vec![0x3C; 512];
        media
            .write_locked(512, &write_data)
            .expect("write should succeed");

        let actual_data = fs::read(&path).expect("read back temp raw file should succeed");
        assert_eq!(actual_data.len(), 513);
        assert_eq!(actual_data[512], 0x3C);

        let mut buffer = vec![0xFF; 512];
        media
            .read_locked(512, &mut buffer)
            .expect("read should succeed");
        assert_eq!(buffer[0], 0x3C);
        assert!(buffer[1..].iter().all(|byte| *byte == 0));
        drop(media);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }

    #[test]
    fn open_aligns_capacity_up_to_configured_sector_boundary() {
        let path = temp_file_path("raw_file_align_configured_sector");
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .expect("create temp raw file should succeed");
        file.write_all(&vec![0u8; 4097])
            .expect("write temp raw file should succeed");
        drop(file);

        let media =
            RawFileMedia::open(&path, false, 4096).expect("open raw file media should succeed");
        assert_eq!(media.size_bytes(), 8192);
        drop(media);

        fs::remove_file(&path).expect("remove temp raw file should succeed");
    }
}
