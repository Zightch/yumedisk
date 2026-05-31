#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LocalBindingKind {
    Dedicated { smid: u64 },
    Shared { smid: u64 },
}
