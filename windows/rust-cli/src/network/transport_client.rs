use std::error::Error;
use std::fmt;
use std::io;
use std::io::Read;
use std::io::Write;
use std::net::Shutdown;
use std::net::TcpStream;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::mpsc;
use std::sync::mpsc::Receiver;
use std::sync::mpsc::SyncSender;
use std::thread;

pub const FRAME_HEADER_BYTES: usize = 2;
pub const MIN_FRAME_PAYLOAD_BYTES: usize = 1;
pub const MAX_FRAME_PAYLOAD_BYTES: usize = 65_536;
const OUTBOUND_QUEUE_CAPACITY: usize = 64;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TransportEndpoint {
    address: String,
}

impl TransportEndpoint {
    pub fn new(address: impl Into<String>) -> Self {
        Self {
            address: address.into(),
        }
    }

    pub fn address(&self) -> &str {
        self.address.as_str()
    }

    fn validate(&self) -> Result<(), TransportError> {
        if self.address.trim().is_empty() {
            return Err(TransportError::InvalidEndpoint);
        }
        Ok(())
    }
}

#[derive(Debug)]
pub struct TransportClient {
    endpoint: TransportEndpoint,
    state: Mutex<TransportState>,
}

#[derive(Debug)]
enum TransportState {
    Disconnected,
    Connected(Arc<ConnectedTransport>),
}

#[derive(Debug)]
struct ConnectedTransport {
    stream: Arc<TcpStream>,
    outgoing_tx: SyncSender<WriterCommand>,
    incoming_rx: Mutex<Receiver<InboundEvent>>,
}

type InboundEvent = Result<Vec<u8>, TransportError>;

#[derive(Debug)]
enum WriterCommand {
    Payload(Vec<u8>),
    Close,
}

#[derive(Debug)]
pub enum TransportError {
    InvalidEndpoint,
    AlreadyConnected,
    NotConnected,
    ConnectionClosed,
    BufferTooSmall {
        provided: usize,
        required: usize,
    },
    PayloadOutOfRange {
        size: usize,
    },
    Io(io::Error),
}

impl fmt::Display for TransportError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidEndpoint => formatter.write_str("invalid-endpoint"),
            Self::AlreadyConnected => formatter.write_str("already-connected"),
            Self::NotConnected => formatter.write_str("not-connected"),
            Self::ConnectionClosed => formatter.write_str("connection-closed"),
            Self::BufferTooSmall { provided, required } => {
                write!(
                    formatter,
                    "frame-buffer-too-small: provided={}, required={}",
                    provided, required
                )
            }
            Self::PayloadOutOfRange { size } => write!(formatter, "payload-out-of-range: {}", size),
            Self::Io(error) => write!(formatter, "transport-io: {}", error),
        }
    }
}

impl Error for TransportError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            _ => None,
        }
    }
}

impl TransportClient {
    pub fn new(endpoint: TransportEndpoint) -> Self {
        Self {
            endpoint,
            state: Mutex::new(TransportState::Disconnected),
        }
    }

    pub fn endpoint(&self) -> &TransportEndpoint {
        &self.endpoint
    }

    pub fn connect(&self) -> Result<(), TransportError> {
        self.endpoint.validate()?;

        let mut state = self.state.lock().expect("transport state poisoned");
        if matches!(&*state, TransportState::Connected(_)) {
            return Err(TransportError::AlreadyConnected);
        }

        let stream = TcpStream::connect(self.endpoint.address()).map_err(TransportError::Io)?;
        stream.set_nodelay(true).map_err(TransportError::Io)?;
        let stream = Arc::new(stream);

        let (outgoing_tx, outgoing_rx) = mpsc::sync_channel(OUTBOUND_QUEUE_CAPACITY);
        let (incoming_tx, incoming_rx) = mpsc::channel();

        spawn_reader_loop(Arc::clone(&stream), incoming_tx.clone());
        spawn_writer_loop(Arc::clone(&stream), outgoing_rx, incoming_tx);

        *state = TransportState::Connected(Arc::new(ConnectedTransport {
            stream,
            outgoing_tx,
            incoming_rx: Mutex::new(incoming_rx),
        }));
        Ok(())
    }

