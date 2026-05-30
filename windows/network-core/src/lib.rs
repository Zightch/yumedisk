mod connection_authenticator;
mod crypto_win32;
mod disk_session;
mod error;
mod gateway_connection;
mod hello_client;
mod protocol_client;
mod session_describer;
mod session_opener;
mod transport_client;

pub mod client {
    pub use crate::connection_authenticator::{AuthGrant, ConnectionAuthenticator};
    pub use crate::disk_session::DiskSession;
    pub use crate::error::NetworkClientError;
    pub use crate::gateway_connection::GatewayConnection;
    pub use crate::protocol_client::SessionCloseNotice;
    pub use crate::protocol_client::SessionDataChangedNotice;
    pub use crate::session_describer::{SessionDescriber, SessionMetadata};
    pub use crate::session_opener::SessionOpener;
}

pub mod protocol {
    pub use crate::protocol_client::{
        ABSOLUTE_MAX_IO_BYTES, AuthFinishRequest, AuthFinishResponse, AuthStartRequest,
        AuthStartResponse, ClientOperationCode, ConnHeartbeatRequest, ConnHeartbeatResponse,
        FLAG_NOTICE, FLAG_RESPONSE, HEADER_SIZE, PROTOCOL_VERSION, ProtocolClientError,
        ProtocolHeader, ProtocolStatusCode, ReadAtRequest, ReadAtResponse,
        SESSION_CLOSE_REASON_CLIENT_CONNECTION_REPLACED, SESSION_CLOSE_REASON_GATEWAY_SHUTDOWN,
        SESSION_CLOSE_REASON_NORMAL_CLOSE, SESSION_CLOSE_REASON_PROTOCOL_ERROR,
        SESSION_CLOSE_REASON_ROUTE_LOST, SESSION_CLOSE_REASON_UPSTREAM_SESSION_CLOSED,
        SessionCloseNotice, SessionDataChangedNotice, SessionDescribeRequest,
        SessionDescribeResponse, SessionOpenRequest, SessionOpenResponse, WriteAtRequest,
        decode_gateway_status, parse_header, parse_request_header,
    };
}

pub mod transport {
    pub use crate::transport_client::{
        MAX_FRAME_PAYLOAD_BYTES, TransportEndpoint, TransportError, read_frame_into, write_frame,
    };
}

#[cfg(any(test, feature = "test-support"))]
pub mod test_support {
    use std::sync::Arc;

    use crate::client::GatewayConnection;
    use crate::transport::TransportEndpoint;

    pub fn stage_connection(
        endpoint: TransportEndpoint,
        session_id: u64,
    ) -> Arc<GatewayConnection> {
        let connection = GatewayConnection::new(endpoint);
        connection
            .begin_session_open()
            .expect("begin session open should succeed");
        connection
            .finish_session_open(session_id)
            .expect("finish session open should succeed");
        connection
    }

    pub fn expect_client_hello(stream: &mut std::net::TcpStream) {
        crate::hello_client::expect_client_hello(stream);
    }

    pub fn clear_session(connection: &Arc<GatewayConnection>, session_id: u64) {
        connection.clear_session(session_id);
    }

    pub fn is_session_active(connection: &Arc<GatewayConnection>, session_id: u64) -> bool {
        connection.is_session_active(session_id)
    }
}

#[cfg(test)]
#[allow(unused_imports)]
mod network {
    pub use crate::client::{
        AuthGrant, ConnectionAuthenticator, DiskSession, GatewayConnection, NetworkClientError,
        SessionCloseNotice, SessionDataChangedNotice, SessionDescriber, SessionMetadata,
        SessionOpener,
    };
    pub(crate) use crate::hello_client::expect_client_hello;
    pub use crate::protocol::{
        ABSOLUTE_MAX_IO_BYTES, AuthFinishRequest, AuthFinishResponse, AuthStartRequest,
        AuthStartResponse, ClientOperationCode, ConnHeartbeatRequest, ConnHeartbeatResponse,
        FLAG_NOTICE, FLAG_RESPONSE, HEADER_SIZE, PROTOCOL_VERSION, ProtocolClientError,
        ProtocolHeader, ProtocolStatusCode, ReadAtRequest, ReadAtResponse,
        SESSION_CLOSE_REASON_CLIENT_CONNECTION_REPLACED, SESSION_CLOSE_REASON_GATEWAY_SHUTDOWN,
        SESSION_CLOSE_REASON_NORMAL_CLOSE, SESSION_CLOSE_REASON_PROTOCOL_ERROR,
        SESSION_CLOSE_REASON_ROUTE_LOST, SESSION_CLOSE_REASON_UPSTREAM_SESSION_CLOSED,
        SessionDescribeRequest, SessionDescribeResponse, SessionOpenRequest, SessionOpenResponse,
        WriteAtRequest, decode_gateway_status, parse_header, parse_request_header,
    };
    pub use crate::transport::TransportEndpoint;

    pub(crate) mod transport_client {
        pub use crate::transport_client::{MAX_FRAME_PAYLOAD_BYTES, read_frame_into, write_frame};
    }
}
