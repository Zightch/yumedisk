use crate::CacheError;

pub trait AtIo: Send + Sync {
    fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError>;

    fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError>;
}
