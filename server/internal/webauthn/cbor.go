package webauthn

import (
	"errors"
	"fmt"
)

// cborValue is the subset of CBOR the WebAuthn wire types need: definite
// length unsigned and negative integers, byte strings, text strings, arrays
// and maps. Map keys stay as entries because COSE_Key mixes integer and text
// keys. Indefinite lengths and tags are rejected: no authenticator that
// supports WebAuthn emits them in attestationObject or COSE_Key.
type cborValue struct {
	kind   byte // 0 uint, 1 negative, 2 bytes, 3 text, 4 array, 5 map, 7 simple
	u      uint64
	b      []byte
	a      []cborValue
	m      []cborEntry
	simple uint8
}

type cborEntry struct {
	key cborValue
	val cborValue
}

var errCBOR = errors.New("cbor: malformed input")

func decodeCBOR(data []byte) (cborValue, error) {
	value, rest, err := decodeCBORAt(data)
	if err != nil {
		return cborValue{}, err
	}
	if len(rest) != 0 {
		return cborValue{}, fmt.Errorf("%w: %d trailing bytes", errCBOR, len(rest))
	}
	return value, nil
}

func decodeCBORAt(data []byte) (cborValue, []byte, error) {
	if len(data) == 0 {
		return cborValue{}, nil, fmt.Errorf("%w: empty", errCBOR)
	}
	major := data[0] >> 5
	additional := data[0] & 0x1f
	rest := data[1:]
	var argument uint64
	switch {
	case additional < 24:
		argument = uint64(additional)
	case additional == 24:
		if len(rest) < 1 {
			return cborValue{}, nil, fmt.Errorf("%w: truncated uint8 argument", errCBOR)
		}
		argument = uint64(rest[0])
		rest = rest[1:]
	case additional == 25:
		if len(rest) < 2 {
			return cborValue{}, nil, fmt.Errorf("%w: truncated uint16 argument", errCBOR)
		}
		argument = uint64(rest[0])<<8 | uint64(rest[1])
		rest = rest[2:]
	case additional == 26:
		if len(rest) < 4 {
			return cborValue{}, nil, fmt.Errorf("%w: truncated uint32 argument", errCBOR)
		}
		argument = uint64(rest[0])<<24 | uint64(rest[1])<<16 | uint64(rest[2])<<8 | uint64(rest[3])
		rest = rest[4:]
	case additional == 27:
		if len(rest) < 8 {
			return cborValue{}, nil, fmt.Errorf("%w: truncated uint64 argument", errCBOR)
		}
		for i := 0; i < 8; i++ {
			argument = argument<<8 | uint64(rest[i])
		}
		rest = rest[8:]
	default:
		return cborValue{}, nil, fmt.Errorf("%w: unsupported additional info %d", errCBOR, additional)
	}

	switch major {
	case 0:
		return cborValue{kind: 0, u: argument}, rest, nil
	case 1:
		return cborValue{kind: 1, u: argument}, rest, nil
	case 2:
		if argument > uint64(len(rest)) {
			return cborValue{}, nil, fmt.Errorf("%w: byte string past end", errCBOR)
		}
		end := int(argument)
		return cborValue{kind: 2, b: rest[:end]}, rest[end:], nil
	case 3:
		if argument > uint64(len(rest)) {
			return cborValue{}, nil, fmt.Errorf("%w: text string past end", errCBOR)
		}
		end := int(argument)
		return cborValue{kind: 3, b: rest[:end]}, rest[end:], nil
	case 4:
		if argument > uint64(len(rest)) {
			return cborValue{}, nil, fmt.Errorf("%w: array longer than input", errCBOR)
		}
		items := make([]cborValue, 0, argument)
		for i := uint64(0); i < argument; i++ {
			item, next, err := decodeCBORAt(rest)
			if err != nil {
				return cborValue{}, nil, err
			}
			items = append(items, item)
			rest = next
		}
		return cborValue{kind: 4, a: items}, rest, nil
	case 5:
		if argument > uint64(len(rest)) {
			return cborValue{}, nil, fmt.Errorf("%w: map longer than input", errCBOR)
		}
		entries := make([]cborEntry, 0, argument)
		for i := uint64(0); i < argument; i++ {
			key, next, err := decodeCBORAt(rest)
			if err != nil {
				return cborValue{}, nil, err
			}
			val, next, err := decodeCBORAt(next)
			if err != nil {
				return cborValue{}, nil, err
			}
			entries = append(entries, cborEntry{key: key, val: val})
			rest = next
		}
		return cborValue{kind: 5, m: entries}, rest, nil
	case 7:
		switch argument {
		case 20, 21, 22:
			return cborValue{kind: 7, simple: uint8(argument)}, rest, nil
		}
		return cborValue{}, nil, fmt.Errorf("%w: unsupported simple value %d", errCBOR, argument)
	}
	return cborValue{}, nil, fmt.Errorf("%w: unsupported major type %d", errCBOR, major)
}

// negativeInt returns the integer value of a kind-0 or kind-1 CBOR item.
func (v cborValue) negativeInt() (int64, bool) {
	switch v.kind {
	case 0:
		if v.u > uint64(1<<62) {
			return 0, false
		}
		return int64(v.u), true
	case 1:
		if v.u >= uint64(1<<62) {
			return 0, false
		}
		return -1 - int64(v.u), true
	}
	return 0, false
}

func (v cborValue) findText(name string) (cborValue, bool) {
	if v.kind != 5 {
		return cborValue{}, false
	}
	for _, entry := range v.m {
		if entry.key.kind == 3 && string(entry.key.b) == name {
			return entry.val, true
		}
	}
	return cborValue{}, false
}

func (v cborValue) findInt(name int64) (cborValue, bool) {
	if v.kind != 5 {
		return cborValue{}, false
	}
	for _, entry := range v.m {
		if key, ok := entry.key.negativeInt(); ok && key == name {
			return entry.val, true
		}
	}
	return cborValue{}, false
}
