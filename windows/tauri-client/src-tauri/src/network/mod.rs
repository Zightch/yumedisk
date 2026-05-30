use std::sync::Mutex;
use std::sync::MutexGuard;

use crate::state::network_client::NetworkClientState;

pub mod cleanup;
pub mod draft_flow;
pub mod event_reconciler;
pub mod gateway_ops;
pub mod runtime_flow;
pub mod uniqueness;
pub mod validation;

pub(crate) const NETWORK_SESSION_MISSING_REASON: &str = "网络盘会话未打开";
pub(crate) const NETWORK_CONNECTION_UNAVAILABLE_REASON: &str = "网络盘连接不可用";
pub(crate) const NETWORK_AUTH_FAILED_REASON: &str = "网络盘认证失败";
pub(crate) const NETWORK_OPEN_FAILED_REASON: &str = "网络盘会话打开失败";
pub(crate) const NETWORK_METADATA_FAILED_REASON: &str = "网络盘元数据获取失败";
pub(crate) const NETWORK_AUTH_MISMATCH_REASON: &str = "认证材料与网络盘不匹配";
pub(crate) const NETWORK_BACKEND_CONFLICT_REASON: &str = "网络盘后端冲突";

pub(crate) fn lock_network_client(
    network_client_mutex: &Mutex<NetworkClientState>,
) -> MutexGuard<'_, NetworkClientState> {
    network_client_mutex
        .lock()
        .expect("network client mutex should not be poisoned")
}
