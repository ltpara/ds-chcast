package account

import (
	"fmt"
	"testing"
	"time"
)

func TestArchiveCheckpoints(t *testing.T) {
	store := openTemp(t, "archive.db")
	if last, err := store.ArchiveCheckpoint(HistoryAudit); err != nil || last != 0 {
		t.Fatalf("fresh checkpoint = %d, %v", last, err)
	}
	if _, err := store.ArchiveCheckpoint(" "); err == nil {
		t.Fatal("empty stream was accepted")
	}
	if err := store.AdvanceArchiveCheckpoint(HistoryAudit, 12); err != nil {
		t.Fatalf("advance: %v", err)
	}
	if err := store.AdvanceArchiveCheckpoint(HistoryAudit, 7); err != nil {
		t.Fatalf("rewind: %v", err)
	}
	if last, err := store.ArchiveCheckpoint(HistoryAudit); err != nil || last != 12 {
		t.Fatalf("checkpoint after rewind = %d, %v", last, err)
	}
	if err := store.AdvanceArchiveCheckpoint(HistoryAudit, 0); err == nil {
		t.Fatal("zero id was accepted")
	}
	path := store.path
	if err := store.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer reopened.Close()
	if last, err := reopened.ArchiveCheckpoint(HistoryAudit); err != nil || last != 12 {
		t.Fatalf("persisted checkpoint = %d, %v", last, err)
	}
	if last, err := reopened.ArchiveCheckpoint(HistoryConnections); err != nil || last != 0 {
		t.Fatalf("unrelated stream checkpoint = %d, %v", last, err)
	}
}

func TestHistoryAscendingPages(t *testing.T) {
	store := openTemp(t, "archive-ascending.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	now := time.Date(2026, 10, 28, 3, 0, 0, 0, time.UTC)
	for _, detail := range []string{"one", "two", "three", "four", "five"} {
		if err := store.AppendAudit(AuditEvent{
			Username: "alice", Action: AuditLogin, Detail: detail, CreatedAt: now, Success: true,
		}); err != nil {
			t.Fatalf("append %s: %v", detail, err)
		}
	}
	page, err := store.AuditEventsAfter(0, 2)
	if err != nil || len(page) != 2 || page[0].Detail != "one" || page[1].Detail != "two" {
		t.Fatalf("audit first page = %+v, %v", page, err)
	}
	page, err = store.AuditEventsAfter(page[len(page)-1].ID, 2)
	if err != nil || len(page) != 2 || page[0].Detail != "three" || page[1].Detail != "four" {
		t.Fatalf("audit second page = %+v, %v", page, err)
	}
	page, err = store.AuditEventsAfter(page[len(page)-1].ID, 2)
	if err != nil || len(page) != 1 || page[0].Detail != "five" {
		t.Fatalf("audit third page = %+v, %v", page, err)
	}
	if page, err := store.AuditEventsAfter(page[0].ID, 0); err != nil || len(page) != 0 {
		t.Fatalf("audit tail page = %+v, %v", page, err)
	}

	for i := 0; i < 3; i++ {
		if _, err := store.RecordConnection(alice.ID, alice.Username, "devbox", "viewer",
			"127.0.0.1", "", now, now.Add(time.Minute), true, fmt.Sprintf("c%d", i)); err != nil {
			t.Fatalf("record connection: %v", err)
		}
		if _, err := store.RecordFileTransfer(alice.ID, alice.Username, "devbox", "upload",
			"dump.bin", "127.0.0.1", "", 1, 1, now, true, fmt.Sprintf("f%d", i)); err != nil {
			t.Fatalf("record transfer: %v", err)
		}
	}
	conns, err := store.ConnectionsAfter(0, 2)
	if err != nil || len(conns) != 2 || conns[0].Detail != "c0" || conns[1].Detail != "c1" {
		t.Fatalf("connection page = %+v, %v", conns, err)
	}
	if conns, err = store.ConnectionsAfter(conns[1].ID, 10); err != nil ||
		len(conns) != 1 || conns[0].Detail != "c2" {
		t.Fatalf("connection tail = %+v, %v", conns, err)
	}
	files, err := store.FileTransfersAfter(0, 2)
	if err != nil || len(files) != 2 || files[0].Detail != "f0" || files[1].Detail != "f1" {
		t.Fatalf("file page = %+v, %v", files, err)
	}
	if files, err = store.FileTransfersAfter(files[1].ID, 10); err != nil ||
		len(files) != 1 || files[0].Detail != "f2" {
		t.Fatalf("file tail = %+v, %v", files, err)
	}
}
