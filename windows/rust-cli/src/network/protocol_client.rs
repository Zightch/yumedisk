use std::error::Error;
use std::fmt;

pub const PROTOCOL_VERSION: u8 = 1;
pub const HEADER_LEN: u8 = 24;
pub const HEADER_SIZE: usize = 24;
pub const FLAG_RESPONSE: u8 = 1 << 0;
pub const FLAG_NOTICE: u8 = 1 << 1;

pub const DISK_ID_BYTES: usize = 16;
pub const AUTH_SALT_BYTES: usize = 16;
pub const AUTH_PROOF_BYTES: usize = 64;
pub const AUTH_CHALLENGE_TOKEN_MIN_BYTES: usize = 1;
pub const AUTH_ALGO_VERSION_V1: u8 = 1;
pub const AUTH_FINISH_RESPONSE_BYTES: usize = 8;
pub const SESSION_DESCRIBE_RESPONSE_BYTES: usize = 16;
pub const SESSION_CLOSE_NOTICE_BYTES: usize = 2;
pub const READ_WRITE_FIXED_BODY_BYTES: usize = 12;
pub const SESSION_FLAG_READ_ONLY: u16 = 1 << 0;
pub const ABSOLUTE_MAX_IO_BYTES: u32 = 65_500;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ClientOperationCode {
    AuthStart = 0x01,
    AuthFinish = 0x02,
    SessionOpen = 0x03,
    SessionDescribe = 0x04,
    SessionCloseNotice = 0x05,
    ReadAt = 0x10,
    WriteAt = 0x11,
    ConnHeartbeat = 0x12,
    Close = 0x13,
}

