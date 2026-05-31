package proto

import (
	"bytes"
	"encoding/binary"

	"github.com/klauspost/compress/zstd"
)

const (
	MaxDataPlaneRawBytes uint32 = 60 * 1024
	CompressZstd1        byte   = 1
	CompressZstd3        byte   = 2
	CompressSolidByte    byte   = 255
)

func encodeDataPlanePayload(data []byte) (byte, []byte) {
	if len(data) == 0 {
		return CompressRaw, nil
	}
	if isSolidBytePayload(data) {
		return CompressSolidByte, []byte{data[0]}
	}

	switch {
	case len(data) < 1024:
	case len(data) < 4096:
		if payload, ok := tryZstdPayload(data, 1); ok {
			return CompressZstd1, payload
		}
	default:
		if payload, ok := tryZstdPayload(data, 3); ok {
			return CompressZstd3, payload
		}
	}

	return CompressRaw, bytes.Clone(data)
}

func tryZstdPayload(data []byte, level int) ([]byte, bool) {
	encoder, err := zstd.NewWriter(
		nil,
		zstd.WithEncoderLevel(zstd.EncoderLevelFromZstd(level)),
	)
	if err != nil {
		return nil, false
	}
	defer encoder.Close()

	encoded := encoder.EncodeAll(data, nil)
	if len(encoded) > int(MaxDataPlaneRawBytes) {
		return nil, false
	}
	if len(data)-len(encoded) < minimumCompressionSavings(len(data)) {
		return nil, false
	}

	return encoded, true
}

func minimumCompressionSavings(rawLen int) int {
	percent := (rawLen*5 + 99) / 100
	if percent < 64 {
		return 64
	}
	return percent
}

func isSolidBytePayload(data []byte) bool {
	first := data[0]
	for _, value := range data[1:] {
		if value != first {
			return false
		}
	}
	return true
}

func decodeDataPlanePayload(compress byte, payload []byte, expectedLength uint32) ([]byte, error) {
	if expectedLength == 0 || expectedLength > MaxDataPlaneRawBytes {
		return nil, ErrSessionBody
	}
	if len(payload) > int(MaxDataPlaneRawBytes) {
		return nil, ErrSessionBody
	}

	switch compress {
	case CompressRaw:
		if len(payload) != int(expectedLength) {
			return nil, ErrSessionBody
		}
		return bytes.Clone(payload), nil
	case CompressZstd1, CompressZstd3:
		decoder, err := zstd.NewReader(nil)
		if err != nil {
			return nil, ErrSessionBody
		}
		defer decoder.Close()

		decoded, err := decoder.DecodeAll(payload, nil)
		if err != nil || len(decoded) != int(expectedLength) {
			return nil, ErrSessionBody
		}
		return decoded, nil
	case CompressSolidByte:
		if len(payload) != 1 {
			return nil, ErrSessionBody
		}
		return bytes.Repeat(payload, int(expectedLength)), nil
	default:
		return nil, ErrSessionBody
	}
}

func BuildWriteRequestBody(offset uint64, data []byte) []byte {
	compress, payload := encodeDataPlanePayload(data)
	body := make([]byte, WriteBodyHeaderSize+len(payload))
	binary.BigEndian.PutUint64(body[0:8], offset)
	binary.BigEndian.PutUint32(body[8:12], uint32(len(data)))
	body[12] = compress
	copy(body[13:], payload)
	return body
}