    pub fn is_connected(&self) -> bool {
        matches!(
            &*self.state.lock().expect("transport state poisoned"),
            TransportState::Connected(_)
        )
    }

    pub fn send_payload(&self, payload: Vec<u8>) -> Result<(), TransportError> {
        validate_payload_size(payload.len())?;

        let connected = self.connected_state()?;
        connected
            .outgoing_tx
            .send(WriterCommand::Payload(payload))
            .map_err(|_| TransportError::ConnectionClosed)
    }

    pub fn recv_payload(&self) -> Result<Vec<u8>, TransportError> {
        let connected = self.connected_state()?;
        let receiver = connected
            .incoming_rx
            .lock()
            .expect("transport incoming receiver poisoned");

        receiver.recv().unwrap_or(Err(TransportError::ConnectionClosed))
    }

    pub fn close(&self) -> Result<(), TransportError> {
        let connected = {
            let mut state = self.state.lock().expect("transport state poisoned");
            match std::mem::replace(&mut *state, TransportState::Disconnected) {
                TransportState::Disconnected => return Ok(()),
                TransportState::Connected(connected) => connected,
            }
        };

        let _ = connected.outgoing_tx.send(WriterCommand::Close);
        connected
            .stream
            .shutdown(Shutdown::Both)
            .map_err(TransportError::Io)
    }

    fn connected_state(&self) -> Result<Arc<ConnectedTransport>, TransportError> {
        let state = self.state.lock().expect("transport state poisoned");
        match &*state {
            TransportState::Disconnected => Err(TransportError::NotConnected),
            TransportState::Connected(connected) => Ok(Arc::clone(connected)),
        }
    }
}

impl Drop for TransportClient {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

pub fn read_frame_into<'buffer, R: Read>(
    reader: &mut R,
    buffer: &'buffer mut [u8],
) -> Result<&'buffer [u8], TransportError> {
    let mut header = [0u8; FRAME_HEADER_BYTES];
    reader
        .read_exact(&mut header)
        .map_err(map_io_error_for_read)?;

    let payload_size = usize::from(u16::from_be_bytes(header)) + 1;
    validate_payload_size(payload_size)?;

    if buffer.len() < payload_size {
        return Err(TransportError::BufferTooSmall {
            provided: buffer.len(),
            required: payload_size,
        });
    }

    let payload = &mut buffer[..payload_size];
    reader
        .read_exact(payload)
        .map_err(map_io_error_for_read)?;
    Ok(payload)
}

pub fn write_frame<W: Write>(writer: &mut W, payload: &[u8]) -> Result<(), TransportError> {
    validate_payload_size(payload.len())?;

    let header = u16::try_from(payload.len() - 1)
        .expect("payload length already validated")
        .to_be_bytes();

    writer.write_all(&header).map_err(TransportError::Io)?;
    writer.write_all(payload).map_err(TransportError::Io)?;
    writer.flush().map_err(TransportError::Io)
}

fn spawn_reader_loop(stream: Arc<TcpStream>, incoming_tx: mpsc::Sender<InboundEvent>) {
    thread::spawn(move || {
        let mut reader = &*stream;
        let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

        loop {
            match read_frame_into(&mut reader, &mut buffer) {
                Ok(payload) => {
                    if incoming_tx.send(Ok(payload.to_vec())).is_err() {
                        return;
                    }
                }
                Err(error) => {
                    let _ = incoming_tx.send(Err(error));
                    return;
                }
            }
        }
    });
}

fn spawn_writer_loop(
    stream: Arc<TcpStream>,
    outgoing_rx: mpsc::Receiver<WriterCommand>,
    incoming_tx: mpsc::Sender<InboundEvent>,
) {
    thread::spawn(move || {
        let mut writer = &*stream;

        while let Ok(command) = outgoing_rx.recv() {
            match command {
                WriterCommand::Payload(payload) => {
                    if let Err(error) = write_frame(&mut writer, &payload) {
                        let _ = incoming_tx.send(Err(error));
                        return;
                    }
                }
                WriterCommand::Close => return,
            }
        }
    });
}