impl ClientOperationCode {
    pub fn from_u8(value: u8) -> Result<Self, ProtocolClientError> {
        match value {
            0x01 => Ok(Self::AuthStart),
            0x02 => Ok(Self::AuthFinish),
            0x03 => Ok(Self::SessionOpen),
            0x04 => Ok(Self::SessionDescribe),
            0x05 => Ok(Self::SessionCloseNotice),
            0x10 => Ok(Self::ReadAt),
            0x11 => Ok(Self::WriteAt),
            0x12 => Ok(Self::ConnHeartbeat),
            0x13 => Ok(Self::Close),
            actual => Err(ProtocolClientError::UnexpectedOpCode {
                expected: None,
                actual,
            }),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProtocolStatusCode {
    Ok,
    ErrProtocolVersion,
    ErrBadHeader,
    ErrBadBody,
    ErrInvalidRequest,
    ErrUnsupportedOp,
    ErrAuthFailed,
    ErrAuthExpired,
    ErrAuthChallengeInvalid,
    ErrAuthIdInvalid,
    ErrAuthIdExpired,
    ErrSessionUnavailable,
    ErrSessionBusy,
    ErrIoOutOfRange,
    ErrIoTooLarge,
    ErrIoReadOnly,
    ErrIoFailed,
    Unknown(u16),
}

impl ProtocolStatusCode {
    pub fn code(self) -> u16 {
        match self {
            Self::Ok => 0x0000,
            Self::ErrProtocolVersion => 0x1001,
            Self::ErrBadHeader => 0x1002,
            Self::ErrBadBody => 0x1003,
            Self::ErrInvalidRequest => 0x1004,
            Self::ErrUnsupportedOp => 0x1005,
            Self::ErrAuthFailed => 0x1101,
            Self::ErrAuthExpired => 0x1102,
            Self::ErrAuthChallengeInvalid => 0x1103,
            Self::ErrAuthIdInvalid => 0x1104,
            Self::ErrAuthIdExpired => 0x1105,
            Self::ErrSessionUnavailable => 0x1201,
            Self::ErrSessionBusy => 0x1202,
            Self::ErrIoOutOfRange => 0x1301,
            Self::ErrIoTooLarge => 0x1302,
            Self::ErrIoReadOnly => 0x1303,
            Self::ErrIoFailed => 0x1304,
            Self::Unknown(code) => code,
        }
    }

    pub fn from_u16(value: u16) -> Self {
        match value {
            0x0000 => Self::Ok,
            0x1001 => Self::ErrProtocolVersion,
            0x1002 => Self::ErrBadHeader,
            0x1003 => Self::ErrBadBody,
            0x1004 => Self::ErrInvalidRequest,
            0x1005 => Self::ErrUnsupportedOp,
            0x1101 => Self::ErrAuthFailed,
            0x1102 => Self::ErrAuthExpired,
            0x1103 => Self::ErrAuthChallengeInvalid,
            0x1104 => Self::ErrAuthIdInvalid,
            0x1105 => Self::ErrAuthIdExpired,
            0x1201 => Self::ErrSessionUnavailable,
            0x1202 => Self::ErrSessionBusy,
            0x1301 => Self::ErrIoOutOfRange,
            0x1302 => Self::ErrIoTooLarge,
            0x1303 => Self::ErrIoReadOnly,
            0x1304 => Self::ErrIoFailed,
            code => Self::Unknown(code),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProtocolHeader {
    pub protocol_version: u8,
    pub header_len: u8,
    pub op_code: ClientOperationCode,
    pub flags: u8,
    pub status_code: ProtocolStatusCode,
    pub reserved: u16,
    pub request_id: u64,
    pub session_id: u64,
}

impl ProtocolHeader {
    pub fn new_request(
        op_code: ClientOperationCode,
        request_id: u64,
        session_id: u64,
    ) -> Result<Self, ProtocolClientError> {
        if request_id == 0 {
            return Err(ProtocolClientError::InvalidRequestId(0));
        }

        Ok(Self {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code,
            flags: 0,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id,
            session_id,
        })
    }

    pub fn encode(self, body: &[u8]) -> Vec<u8> {
        let mut payload = vec![0u8; HEADER_SIZE + body.len()];
        payload[0] = self.protocol_version;
        payload[1] = self.header_len;
        payload[2] = self.op_code as u8;
        payload[3] = self.flags;
        payload[4..6].copy_from_slice(&self.status_code.code().to_be_bytes());
        payload[6..8].copy_from_slice(&self.reserved.to_be_bytes());
        payload[8..16].copy_from_slice(&self.request_id.to_be_bytes());
        payload[16..24].copy_from_slice(&self.session_id.to_be_bytes());
        payload[HEADER_SIZE..].copy_from_slice(body);
        payload
    }

    pub fn is_response(self) -> bool {
        self.flags == FLAG_RESPONSE
    }

    pub fn is_notice(self) -> bool {
        self.flags == FLAG_NOTICE
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthStartRequest {
    pub disk_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthStartResponse {
    pub algo_version: u8,
    pub ttl_seconds: u16,
    pub salt: [u8; AUTH_SALT_BYTES],
    pub challenge_token: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthFinishRequest {
    pub challenge_token: Vec<u8>,
    pub proof: [u8; AUTH_PROOF_BYTES],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthFinishResponse {
    pub auth_id: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionOpenRequest {
    pub auth_id: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionOpenResponse {
    pub session_id: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionDescribeRequest {
    pub session_id: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionDescribeResponse {
    pub session_id: u64,
    pub disk_size_bytes: u64,
    pub max_io_bytes: u32,
    pub read_only: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReadAtRequest {
    pub session_id: u64,
    pub offset: u64,
    pub length: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReadAtResponse {
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WriteAtRequest {
    pub session_id: u64,
    pub offset: u64,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnHeartbeatRequest;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnHeartbeatResponse;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CloseRequest {
    pub session_id: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionCloseNotice {
    pub session_id: u64,
    pub reason_code: u16,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProtocolClientError {
    PayloadTooSmall {
        actual: usize,
    },
    InvalidProtocolVersion(u8),
    InvalidHeaderLength(u8),
    ReservedNonZero(u16),
    UnexpectedFlags {
        expected: u8,
        actual: u8,
    },
    UnexpectedOpCode {
        expected: Option<ClientOperationCode>,
        actual: u8,
    },
    UnexpectedRequestId {
        expected: u64,
        actual: u64,
    },
    UnexpectedSessionId {
        expected: Option<u64>,
        actual: u64,
    },
    InvalidRequestId(u64),
    InvalidDiskId,
    InvalidBody(&'static str),
    GatewayStatus(ProtocolStatusCode),
}

impl fmt::Display for ProtocolClientError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::PayloadTooSmall { actual } => write!(formatter, "payload-too-small: {}", actual),
            Self::InvalidProtocolVersion(actual) => {
                write!(formatter, "invalid-protocol-version: {}", actual)
            }
            Self::InvalidHeaderLength(actual) => {
                write!(formatter, "invalid-header-length: {}", actual)
            }
            Self::ReservedNonZero(actual) => write!(formatter, "reserved-non-zero: {}", actual),
            Self::UnexpectedFlags { expected, actual } => {
                write!(
                    formatter,
                    "unexpected-flags: expected={}, actual={}",
                    expected, actual
                )
            }
            Self::UnexpectedOpCode { expected, actual } => match expected {
                Some(expected) => write!(
                    formatter,
                    "unexpected-op-code: expected=0x{:02x}, actual=0x{:02x}",
                    *expected as u8, actual
                ),
                None => write!(formatter, "unexpected-op-code: actual=0x{:02x}", actual),
            },
            Self::UnexpectedRequestId { expected, actual } => write!(
                formatter,
                "unexpected-request-id: expected={}, actual={}",
                expected, actual
            ),
            Self::UnexpectedSessionId { expected, actual } => match expected {
                Some(expected) => write!(
                    formatter,
                    "unexpected-session-id: expected={}, actual={}",
                    expected, actual
                ),
                None => write!(formatter, "unexpected-session-id: actual={}", actual),
            },
            Self::InvalidRequestId(actual) => write!(formatter, "invalid-request-id: {}", actual),
            Self::InvalidDiskId => formatter.write_str("invalid-disk-id"),
            Self::InvalidBody(reason) => write!(formatter, "invalid-body: {}", reason),
            Self::GatewayStatus(status) => {
                write!(formatter, "gateway-status: 0x{:04x}", status.code())
            }
        }
    }
}

impl Error for ProtocolClientError {}

impl AuthStartRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        let body = encode_disk_id(&self.disk_id)?;
        Ok(
            ProtocolHeader::new_request(ClientOperationCode::AuthStart, request_id, 0)?
                .encode(&body),
        )
    }
}

impl AuthStartResponse {
    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
    ) -> Result<Self, ProtocolClientError> {
        let (_, body) = decode_success_response(
            payload,
            ClientOperationCode::AuthStart,
            expected_request_id,
            Some(0),
        )?;
        decode_auth_start_response_body(body)
    }
}

impl AuthFinishRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        if self.challenge_token.len() < AUTH_CHALLENGE_TOKEN_MIN_BYTES
            || self.challenge_token.len() > u16::MAX as usize
        {
            return Err(ProtocolClientError::InvalidBody("challenge_token_len"));
        }

        let mut body = vec![0u8; 2 + self.challenge_token.len() + AUTH_PROOF_BYTES];
        body[0..2].copy_from_slice(&(self.challenge_token.len() as u16).to_be_bytes());
        body[2..2 + self.challenge_token.len()].copy_from_slice(&self.challenge_token);
        body[2 + self.challenge_token.len()..].copy_from_slice(&self.proof);
        Ok(
            ProtocolHeader::new_request(ClientOperationCode::AuthFinish, request_id, 0)?
                .encode(&body),
        )
    }

    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
    ) -> Result<AuthFinishResponse, ProtocolClientError> {
        let (_, body) = decode_success_response(
            payload,
            ClientOperationCode::AuthFinish,
            expected_request_id,
            Some(0),
        )?;
        if body.len() != AUTH_FINISH_RESPONSE_BYTES {
            return Err(ProtocolClientError::InvalidBody("auth_finish_response_len"));
        }

        let auth_id = u64::from_be_bytes(body.try_into().expect("slice length fixed"));
        if auth_id == 0 {
            return Err(ProtocolClientError::InvalidBody("auth_id"));
        }

        Ok(AuthFinishResponse { auth_id })
    }
}

impl SessionOpenRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        if self.auth_id == 0 {
            return Err(ProtocolClientError::InvalidBody("auth_id"));
        }

        Ok(
            ProtocolHeader::new_request(ClientOperationCode::SessionOpen, request_id, 0)?
                .encode(&self.auth_id.to_be_bytes()),
        )
    }
}

impl SessionOpenResponse {
    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
    ) -> Result<Self, ProtocolClientError> {
        let (header, body) = decode_success_response(
            payload,
            ClientOperationCode::SessionOpen,
            expected_request_id,
            None,
        )?;
        if header.session_id == 0 {
            return Err(ProtocolClientError::UnexpectedSessionId {
                expected: None,
                actual: 0,
            });
        }
        if !body.is_empty() {
            return Err(ProtocolClientError::InvalidBody(
                "session_open_response_body",
            ));
        }

        Ok(Self {
            session_id: header.session_id,
        })
    }
}

impl SessionDescribeRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        validate_non_zero_session_id(self.session_id)?;
        Ok(ProtocolHeader::new_request(
            ClientOperationCode::SessionDescribe,
            request_id,
            self.session_id,
        )?
        .encode(&[]))
    }
}

impl SessionDescribeResponse {
    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
        expected_session_id: u64,
    ) -> Result<Self, ProtocolClientError> {
        let (_, body) = decode_success_response(
            payload,
            ClientOperationCode::SessionDescribe,
            expected_request_id,
            Some(expected_session_id),
        )?;
        if body.len() != SESSION_DESCRIBE_RESPONSE_BYTES {
            return Err(ProtocolClientError::InvalidBody(
                "session_describe_response_len",
            ));
        }

        let disk_size_bytes =
            u64::from_be_bytes(body[0..8].try_into().expect("slice length fixed"));
        let max_io_bytes = u32::from_be_bytes(body[8..12].try_into().expect("slice length fixed"));
        let session_flags =
            u16::from_be_bytes(body[12..14].try_into().expect("slice length fixed"));
        let reserved = u16::from_be_bytes(body[14..16].try_into().expect("slice length fixed"));

        if reserved != 0 {
            return Err(ProtocolClientError::InvalidBody(
                "session_describe_reserved",
            ));
        }
        if session_flags & !SESSION_FLAG_READ_ONLY != 0 {
            return Err(ProtocolClientError::InvalidBody("session_flags"));
        }
        if disk_size_bytes == 0 {
            return Err(ProtocolClientError::InvalidBody("disk_size_bytes"));
        }
        if max_io_bytes == 0 || max_io_bytes > ABSOLUTE_MAX_IO_BYTES {
            return Err(ProtocolClientError::InvalidBody("max_io_bytes"));
        }

        Ok(Self {
            session_id: expected_session_id,
            disk_size_bytes,
            max_io_bytes,
            read_only: session_flags & SESSION_FLAG_READ_ONLY != 0,
        })
    }
}

impl ReadAtRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        validate_non_zero_session_id(self.session_id)?;
        validate_io_length(self.length)?;

        let mut body = [0u8; READ_WRITE_FIXED_BODY_BYTES];
        body[0..8].copy_from_slice(&self.offset.to_be_bytes());
        body[8..12].copy_from_slice(&self.length.to_be_bytes());
        Ok(
            ProtocolHeader::new_request(ClientOperationCode::ReadAt, request_id, self.session_id)?
                .encode(&body),
        )
    }
}

impl ReadAtResponse {
    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
        expected_session_id: u64,
        expected_length: u32,
    ) -> Result<Self, ProtocolClientError> {
        let (_, body) = decode_success_response(
            payload,
            ClientOperationCode::ReadAt,
            expected_request_id,
            Some(expected_session_id),
        )?;

        if body.len() != expected_length as usize {
            return Err(ProtocolClientError::InvalidBody("read_response_length"));
        }

        Ok(Self {
            data: body.to_vec(),
        })
    }
}

impl WriteAtRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        validate_non_zero_session_id(self.session_id)?;
        let length = u32::try_from(self.data.len())
            .map_err(|_| ProtocolClientError::InvalidBody("write_length"))?;
        validate_io_length(length)?;

        let mut body = vec![0u8; READ_WRITE_FIXED_BODY_BYTES + self.data.len()];
        body[0..8].copy_from_slice(&self.offset.to_be_bytes());
        body[8..12].copy_from_slice(&length.to_be_bytes());
        body[12..].copy_from_slice(&self.data);
        Ok(
            ProtocolHeader::new_request(ClientOperationCode::WriteAt, request_id, self.session_id)?
                .encode(&body),
        )
    }

    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
        expected_session_id: u64,
    ) -> Result<(), ProtocolClientError> {
        let (_, body) = decode_success_response(
            payload,
            ClientOperationCode::WriteAt,
            expected_request_id,
            Some(expected_session_id),
        )?;
        if !body.is_empty() {
            return Err(ProtocolClientError::InvalidBody("write_response_body"));
        }
        Ok(())
    }
}

impl ConnHeartbeatRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        Ok(
            ProtocolHeader::new_request(ClientOperationCode::ConnHeartbeat, request_id, 0)?
                .encode(&[]),
        )
    }
}

