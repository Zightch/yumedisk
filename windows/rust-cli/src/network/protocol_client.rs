#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum ClientOperationCode {
    AuthStart = 0x0001,
    AuthFinish = 0x0002,
    SessionOpen = 0x0010,
    ReadAt = 0x0020,
    WriteAt = 0x0021,
    Ping = 0x0030,
    Close = 0x0031,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthStartRequest {
    pub disk_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthFinishRequest {
    pub disk_id: String,
    pub proof: [u8; 64],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionOpenRequest {
    pub disk_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionOpenResponse {
    pub session_id: u64,
    pub disk_size_bytes: u64,
    pub read_only: bool,
    pub max_io_bytes: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReadAtRequest {
    pub session_id: u64,
    pub offset: u64,
    pub length: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WriteAtRequest {
    pub session_id: u64,
    pub offset: u64,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PingRequest {
    pub session_id: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CloseRequest {
    pub session_id: u64,
}
