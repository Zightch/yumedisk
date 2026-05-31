use std::error::Error;
use std::fmt::{self, Display, Formatter};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CacheError {
    InvalidConfig(&'static str),
    InvalidRange {
        offset: u64,
        length: usize,
    },
    BufferTooSmall {
        context: &'static str,
        expected: usize,
        actual: usize,
    },
    ArithmeticOverflow(&'static str),
    InvalidBlockDataLength {
        expected: usize,
        actual: usize,
    },
    InvalidValidLength {
        valid_len: usize,
        block_size: usize,
    },
    MisalignedRightIo {
        offset: u64,
        length: usize,
        block_size: usize,
    },
    ResidentBlockAlreadyExists {
        block_index: u64,
    },
    InvariantViolation(&'static str),
    NotImplemented,
}

impl Display for CacheError {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidConfig(reason) => write!(f, "invalid cache config: {reason}"),
            Self::InvalidRange { offset, length } => {
                write!(f, "invalid request range: offset={offset}, length={length}")
            }
            Self::BufferTooSmall {
                context,
                expected,
                actual,
            } => write!(
                f,
                "buffer too small for {context}: expected at least {expected} bytes, got {actual}"
            ),
            Self::ArithmeticOverflow(context) => {
                write!(f, "arithmetic overflow while computing {context}")
            }
            Self::InvalidBlockDataLength { expected, actual } => write!(
                f,
                "invalid resident block data length: expected {expected} bytes, got {actual}"
            ),
            Self::InvalidValidLength {
                valid_len,
                block_size,
            } => write!(
                f,
                "invalid resident valid length: valid_len={valid_len}, block_size={block_size}"
            ),
            Self::MisalignedRightIo {
                offset,
                length,
                block_size,
            } => write!(
                f,
                "right-side io must be block aligned: offset={offset}, length={length}, block_size={block_size}"
            ),
            Self::ResidentBlockAlreadyExists { block_index } => {
                write!(
                    f,
                    "resident block already exists: block_index={block_index}"
                )
            }
            Self::InvariantViolation(reason) => write!(f, "cache invariant violation: {reason}"),
            Self::NotImplemented => write!(f, "cache operation is not implemented"),
        }
    }
}

impl Error for CacheError {}
