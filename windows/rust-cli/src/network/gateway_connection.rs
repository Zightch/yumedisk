use std::collections::HashMap;
use std::collections::HashSet;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;

use super::transport_client::TransportClient;
use super::transport_client::TransportEndpoint;

#[derive(Debug)]
pub struct GatewayConnection {
    endpoint: TransportEndpoint,
    transport: TransportClient,
    next_request_id: AtomicU32,
    pending_requests: Mutex<HashMap<u32, PendingRequest>>,
    authorized_disk_ids: Mutex<HashSet<String>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PendingRequest {
    pub opcode: u16,
}

impl GatewayConnection {
    pub fn new(endpoint: TransportEndpoint) -> Arc<Self> {
        Arc::new(Self {
            transport: TransportClient::new(endpoint.clone()),
            endpoint,
            next_request_id: AtomicU32::new(1),
            pending_requests: Mutex::new(HashMap::new()),
            authorized_disk_ids: Mutex::new(HashSet::new()),
        })
    }

    pub fn endpoint(&self) -> &TransportEndpoint {
        &self.endpoint
    }

    pub fn transport(&self) -> &TransportClient {
        &self.transport
    }

    pub fn allocate_request_id(&self) -> u32 {
        self.next_request_id.fetch_add(1, Ordering::Relaxed)
    }

    pub fn insert_pending_request(&self, request_id: u32, pending: PendingRequest) {
        self.pending_requests
            .lock()
            .expect("pending_requests poisoned")
            .insert(request_id, pending);
    }

    pub fn remove_pending_request(&self, request_id: u32) -> Option<PendingRequest> {
        self.pending_requests
            .lock()
            .expect("pending_requests poisoned")
            .remove(&request_id)
    }

    pub fn mark_authorized(&self, disk_id: impl Into<String>) {
        self.authorized_disk_ids
            .lock()
            .expect("authorized_disk_ids poisoned")
            .insert(disk_id.into());
    }

    pub fn is_authorized(&self, disk_id: &str) -> bool {
        self.authorized_disk_ids
            .lock()
            .expect("authorized_disk_ids poisoned")
            .contains(disk_id)
    }
}
