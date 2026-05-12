use std::collections::BTreeMap;
use std::sync::RwLock;

use backend_rust::BackendError;
use backend_rust::Media;

const SPARSE_PAGE_BYTES: usize = 4096;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DenseMemoryMediaError {
    SizeExceedsProcessLimit,
    AllocationFailed,
}

pub struct DenseMemoryMedia {
    size_bytes: u64,
    buffer: RwLock<Vec<u8>>,
}

impl DenseMemoryMedia {
    pub fn new(size_bytes: u64) -> Result<Self, DenseMemoryMediaError> {
        let length = usize::try_from(size_bytes)
            .map_err(|_| DenseMemoryMediaError::SizeExceedsProcessLimit)?;

        let mut buffer = Vec::new();
        buffer
            .try_reserve_exact(length)
            .map_err(|_| DenseMemoryMediaError::AllocationFailed)?;

        unsafe {
            buffer.set_len(length);
        }
        buffer.fill(0);

        Ok(Self {
            size_bytes,
            buffer: RwLock::new(buffer),
        })
    }
}

impl Media for DenseMemoryMedia {
    fn size_bytes(&self) -> u64 {
        self.size_bytes
    }

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
        validate_range(self.size_bytes, offset, buffer.len())?;

        let begin = usize::try_from(offset).map_err(|_| BackendError::InvalidParameter)?;
        let end = begin
            .checked_add(buffer.len())
            .ok_or(BackendError::InvalidParameter)?;

        let data = self.buffer.read().expect("dense memory media poisoned");
        buffer.copy_from_slice(&data[begin..end]);
        Ok(())
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        validate_range(self.size_bytes, offset, data.len())?;

        let begin = usize::try_from(offset).map_err(|_| BackendError::InvalidParameter)?;
        let end = begin
            .checked_add(data.len())
            .ok_or(BackendError::InvalidParameter)?;

        let mut buffer = self.buffer.write().expect("dense memory media poisoned");
        buffer[begin..end].copy_from_slice(data);
        Ok(())
    }
}

pub struct SparseMemoryMedia {
    size_bytes: u64,
    pages: RwLock<BTreeMap<u64, Vec<u8>>>,
}

impl SparseMemoryMedia {
    pub fn new(size_bytes: u64) -> Self {
        Self {
            size_bytes,
            pages: RwLock::new(BTreeMap::new()),
        }
    }
}

impl Media for SparseMemoryMedia {
    fn size_bytes(&self) -> u64 {
        self.size_bytes
    }

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
        validate_range(self.size_bytes, offset, buffer.len())?;

        buffer.fill(0);

        let pages = self.pages.read().expect("sparse memory media poisoned");
        let mut current_offset = offset;
        let mut buffer_offset = 0usize;

        while buffer_offset < buffer.len() {
            let page_index = current_offset / SPARSE_PAGE_BYTES as u64;
            let page_offset = (current_offset % SPARSE_PAGE_BYTES as u64) as usize;
            let copy_length = (SPARSE_PAGE_BYTES - page_offset).min(buffer.len() - buffer_offset);

            if let Some(page) = pages.get(&page_index) {
                buffer[buffer_offset..buffer_offset + copy_length]
                    .copy_from_slice(&page[page_offset..page_offset + copy_length]);
            }

            current_offset += copy_length as u64;
            buffer_offset += copy_length;
        }

        Ok(())
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        validate_range(self.size_bytes, offset, data.len())?;

        let mut pages = self.pages.write().expect("sparse memory media poisoned");
        let mut current_offset = offset;
        let mut data_offset = 0usize;

        while data_offset < data.len() {
            let page_index = current_offset / SPARSE_PAGE_BYTES as u64;
            let page_offset = (current_offset % SPARSE_PAGE_BYTES as u64) as usize;
            let copy_length = (SPARSE_PAGE_BYTES - page_offset).min(data.len() - data_offset);

            let page = pages
                .entry(page_index)
                .or_insert_with(|| vec![0u8; SPARSE_PAGE_BYTES]);

            page[page_offset..page_offset + copy_length]
                .copy_from_slice(&data[data_offset..data_offset + copy_length]);

            current_offset += copy_length as u64;
            data_offset += copy_length;
        }

        Ok(())
    }
}

fn validate_range(size_bytes: u64, offset: u64, data_length: usize) -> Result<(), BackendError> {
    let request_length = u64::try_from(data_length).map_err(|_| BackendError::InvalidParameter)?;
    let end = offset
        .checked_add(request_length)
        .ok_or(BackendError::InvalidParameter)?;

    if end > size_bytes {
        return Err(BackendError::InvalidParameter);
    }

    Ok(())
}