impl ConnHeartbeatResponse {
    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
    ) -> Result<Self, ProtocolClientError> {
        let (_, body) = decode_success_response(
            payload,
            ClientOperationCode::ConnHeartbeat,
            expected_request_id,
            Some(0),
        )?;
        if !body.is_empty() {
            return Err(ProtocolClientError::InvalidBody(
                "conn_heartbeat_response_body",
            ));
        }
        Ok(Self)
    }
}

impl CloseRequest {
    pub fn encode_request(&self, request_id: u64) -> Result<Vec<u8>, ProtocolClientError> {
        validate_non_zero_session_id(self.session_id)?;
        Ok(
            ProtocolHeader::new_request(ClientOperationCode::Close, request_id, self.session_id)?
                .encode(&[]),
        )
    }

    pub fn decode_response(
        payload: &[u8],
        expected_request_id: u64,
        expected_session_id: u64,
    ) -> Result<(), ProtocolClientError> {
        let (_, body) = decode_success_response(
            payload,
            ClientOperationCode::Close,
            expected_request_id,
            Some(expected_session_id),
        )?;
        if !body.is_empty() {
            return Err(ProtocolClientError::InvalidBody("close_response_body"));
        }
        Ok(())
    }
}

impl SessionCloseNotice {
    pub fn decode_notice(payload: &[u8]) -> Result<Self, ProtocolClientError> {
        let header = parse_header(payload)?;
        if header.flags != FLAG_NOTICE {
            return Err(ProtocolClientError::UnexpectedFlags {
                expected: FLAG_NOTICE,
                actual: header.flags,
            });
        }
        if header.reserved != 0 {
            return Err(ProtocolClientError::ReservedNonZero(header.reserved));
        }
        if header.op_code != ClientOperationCode::SessionCloseNotice {
            return Err(ProtocolClientError::UnexpectedOpCode {
                expected: Some(ClientOperationCode::SessionCloseNotice),
                actual: header.op_code as u8,
            });
        }
        if header.status_code != ProtocolStatusCode::Ok {
            return Err(ProtocolClientError::InvalidBody("notice_status_code"));
        }
        if header.request_id != 0 || header.session_id == 0 {
            return Err(ProtocolClientError::InvalidBody(
                "session_close_notice_header",
            ));
        }
        let body = &payload[HEADER_SIZE..];
        if body.len() != SESSION_CLOSE_NOTICE_BYTES {
            return Err(ProtocolClientError::InvalidBody("session_close_notice_len"));
        }
        let reason_code = u16::from_be_bytes(body.try_into().expect("slice length fixed"));
        Ok(Self {
            session_id: header.session_id,
            reason_code,
        })
    }
}

