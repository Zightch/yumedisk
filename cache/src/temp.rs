use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use crate::CacheError;
#[cfg(any(test, feature = "test-hooks"))]
use crate::test_support::{TempFailureController, TempFaultOperation};
#[cfg(not(any(test, feature = "test-hooks")))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TempFaultOperation {
    Write,
    Read,
    Delete,
}

#[derive(Debug, Clone)]
pub(crate) struct TempStore {
    root: PathBuf,
    #[cfg(any(test, feature = "test-hooks"))]
    failures: TempFailureController,
}

impl TempStore {
    pub(crate) fn new(root: PathBuf) -> Self {
        Self {
            root,
            #[cfg(any(test, feature = "test-hooks"))]
            failures: TempFailureController::default(),
        }
    }

    #[cfg(any(test, feature = "test-hooks"))]
    pub(crate) fn with_failures(mut self, failures: TempFailureController) -> Self {
        self.failures = failures;
        self
    }

    pub(crate) fn path_for_block(&self, block_index: u64) -> PathBuf {
        self.root.join(format!("block-{block_index}.temp"))
    }

    pub(crate) fn write_block(&self, block_index: u64, data: &[u8]) -> Result<(), CacheError> {
        let final_path = self.path_for_block(block_index);
        let pending_path = self.pending_path_for_block(block_index);
        let backup_path = self.backup_path_for_block(block_index);
        self.inject_failure(
            TempFaultOperation::Write,
            block_index,
            &pending_path,
            "write temp file",
        )?;

        self.remove_if_exists(&pending_path)?;
        self.remove_if_exists(&backup_path)?;
        self.write_pending_file(&pending_path, data)?;

        let final_exists = final_path.exists();
        if final_exists {
            fs::rename(&final_path, &backup_path).map_err(|error| CacheError::TempIo {
                operation: "rename temp file to backup",
                path: final_path.clone(),
                kind: error.kind(),
            })?;
        }

        match fs::rename(&pending_path, &final_path) {
            Ok(()) => {
                let _ = self.remove_if_exists(&backup_path);
                Ok(())
            }
            Err(error) => {
                if final_exists {
                    let _ = fs::rename(&backup_path, &final_path);
                }
                let _ = self.remove_if_exists(&pending_path);
                Err(CacheError::TempIo {
                    operation: "rename pending temp file into place",
                    path: pending_path,
                    kind: error.kind(),
                })
            }
        }
    }

    pub(crate) fn read_block(&self, block_index: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        let path = self.path_for_block(block_index);
        self.inject_failure(
            TempFaultOperation::Read,
            block_index,
            &path,
            "read temp file",
        )?;
        let mut file = File::open(&path).map_err(|error| CacheError::TempIo {
            operation: "open temp file for read",
            path: path.clone(),
            kind: error.kind(),
        })?;
        file.read_exact(buffer).map_err(|error| CacheError::TempIo {
            operation: "read temp file",
            path,
            kind: error.kind(),
        })
    }

    #[allow(dead_code)]
    pub(crate) fn remove_block(&self, block_index: u64) -> Result<(), CacheError> {
        let path = self.path_for_block(block_index);
        self.inject_failure(
            TempFaultOperation::Delete,
            block_index,
            &path,
            "remove temp file",
        )?;
        self.remove_if_exists(&path)
    }

    fn pending_path_for_block(&self, block_index: u64) -> PathBuf {
        self.root.join(format!("block-{block_index}.temp.write"))
    }

    fn backup_path_for_block(&self, block_index: u64) -> PathBuf {
        self.root.join(format!("block-{block_index}.temp.prev"))
    }

    fn write_pending_file(&self, path: &Path, data: &[u8]) -> Result<(), CacheError> {
        let mut file = File::create(path).map_err(|error| CacheError::TempIo {
            operation: "create temp file",
            path: path.to_path_buf(),
            kind: error.kind(),
        })?;
        if let Err(error) = file.write_all(data) {
            let _ = self.remove_if_exists(path);
            return Err(CacheError::TempIo {
                operation: "write temp file",
                path: path.to_path_buf(),
                kind: error.kind(),
            });
        }
        if let Err(error) = file.sync_all() {
            let _ = self.remove_if_exists(path);
            return Err(CacheError::TempIo {
                operation: "sync temp file",
                path: path.to_path_buf(),
                kind: error.kind(),
            });
        }

        Ok(())
    }

