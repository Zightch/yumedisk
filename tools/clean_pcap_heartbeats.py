#!/usr/bin/env python3
"""
Remove heartbeat-only packets from a pcapng capture.

This script is designed for `pktmon etl2pcap` output. It preserves every
pcapng block verbatim except packet blocks whose TCP payload consists only of
complete framed transport heartbeat messages.

Current heartbeat detection covers:
- client <-> gateway `ConnHeartbeat` (`op_code = 0x12`)
- gateway <-> storer `LinkHeartbeat` (`op_code = 0x21`)

Limitations:
- It only drops packets whose TCP payload is made entirely of whole heartbeat
  frames.
- If a heartbeat frame is coalesced with non-heartbeat frames in the same TCP
  packet, or fragmented across TCP packets, that packet is kept.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


PCAPNG_SECTION_HEADER = 0x0A0D0D0A
PCAPNG_INTERFACE_DESCRIPTION = 0x00000001
PCAPNG_SIMPLE_PACKET = 0x00000003
PCAPNG_ENHANCED_PACKET = 0x00000006

BYTE_ORDER_MAGIC_LE = b"\x4d\x3c\x2b\x1a"
BYTE_ORDER_MAGIC_BE = b"\x1a\x2b\x3c\x4d"

LINKTYPE_ETHERNET = 1
LINKTYPE_RAW = 101

ETHERTYPE_VLAN = 0x8100
ETHERTYPE_QINQ = 0x88A8
ETHERTYPE_IPV4 = 0x0800
ETHERTYPE_IPV6 = 0x86DD
IPPROTO_TCP = 6

FRAME_HEADER_BYTES = 2
PROTOCOL_VERSION = 2
PROTOCOL_HEADER_LEN = 24
FLAG_RESPONSE = 1
OP_CONN_HEARTBEAT = 0x12
OP_LINK_HEARTBEAT = 0x21


@dataclass
class Stats:
    total_packet_blocks: int = 0
    removed_packet_blocks: int = 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Remove heartbeat-only packets from a pcapng capture."
    )
    parser.add_argument("--input", required=True, help="Input pcapng file")
    parser.add_argument("--output", required=True, help="Output pcapng file")
    return parser.parse_args()


def align4(length: int) -> int:
    return (length + 3) & ~3


def read_u16(data: bytes, offset: int, endian: str) -> int:
    return struct.unpack_from(f"{endian}H", data, offset)[0]


def read_u32(data: bytes, offset: int, endian: str) -> int:
    return struct.unpack_from(f"{endian}I", data, offset)[0]


def determine_section_endian(block: bytes) -> str:
    if len(block) < 12:
        raise ValueError("section header block too small")
    magic = block[8:12]
    if magic == BYTE_ORDER_MAGIC_LE:
        return "<"
    if magic == BYTE_ORDER_MAGIC_BE:
        return ">"
    raise ValueError("unsupported pcapng byte-order magic")


def parse_blocks(data: bytes):
    offset = 0
    current_endian: str | None = None
    while offset < len(data):
        if offset + 12 > len(data):
            raise ValueError("truncated pcapng block header")

        raw_type = data[offset : offset + 4]
        if raw_type == b"\x0a\x0d\x0d\x0a":
            tentative = data[offset : offset + 32]
            current_endian = determine_section_endian(tentative)
            block_type = PCAPNG_SECTION_HEADER
            block_len = read_u32(data, offset + 4, current_endian)
        else:
            if current_endian is None:
                raise ValueError("pcapng file does not start with a section header")
            block_type = read_u32(data, offset, current_endian)
            block_len = read_u32(data, offset + 4, current_endian)

        if block_len < 12 or offset + block_len > len(data):
            raise ValueError("invalid pcapng block length")

        block = data[offset : offset + block_len]
        trailing_len = read_u32(block, block_len - 4, current_endian)
        if trailing_len != block_len:
            raise ValueError("pcapng block trailing length mismatch")

        yield current_endian, block_type, block
        offset += block_len


def extract_packet_bytes(
    block_type: int, block: bytes, endian: str, interface_linktypes: list[int]
) -> tuple[int | None, bytes | None]:
    if block_type == PCAPNG_ENHANCED_PACKET:
        interface_id = read_u32(block, 8, endian)
        if interface_id >= len(interface_linktypes):
            return None, None
        captured_len = read_u32(block, 20, endian)
        packet_start = 28
        packet_end = packet_start + captured_len
        if packet_end > len(block) - 4:
            return None, None
        return interface_linktypes[interface_id], block[packet_start:packet_end]

    if block_type == PCAPNG_SIMPLE_PACKET:
        if not interface_linktypes:
            return None, None
        original_len = read_u32(block, 8, endian)
        packet_start = 12
        packet_storage_end = len(block) - 4
        packet_storage = block[packet_start:packet_storage_end]
        packet_len = min(original_len, len(packet_storage))
        return interface_linktypes[0], packet_storage[:packet_len]

    return None, None


def tcp_payload_from_packet(linktype: int, packet: bytes) -> bytes | None:
    if linktype == LINKTYPE_ETHERNET:
        return tcp_payload_from_ethernet(packet)
    if linktype == LINKTYPE_RAW:
        return tcp_payload_from_raw_ip(packet)
    return None


def tcp_payload_from_ethernet(packet: bytes) -> bytes | None:
    if len(packet) < 14:
        return None

    offset = 12
    ether_type = struct.unpack_from("!H", packet, offset)[0]
    offset += 2
    while ether_type in (ETHERTYPE_VLAN, ETHERTYPE_QINQ):
        if len(packet) < offset + 4:
            return None
        ether_type = struct.unpack_from("!H", packet, offset + 2)[0]
        offset += 4

    if ether_type == ETHERTYPE_IPV4:
        return tcp_payload_from_ipv4(packet[offset:])
    if ether_type == ETHERTYPE_IPV6:
        return tcp_payload_from_ipv6(packet[offset:])
    return None


def tcp_payload_from_raw_ip(packet: bytes) -> bytes | None:
    if not packet:
        return None
    version = packet[0] >> 4
    if version == 4:
        return tcp_payload_from_ipv4(packet)
    if version == 6:
        return tcp_payload_from_ipv6(packet)
    return None


def tcp_payload_from_ipv4(packet: bytes) -> bytes | None:
    if len(packet) < 20:
        return None
    version = packet[0] >> 4
    ihl = (packet[0] & 0x0F) * 4
    if version != 4 or ihl < 20 or len(packet) < ihl:
        return None
    if packet[9] != IPPROTO_TCP:
        return None
    total_length = struct.unpack_from("!H", packet, 2)[0]
    if total_length < ihl:
        return None
    available = min(total_length, len(packet))
    return tcp_payload_from_tcp_segment(packet[ihl:available])


def tcp_payload_from_ipv6(packet: bytes) -> bytes | None:
    if len(packet) < 40:
        return None
    if packet[0] >> 4 != 6:
        return None
    next_header = packet[6]
    payload_length = struct.unpack_from("!H", packet, 4)[0]
    if next_header != IPPROTO_TCP:
        return None
    available = min(40 + payload_length, len(packet))
    return tcp_payload_from_tcp_segment(packet[40:available])


def tcp_payload_from_tcp_segment(segment: bytes) -> bytes | None:
    if len(segment) < 20:
        return None
    data_offset = (segment[12] >> 4) * 4
    if data_offset < 20 or len(segment) < data_offset:
        return None
    return segment[data_offset:]


def tcp_payload_is_heartbeat_only(payload: bytes) -> bool:
    if not payload:
        return False

    offset = 0
    saw_heartbeat = False
    while offset + FRAME_HEADER_BYTES <= len(payload):
        frame_len = struct.unpack_from("!H", payload, offset)[0] + 1
        frame_start = offset + FRAME_HEADER_BYTES
        frame_end = frame_start + frame_len
        if frame_end > len(payload):
            return False
        if not transport_frame_is_heartbeat(payload[frame_start:frame_end]):
            return False
        saw_heartbeat = True
        offset = frame_end

    return saw_heartbeat and offset == len(payload)


def transport_frame_is_heartbeat(frame: bytes) -> bool:
    if len(frame) not in (24, 32):
        return False
    if frame[0] != PROTOCOL_VERSION or frame[1] != PROTOCOL_HEADER_LEN:
        return False

    op_code = frame[2]
    flags = frame[3]
    reserved = struct.unpack_from("!H", frame, 6)[0]
    request_id = struct.unpack_from("!Q", frame, 8)[0]
    session_id = struct.unpack_from("!Q", frame, 16)[0]

    if flags not in (0, FLAG_RESPONSE):
        return False
    if reserved != 0 or request_id == 0 or session_id != 0:
        return False

    if op_code == OP_CONN_HEARTBEAT:
        return len(frame) == 24
    if op_code == OP_LINK_HEARTBEAT:
        return len(frame) == 32
    return False


def clean_capture(input_path: Path, output_path: Path) -> Stats:
    data = input_path.read_bytes()
    output = bytearray()
    interface_linktypes: list[int] = []
    stats = Stats()

    for endian, block_type, block in parse_blocks(data):
        if block_type == PCAPNG_SECTION_HEADER:
            interface_linktypes = []
            output.extend(block)
            continue

        if block_type == PCAPNG_INTERFACE_DESCRIPTION:
            interface_linktypes.append(read_u16(block, 8, endian))
            output.extend(block)
            continue

        if block_type in (PCAPNG_ENHANCED_PACKET, PCAPNG_SIMPLE_PACKET):
            stats.total_packet_blocks += 1
            linktype, packet = extract_packet_bytes(
                block_type, block, endian, interface_linktypes
            )
            if linktype is not None and packet is not None:
                tcp_payload = tcp_payload_from_packet(linktype, packet)
                if tcp_payload is not None and tcp_payload_is_heartbeat_only(tcp_payload):
                    stats.removed_packet_blocks += 1
                    continue

        output.extend(block)

    output_path.write_bytes(output)
    return stats


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    output_path = Path(args.output)

    try:
        stats = clean_capture(input_path, output_path)
    except Exception as error:  # pragma: no cover - CLI error path
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(
        "clean_pcap_heartbeats "
        f"input={input_path} output={output_path} "
        f"packet_blocks={stats.total_packet_blocks} "
        f"removed={stats.removed_packet_blocks}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