pub fn parse_header(payload: &[u8]) -> Result<ProtocolHeader, ProtocolClientError> {
    if payload.len() < HEADER_SIZE {
        return Err(ProtocolClientError::PayloadTooSmall {
            actual: payload.len(),
        });
    }

    let protocol_version = payload[0];
    if protocol_version != PROTOCOL_VERSION {
        return Err(ProtocolClientError::InvalidProtocolVersion(
            protocol_version,
        ));
    }

    let header_len = payload[1];
    if header_len != HEADER_LEN {
        return Err(ProtocolClientError::InvalidHeaderLength(header_len));
    }

    let op_code = ClientOperationCode::from_u8(payload[2])?;
    let flags = payload[3];
    let status_code = ProtocolStatusCode::from_u16(u16::from_be_bytes(
        payload[4..6].try_into().expect("slice length fixed"),
    ));
    let reserved = u16::from_be_bytes(payload[6..8].try_into().expect("slice length fixed"));
    let request_id = u64::from_be_bytes(payload[8..16].try_into().expect("slice length fixed"));
    let session_id = u64::from_be_bytes(payload[16..24].try_into().expect("slice length fixed"));

    Ok(ProtocolHeader {
        protocol_version,
        header_len,
        op_code,
        flags,
        status_code,
        reserved,
        request_id,
        session_id,
    })
}

