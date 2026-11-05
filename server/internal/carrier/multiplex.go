package carrier

import "encoding/binary"

// Multiplex frames exist only on authenticated host links. Viewer-facing
// payloads remain opaque QUIC datagrams and need no protocol change.
const muxMagic = "CHASTM1\n"
const MuxHeader = len(muxMagic) + 8
const MaxMuxPeers = 64

func EncodeMux(id uint64, payload []byte) []byte {
	if id == 0 || len(payload)+MuxHeader > MaxPayload {
		return nil
	}
	out := make([]byte, MuxHeader+len(payload))
	copy(out, muxMagic)
	binary.BigEndian.PutUint64(out[len(muxMagic):], id)
	copy(out[MuxHeader:], payload)
	return out
}

func DecodeMux(frame []byte) (uint64, []byte, bool) {
	if len(frame) < MuxHeader || len(frame) > MaxPayload || string(frame[:len(muxMagic)]) != muxMagic {
		return 0, nil, false
	}
	id := binary.BigEndian.Uint64(frame[len(muxMagic):])
	return id, frame[MuxHeader:], id != 0
}
