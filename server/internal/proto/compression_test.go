package proto

import (
	"bytes"
	"testing"
)

func TestBuildReadResponseBodyUsesSolidByteBlock(t *testing.T) {
	data := bytes.Repeat([]byte{0xCC}, 2048)

	body := BuildReadResponseBody(data)
	if body[0] != CompressSolidByte {
		t.Fatalf("unexpected compress code: got=%d want=%d", body[0], CompressSolidByte)
	}
	if len(body) != 2 || body[1] != 0xCC {
		t.Fatalf("unexpected solid-byte payload: %v", body)
	}

	decoded, err := ParseReadResponseBody(body, uint32(len(data)))
	if err != nil {
		t.Fatalf("decode solid-byte payload: %v", err)
	}
	if !bytes.Equal(decoded, data) {
		t.Fatalf("decoded solid-byte payload mismatch")
	}
}

func TestBuildReadResponseBodyUsesZstdLevelOne(t *testing.T) {
	data := bytes.Repeat([]byte("ABCD"), 600)

	body := BuildReadResponseBody(data)
	if body[0] != CompressZstd1 {
		t.Fatalf("unexpected compress code: got=%d want=%d", body[0], CompressZstd1)
	}

	decoded, err := ParseReadResponseBody(body, uint32(len(data)))
	if err != nil {
		t.Fatalf("decode zstd-1 payload: %v", err)
	}
	if !bytes.Equal(decoded, data) {
		t.Fatalf("decoded zstd-1 payload mismatch")
	}
}

func TestBuildReadResponseBodyUsesZstdLevelThree(t *testing.T) {
	data := bytes.Repeat([]byte("ABCDEFGH"), 1024)

	body := BuildReadResponseBody(data)
	if body[0] != CompressZstd3 {
		t.Fatalf("unexpected compress code: got=%d want=%d", body[0], CompressZstd3)
	}

	decoded, err := ParseReadResponseBody(body, uint32(len(data)))
	if err != nil {
		t.Fatalf("decode zstd-3 payload: %v", err)
	}
	if !bytes.Equal(decoded, data) {
		t.Fatalf("decoded zstd-3 payload mismatch")
	}
}

func TestBuildReadResponseBodyFallsBackToRawWhenCompressionIsNotBeneficial(t *testing.T) {
	data := incompressibleTestData(2048)

	body := BuildReadResponseBody(data)
	if body[0] != CompressRaw {
		t.Fatalf("unexpected compress code: got=%d want=%d", body[0], CompressRaw)
	}

	decoded, err := ParseReadResponseBody(body, uint32(len(data)))
	if err != nil {
		t.Fatalf("decode raw payload: %v", err)
	}
	if !bytes.Equal(decoded, data) {
		t.Fatalf("decoded raw payload mismatch")
	}
}

func TestBuildWriteRequestBodyAndParseReadWriteBodyRoundTripZstd(t *testing.T) {
	data := bytes.Repeat([]byte("ABCDEFGH"), 1024)

	body := BuildWriteRequestBody(4096, data)
	if body[12] != CompressZstd3 {
		t.Fatalf("unexpected compress code: got=%d want=%d", body[12], CompressZstd3)
	}

	offset, length, decoded, err := ParseReadWriteBody(body)
	if err != nil {
		t.Fatalf("parse compressed write body: %v", err)
	}
	if offset != 4096 {
		t.Fatalf("unexpected offset: got=%d want=%d", offset, 4096)
	}
	if length != uint32(len(data)) {
		t.Fatalf("unexpected length: got=%d want=%d", length, len(data))
	}
	if !bytes.Equal(decoded, data) {
		t.Fatalf("decoded write payload mismatch")
	}
}

func TestParseReadResponseBodyRejectsUnknownCompressCode(t *testing.T) {
	if _, err := ParseReadResponseBody([]byte{9, 'D', 'A', 'T', 'A'}, 4); err != ErrSessionBody {
		t.Fatalf("expected unknown code to fail with ErrSessionBody, got %v", err)
	}
}

func TestParseReadResponseBodyRejectsBadSolidByteLength(t *testing.T) {
	if _, err := ParseReadResponseBody([]byte{CompressSolidByte, 'A', 'B'}, 4); err != ErrSessionBody {
		t.Fatalf("expected bad solid-byte length to fail with ErrSessionBody, got %v", err)
	}
}

func TestParseReadResponseBodyRejectsBadZstdPayload(t *testing.T) {
	if _, err := ParseReadResponseBody([]byte{CompressZstd1, 1, 2, 3, 4}, 4); err != ErrSessionBody {
		t.Fatalf("expected bad zstd payload to fail with ErrSessionBody, got %v", err)
	}
}

func TestParseReadResponseBodyRejectsDecodedLengthMismatch(t *testing.T) {
	data := bytes.Repeat([]byte("ABCD"), 600)
	body := BuildReadResponseBody(data)
	if body[0] != CompressZstd1 {
		t.Fatalf("unexpected compress code: got=%d want=%d", body[0], CompressZstd1)
	}

	if _, err := ParseReadResponseBody(body, uint32(len(data)+64)); err != ErrSessionBody {
		t.Fatalf("expected length mismatch to fail with ErrSessionBody, got %v", err)
	}
}

func incompressibleTestData(length int) []byte {
	state := uint64(0x9E3779B97F4A7C15)
	output := make([]byte, 0, length)
	for range length {
		state ^= state << 7
		state ^= state >> 9
		state *= 0xA24BAED4963EE407
		output = append(output, byte(state))
	}
	return output
}
