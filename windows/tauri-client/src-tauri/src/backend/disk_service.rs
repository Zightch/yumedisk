use backend_rust::BackendContext;
use backend_rust::ManagedDiskSnapshot;

pub fn query_managed_disks(backend: &BackendContext) -> Vec<ManagedDiskSnapshot> {
    backend.snapshot_managed_disks()
}