fn validate_payload_size(size: usize) -> Result<(), TransportError> {
    if !(MIN_FRAME_PAYLOAD_BYTES..=MAX_FRAME_PAYLOAD_BYTES).contains(&size) {
        return Err(TransportError::PayloadOutOfRange { size });
    }
    Ok(())
}

fn map_io_error_for_read(error: io::Error) -> TransportError {
    if matches!(
        error.kind(),
        io::ErrorKind::UnexpectedEof
            | io::ErrorKind::ConnectionAborted
            | io::ErrorKind::ConnectionReset
            | io::ErrorKind::BrokenPipe
            | io::ErrorKind::NotConnected
    ) {
        return TransportError::ConnectionClosed;
    }
    TransportError::Io(error)
}

#[cfg(test)]
mod tests {
    use super::MAX_FRAME_PAYLOAD_BYTES;
    use super::TransportClient;
    use super::TransportEndpoint;
    use super::TransportError;
    use super::read_frame_into;
    use super::write_frame;
    use std::io::Cursor;
    use std::net::TcpListener;
    use std::thread;

    #[test]
    fn write_frame_encodes_payload_size_minus_one() {
        let mut bytes = Vec::new();
        write_frame(&mut bytes, b"abc").expect("write_frame should succeed");
        assert_eq!(bytes, vec![0x00, 0x02, b'a', b'b', b'c']);
    }

    #[test]
    fn read_frame_into_decodes_payload() {
        let mut bytes = Cursor::new(vec![0x00, 0x02, b'a', b'b', b'c']);
        let mut buffer = [0u8; 16];
        let payload = read_frame_into(&mut bytes, &mut buffer).expect("read_frame_into should succeed");
        assert_eq!(payload, b"abc");
    }

    #[test]
    fn read_frame_into_rejects_small_buffer() {
        let mut bytes = Cursor::new(vec![0x00, 0x02, b'a', b'b', b'c']);
        let mut buffer = [0u8; 2];
        let error = read_frame_into(&mut bytes, &mut buffer).expect_err("read_frame_into should fail");
        match error {
            TransportError::BufferTooSmall { provided, required } => {
                assert_eq!(provided, 2);
                assert_eq!(required, 3);
            }
            other => panic!("unexpected error: {}", other),
        }
    }

    #[test]
    fn transport_client_runs_framed_send_and_receive_loops() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            write_frame(&mut stream, b"server-ready").expect("server write should succeed");

            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];
            let first = read_frame_into(&mut stream, &mut buffer)
                .expect("server read first frame should succeed")
                .to_vec();
            assert_eq!(first, b"client-one");
            write_frame(&mut stream, b"ack-one").expect("server ack-one should succeed");

            let second = read_frame_into(&mut stream, &mut buffer)
                .expect("server read second frame should succeed")
                .to_vec();
            assert_eq!(second, b"client-two");
            write_frame(&mut stream, b"ack-two").expect("server ack-two should succeed");
        });

        let transport = TransportClient::new(TransportEndpoint::new(address.to_string()));
        transport.connect().expect("connect should succeed");

        let ready = transport.recv_payload().expect("client should receive ready");
        assert_eq!(ready, b"server-ready");

        transport
            .send_payload(b"client-one".to_vec())
            .expect("client first send should succeed");
        let ack_one = transport.recv_payload().expect("client should receive ack-one");
        assert_eq!(ack_one, b"ack-one");

        transport
            .send_payload(b"client-two".to_vec())
            .expect("client second send should succeed");
        let ack_two = transport.recv_payload().expect("client should receive ack-two");
        assert_eq!(ack_two, b"ack-two");

        transport.close().expect("close should succeed");
        server.join().expect("server thread should join");
    }
}
