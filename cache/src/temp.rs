use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use crate::CacheError;

#[derive(Debug, Clone)]
pub(crate) struct TempStore {
    root: PathBuf,
}

impl TempStore {
    pub(crate) fn new(root: PathBuf) -> Self {
        Self { root }
    }

    pub(crate) fn path_for_block(&self, block_index: u64) -> PathBuf {
        self.root.join(format!("block-{block_index}.temp"))
    }

    pub(crate) fn write_block(&self, block_index: u64, data: &[u8]) -> Result<(), CacheError> {
        let final_path = self.path_for_block(block_index);
        let pending_path = self.pending_path_for_block(block_index);
        let backup_path = self.backup_path_for_block(block_index);

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
}
