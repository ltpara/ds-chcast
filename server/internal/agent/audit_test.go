package agent

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func appendSpool(t *testing.T, path, text string) {
	t.Helper()
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		t.Fatalf("open spool: %v", err)
	}
	defer file.Close()
	if _, err := file.WriteString(text); err != nil {
		t.Fatalf("write spool: %v", err)
	}
}

func waitFor(t *testing.T, timeout time.Duration, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if check() {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestAuditSpoolForwardsHostEvents(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	spool := filepath.Join(t.TempDir(), "audit.jsonl")

	host, err := StartHost(ctx, HostConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		ID:         "devbox",
		Key:        "host-secret",
		LocalAddr:  echo.LocalAddr().String(),
		ListenAddr: "127.0.0.1:0",
		Keepalive:  300 * time.Millisecond,
		AuditSpool: spool,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start host agent: %v", err)
	}
	t.Cleanup(host.Close)

	started := time.Now().Add(-time.Minute).UnixMilli()
	ended := time.Now().UnixMilli()
	appendSpool(t, spool, `{"kind":"session","audit_ref":"00112233aabbccdd","started_at_ms":`+
		itoa(started)+`,"ended_at_ms":`+itoa(ended)+`,"success":true,"detail":"connected"}`+"\n")

	waitFor(t, 10*time.Second, "host session event", func() bool {
		events, err := s.Accounts.ConnectionsForHost("devbox", 10)
		if err != nil || len(events) != 1 {
			return false
		}
		row := events[0]
		return row.HostID == "devbox" && row.Source == "host" &&
			row.AuditRef == "00112233aabbccdd" && row.Username == "-" &&
			row.Success && row.Detail == "connected" && row.StartedAt.UnixMilli() == started
	})
	waitFor(t, 5*time.Second, "spool removal", func() bool {
		_, err := os.Stat(spool)
		return os.IsNotExist(err)
	})

	// A half-written line is not forwarded until the writer finishes it.
	occurred := time.Now().UnixMilli()
	line := `{"kind":"file","audit_ref":"00112233aabbccdd","direction":"upload","name":"a.txt",` +
		`"size":10,"occurred_at_ms":` + itoa(occurred) + `,"success":true}`
	appendSpool(t, spool, line)
	time.Sleep(auditSpoolInterval + 500*time.Millisecond)
	if rows, err := s.Accounts.FileTransfersForHost("devbox", 10); err != nil || len(rows) != 0 {
		t.Fatalf("partial line was forwarded: %+v, %v", rows, err)
	}
	appendSpool(t, spool, "\n")
	waitFor(t, 10*time.Second, "host file event", func() bool {
		rows, err := s.Accounts.FileTransfersForHost("devbox", 10)
		if err != nil || len(rows) != 1 {
			return false
		}
		row := rows[0]
		return row.HostID == "devbox" && row.Source == "host" && row.Direction == "upload" &&
			row.Name == "a.txt" && row.Size == 10 && row.Files == 1 && row.Success
	})

	// Unknown kinds are dropped instead of blocking later events.
	appendSpool(t, spool, `{"kind":"network","audit_ref":"00112233aabbccdd"}`+"\n")
	waitFor(t, 5*time.Second, "unknown kind removal", func() bool {
		_, err := os.Stat(spool)
		return os.IsNotExist(err)
	})
}

func itoa(value int64) string {
	return strconv.FormatInt(value, 10)
}
