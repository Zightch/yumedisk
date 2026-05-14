pub fn query_component_versions(
    backend: &backend_rust::BackendContext,
) -> backend_rust::ComponentVersionSnapshot {
    backend.query_component_version_snapshot()
}
