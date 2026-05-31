use std::collections::HashMap;
use std::fmt;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

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

#[derive(Debug, Default)]
pub struct ManualGateController {
    state: Mutex<HashMap<HookPoint, GateState>>,
    state_changed: Condvar,
}

#[derive(Debug, Clone, Copy, Default)]
struct GateState {
    arrived: usize,
    released: usize,
    open: bool,
}

impl ManualGateController {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn wait_until_reached(
        &self,
        point: HookPoint,
        expected_arrivals: usize,
        timeout: Duration,
    ) -> bool {
        let deadline = Instant::now() + timeout;
        let mut state = self.state.lock().unwrap();
        loop {
            let arrivals = state.get(&point).map_or(0, |gate| gate.arrived);
            if arrivals >= expected_arrivals {
                return true;
            }
            let now = Instant::now();
            if now >= deadline {
                return false;
            }
            let wait_result = self
                .state_changed
                .wait_timeout(state, deadline.saturating_duration_since(now))
                .unwrap();
            state = wait_result.0;
        }
    }

    pub fn arrival_count(&self, point: HookPoint) -> usize {
        self.state
            .lock()
            .unwrap()
            .get(&point)
            .map_or(0, |gate| gate.arrived)
    }

    pub fn release_one(&self, point: HookPoint) {
        let mut state = self.state.lock().unwrap();
        let gate = state.entry(point).or_default();
        gate.released += 1;
        drop(state);
        self.state_changed.notify_all();
    }

    pub fn release_all_current(&self, point: HookPoint) {
        let mut state = self.state.lock().unwrap();
        let gate = state.entry(point).or_default();
        gate.released = gate.arrived;
        drop(state);
        self.state_changed.notify_all();
    }

    pub fn open(&self, point: HookPoint) {
        let mut state = self.state.lock().unwrap();
        state.entry(point).or_default().open = true;
        drop(state);
        self.state_changed.notify_all();
    }
}

impl GateHook for ManualGateController {
    fn reach(&self, point: HookPoint) {
        let mut state = self.state.lock().unwrap();
        let ordinal = {
            let gate = state.entry(point).or_default();
            gate.arrived += 1;
            gate.arrived
        };
        self.state_changed.notify_all();

        while !state.get(&point).is_some_and(|gate| gate.open)
            && state.get(&point).map_or(0, |gate| gate.released) < ordinal
        {
            state = self.state_changed.wait(state).unwrap();
        }
    }
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

#[cfg(test)]
mod tests {
    use std::sync::Arc;
    use std::thread;
    use std::time::Duration;

    use super::{GateHook, HookPoint, ManualGateController};

    #[test]
    fn manual_gate_controller_blocks_until_released() {
        let gate = Arc::new(ManualGateController::new());
        let worker_gate = Arc::clone(&gate);
        let worker = thread::spawn(move || {
            worker_gate.reach(HookPoint::BeforeRightRead);
        });

        assert!(gate.wait_until_reached(HookPoint::BeforeRightRead, 1, Duration::from_secs(1)));
        assert_eq!(gate.arrival_count(HookPoint::BeforeRightRead), 1);
        gate.release_one(HookPoint::BeforeRightRead);
        worker.join().unwrap();
    }

    #[test]
    fn manual_gate_controller_open_makes_future_arrivals_pass_through() {
        let gate = Arc::new(ManualGateController::new());
        gate.open(HookPoint::AfterRightRead);

        let worker_gate = Arc::clone(&gate);
        let worker = thread::spawn(move || {
            worker_gate.reach(HookPoint::AfterRightRead);
        });

        worker.join().unwrap();
        assert_eq!(gate.arrival_count(HookPoint::AfterRightRead), 1);
    }
}
