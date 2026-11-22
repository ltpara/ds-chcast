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
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

func TestResumableUploadReclaimsAbandonedSession(t *testing.T) {
	archive, _, user := openArchive(t, 0, 0)
	upload, err := archive.BeginUpload(user.ID, "clip.mp4", "", 0, 0, 4)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	if _, err := archive.AppendUpload(user.ID, upload.ID, 0, 4, strings.NewReader("abcd")); err != nil {
		t.Fatalf("append: %v", err)
	}
	// Age the part file past the idle window, as an uploader that crashed
	// mid-transfer would leave it.
	stale := time.Now().Add(-uploadIdleTTL - time.Minute)
	part := filepath.Join(archive.Dir(), user.ID, tempPrefix+"session-"+upload.ID+".part")
	if err := os.Chtimes(part, stale, stale); err != nil {
		t.Fatalf("age part: %v", err)
	}
	if _, err := archive.UploadStatus(user.ID, upload.ID); !errors.Is(err, ErrUnknownUpload) {
		t.Fatalf("idle status = %v", err)
	}
	// The next BeginUpload reclaims the files so the slot and quota return.
	if _, err := archive.BeginUpload(user.ID, "clip2.mp4", "", 0, 0, 4); err != nil {
		t.Fatalf("begin after idle: %v", err)
	}
	if _, err := os.Stat(part); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("idle part survived reclaim: %v", err)
	}
}

func TestAppendUploadWithDigestRollsBackMismatch(t *testing.T) {
	archive, _, user := openArchive(t, 0, 0)
	const content = "abcdefgh"
	contentHash := sha256.Sum256([]byte(content))
	digest := hex.EncodeToString(contentHash[:])
	upload, err := archive.BeginUpload(user.ID, "clip.mp4", "host", 0, 0, int64(len(content)))
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	if _, err := archive.AppendUploadWithDigest(user.ID, upload.ID, 0, int64(len(content)),
		strings.NewReader("ABCDEFGH"), digest); !errors.Is(err, ErrUploadDigest) {
		t.Fatalf("mismatched digest = %v", err)
	}
	status, err := archive.UploadStatus(user.ID, upload.ID)
	if err != nil || status.Offset != 0 {
		t.Fatalf("status after mismatch = %+v, %v", status, err)
	}
	status, err = archive.AppendUploadWithDigest(user.ID, upload.ID, 0, int64(len(content)),
		strings.NewReader(content), digest)
	if err != nil || status.Offset != int64(len(content)) {
		t.Fatalf("valid append = %+v, %v", status, err)
	}
	if _, err := archive.AppendUploadWithDigest(user.ID, upload.ID, int64(len(content)), 1,
		strings.NewReader("x"), "not-a-digest"); !errors.Is(err, ErrUploadDigest) {
		t.Fatalf("invalid digest = %v", err)
	}
}

func TestResumableUploadReclaimWaitsForWriterAndRechecksIdle(t *testing.T) {
	archive, _, user := openArchive(t, 0, 0)
	upload, err := archive.BeginUpload(user.ID, "clip.mp4", "", 0, 0, 4)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	part := archive.uploadPartPath(user.ID, upload.ID)
	stale := time.Now().Add(-uploadIdleTTL - time.Minute)
	if err := os.Chtimes(part, stale, stale); err != nil {
		t.Fatalf("age part: %v", err)
	}

	lock := archive.uploadLock(upload.ID)
	lock.Lock()
	done := make(chan error, 1)
	go func() {
		_, beginErr := archive.BeginUpload(user.ID, "next.mp4", "", 0, 0, 4)
		done <- beginErr
	}()
	select {
	case beginErr := <-done:
		lock.Unlock()
		t.Fatalf("reclaim did not wait for active upload lock: %v", beginErr)
	case <-time.After(100 * time.Millisecond):
	}
	if err := os.Chtimes(part, time.Now(), time.Now()); err != nil {
		lock.Unlock()
		t.Fatalf("refresh part: %v", err)
	}
	lock.Unlock()
	if err := <-done; err != nil {
		t.Fatalf("begin after writer: %v", err)
	}
	if _, err := archive.UploadStatus(user.ID, upload.ID); err != nil {
		t.Fatalf("active upload was reclaimed: %v", err)
	}
}

func TestResumableUploadRecoversMissingPartFile(t *testing.T) {
	archive, _, user := openArchive(t, 0, 0)
	upload, err := archive.BeginUpload(user.ID, "clip.mp4", "", 0, 0, 4)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	part := filepath.Join(archive.Dir(), user.ID, tempPrefix+"session-"+upload.ID+".part")
	if err := os.Remove(part); err != nil {
		t.Fatalf("remove part: %v", err)
	}
	// Status must report the durable offset the client can resume from, and the
	// append that follows must recreate the file instead of failing forever.
	status, err := archive.UploadStatus(user.ID, upload.ID)
	if err != nil || status.Offset != 0 {
		t.Fatalf("status without part = %+v %v", status, err)
	}
	if _, err := archive.AppendUpload(user.ID, upload.ID, 0, 4, strings.NewReader("abcd")); err != nil {
		t.Fatalf("append after part loss: %v", err)
	}
	if _, err := archive.CompleteUpload(user.ID, upload.ID); err != nil {
		t.Fatalf("complete after part loss: %v", err)
	}
}

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

func TestUploadIdempotencyResumesAndReturnsCompletedRecording(t *testing.T) {
	archive, store, user := openArchive(t, 0, 0)
	const key = "0123456789abcdef0123456789abcdef"
	metadata := func(name string) (Upload, *account.Recording, error) {
		return archive.BeginUploadWithKey(
			user.ID, name, "devbox", 1700000000000, 5000, 4, key,
		)
	}
	upload, completed, err := metadata("clip.mp4")
	if err != nil || completed != nil || upload.IdempotencyKey != key {
		t.Fatalf("begin = %+v, %+v, %v", upload, completed, err)
	}
	resumed, completed, err := metadata("clip.mp4")
	if err != nil || completed != nil || resumed.ID != upload.ID || resumed.Offset != 0 {
		t.Fatalf("duplicate begin = %+v, %+v, %v", resumed, completed, err)
	}
	if _, _, err := metadata("different.mp4"); !errors.Is(err, ErrUploadKeyConflict) {
		t.Fatalf("conflicting begin = %v", err)
	}
	if _, err := archive.AppendUpload(user.ID, upload.ID, 0, 4, strings.NewReader("data")); err != nil {
		t.Fatalf("append: %v", err)
	}
	recording, err := archive.CompleteUpload(user.ID, upload.ID)
	if err != nil || recording.UploadKey != key {
		t.Fatalf("complete = %+v, %v", recording, err)
	}
	restarted, err := New(Config{Dir: archive.Dir(), Accounts: store})
	if err != nil {
		t.Fatalf("restart: %v", err)
	}
	newUpload, alreadyCompleted, err := restarted.BeginUploadWithKey(
		user.ID, "clip.mp4", "devbox", 1700000000000, 5000, 4, key,
	)
	if err != nil || newUpload.ID != "" || alreadyCompleted == nil ||
		alreadyCompleted.ID != recording.ID {
		t.Fatalf("completed begin = %+v, %+v, %v", newUpload, alreadyCompleted, err)
	}
	_, count, err := store.RecordingUsage(user.ID)
	if err != nil || count != 1 {
		t.Fatalf("archive entries = %d, %v", count, err)
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
