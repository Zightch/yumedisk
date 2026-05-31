use std::ops::Range;

use crate::CacheError;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct BlockLayout {
    block_size_bytes: u32,
}

#[allow(dead_code)]
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct TouchedBlock {
    pub(crate) block_index: u64,
    pub(crate) block_base: u64,
    pub(crate) block_offset: usize,
    pub(crate) buffer_offset: usize,
    pub(crate) valid_len: usize,
}

impl BlockLayout {
    pub(crate) fn new(block_size_bytes: u32) -> Result<Self, CacheError> {
        if block_size_bytes == 0 {
            return Err(CacheError::InvalidConfig(
                "block_size_bytes must be greater than 0",
            ));
        }

        Ok(Self { block_size_bytes })
    }

    pub(crate) fn block_size_u64(&self) -> u64 {
        u64::from(self.block_size_bytes)
    }

    pub(crate) fn block_size_usize(&self) -> Result<usize, CacheError> {
        usize::try_from(self.block_size_bytes)
            .map_err(|_| CacheError::ArithmeticOverflow("block_size_bytes -> usize"))
    }

    pub(crate) fn block_index(&self, offset: u64) -> u64 {
        offset / self.block_size_u64()
    }

    pub(crate) fn block_base(&self, block_index: u64) -> Result<u64, CacheError> {
        block_index
            .checked_mul(self.block_size_u64())
            .ok_or(CacheError::ArithmeticOverflow("block_base"))
    }

    #[allow(dead_code)]
    pub(crate) fn touched_blocks(
        &self,
        offset: u64,
        length: usize,
    ) -> Result<Vec<TouchedBlock>, CacheError> {
        if length == 0 {
            return Ok(Vec::new());
        }

        let block_size = self.block_size_u64();
        let length_u64 =
            u64::try_from(length).map_err(|_| CacheError::ArithmeticOverflow("request length"))?;
        let end = offset
            .checked_add(length_u64)
            .ok_or(CacheError::InvalidRange { offset, length })?;
        let first_block = self.block_index(offset);
        let last_block = self.block_index(end - 1);
        let block_count_u64 = last_block - first_block + 1;
        let block_count = usize::try_from(block_count_u64)
            .map_err(|_| CacheError::ArithmeticOverflow("touched block count"))?;
        let mut blocks = Vec::with_capacity(block_count);
        let mut buffer_offset = 0usize;

        for block_index in first_block..=last_block {
            let block_base = self.block_base(block_index)?;
            let block_offset_u64 = if block_index == first_block {
                offset - block_base
            } else {
                0
            };
            let block_end = block_base
                .checked_add(block_size)
                .ok_or(CacheError::ArithmeticOverflow("block end"))?;
            let covered_end = end.min(block_end);
            let logical_start = block_base
                .checked_add(block_offset_u64)
                .ok_or(CacheError::ArithmeticOverflow("logical start"))?;
            let valid_len_u64 = covered_end
                .checked_sub(logical_start)
                .ok_or(CacheError::ArithmeticOverflow("valid_len"))?;
            let block_offset = usize::try_from(block_offset_u64)
                .map_err(|_| CacheError::ArithmeticOverflow("block_offset"))?;
            let valid_len = usize::try_from(valid_len_u64)
                .map_err(|_| CacheError::ArithmeticOverflow("valid_len"))?;

            blocks.push(TouchedBlock {
                block_index,
                block_base,
                block_offset,
                buffer_offset,
                valid_len,
            });

            buffer_offset = buffer_offset
                .checked_add(valid_len)
                .ok_or(CacheError::ArithmeticOverflow("buffer_offset"))?;
        }

        debug_assert_eq!(buffer_offset, length);
        Ok(blocks)
    }

    #[allow(dead_code)]
    pub(crate) fn validate_right_io(&self, offset: u64, length: usize) -> Result<(), CacheError> {
        let block_size_u64 = self.block_size_u64();
        let block_size = self.block_size_usize()?;
        if offset % block_size_u64 != 0 || length != block_size {
            return Err(CacheError::MisalignedRightIo {
                offset,
                length,
                block_size,
            });
        }

        Ok(())
    }
}

#[allow(dead_code)]
impl TouchedBlock {
    pub(crate) fn block_range(&self) -> Range<usize> {
        self.block_offset..self.block_offset + self.valid_len
    }

    pub(crate) fn buffer_range(&self) -> Range<usize> {
        self.buffer_offset..self.buffer_offset + self.valid_len
    }

