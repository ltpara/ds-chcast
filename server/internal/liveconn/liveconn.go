// Package liveconn defines the wire contract shared by the host agent, the
// server state machine and the administrator console. Runtime and connection
// identifiers are random opaque values; neither may be derived from a process
// local counter or a viewer address.
package liveconn

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strings"
	"time"
)

const (
	// Freshness is the longest gap allowed before a runtime's last report is
	// shown as stale. Stale is unknown, not evidence that its viewers left.
	Freshness = 45 * time.Second
	// HeartbeatInterval is the host agent's normal snapshot cadence.
	HeartbeatInterval = 5 * time.Second
	// CommandTTL bounds how long a queued disconnect may affect a connection.
	CommandTTL = 60 * time.Second

	MaxSnapshotConnections = 256
	MaxQueuedCommands      = 32
)

const (
	ConnectionActive = "active"
	ConnectionClosed = "closed"
	ConnectionStale  = "stale"

	CommandQueued    = "queued"
	CommandDelivered = "delivered"
	CommandApplied   = "applied"
	CommandFailed    = "failed"
	CommandExpired   = "expired"

	OperationDisconnect = "disconnect"
)

var (
	ErrInvalid  = errors.New("invalid live connection data")
	ErrConflict = errors.New("live connection conflict")
)

// Connection is one admitted session reported by a native host runtime.
type Connection struct {
	ConnectionID string `json:"connection_id"`
	PeerAddress  string `json:"peer_address,omitempty"`
	Viewer       string `json:"viewer,omitempty"`
	Mode         string `json:"mode,omitempty"`
	Route        string `json:"route,omitempty"`
	Permissions  uint32 `json:"permissions,omitempty"`
	Controller   bool   `json:"controller,omitempty"`
	StartedAtMS  int64  `json:"started_at_ms"`
}

// Snapshot is a complete bounded view of the active connections owned by one
// host process. A higher sequence that omits a connection is the authoritative
// native confirmation that the connection ended.
type Snapshot struct {
	RuntimeID string `json:"runtime_id"`
	Sequence  uint64 `json:"sequence"`
	// Terminal is a graceful shutdown report. Once accepted, this runtime
	// cannot be revived; a restarted host process must choose a new runtime ID.
	Terminal    bool         `json:"terminal,omitempty"`
	Connections []Connection `json:"connections"`
}

// Command is the durable, exact disconnect request polled by one runtime.
type Command struct {
	CommandID    string `json:"command_id"`
	HostID       string `json:"host_id"`
	RuntimeID    string `json:"runtime_id"`
	ConnectionID string `json:"connection_id"`
	Operation    string `json:"operation"`
	RequestKey   string `json:"request_key"`
	Reason       string `json:"reason,omitempty"`
	Status       string `json:"status"`
	IssuedAtMS   int64  `json:"issued_at_ms"`
	DeadlineMS   int64  `json:"deadline_ms"`
}

// Receipt is a native result for one exact command. Applied is accepted only
// when ConfirmedAbsent is true.
type Receipt struct {
	ReceiptSequence uint64 `json:"receipt_sequence"`
	CommandID       string `json:"command_id"`
	Status          string `json:"status"`
	ConfirmedAbsent bool   `json:"confirmed_absent"`
	Detail          string `json:"detail,omitempty"`
}

// SignedCommands is the authenticated server response to a host poll. The
// digest covers the exact command list, so commands cannot be inserted,
// removed or altered between the server and the native host.
type SignedCommands struct {
	Commands  []Command `json:"commands"`
	Timestamp int64     `json:"timestamp"`
	Signature string    `json:"signature"`
}