    fn remove_if_exists(&self, path: &Path) -> Result<(), CacheError> {
        match fs::remove_file(path) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(CacheError::TempIo {
                operation: "remove temp file",
                path: path.to_path_buf(),
                kind: error.kind(),
            }),
        }
    }

    #[cfg(any(test, feature = "test-hooks"))]
    fn inject_failure(
        &self,
        operation: TempFaultOperation,
        block_index: u64,
        path: &Path,
        label: &'static str,
    ) -> Result<(), CacheError> {
        match self.failures.maybe_fail(operation, block_index) {
            Some(kind) => Err(CacheError::TempIo {
                operation: label,
                path: path.to_path_buf(),
                kind,
            }),
            None => Ok(()),
        }
    }

    #[cfg(not(any(test, feature = "test-hooks")))]
    fn inject_failure(
        &self,
        _operation: TempFaultOperation,
        _block_index: u64,
        _path: &Path,
        _label: &'static str,
    ) -> Result<(), CacheError> {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::io::ErrorKind;

    use super::TempStore;
    use crate::CacheError;
    use crate::test_support::{TempFailureController, TempFaultOperation, TestTempDir};

    #[test]
    fn write_failure_is_injected_once_then_recovers() {
        let dir = TestTempDir::with_prefix("cache-temp-write-fault");
        let failures = TempFailureController::new();
        failures.fail_once(
            TempFaultOperation::Write,
            Some(0),
            ErrorKind::PermissionDenied,
        );
        let store = TempStore::new(dir.path().to_path_buf()).with_failures(failures);

        let error = store.write_block(0, &[7u8; 32]).unwrap_err();
        assert_eq!(
            error,
            CacheError::TempIo {
                operation: "write temp file",
                path: store.pending_path_for_block(0),
                kind: ErrorKind::PermissionDenied,
            }
        );
        assert!(!store.path_for_block(0).exists());

        store.write_block(0, &[8u8; 32]).unwrap();
        assert!(store.path_for_block(0).exists());
    }

    #[test]
    fn read_failure_persistent_rule_can_be_cleared() {
        let dir = TestTempDir::with_prefix("cache-temp-read-fault");
        let failures = TempFailureController::new();
        let rule_id =
            failures.fail_persistently(TempFaultOperation::Read, Some(0), ErrorKind::Interrupted);
        let store = TempStore::new(dir.path().to_path_buf()).with_failures(failures.clone());
        let data = [9u8; 32];
        let mut buffer = [0u8; 32];

        store.write_block(0, &data).unwrap();
        let error = store.read_block(0, &mut buffer).unwrap_err();

        assert_eq!(
            error,
            CacheError::TempIo {
                operation: "read temp file",
                path: store.path_for_block(0),
                kind: ErrorKind::Interrupted,
            }
        );
        assert_eq!(buffer, [0u8; 32]);

        assert!(failures.clear_rule(rule_id));
        store.read_block(0, &mut buffer).unwrap();
        assert_eq!(buffer, data);
    }

    #[test]
    fn delete_failure_keeps_file_until_rule_is_cleared() {
        let dir = TestTempDir::with_prefix("cache-temp-delete-fault");
        let failures = TempFailureController::new();
        let rule_id = failures.fail_persistently(
            TempFaultOperation::Delete,
            Some(0),
            ErrorKind::PermissionDenied,
        );
        let store = TempStore::new(dir.path().to_path_buf()).with_failures(failures.clone());

        store.write_block(0, &[5u8; 32]).unwrap();
        let error = store.remove_block(0).unwrap_err();

        assert_eq!(
            error,
            CacheError::TempIo {
                operation: "remove temp file",
                path: store.path_for_block(0),
                kind: ErrorKind::PermissionDenied,
            }
        );
        assert!(store.path_for_block(0).exists());

        assert!(failures.clear_rule(rule_id));
        store.remove_block(0).unwrap();
        assert!(!store.path_for_block(0).exists());
    }
}
