use std::io::ErrorKind;
use std::sync::{Arc, Mutex};

use super::io::IoOperation;
use crate::CacheError;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FailureRuleId(u64);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TempFaultOperation {
    Write,
    Read,
    Delete,
}

#[derive(Debug, Clone)]
pub struct IoFailureController {
    inner: Arc<Mutex<RuleStore<IoOperation, CacheError>>>,
}

#[derive(Debug, Clone)]
pub struct TempFailureController {
    inner: Arc<Mutex<RuleStore<TempFaultOperation, ErrorKind>>>,
}

#[derive(Debug)]
struct RuleStore<Op, Err> {
    next_rule_id: u64,
    rules: Vec<RuleState<Op, Err>>,
}

#[derive(Debug)]
struct RuleState<Op, Err> {
    id: FailureRuleId,
    operation: Op,
    block_index: Option<u64>,
    trigger_on_match: usize,
    persistent: bool,
    seen_matches: usize,
    error: Err,
}

impl IoFailureController {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn fail_once(
        &self,
        operation: IoOperation,
        block_index: Option<u64>,
        error: CacheError,
    ) -> FailureRuleId {
        self.fail_matching_call(operation, block_index, 1, error)
    }

    pub fn fail_matching_call(
        &self,
        operation: IoOperation,
        block_index: Option<u64>,
        matching_call: usize,
        error: CacheError,
    ) -> FailureRuleId {
        self.inner
            .lock()
            .unwrap()
            .add_rule(operation, block_index, matching_call, false, error)
    }

    pub fn fail_persistently(
        &self,
        operation: IoOperation,
        block_index: Option<u64>,
        error: CacheError,
    ) -> FailureRuleId {
        self.inner
            .lock()
            .unwrap()
            .add_rule(operation, block_index, 1, true, error)
    }

    pub fn clear_rule(&self, rule_id: FailureRuleId) -> bool {
        self.inner.lock().unwrap().clear_rule(rule_id)
    }

    pub fn clear_all(&self) {
        self.inner.lock().unwrap().clear_all();
    }

    pub(crate) fn maybe_fail(
        &self,
        operation: IoOperation,
        block_index: u64,
    ) -> Option<CacheError> {
        self.inner
            .lock()
            .unwrap()
            .maybe_fail(operation, block_index)
    }
}

impl TempFailureController {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn fail_once(
        &self,
        operation: TempFaultOperation,
        block_index: Option<u64>,
        kind: ErrorKind,
    ) -> FailureRuleId {
        self.fail_matching_call(operation, block_index, 1, kind)
    }

    pub fn fail_matching_call(
        &self,
        operation: TempFaultOperation,
        block_index: Option<u64>,
        matching_call: usize,
        kind: ErrorKind,
    ) -> FailureRuleId {
        self.inner
            .lock()
            .unwrap()
            .add_rule(operation, block_index, matching_call, false, kind)
    }

    pub fn fail_persistently(
        &self,
        operation: TempFaultOperation,
        block_index: Option<u64>,
        kind: ErrorKind,
    ) -> FailureRuleId {
        self.inner
            .lock()
            .unwrap()
            .add_rule(operation, block_index, 1, true, kind)
    }

    pub fn clear_rule(&self, rule_id: FailureRuleId) -> bool {
        self.inner.lock().unwrap().clear_rule(rule_id)
    }

    pub fn clear_all(&self) {
        self.inner.lock().unwrap().clear_all();
    }

    pub(crate) fn maybe_fail(
        &self,
        operation: TempFaultOperation,
        block_index: u64,
    ) -> Option<ErrorKind> {
        self.inner
            .lock()
            .unwrap()
            .maybe_fail(operation, block_index)
    }
}

impl<Op, Err> RuleStore<Op, Err>
where
    Op: Copy + Eq,
    Err: Clone,
{
    fn add_rule(
        &mut self,
        operation: Op,
        block_index: Option<u64>,
        trigger_on_match: usize,
        persistent: bool,
        error: Err,
    ) -> FailureRuleId {
        assert!(
            trigger_on_match > 0,
            "trigger_on_match must be greater than 0"
        );
        let rule_id = FailureRuleId(self.next_rule_id);
        self.next_rule_id += 1;
        self.rules.push(RuleState {
            id: rule_id,
            operation,
            block_index,
            trigger_on_match,
            persistent,
            seen_matches: 0,
            error,
        });
        rule_id
    }

    fn clear_rule(&mut self, rule_id: FailureRuleId) -> bool {
        let before_len = self.rules.len();
        self.rules.retain(|rule| rule.id != rule_id);
        self.rules.len() != before_len
    }

    fn clear_all(&mut self) {
        self.rules.clear();
    }

    fn maybe_fail(&mut self, operation: Op, block_index: u64) -> Option<Err> {
        let mut failed = None;
        let mut remove_index = None;
        for (index, rule) in self.rules.iter_mut().enumerate() {
            if rule.operation != operation {
                continue;
            }
            if let Some(expected_block_index) = rule.block_index {
                if expected_block_index != block_index {
                    continue;
                }
            }

            rule.seen_matches += 1;
            if rule.seen_matches < rule.trigger_on_match {
                continue;
            }

            failed = Some(rule.error.clone());
            if !rule.persistent {
                remove_index = Some(index);
            }
            break;
        }

        if let Some(index) = remove_index {
            self.rules.remove(index);
        }
        failed
    }
}

impl<Op, Err> Default for RuleStore<Op, Err> {
    fn default() -> Self {
        Self {
            next_rule_id: 1,
            rules: Vec::new(),
        }
    }
}

impl Default for IoFailureController {
    fn default() -> Self {
        Self {
            inner: Arc::new(Mutex::new(RuleStore::default())),
        }
    }
}

impl Default for TempFailureController {
    fn default() -> Self {
        Self {
            inner: Arc::new(Mutex::new(RuleStore::default())),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::io::ErrorKind;

    use super::{IoFailureController, TempFailureController, TempFaultOperation};
    use crate::{CacheError, test_support::IoOperation};

    #[test]
    fn io_failure_controller_fails_only_on_requested_matching_call() {
        let failures = IoFailureController::new();
        failures.fail_matching_call(IoOperation::Read, Some(7), 2, CacheError::NotImplemented);

        assert_eq!(failures.maybe_fail(IoOperation::Read, 7), None);
        assert_eq!(
            failures.maybe_fail(IoOperation::Read, 7),
            Some(CacheError::NotImplemented)
        );
        assert_eq!(failures.maybe_fail(IoOperation::Read, 7), None);
    }

    #[test]
    fn temp_failure_controller_persistent_rule_can_be_cleared() {
        let failures = TempFailureController::new();
        let rule_id = failures.fail_persistently(
            TempFaultOperation::Delete,
            Some(3),
            ErrorKind::PermissionDenied,
        );

        assert_eq!(
            failures.maybe_fail(TempFaultOperation::Delete, 3),
            Some(ErrorKind::PermissionDenied)
        );
        assert_eq!(
            failures.maybe_fail(TempFaultOperation::Delete, 3),
            Some(ErrorKind::PermissionDenied)
        );
        assert!(failures.clear_rule(rule_id));
        assert_eq!(failures.maybe_fail(TempFaultOperation::Delete, 3), None);
    }
}