// Normalize validates a snapshot and returns a canonical copy with connections
// sorted by ID. Reordering the same complete set is therefore an idempotent
// retry rather than a conflicting snapshot.
func (s Snapshot) Normalize() (Snapshot, error) {
	runtimeID := strings.ToLower(strings.TrimSpace(s.RuntimeID))
	if !validOpaqueID(runtimeID) {
		return Snapshot{}, fmt.Errorf("%w: runtime_id must be 32 lowercase hex characters", ErrInvalid)
	}
	if s.Sequence == 0 || s.Sequence > math.MaxInt64 {
		return Snapshot{}, fmt.Errorf("%w: sequence must be between 1 and %d", ErrInvalid, int64(math.MaxInt64))
	}
	if len(s.Connections) > MaxSnapshotConnections {
		return Snapshot{}, fmt.Errorf("%w: more than %d connections", ErrInvalid, MaxSnapshotConnections)
	}
	out := Snapshot{
		RuntimeID: runtimeID, Sequence: s.Sequence, Terminal: s.Terminal,
		Connections: make([]Connection, 0, len(s.Connections)),
	}
	seen := make(map[string]bool, len(s.Connections))
	for _, item := range s.Connections {
		item.ConnectionID = strings.ToLower(strings.TrimSpace(item.ConnectionID))
		if !validOpaqueID(item.ConnectionID) {
			return Snapshot{}, fmt.Errorf("%w: connection_id must be 32 lowercase hex characters", ErrInvalid)
		}
		if seen[item.ConnectionID] {
			return Snapshot{}, fmt.Errorf("%w: duplicate connection_id", ErrInvalid)
		}
		seen[item.ConnectionID] = true
		item.PeerAddress = boundedText(item.PeerAddress, 128)
		item.Viewer = boundedText(item.Viewer, 128)
		item.Mode = boundedText(item.Mode, 32)
		item.Route = boundedText(item.Route, 32)
		if item.StartedAtMS <= 0 {
			return Snapshot{}, fmt.Errorf("%w: started_at_ms must be positive", ErrInvalid)
		}
		out.Connections = append(out.Connections, item)
	}
	if out.Terminal && len(out.Connections) != 0 {
		return Snapshot{}, fmt.Errorf("%w: terminal snapshot cannot contain active connections", ErrInvalid)
	}
	sort.Slice(out.Connections, func(i, j int) bool {
		return out.Connections[i].ConnectionID < out.Connections[j].ConnectionID
	})
	return out, nil
}

// Digest binds a snapshot sequence to its complete content.
func Digest(s Snapshot) (string, error) {
	normalized, err := s.Normalize()
	if err != nil {
		return "", err
	}
	encoded, err := json.Marshal(normalized)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(encoded)
	return hex.EncodeToString(sum[:]), nil
}

// CommandsDigest binds the exact canonical command list returned to one host
// runtime. Sorting makes repeated delivery order-independent while preserving
// every command field that may affect the native action or receipt.
func CommandsDigest(commands []Command) (string, error) {
	if len(commands) > MaxQueuedCommands {
		return "", fmt.Errorf("%w: more than %d commands", ErrInvalid, MaxQueuedCommands)
	}
	normalized := make([]Command, len(commands))
	for i, command := range commands {
		item, err := command.Normalize()
		if err != nil {
			return "", err
		}
		normalized[i] = item
	}
	sort.Slice(normalized, func(i, j int) bool {
		return normalized[i].CommandID < normalized[j].CommandID
	})
	encoded, err := json.Marshal(normalized)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(encoded)
	return hex.EncodeToString(sum[:]), nil
}

