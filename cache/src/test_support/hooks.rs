use std::fmt;
use std::sync::Arc;

use super::CacheSnapshot;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum HookPoint {
    DebugSnapshot,
    BeforeRightRead,
    AfterRightRead,
    BeforeRightWrite,
    AfterRightWrite,
    BeforeDirtyVictimSpillTempWrite,
    AfterDirtyVictimSpillTempWrite,
}

pub trait GateHook: Send + Sync {
    fn reach(&self, point: HookPoint);
}

pub trait StateDumpHook: Send + Sync {
    fn observe(&self, point: HookPoint, snapshot: &CacheSnapshot);
}

#[derive(Default)]
struct NoopGateHook;

impl GateHook for NoopGateHook {
    fn reach(&self, _point: HookPoint) {}
}

#[derive(Default)]
struct NoopStateDumpHook;

impl StateDumpHook for NoopStateDumpHook {
    fn observe(&self, _point: HookPoint, _snapshot: &CacheSnapshot) {}
}

#[derive(Clone)]
pub struct TestHooks {
    gate: Arc<dyn GateHook>,
    state_dump: Arc<dyn StateDumpHook>,
}

impl TestHooks {
    pub fn disabled() -> Self {
        Self::default()
    }

    pub fn new(gate: Arc<dyn GateHook>, state_dump: Arc<dyn StateDumpHook>) -> Self {
        Self { gate, state_dump }
    }

    pub fn with_gate(gate: Arc<dyn GateHook>) -> Self {
        Self {
            gate,
            state_dump: Arc::new(NoopStateDumpHook),
        }
    }

    pub fn with_state_dump(state_dump: Arc<dyn StateDumpHook>) -> Self {
        Self {
            gate: Arc::new(NoopGateHook),
            state_dump,
        }
    }

    pub(crate) fn reach_gate(&self, point: HookPoint) {
        self.gate.reach(point);
    }

    pub(crate) fn observe_state(&self, point: HookPoint, snapshot: &CacheSnapshot) {
        self.state_dump.observe(point, snapshot);
    }
}

impl Default for TestHooks {
    fn default() -> Self {
        Self {
            gate: Arc::new(NoopGateHook),
            state_dump: Arc::new(NoopStateDumpHook),
        }
    }
}

impl fmt::Debug for TestHooks {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("TestHooks(..)")
    }
}