pub fn parse_request_header(payload: &[u8]) -> Result<ProtocolHeader, ProtocolClientError> {
    let header = parse_header(payload)?;
    if header.flags != 0 {
        return Err(ProtocolClientError::UnexpectedFlags {
            expected: 0,
            actual: header.flags,
        });
    }
    if header.status_code != ProtocolStatusCode::Ok {
        return Err(ProtocolClientError::InvalidBody("request_status_code"));
    }
    if header.reserved != 0 {
        return Err(ProtocolClientError::ReservedNonZero(header.reserved));
    }
    if header.request_id == 0 {
        return Err(ProtocolClientError::InvalidRequestId(0));
    }
    Ok(header)
}

pub fn decode_gateway_status(payload: &[u8]) -> Result<ProtocolStatusCode, ProtocolClientError> {
    Ok(parse_header(payload)?.status_code)
}

fn decode_success_response<'payload>(
    payload: &'payload [u8],
    expected_op_code: ClientOperationCode,
    expected_request_id: u64,
    expected_session_id: Option<u64>,
) -> Result<(ProtocolHeader, &'payload [u8]), ProtocolClientError> {
    let header = parse_header(payload)?;
    validate_response_header(
        header,
        expected_op_code,
        expected_request_id,
        expected_session_id,
    )?;
    if header.status_code != ProtocolStatusCode::Ok {
        return Err(ProtocolClientError::GatewayStatus(header.status_code));
    }
    Ok((header, &payload[HEADER_SIZE..]))
}

