use crate::error::BackendError;

pub trait Media: Send + Sync + 'static {
    fn size_bytes(&self) -> u64;

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError>;

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError>;
}
