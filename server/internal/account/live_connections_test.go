package account

import (
	"errors"
	"path/filepath"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/liveconn"
)

const (
	liveRuntimeA = "11111111111111111111111111111111"
	liveRuntimeB = "22222222222222222222222222222222"
	liveConnA    = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	liveConnB    = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	liveConnC    = "cccccccccccccccccccccccccccccccc"
)

func testLiveSnapshot(runtimeID string, sequence uint64, terminal bool, ids ...string) liveconn.Snapshot {
	snapshot := liveconn.Snapshot{RuntimeID: runtimeID, Sequence: sequence, Terminal: terminal}
	for i, id := range ids {
		snapshot.Connections = append(snapshot.Connections, liveconn.Connection{
			ConnectionID: id,
			PeerAddress:  "192.0.2.10",
			Viewer:       "operator",
			Mode:         "control",
			Route:        "relay",
			Permissions:  7,
			Controller:   i == 0,
			StartedAtMS:  time.Date(2026, 11, 16, 1, 0, 0, 0, time.UTC).UnixMilli(),
		})
	}
	return snapshot
}

func mustLiveSnapshot(t *testing.T, store *Store, snapshot liveconn.Snapshot) {
	t.Helper()
	if _, err := store.ApplyLiveSnapshot("devbox", snapshot); err != nil {
		t.Fatalf("apply snapshot sequence %d: %v", snapshot.Sequence, err)
	}
}

func TestLiveConnectionSnapshotFreshnessAndTerminalState(t *testing.T) {
	store := openGroupStore(t)
	now := time.Date(2026, 11, 16, 2, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	first := testLiveSnapshot(liveRuntimeA, 1, false, liveConnA, liveConnB)
	mustLiveSnapshot(t, store, first)

	items, err := store.LiveConnections(100)
	if err != nil || len(items) != 2 {
		t.Fatalf("initial list = %+v, %v", items, err)
	}
	for _, item := range items {
		if item.State != liveconn.ConnectionActive {
			t.Fatalf("initial state = %+v", item)
		}
	}

	now = now.Add(30 * time.Second)
	// Connection order is not part of snapshot identity; a retry refreshes the
	// runtime and its active sessions together.
	retry := first
	retry.Connections = append([]liveconn.Connection(nil), first.Connections...)
	retry.Connections[0], retry.Connections[1] = retry.Connections[1], retry.Connections[0]
	mustLiveSnapshot(t, store, retry)
	items, err = store.LiveConnections(100)
	if err != nil || len(items) != 2 {
		t.Fatalf("retry list = %+v, %v", items, err)
	}
	for _, item := range items {
		if !item.LastHeartbeatAt.Equal(now) || item.State != liveconn.ConnectionActive {
			t.Fatalf("retry freshness = %+v, now=%v", item, now)
		}
	}

	now = now.Add(liveconn.Freshness + time.Millisecond)
	items, err = store.LiveConnections(100)
	if err != nil || len(items) != 2 {
		t.Fatalf("stale list = %+v, %v", items, err)
	}
	for _, item := range items {
		if item.State != liveconn.ConnectionStale {
			t.Fatalf("stale state = %+v", item)
		}
	}
	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 2, false, liveConnA, liveConnB))
	items, err = store.LiveConnections(100)
	if err != nil || len(items) != 2 {
		t.Fatalf("recovered list = %+v, %v", items, err)
	}
	for _, item := range items {
		if item.State != liveconn.ConnectionActive {
			t.Fatalf("recovered state = %+v", item)
		}
	}

	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 3, false, liveConnA))
	closed, err := store.LiveConnection("devbox", liveConnB)
	if err != nil || closed.State != liveconn.ConnectionClosed {
		t.Fatalf("closed detail = %+v, %v", closed, err)
	}
	items, err = store.LiveConnections(100)
	if err != nil || len(items) != 1 || items[0].ConnectionID != liveConnA {
		t.Fatalf("closed connection leaked into directory: %+v, %v", items, err)
	}

	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 4, true))
	if _, err := store.ApplyLiveSnapshot("devbox", testLiveSnapshot(liveRuntimeA, 5, false, liveConnC)); !errors.Is(err, ErrLiveRuntimeTerminal) {
		t.Fatalf("terminal runtime revived: %v", err)
	}
}

