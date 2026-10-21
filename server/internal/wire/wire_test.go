package wire

import (
	"strings"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	in := Message{
		Type:      TypeKeepaliveAck,
		HostID:    "devbox",
		KeyHash:   strings.Repeat("a", 64),
		Candidate: "192.0.2.10:5000",
		Relay:     "192.0.2.1:41000",
		OK:        true,
		ExpiresMS: 30000,
	}
	out, err := Decode(Encode(in))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if out != in {
		t.Fatalf("round trip changed the message:\n got %+v\nwant %+v", out, in)
	}
}

func TestRejectsNonControl(t *testing.T) {
	for _, data := range [][]byte{
		[]byte("quic packet"),
		[]byte(Magic + "not json"),
		[]byte(Magic + `{"type":""}`),
		[]byte(strings.Repeat("x", MaxControlSize+1)),
	} {
		if _, err := Decode(data); err == nil {
			t.Fatalf("decoded invalid packet %q", data)
		}
	}
}

func TestIsControl(t *testing.T) {
	if !IsControl(Encode(Message{Type: TypePunch})) {
		t.Fatal("encoded control packet was not recognised")
	}
	if IsControl([]byte{0x44, 0x00, 0x00, 0x00}) {
		t.Fatal("QUIC-looking bytes were treated as control")
	}
}