fn validate_response_header(
    header: ProtocolHeader,
    expected_op_code: ClientOperationCode,
    expected_request_id: u64,
    expected_session_id: Option<u64>,
) -> Result<(), ProtocolClientError> {
    if header.flags != FLAG_RESPONSE {
        return Err(ProtocolClientError::UnexpectedFlags {
            expected: FLAG_RESPONSE,
            actual: header.flags,
        });
    }
    if header.reserved != 0 {
        return Err(ProtocolClientError::ReservedNonZero(header.reserved));
    }
    if header.op_code != expected_op_code {
        return Err(ProtocolClientError::UnexpectedOpCode {
            expected: Some(expected_op_code),
            actual: header.op_code as u8,
        });
    }
    if header.request_id != expected_request_id {
        return Err(ProtocolClientError::UnexpectedRequestId {
            expected: expected_request_id,
            actual: header.request_id,
        });
    }
    if let Some(expected_session_id) = expected_session_id {
        if header.session_id != expected_session_id {
            return Err(ProtocolClientError::UnexpectedSessionId {
                expected: Some(expected_session_id),
                actual: header.session_id,
            });
        }
    }
    Ok(())
}

fn encode_disk_id(disk_id: &str) -> Result<[u8; DISK_ID_BYTES], ProtocolClientError> {
    let bytes = disk_id.as_bytes();
    if bytes.len() != DISK_ID_BYTES {
        return Err(ProtocolClientError::InvalidDiskId);
    }
    if !bytes.iter().copied().all(is_ascii_alphanumeric) {
        return Err(ProtocolClientError::InvalidDiskId);
    }

    let mut encoded = [0u8; DISK_ID_BYTES];
    encoded.copy_from_slice(bytes);
    Ok(encoded)
}

fn decode_auth_start_response_body(body: &[u8]) -> Result<AuthStartResponse, ProtocolClientError> {
    if body.len() < 21 {
        return Err(ProtocolClientError::InvalidBody("auth_start_response_len"));
    }

    let algo_version = body[0];
    let ttl_seconds = u16::from_be_bytes(body[1..3].try_into().expect("slice length fixed"));
    if algo_version != AUTH_ALGO_VERSION_V1 {
        return Err(ProtocolClientError::InvalidBody("algo_version"));
    }
    if ttl_seconds == 0 {
        return Err(ProtocolClientError::InvalidBody("ttl_seconds"));
    }

    let mut salt = [0u8; AUTH_SALT_BYTES];
    salt.copy_from_slice(&body[3..19]);

    let challenge_token_len =
        u16::from_be_bytes(body[19..21].try_into().expect("slice length fixed")) as usize;
    if challenge_token_len < AUTH_CHALLENGE_TOKEN_MIN_BYTES {
        return Err(ProtocolClientError::InvalidBody("challenge_token_len"));
    }
    if body.len() != 21 + challenge_token_len {
        return Err(ProtocolClientError::InvalidBody("auth_start_response_len"));
    }

    Ok(AuthStartResponse {
        algo_version,
        ttl_seconds,
        salt,
        challenge_token: body[21..].to_vec(),
    })
}

fn validate_non_zero_session_id(session_id: u64) -> Result<(), ProtocolClientError> {
    if session_id == 0 {
        return Err(ProtocolClientError::UnexpectedSessionId {
            expected: None,
            actual: 0,
        });
    }
    Ok(())
}

fn validate_io_length(length: u32) -> Result<(), ProtocolClientError> {
    if length == 0 || length > ABSOLUTE_MAX_IO_BYTES {
        return Err(ProtocolClientError::InvalidBody("io_length"));
    }
    Ok(())
}

fn is_ascii_alphanumeric(byte: u8) -> bool {
    byte.is_ascii_alphanumeric()
}

