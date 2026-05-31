use std::path::PathBuf;

use crate::temp::TempStore;

#[derive(Debug, Clone)]
pub(crate) struct CacheDeps {
    temp: TempStore,
    #[cfg(any(test, feature = "test-hooks"))]
    hooks: crate::test_support::TestHooks,
}

impl CacheDeps {
    pub(crate) fn new(temp_dir: PathBuf) -> Self {
        Self {
            temp: TempStore::new(temp_dir),
            #[cfg(any(test, feature = "test-hooks"))]
            hooks: crate::test_support::TestHooks::default(),
        }
    }

    pub(crate) fn temp(&self) -> &TempStore {
        &self.temp
    }

    pub(crate) fn temp_clone(&self) -> TempStore {
        self.temp.clone()
    }

    #[cfg(any(test, feature = "test-hooks"))]
    pub(crate) fn with_test_hooks(mut self, hooks: crate::test_support::TestHooks) -> Self {
        self.hooks = hooks;
        self
    }

    #[cfg(any(test, feature = "test-hooks"))]
    pub(crate) fn hooks(&self) -> &crate::test_support::TestHooks {
        &self.hooks
    }
}