// Normalize validates fields that are authoritative for a polled command.
// Queued and delivered are accepted only for persisted records; a host agent
// requires the exact queued command shape before it invokes the native side.
func (c Command) Normalize() (Command, error) {
	c.CommandID = strings.TrimSpace(c.CommandID)
	c.HostID = strings.TrimSpace(c.HostID)
	c.RuntimeID = strings.ToLower(strings.TrimSpace(c.RuntimeID))
	c.ConnectionID = strings.ToLower(strings.TrimSpace(c.ConnectionID))
	c.Operation = strings.ToLower(strings.TrimSpace(c.Operation))
	c.RequestKey = strings.TrimSpace(c.RequestKey)
	c.Reason = boundedText(c.Reason, 512)
	c.Status = strings.ToLower(strings.TrimSpace(c.Status))
	if c.CommandID == "" || len(c.CommandID) > 64 {
		return Command{}, fmt.Errorf("%w: command_id is required", ErrInvalid)
	}
	if c.HostID == "" || len(c.HostID) > 64 {
		return Command{}, fmt.Errorf("%w: host_id is required", ErrInvalid)
	}
	if !validOpaqueID(c.RuntimeID) {
		return Command{}, fmt.Errorf("%w: runtime_id must be 32 lowercase hex characters", ErrInvalid)
	}
	if !validOpaqueID(c.ConnectionID) {
		return Command{}, fmt.Errorf("%w: connection_id must be 32 lowercase hex characters", ErrInvalid)
	}
	if c.Operation != OperationDisconnect {
		return Command{}, fmt.Errorf("%w: unsupported operation", ErrInvalid)
	}
	if !ValidRequestKey(c.RequestKey) {
		return Command{}, fmt.Errorf("%w: invalid request_key", ErrInvalid)
	}
	switch c.Status {
	case "", CommandQueued, CommandDelivered, CommandApplied, CommandFailed, CommandExpired:
	default:
		return Command{}, fmt.Errorf("%w: unsupported command status", ErrInvalid)
	}
	if c.IssuedAtMS <= 0 || c.DeadlineMS <= c.IssuedAtMS {
		return Command{}, fmt.Errorf("%w: invalid command deadline", ErrInvalid)
	}
	return c, nil
}

// NormalizeReceipt validates the native result and returns bounded text.
func (r Receipt) Normalize() (Receipt, error) {
	if r.ReceiptSequence == 0 || r.ReceiptSequence > math.MaxInt64 {
		return Receipt{}, fmt.Errorf("%w: receipt_sequence must be between 1 and %d", ErrInvalid, int64(math.MaxInt64))
	}
	r.CommandID = strings.TrimSpace(r.CommandID)
	if r.CommandID == "" || len(r.CommandID) > 64 {
		return Receipt{}, fmt.Errorf("%w: command_id is required", ErrInvalid)
	}
	r.Status = strings.ToLower(strings.TrimSpace(r.Status))
	switch r.Status {
	case CommandApplied:
		if !r.ConfirmedAbsent {
			return Receipt{}, fmt.Errorf("%w: applied requires confirmed_absent", ErrInvalid)
		}
	case CommandFailed:
	default:
		return Receipt{}, fmt.Errorf("%w: receipt status must be applied or failed", ErrInvalid)
	}
	r.Detail = boundedText(r.Detail, 512)
	return r, nil
}

func ValidRequestKey(value string) bool {
	value = strings.TrimSpace(value)
	return len(value) >= 8 && len(value) <= 128 && textOnly(value)
}

func ValidRuntimeID(value string) bool {
	return validOpaqueID(strings.ToLower(strings.TrimSpace(value)))
}

func ValidConnectionID(value string) bool {
	return validOpaqueID(strings.ToLower(strings.TrimSpace(value)))
}

func validOpaqueID(value string) bool {
	if len(value) != 32 {
		return false
	}
	for _, c := range value {
		if (c < '0' || c > '9') && (c < 'a' || c > 'f') {
			return false
		}
	}
	return true
}

func textOnly(value string) bool {
	for _, c := range value {
		if (c < 'a' || c > 'z') && (c < 'A' || c > 'Z') &&
			(c < '0' || c > '9') && c != '-' && c != '_' {
			return false
		}
	}
	return true
}

func boundedText(value string, max int) string {
	value = strings.TrimSpace(value)
	if len(value) <= max {
		return value
	}
	return value[:max]
}
