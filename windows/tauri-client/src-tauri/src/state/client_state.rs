use backend_rust::BackendContext;

pub struct ClientState {
    pub backend: BackendContext,
}

impl Default for ClientState {
    fn default() -> Self {
        Self {
            backend: BackendContext::new(),
        }
    }
}
