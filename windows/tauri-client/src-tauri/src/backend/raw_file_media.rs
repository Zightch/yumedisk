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
    OpenFailed { read_write: io::Error, read_only: io::Error },
    MetadataFailed(io::Error),
    EmptyFile,
}

pub struct RawFileMedia {
    file: Mutex<File>,
    size_bytes: u64,
    read_only: bool,
}

impl RawFileMedia {
    pub fn open(path: &Path) -> Result<Self, RawFileMediaError> {
        let (file, read_only) = match OpenOptions::new().read(true).write(true).open(path) {
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

        let size_bytes = file
            .metadata()
            .map_err(RawFileMediaError::MetadataFailed)?
            .len();

        if size_bytes == 0 {
            return Err(RawFileMediaError::EmptyFile);
        }

        Ok(Self {
            file: Mutex::new(file),
            size_bytes,
            read_only,
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

        let file = self.file.lock().expect("raw file media mutex should not be poisoned");
        read_all_at(&file, offset, buffer).map_err(|_| BackendError::InvalidParameter)
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        if data.is_empty() {
            return Ok(());
        }

        if self.read_only {
            return Err(BackendError::InvalidParameter);
        }

        let file = self.file.lock().expect("raw file media mutex should not be poisoned");
        write_all_at(&file, offset, data).map_err(|_| BackendError::InvalidParameter)
    }
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