func TestLiveConnectionSequenceAndIdentityConflicts(t *testing.T) {
	store := openGroupStore(t)
	store.SetClock(func() time.Time {
		return time.Date(2026, 11, 16, 3, 0, 0, 0, time.UTC)
	})
	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 1, false, liveConnA))
	if _, err := store.ApplyLiveSnapshot("devbox", testLiveSnapshot(liveRuntimeA, 2, false, liveConnB)); err != nil {
		t.Fatal(err)
	}
	if _, err := store.ApplyLiveSnapshot("devbox", testLiveSnapshot(liveRuntimeA, 2, false, liveConnA)); !errors.Is(err, ErrLiveSequenceConflict) {
		t.Fatalf("same sequence changed content: %v", err)
	}
	if _, err := store.ApplyLiveSnapshot("devbox", testLiveSnapshot(liveRuntimeA, 1, false, liveConnB)); !errors.Is(err, ErrLiveSequenceConflict) {
		t.Fatalf("lower sequence accepted: %v", err)
	}
	if _, err := store.ApplyLiveSnapshot("devbox", testLiveSnapshot(liveRuntimeB, 1, false, liveConnA)); !errors.Is(err, liveconn.ErrConflict) {
		t.Fatalf("connection id reused across runtime: %v", err)
	}
	if _, err := store.ApplyLiveSnapshot("devbox", testLiveSnapshot(liveRuntimeB, 1, false, liveConnC)); err != nil {
		t.Fatalf("second runtime rejected: %v", err)
	}
	items, err := store.LiveConnections(100)
	if err != nil || len(items) != 2 {
		t.Fatalf("multi-runtime list = %+v, %v", items, err)
	}
}

func TestLiveDisconnectIdempotencyReceiptAndExactTarget(t *testing.T) {
	store := openGroupStore(t)
	now := time.Date(2026, 11, 16, 4, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 1, false, liveConnA, liveConnB))

	first, err := store.DisconnectLiveConnection("devbox", liveConnA, "request-0001", "alice", "support")
	if err != nil {
		t.Fatal(err)
	}
	retry, err := store.DisconnectLiveConnection("devbox", liveConnA, "request-0001", "alice", "support")
	if err != nil || retry != first {
		t.Fatalf("idempotent retry = %+v, %v; first=%+v", retry, err, first)
	}
	if _, err := store.DisconnectLiveConnection("devbox", liveConnB, "request-0001", "alice", "support"); !errors.Is(err, liveconn.ErrConflict) {
		t.Fatalf("request key reused for another target: %v", err)
	}
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, liveconn.Receipt{
		ReceiptSequence: 1, CommandID: first.CommandID, Status: liveconn.CommandFailed,
		Detail: "the host has not polled the command",
	}); !errors.Is(err, ErrLiveCommandReceipt) {
		t.Fatalf("failed receipt before delivery = %v", err)
	}

	commands, err := store.PollLiveCommands("devbox", liveRuntimeA, 32)
	if err != nil || len(commands) != 1 || commands[0].CommandID != first.CommandID {
		t.Fatalf("poll = %+v, %v", commands, err)
	}
	var firstDeliveredAt int64
	if err := store.db.QueryRow(`SELECT delivered_at FROM live_connection_commands
		WHERE command_id = ?`, first.CommandID).Scan(&firstDeliveredAt); err != nil || firstDeliveredAt != now.UnixMilli() {
		t.Fatalf("first delivered_at = %d, %v; want %d", firstDeliveredAt, err, now.UnixMilli())
	}
	now = now.Add(5 * time.Second)
	commands, err = store.PollLiveCommands("devbox", liveRuntimeA, 32)
	if err != nil || len(commands) != 1 || commands[0].CommandID != first.CommandID {
		t.Fatalf("unacknowledged command redelivery = %+v, %v", commands, err)
	}
	var deliveredAt int64
	if err := store.db.QueryRow(`SELECT delivered_at FROM live_connection_commands
		WHERE command_id = ?`, first.CommandID).Scan(&deliveredAt); err != nil || deliveredAt != firstDeliveredAt {
		t.Fatalf("redelivery changed delivered_at = %d, %v; want %d", deliveredAt, err, firstDeliveredAt)
	}
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, liveconn.Receipt{
		ReceiptSequence: 1, CommandID: first.CommandID, Status: liveconn.CommandApplied,
	}); !errors.Is(err, liveconn.ErrInvalid) {
		t.Fatalf("applied without absence confirmation: %v", err)
	}
	receipt := liveconn.Receipt{
		ReceiptSequence: 1, CommandID: first.CommandID, Status: liveconn.CommandApplied,
		ConfirmedAbsent: true, Detail: "native session removed",
	}
	if got, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, receipt); err != nil || got != receipt {
		t.Fatalf("receipt = %+v, %v", got, err)
	}
	if got, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, receipt); err != nil || got != receipt {
		t.Fatalf("receipt retry = %+v, %v", got, err)
	}
	if commands, err := store.PollLiveCommands("devbox", liveRuntimeA, 32); err != nil || len(commands) != 0 {
		t.Fatalf("settled command redelivered: %+v, %v", commands, err)
	}
	lower := receipt
	lower.ReceiptSequence = 2
	lower.Detail = "later native result"
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, lower); err != nil {
		t.Fatalf("second receipt = %v", err)
	}
	lower.ReceiptSequence = 1
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, lower); !errors.Is(err, ErrLiveCommandReceipt) {
		t.Fatalf("lower sequence = %v", err)
	}
	closed, err := store.LiveConnection("devbox", liveConnA)
	if err != nil || closed.State != liveconn.ConnectionClosed ||
		closed.CommandStatus != liveconn.CommandApplied || !closed.ReceiptConfirmedAbsent {
		t.Fatalf("target detail = %+v, %v", closed, err)
	}
	other, err := store.LiveConnection("devbox", liveConnB)
	if err != nil || other.State != liveconn.ConnectionActive {
		t.Fatalf("other connection changed = %+v, %v", other, err)
	}
}

