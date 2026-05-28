#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LocalBindingKind {
    Dedicated,
    Shared { smid: u64 },
}
