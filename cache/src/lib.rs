mod block;
mod cache;
#[cfg(test)]
mod cache_p2_tests;
mod config;
mod deps;
mod error;
mod io;
mod resident;
mod temp;
#[cfg(any(test, feature = "test-hooks"))]
pub mod test_support;

pub use cache::Cache;
pub use config::CacheConfig;
pub use error::CacheError;
pub use error::RightIoErrorKind;
pub use io::AtIo;
