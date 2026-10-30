package recordings

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

func openArchive(t *testing.T, quota, max int64) (*Archive, *account.Store, account.User) {
	t.Helper()
	store, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatalf("account store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	archive, err := New(Config{
		Dir:       filepath.Join(t.TempDir(), "recordings"),
		Quota:     quota,
		MaxUpload: max,
		Accounts:  store,
	})
	if err != nil {
		t.Fatalf("archive: %v", err)
	}
	return archive, store, user
}

func TestArchiveSaveOpenRemove(t *testing.T) {
	archive, store, user := openArchive(t, 0, 0)
	rec, err := archive.Save(user.ID, `..\evil/录制.MP4`, "devbox", 1700000000000, 5000,
		strings.NewReader("mp4-bytes"), 9)
	if err != nil {
		t.Fatalf("save: %v", err)
	}
	if rec.Name != "录制.MP4" {
		t.Fatalf("name = %q", rec.Name)
	}
	if rec.Size != 9 || rec.PeerID != "devbox" || rec.StartedAtMS != 1700000000000 || rec.DurationMS != 5000 {
		t.Fatalf("metadata = %+v", rec)
	}
	file, opened, err := archive.Open(user.ID, rec.ID)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	data := make([]byte, 9)
	if _, err := file.Read(data); err != nil {
		t.Fatalf("read: %v", err)
	}
	file.Close()
	if string(data) != "mp4-bytes" || opened.ID != rec.ID {
		t.Fatalf("opened = %+v %q", opened, data)
	}
	if _, _, err := archive.Open("someone-else", rec.ID); !errors.Is(err, account.ErrUnknownRecording) {
		t.Fatalf("foreign open = %v", err)
	}
	if _, err := archive.Remove(user.ID, rec.ID); err != nil {
		t.Fatalf("remove: %v", err)
	}
	if _, err := os.Stat(filepath.Join(archive.Dir(), user.ID, rec.ID+".mp4")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("file after remove: %v", err)
	}
	usage, files, err := store.RecordingUsage(user.ID)
	if err != nil || usage != 0 || files != 0 {
		t.Fatalf("usage after remove = %d/%d %v", usage, files, err)
	}
}

func TestArchiveSaveRejects(t *testing.T) {
	archive, _, user := openArchive(t, 10, 8)
	if _, err := archive.Save(user.ID, "a.mp4", "", 0, 0, strings.NewReader("123456789"), -1); !errors.Is(err, ErrTooLarge) {
		t.Fatalf("streamed oversized = %v", err)
	}
	if _, err := archive.Save(user.ID, "a.mp4", "", 0, 0, strings.NewReader("123456789"), 9); !errors.Is(err, ErrTooLarge) {
		t.Fatalf("declared oversized = %v", err)
	}
	if _, err := archive.Save(user.ID, "a.mp4", "", 0, 0, strings.NewReader(""), 0); !errors.Is(err, ErrEmpty) {
		t.Fatalf("empty = %v", err)
	}
	if _, err := archive.Save(user.ID, "a.mp4", "", 0, 0, strings.NewReader("123456"), 6); err != nil {
		t.Fatalf("first save: %v", err)
	}
	if _, err := archive.Save(user.ID, "a.mp4", "", 0, 0, strings.NewReader("123456"), 6); !errors.Is(err, ErrQuota) {
		t.Fatalf("quota = %v", err)
	}
	entries, err := os.ReadDir(filepath.Join(archive.Dir(), user.ID))
	if err != nil {
		t.Fatalf("read dir: %v", err)
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), tempPrefix) {
			t.Fatalf("temp file left behind: %s", entry.Name())
		}
	}
}

func TestArchiveRetentionAndOrphanSweep(t *testing.T) {
	archive, store, user := openArchive(t, 0, 0)
	base := time.Now().Add(-72 * time.Hour)
	clock := base
	store.SetClock(func() time.Time { return clock })
	old, err := archive.Save(user.ID, "old.mp4", "", 0, 0, strings.NewReader("old"), 3)
	if err != nil {
		t.Fatalf("save old: %v", err)
	}
	clock = base.Add(24 * time.Hour)
	orphan, err := archive.Save(user.ID, "orphan.mp4", "", 0, 0, strings.NewReader("orphan"), 6)
	if err != nil {
		t.Fatalf("save orphan: %v", err)
	}
	clock = base.Add(36 * time.Hour)
	live, err := archive.Save(user.ID, "live.mp4", "", 0, 0, strings.NewReader("live"), 4)
	if err != nil {
		t.Fatalf("save live: %v", err)
	}

	purged, err := archive.PurgeExpired(base.Add(12 * time.Hour))
	if err != nil || purged != 1 {
		t.Fatalf("purge = %d %v", purged, err)
	}
	if _, err := os.Stat(filepath.Join(archive.Dir(), user.ID, old.ID+".mp4")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("expired file: %v", err)
	}
	file, _, err := archive.Open(user.ID, live.ID)
	if err != nil {
		t.Fatalf("live after purge: %v", err)
	}
	file.Close()

	// Remove one row behind the archive's back, then age the files so the
	// orphan sweep treats the leftover as abandoned.
	if _, found, err := store.DeleteRecording(user.ID, orphan.ID); err != nil || !found {
		t.Fatalf("drop orphan row = %v %v", found, err)
	}
	orphanPath := filepath.Join(archive.Dir(), user.ID, orphan.ID+".mp4")
	livePath := filepath.Join(archive.Dir(), user.ID, live.ID+".mp4")
	tempPath := filepath.Join(archive.Dir(), user.ID, tempPrefix+"stale")
	if err := os.WriteFile(tempPath, []byte("partial"), 0o600); err != nil {
		t.Fatalf("temp file: %v", err)
	}
	stale := base
	for _, path := range []string{orphanPath, livePath, tempPath} {
		if err := os.Chtimes(path, stale, stale); err != nil {
			t.Fatalf("chtimes %s: %v", path, err)
		}
	}
	deadDir := filepath.Join(archive.Dir(), "00112233445566778899aabbccddeeff")
	if err := os.MkdirAll(deadDir, 0o700); err != nil {
		t.Fatalf("dead dir: %v", err)
	}
	if err := os.Chtimes(deadDir, stale, stale); err != nil {
		t.Fatalf("chtimes dead dir: %v", err)
	}

	removed, err := archive.PurgeOrphans(time.Now())
	if err != nil {
		t.Fatalf("orphan sweep: %v", err)
	}
	if removed != 3 {
		t.Fatalf("orphan removed = %d, want 3", removed)
	}
	for _, path := range []string{orphanPath, tempPath, deadDir} {
		if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
			t.Fatalf("%s survived the sweep: %v", path, err)
		}
	}
	file, _, err = archive.Open(user.ID, live.ID)
	if err != nil {
		t.Fatalf("live file swept: %v", err)
	}
	file.Close()

	// With the last row gone the account directory itself is abandoned.
	if _, found, err := store.DeleteRecording(user.ID, live.ID); err != nil || !found {
		t.Fatalf("drop live row = %v %v", found, err)
	}
	if err := os.Chtimes(filepath.Join(archive.Dir(), user.ID), stale, stale); err != nil {
		t.Fatalf("chtimes account dir: %v", err)
	}
	if removed, err := archive.PurgeOrphans(time.Now()); err != nil || removed != 1 {
		t.Fatalf("account dir sweep = %d %v", removed, err)
	}
	if _, err := os.Stat(filepath.Join(archive.Dir(), user.ID)); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("account dir survived: %v", err)
	}
}