func TestLiveConnectionListsExpireCommandsWithoutHostOrDetailRead(t *testing.T) {
	for _, status := range []string{liveconn.CommandQueued, liveconn.CommandDelivered} {
		t.Run(status, func(t *testing.T) {
			store := openGroupStore(t)
			now := time.Date(2026, 11, 16, 5, 0, 0, 0, time.UTC)
			store.SetClock(func() time.Time { return now })
			if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
				t.Fatal(err)
			}
			if _, err := store.AddGroupDevice("lab", "devbox", "alice"); err != nil {
				t.Fatal(err)
			}
			mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 1, false, liveConnA, liveConnB))
			command, err := store.DisconnectLiveConnection("devbox", liveConnA, "request-expire", "alice", "")
			if err != nil {
				t.Fatal(err)
			}
			if status == liveconn.CommandDelivered {
				if _, err := store.PollLiveCommands("devbox", liveRuntimeA, 32); err != nil {
					t.Fatal(err)
				}
			}
			lists := map[string]func() ([]LiveConnection, error){
				"global": func() ([]LiveConnection, error) { return store.LiveConnections(100) },
				"group":  func() ([]LiveConnection, error) { return store.LiveConnectionsInGroup("lab", 100) },
				"devices": func() ([]LiveConnection, error) {
					return store.LiveConnectionsForDevices(map[string]bool{"devbox": true}, 100)
				},
			}
			for _, offset := range []time.Duration{-time.Millisecond, 0, time.Second} {
				now = time.UnixMilli(command.DeadlineMS).Add(offset)
				wantStatus := liveconn.CommandExpired
				if offset < 0 {
					wantStatus = status
				}
				for name, list := range lists {
					items, err := list()
					if err != nil || len(items) != 2 {
						t.Fatalf("%s at %v = %+v, %v", name, offset, items, err)
					}
					for _, item := range items {
						if item.State != liveconn.ConnectionStale || item.ReceiptConfirmedAbsent {
							t.Fatalf("expiry must not imply disconnect: %+v", item)
						}
						if item.ConnectionID == liveConnA && (item.CommandID != command.CommandID || item.CommandStatus != wantStatus) {
							t.Fatalf("%s at %v: target = %+v, want %s", name, offset, item, wantStatus)
						}
						if item.ConnectionID == liveConnB && item.CommandID != "" {
							t.Fatalf("other connection changed: %+v", item)
						}
					}
				}
			}
		})
	}
}