#[cfg(test)]
mod tests {
    use super::AUTH_ALGO_VERSION_V1;
    use super::AUTH_PROOF_BYTES;
    use super::AUTH_SALT_BYTES;
    use super::AuthFinishRequest;
    use super::AuthStartRequest;
    use super::AuthStartResponse;
    use super::ClientOperationCode;
    use super::CloseRequest;
    use super::ConnHeartbeatRequest;
    use super::ConnHeartbeatResponse;
    use super::FLAG_NOTICE;
    use super::FLAG_RESPONSE;
    use super::HEADER_LEN;
    use super::HEADER_SIZE;
    use super::PROTOCOL_VERSION;
    use super::ProtocolClientError;
    use super::ProtocolHeader;
    use super::ProtocolStatusCode;
    use super::ReadAtRequest;
    use super::ReadAtResponse;
    use super::SessionCloseNotice;
    use super::SessionDescribeRequest;
    use super::SessionDescribeResponse;
    use super::SessionOpenRequest;
    use super::SessionOpenResponse;
    use super::WriteAtRequest;

    #[test]
    fn auth_start_request_matches_documented_layout() {
        let payload = AuthStartRequest {
            disk_id: "A1b2C3d4E5f6G7h8".to_string(),
        }
        .encode_request(9)
        .expect("encode should succeed");

        assert_eq!(payload.len(), HEADER_SIZE + 16);
        assert_eq!(payload[0], PROTOCOL_VERSION);
        assert_eq!(payload[1], HEADER_LEN);
        assert_eq!(payload[2], ClientOperationCode::AuthStart as u8);
        assert_eq!(&payload[HEADER_SIZE..], b"A1b2C3d4E5f6G7h8");
    }

    #[test]
    fn auth_start_response_decodes_server_shape() {
        let mut body = Vec::new();
        body.push(AUTH_ALGO_VERSION_V1);
        body.extend_from_slice(&123u16.to_be_bytes());
        body.extend_from_slice(&[7u8; AUTH_SALT_BYTES]);
        body.extend_from_slice(&3u16.to_be_bytes());
        body.extend_from_slice(b"tok");
        let payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code: ClientOperationCode::AuthStart,
            flags: FLAG_RESPONSE,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id: 11,
            session_id: 0,
        }
        .encode(&body);

