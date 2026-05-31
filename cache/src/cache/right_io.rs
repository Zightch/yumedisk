use super::Cache;
use super::state::CacheState;
use crate::block::TouchedBlock;
use crate::resident::LoadState;
#[cfg(any(test, feature = "test-hooks"))]
use crate::test_support::HookPoint;
use crate::{AtIo, CacheError};

impl<R: AtIo + 'static> Cache<R> {
    pub(super) fn copy_ready_block(
        &self,
        state: &CacheState,
        touched: &TouchedBlock,
        buffer: &mut [u8],
    ) -> Result<(), CacheError> {
        let entry =
            state
                .resident
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

    pub(super) fn patch_slice<'a>(
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
    pub(super) fn read_right_block(
        &self,
        offset: u64,
        buffer: &mut [u8],
    ) -> Result<(), CacheError> {
        self.layout.validate_right_io(offset, buffer.len())?;
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::BeforeRightRead);
        let result = self.right.read_at(offset, buffer);
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::AfterRightRead);
        result
    }

    pub(super) fn read_temp_block(
        &self,
        block_index: u64,
        buffer: &mut [u8],
    ) -> Result<(), CacheError> {
        let expected = self.layout.block_size_usize()?;
        if buffer.len() != expected {
            return Err(CacheError::InvalidBlockDataLength {
                expected,
                actual: buffer.len(),
            });
        }

        self.deps.temp().read_block(block_index, buffer)
    }

    #[allow(dead_code)]
    pub(super) fn write_right_block(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        self.layout.validate_right_io(offset, data.len())?;
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::BeforeRightWrite);
        let result = self.right.write_at(offset, data);
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::AfterRightWrite);
        result
    }
}