func TestLiveDisconnectExpiryAndFailedReceipt(t *testing.T) {
	store := openGroupStore(t)
	now := time.Date(2026, 11, 16, 5, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 1, false, liveConnA, liveConnB))

	command, err := store.DisconnectLiveConnection("devbox", liveConnA, "request-expire", "alice", "")
	if err != nil {
		t.Fatal(err)
	}
	now = now.Add(liveconn.CommandTTL + time.Second)
	expired, err := store.LiveConnection("devbox", liveConnA)
	if err != nil || expired.CommandStatus != liveconn.CommandExpired ||
		expired.State != liveconn.ConnectionStale {
		t.Fatalf("reading expired command = %+v, %v", expired, err)
	}
	if queued, err := store.PollLiveCommands("devbox", liveRuntimeA, 32); err != nil || len(queued) != 0 {
		t.Fatalf("expired command delivered: %+v, %v", queued, err)
	}
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, liveconn.Receipt{
		ReceiptSequence: 1, CommandID: command.CommandID, Status: liveconn.CommandApplied,
		ConfirmedAbsent: true,
	}); !errors.Is(err, ErrLiveCommandExpired) {
		t.Fatalf("late applied receipt = %v", err)
	}

	// Commands can remain queued longer than the freshness window. A host
	// heartbeat using the already accepted snapshot refreshes the runtime
	// without pretending that any connection left.
	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 1, false, liveConnA, liveConnB))
	failed, err := store.DisconnectLiveConnection("devbox", liveConnB, "request-failed", "alice", "")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, liveconn.Receipt{
		ReceiptSequence: 1, CommandID: failed.CommandID, Status: liveconn.CommandFailed,
		Detail: "native API rejected the operation",
	}); !errors.Is(err, ErrLiveCommandReceipt) {
		t.Fatalf("failed receipt before delivery = %v", err)
	}
	if _, err := store.PollLiveCommands("devbox", liveRuntimeA, 32); err != nil {
		t.Fatalf("poll failed command: %v", err)
	}
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, liveconn.Receipt{
		ReceiptSequence: 1, CommandID: failed.CommandID, Status: liveconn.CommandFailed,
		Detail: "native API rejected the operation",
	}); err != nil {
		t.Fatalf("failed receipt before deadline: %v", err)
	}
	detail, err := store.LiveConnection("devbox", liveConnB)
	if err != nil || detail.CommandStatus != liveconn.CommandFailed ||
		detail.State != liveconn.ConnectionActive {
		t.Fatalf("failed command changed live connection: %+v, %v", detail, err)
	}
}

func TestLiveConnectionsSurviveSQLiteRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "live.db")
	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 11, 16, 6, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	mustLiveSnapshot(t, store, testLiveSnapshot(liveRuntimeA, 1, false, liveConnA))
	command, err := store.DisconnectLiveConnection("devbox", liveConnA, "request-restart", "alice", "")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.PollLiveCommands("devbox", liveRuntimeA, 32); err != nil {
		t.Fatal(err)
	}
	if _, err := store.RecordLiveCommandReceipt("devbox", liveRuntimeA, liveconn.Receipt{
		ReceiptSequence: 1, CommandID: command.CommandID, Status: liveconn.CommandApplied,
		ConfirmedAbsent: true, Detail: "removed",
	}); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	store.SetClock(func() time.Time { return now })
	detail, err := store.LiveConnection("devbox", liveConnA)
	if err != nil || detail.State != liveconn.ConnectionClosed ||
		detail.CommandStatus != liveconn.CommandApplied {
		t.Fatalf("restart state = %+v, %v", detail, err)
	}
}