        let decoded =
            AuthStartResponse::decode_response(&payload, 11).expect("decode should succeed");
        assert_eq!(decoded.ttl_seconds, 123);
        assert_eq!(decoded.salt, [7u8; AUTH_SALT_BYTES]);
        assert_eq!(decoded.challenge_token, b"tok");
    }

    #[test]
    fn auth_finish_request_and_response_match_documented_layout() {
        let mut proof = [0u8; AUTH_PROOF_BYTES];
        proof[0] = 0xAA;
        proof[63] = 0xBB;

        let payload = AuthFinishRequest {
            challenge_token: b"token".to_vec(),
            proof,
        }
        .encode_request(77)
        .expect("encode should succeed");

        assert_eq!(payload[2], ClientOperationCode::AuthFinish as u8);
        assert_eq!(&payload[HEADER_SIZE..HEADER_SIZE + 2], &5u16.to_be_bytes());
        assert_eq!(&payload[HEADER_SIZE + 2..HEADER_SIZE + 7], b"token");
        assert_eq!(payload[HEADER_SIZE + 7], 0xAA);
        assert_eq!(payload.last().copied(), Some(0xBB));

        let response_payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code: ClientOperationCode::AuthFinish,
            flags: FLAG_RESPONSE,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id: 77,
            session_id: 0,
        }
        .encode(&42u64.to_be_bytes());
        let response = AuthFinishRequest::decode_response(&response_payload, 77)
            .expect("decode should succeed");
        assert_eq!(response.auth_id, 42);
    }

    #[test]
    fn session_open_request_and_response_follow_new_wire() {
        let payload = SessionOpenRequest { auth_id: 88 }
            .encode_request(12)
            .expect("encode should succeed");
        assert_eq!(payload[2], ClientOperationCode::SessionOpen as u8);
        assert_eq!(&payload[HEADER_SIZE..], &88u64.to_be_bytes());

        let response_payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code: ClientOperationCode::SessionOpen,
            flags: FLAG_RESPONSE,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id: 12,
            session_id: 99,
        }
        .encode(&[]);
        let response = SessionOpenResponse::decode_response(&response_payload, 12)
            .expect("decode should succeed");
        assert_eq!(response.session_id, 99);
    }

    #[test]
    fn session_describe_request_and_response_follow_new_wire() {
        let payload = SessionDescribeRequest { session_id: 9 }
            .encode_request(21)
            .expect("encode should succeed");
        assert_eq!(payload[2], ClientOperationCode::SessionDescribe as u8);
        assert_eq!(payload.len(), HEADER_SIZE);
        assert_eq!(&payload[16..24], &9u64.to_be_bytes());

        let mut body = Vec::new();
        body.extend_from_slice(&4096u64.to_be_bytes());
        body.extend_from_slice(&60_000u32.to_be_bytes());
        body.extend_from_slice(&1u16.to_be_bytes());
        body.extend_from_slice(&0u16.to_be_bytes());
        let response_payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code: ClientOperationCode::SessionDescribe,
            flags: FLAG_RESPONSE,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id: 21,
            session_id: 9,
        }
        .encode(&body);
        let response = SessionDescribeResponse::decode_response(&response_payload, 21, 9)
            .expect("decode should succeed");
        assert_eq!(response.session_id, 9);
        assert_eq!(response.disk_size_bytes, 4096);
        assert_eq!(response.max_io_bytes, 60_000);
        assert!(response.read_only);
    }

    #[test]
    fn read_and_write_requests_match_server_body_layout() {
        let read = ReadAtRequest {
            session_id: 5,
            offset: 8,
            length: 4,
        }
        .encode_request(31)
        .expect("read encode should succeed");
        assert_eq!(&read[HEADER_SIZE..HEADER_SIZE + 8], &8u64.to_be_bytes());
        assert_eq!(
            &read[HEADER_SIZE + 8..HEADER_SIZE + 12],
            &4u32.to_be_bytes()
        );

        let write = WriteAtRequest {
            session_id: 5,
            offset: 8,
            data: b"ABCD".to_vec(),
        }
        .encode_request(32)
        .expect("write encode should succeed");
        assert_eq!(&write[HEADER_SIZE..HEADER_SIZE + 8], &8u64.to_be_bytes());
        assert_eq!(
            &write[HEADER_SIZE + 8..HEADER_SIZE + 12],
            &4u32.to_be_bytes()
        );
        assert_eq!(&write[HEADER_SIZE + 12..], b"ABCD");
    }

    #[test]
    fn conn_heartbeat_and_close_follow_documented_shapes() {
        let heartbeat_payload = ConnHeartbeatRequest
            .encode_request(21)
            .expect("heartbeat encode should succeed");
        assert_eq!(heartbeat_payload.len(), HEADER_SIZE);
        assert_eq!(
            heartbeat_payload[2],
            ClientOperationCode::ConnHeartbeat as u8
        );
        assert_eq!(&heartbeat_payload[16..24], &0u64.to_be_bytes());

        let response_payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code: ClientOperationCode::ConnHeartbeat,
            flags: FLAG_RESPONSE,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id: 21,
            session_id: 0,
        }
        .encode(&[]);
        ConnHeartbeatResponse::decode_response(&response_payload, 21)
            .expect("heartbeat decode should succeed");

        let close_payload = CloseRequest { session_id: 7 }
            .encode_request(22)
            .expect("close encode should succeed");
        assert_eq!(close_payload.len(), HEADER_SIZE);
    }

    #[test]
    fn session_close_notice_requires_notice_flag_and_zero_request_id() {
        let payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code: ClientOperationCode::SessionCloseNotice,
            flags: FLAG_NOTICE,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id: 0,
            session_id: 77,
        }
        .encode(&1u16.to_be_bytes());

        let decoded = SessionCloseNotice::decode_notice(&payload).expect("decode should succeed");
        assert_eq!(decoded.session_id, 77);
        assert_eq!(decoded.reason_code, 1);
    }

    #[test]
    fn gateway_error_status_is_mapped_without_body_guessing() {
        let payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: HEADER_LEN,
            op_code: ClientOperationCode::ReadAt,
            flags: FLAG_RESPONSE,
            status_code: ProtocolStatusCode::ErrIoOutOfRange,
            reserved: 0,
            request_id: 41,
            session_id: 9,
        }
        .encode(&[]);

        let error =
            ReadAtResponse::decode_response(&payload, 41, 9, 16).expect_err("decode should fail");
        assert_eq!(
            error,
            ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoOutOfRange)
        );
    }
}
