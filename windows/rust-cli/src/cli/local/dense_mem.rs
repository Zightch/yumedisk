use std::sync::Arc;
use std::sync::RwLock;

use backend_rust::BackendError;
use backend_rust::Media;

#[derive(Debug, Clone)]
pub struct DenseMem {
    bytes: Arc<RwLock<Vec<u8>>>,
}

impl DenseMem {
    pub fn new(size_bytes: u64) -> Result<Self, BackendError> {
        let size = usize::try_from(size_bytes).map_err(|_| BackendError::InvalidDiskSizeBytes)?;
        Ok(Self {
            bytes: Arc::new(RwLock::new(vec![0; size])),
        })
    }
}

impl Media for DenseMem {
    fn size_bytes(&self) -> u64 {
        self.bytes.read().expect("dense mem poisoned").len() as u64
    }

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
        let bytes = self.bytes.read().expect("dense mem poisoned");
        let begin = usize::try_from(offset).map_err(|_| BackendError::InvalidParameter)?;
        let end = begin
            .checked_add(buffer.len())
            .ok_or(BackendError::InvalidParameter)?;
        if end > bytes.len() {
            return Err(BackendError::InvalidParameter);
        }
        buffer.copy_from_slice(&bytes[begin..end]);
        Ok(())
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        let mut bytes = self.bytes.write().expect("dense mem poisoned");
        let begin = usize::try_from(offset).map_err(|_| BackendError::InvalidParameter)?;
        let end = begin
            .checked_add(data.len())
            .ok_or(BackendError::InvalidParameter)?;
        if end > bytes.len() {
            return Err(BackendError::InvalidParameter);
        }
        bytes[begin..end].copy_from_slice(data);
        Ok(())
    }
}
