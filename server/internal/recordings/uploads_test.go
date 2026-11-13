package recordings

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestResumableUploadSurvivesArchiveRestart(t *testing.T) {
	archive, store, user := openArchive(t, 0, 0)
	upload, err := archive.BeginUpload(user.ID, `..\录制.MP4`, "devbox", 1700000000000, 5000, 10)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	if upload.Offset != 0 || upload.Size != 10 || upload.Name != "录制.MP4" {
		t.Fatalf("begin = %+v", upload)
	}
	got, err := archive.AppendUpload(user.ID, upload.ID, 0, 4, strings.NewReader("abcd"))
	if err != nil || got.Offset != 4 {
		t.Fatalf("first chunk = %+v %v", got, err)
	}
	_, err = archive.AppendUpload(user.ID, upload.ID, 0, 4, strings.NewReader("abcd"))
	var offsetErr *UploadOffsetError
	if !errors.As(err, &offsetErr) || offsetErr.Actual != 4 {
		t.Fatalf("stale append = %v", err)
	}
	if _, err := archive.CompleteUpload(user.ID, upload.ID); !errors.Is(err, ErrUploadIncomplete) {
		t.Fatalf("incomplete = %v", err)
	}

	// A server restart discovers the part and state files without an in-memory
	// offset. The next chunk continues from the durable file length.
	restarted, err := New(Config{Dir: archive.Dir(), Accounts: store})
	if err != nil {
		t.Fatalf("restart archive: %v", err)
	}
	got, err = restarted.UploadStatus(user.ID, upload.ID)
	if err != nil || got.Offset != 4 {
		t.Fatalf("restart status = %+v %v", got, err)
	}
	if _, err := restarted.AppendUpload(user.ID, upload.ID, 4, 6, strings.NewReader("efghij")); err != nil {
		t.Fatalf("resume: %v", err)
	}
	rec, err := restarted.CompleteUpload(user.ID, upload.ID)
	if err != nil {
		t.Fatalf("complete: %v", err)
	}
	want := sha256.Sum256([]byte("abcdefghij"))
	if rec.Size != 10 || rec.SHA256 != hex.EncodeToString(want[:]) || rec.PeerID != "devbox" {
		t.Fatalf("recording = %+v", rec)
	}
	file, _, err := restarted.Open(user.ID, rec.ID)
	if err != nil {
		t.Fatalf("open completed: %v", err)
	}
	file.Close()
	if _, err := restarted.UploadStatus(user.ID, upload.ID); !errors.Is(err, ErrUnknownUpload) {
		t.Fatalf("completed session remains: %v", err)
	}
}

func TestResumableUploadRollsBackShortChunkAndIsolatesAccounts(t *testing.T) {
	archive, store, user := openArchive(t, 0, 0)
	other, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	upload, err := archive.BeginUpload(user.ID, "clip.mp4", "", 0, 0, 4)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	if _, err := archive.AppendUpload(user.ID, upload.ID, 0, 4, bytes.NewReader([]byte("abc"))); !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Fatalf("short chunk = %v", err)
	}
	status, err := archive.UploadStatus(user.ID, upload.ID)
	if err != nil || status.Offset != 0 {
		t.Fatalf("offset after rollback = %+v %v", status, err)
	}
	if _, err := archive.UploadStatus(other.ID, upload.ID); !errors.Is(err, ErrUnknownUpload) {
		t.Fatalf("foreign status = %v", err)
	}
	if err := archive.RemoveUpload(other.ID, upload.ID); !errors.Is(err, ErrUnknownUpload) {
		t.Fatalf("foreign remove = %v", err)
	}
	if err := archive.RemoveUpload(user.ID, upload.ID); err != nil {
		t.Fatalf("remove: %v", err)
	}
	if _, err := os.Stat(filepath.Join(archive.Dir(), user.ID, tempPrefix+"session-"+upload.ID+".part")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("part after remove: %v", err)
	}
}