    pub(crate) fn copy_from_block(
        &self,
        block: &[u8],
        buffer: &mut [u8],
    ) -> Result<(), CacheError> {
        let block_end = self.block_offset + self.valid_len;
        if block.len() < block_end {
            return Err(CacheError::BufferTooSmall {
                context: "block read slice",
                expected: block_end,
                actual: block.len(),
            });
        }

        let buffer_end = self.buffer_offset + self.valid_len;
        if buffer.len() < buffer_end {
            return Err(CacheError::BufferTooSmall {
                context: "request read buffer",
                expected: buffer_end,
                actual: buffer.len(),
            });
        }

        buffer[self.buffer_range()].copy_from_slice(&block[self.block_range()]);
        Ok(())
    }

    pub(crate) fn patch_into_block(&self, data: &[u8], block: &mut [u8]) -> Result<(), CacheError> {
        let buffer_end = self.buffer_offset + self.valid_len;
        if data.len() < buffer_end {
            return Err(CacheError::BufferTooSmall {
                context: "request write buffer",
                expected: buffer_end,
                actual: data.len(),
            });
        }

        let block_end = self.block_offset + self.valid_len;
        if block.len() < block_end {
            return Err(CacheError::BufferTooSmall {
                context: "block write slice",
                expected: block_end,
                actual: block.len(),
            });
        }

        block[self.block_range()].copy_from_slice(&data[self.buffer_range()]);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::BlockLayout;
    use crate::CacheError;

    #[test]
    fn touched_blocks_returns_empty_for_zero_length_request() {
        let layout = BlockLayout::new(32).unwrap();
        assert_eq!(layout.touched_blocks(7, 0).unwrap(), Vec::new());
    }

    #[test]
    fn touched_blocks_maps_single_block_request() {
        let layout = BlockLayout::new(32).unwrap();
        let blocks = layout.touched_blocks(5, 7).unwrap();
        assert_eq!(blocks.len(), 1);
        assert_eq!(blocks[0].block_index, 0);
        assert_eq!(blocks[0].block_base, 0);
        assert_eq!(blocks[0].block_offset, 5);
        assert_eq!(blocks[0].buffer_offset, 0);
        assert_eq!(blocks[0].valid_len, 7);
    }

    #[test]
    fn touched_blocks_maps_cross_block_request_in_order() {
        let layout = BlockLayout::new(32).unwrap();
        let blocks = layout.touched_blocks(28, 40).unwrap();
        assert_eq!(blocks.len(), 3);

        assert_eq!(blocks[0].block_index, 0);
        assert_eq!(blocks[0].block_base, 0);
        assert_eq!(blocks[0].block_offset, 28);
        assert_eq!(blocks[0].buffer_offset, 0);
        assert_eq!(blocks[0].valid_len, 4);

        assert_eq!(blocks[1].block_index, 1);
        assert_eq!(blocks[1].block_base, 32);
        assert_eq!(blocks[1].block_offset, 0);
        assert_eq!(blocks[1].buffer_offset, 4);
        assert_eq!(blocks[1].valid_len, 32);

        assert_eq!(blocks[2].block_index, 2);
        assert_eq!(blocks[2].block_base, 64);
        assert_eq!(blocks[2].block_offset, 0);
        assert_eq!(blocks[2].buffer_offset, 36);
        assert_eq!(blocks[2].valid_len, 4);
    }

    #[test]
    fn touched_blocks_rejects_overflowing_request_end() {
        let layout = BlockLayout::new(32).unwrap();
        let error = layout.touched_blocks(u64::MAX - 3, 8).unwrap_err();
        assert_eq!(
            error,
            CacheError::InvalidRange {
                offset: u64::MAX - 3,
                length: 8,
            }
        );
    }

    #[test]
    fn touched_block_copy_from_block_uses_mapped_ranges() {
        let layout = BlockLayout::new(16).unwrap();
        let block = (0u8..16).collect::<Vec<_>>();
        let mut buffer = [0u8; 10];
        let touched = layout.touched_blocks(3, 10).unwrap();

        touched[0].copy_from_block(&block, &mut buffer).unwrap();
        assert_eq!(&buffer[..], &[3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
    }

    #[test]
    fn touched_block_patch_into_block_uses_mapped_ranges() {
        let layout = BlockLayout::new(16).unwrap();
        let mut block = [0u8; 16];
        let data = [9u8, 8, 7, 6, 5];
        let touched = layout.touched_blocks(6, data.len()).unwrap();

        touched[0].patch_into_block(&data, &mut block).unwrap();
        assert_eq!(&block[6..11], &data);
    }
}
