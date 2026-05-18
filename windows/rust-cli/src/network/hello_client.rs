use std::fmt;
use std::io::Read;
use std::io::Write;
use std::net::TcpStream;

pub const HELLO_VERSION: u8 = 1;
const HEADER_SIZE: usize = 12;
const MAX_BODY_BYTES: usize = 64 * 1024;

const MESSAGE_TYPE_REQUEST: u8 = 1;
const MESSAGE_TYPE_RESPONSE: u8 = 2;

const STATUS_OK: u16 = 0;
const STATUS_PROTOCOL_VERSION: u16 = 1;

const HEADER_MAGIC: [u8; 4] = [b'Y', b'D', b'H', b'L'];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HelloResponse {
    pub server_capabilities: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HelloError {
    InvalidHello,
    UnsupportedVersion(u8),
    UnexpectedStatus(u16),
    PayloadTooLarge(usize),
    Io(String),
}

impl fmt::Display for HelloError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidHello => formatter.write_str("invalid-hello"),
            Self::UnsupportedVersion(actual) => {
                write!(formatter, "hello-unsupported-version: {}", actual)
            }
            Self::UnexpectedStatus(status) => {
                write!(formatter, "hello-unexpected-status: {}", status)
            }
            Self::PayloadTooLarge(size) => write!(formatter, "hello-payload-too-large: {}", size),
            Self::Io(message) => write!(formatter, "hello-io: {}", message),
        }
    }
}

pub fn perform_client_hello(stream: &mut TcpStream) -> Result<HelloResponse, HelloError> {
    write_message(stream, HELLO_VERSION, MESSAGE_TYPE_REQUEST, 0, &[])?;
    let (version, message_type, status, body) = read_message(stream)?;
    if message_type != MESSAGE_TYPE_RESPONSE {
        return Err(HelloError::InvalidHello);
    }
    if version != HELLO_VERSION {
        return Err(HelloError::UnsupportedVersion(version));
    }
    match status {
        STATUS_OK => Ok(HelloResponse {
            server_capabilities: body,
        }),
        STATUS_PROTOCOL_VERSION => Err(HelloError::UnsupportedVersion(version)),
        other => Err(HelloError::UnexpectedStatus(other)),
    }
}

#[cfg(test)]
pub(crate) fn expect_client_hello(stream: &mut TcpStream) {
    let (version, message_type, status, body) =
        read_message(stream).expect("read client hello should succeed");
    assert_eq!(version, HELLO_VERSION);
    assert_eq!(message_type, MESSAGE_TYPE_REQUEST);
    assert_eq!(status, 0);
    assert!(body.is_empty());
    write_message(stream, HELLO_VERSION, MESSAGE_TYPE_RESPONSE, STATUS_OK, &[])
        .expect("write hello response should succeed");
}

fn write_message(
    stream: &mut TcpStream,
    version: u8,
    message_type: u8,
    status: u16,
    body: &[u8],
) -> Result<(), HelloError> {
    if body.len() > MAX_BODY_BYTES {
        return Err(HelloError::PayloadTooLarge(body.len()));
    }

    let mut header = [0u8; HEADER_SIZE];
    header[0..4].copy_from_slice(&HEADER_MAGIC);
    header[4] = version;
    header[5] = message_type;
    header[6..8].copy_from_slice(&status.to_be_bytes());
    header[8..12].copy_from_slice(&(body.len() as u32).to_be_bytes());

    stream
        .write_all(&header)
        .map_err(|error| HelloError::Io(error.to_string()))?;
    if body.is_empty() {
        return Ok(());
    }
    stream
        .write_all(body)
        .map_err(|error| HelloError::Io(error.to_string()))
}

fn read_message(stream: &mut TcpStream) -> Result<(u8, u8, u16, Vec<u8>), HelloError> {
    let mut header = [0u8; HEADER_SIZE];
    stream
        .read_exact(&mut header)
        .map_err(|error| HelloError::Io(error.to_string()))?;
    if header[0..4] != HEADER_MAGIC {
        return Err(HelloError::InvalidHello);
    }

    let body_len = u32::from_be_bytes(
        header[8..12]
            .try_into()
            .expect("hello body length slice should be fixed"),
    ) as usize;
    if body_len > MAX_BODY_BYTES {
        return Err(HelloError::PayloadTooLarge(body_len));
    }

    let mut body = vec![0u8; body_len];
    if body_len > 0 {
        stream
            .read_exact(&mut body)
            .map_err(|error| HelloError::Io(error.to_string()))?;
    }
    Ok((
        header[4],
        header[5],
        u16::from_be_bytes(
            header[6..8]
                .try_into()
                .expect("hello status slice should be fixed"),
        ),
        body,
    ))
}

#[cfg(test)]
mod tests {
    use super::HELLO_VERSION;
    use super::perform_client_hello;
    use std::io::Read;
    use std::io::Write;
    use std::net::TcpListener;
    use std::thread;

    #[test]
    fn perform_client_hello_exchanges_empty_capabilities() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut request = [0u8; 12];
            stream
                .read_exact(&mut request)
                .expect("read hello request should succeed");
            assert_eq!(&request[0..4], b"YDHL");
            assert_eq!(request[4], HELLO_VERSION);
            assert_eq!(request[5], 1);

            stream
                .write_all(&[b'Y', b'D', b'H', b'L', HELLO_VERSION, 2, 0, 0, 0, 0, 0, 0])
                .expect("write hello response should succeed");
        });

        let mut stream = std::net::TcpStream::connect(address).expect("connect should succeed");
        let response = perform_client_hello(&mut stream).expect("hello should succeed");
        assert!(response.server_capabilities.is_empty());

        server.join().expect("server should join");
    }
}
