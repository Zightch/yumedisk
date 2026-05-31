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
    MisalignedRightIo {
        offset: u64,
        length: usize,
        block_size: usize,
    },
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
            Self::MisalignedRightIo {
                offset,
                length,
                block_size,
            } => write!(
                f,
                "right-side io must be block aligned: offset={offset}, length={length}, block_size={block_size}"
            ),
            Self::NotImplemented => write!(f, "cache operation is not implemented"),
        }
    }
}

impl Error for CacheError {}
