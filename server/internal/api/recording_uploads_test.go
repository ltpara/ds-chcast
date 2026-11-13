package api_test

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type resumableUpload struct {
	ID          string `json:"id"`
	Offset      int64  `json:"offset"`
	Size        int64  `json:"size"`
	Name        string `json:"name"`
	PeerID      string `json:"peer_id"`
	StartedAtMS int64  `json:"started_at_ms"`
	DurationMS  int64  `json:"duration_ms"`
	CreatedAtMS int64  `json:"created_at_ms"`
}

type resumableUploadReply struct {
	Upload resumableUpload `json:"upload"`
}

func beginRecordingUpload(t *testing.T, base, token, name string, size, startedAtMS, durationMS int64) (int, []byte) {
	t.Helper()
	body, err := json.Marshal(map[string]any{
		"name":          name,
		"peer_id":       "devbox",
		"size":          size,
		"started_at_ms": startedAtMS,
		"duration_ms":   durationMS,
	})
	if err != nil {
		t.Fatal(err)
	}
	return request(t, http.MethodPost, base+"/v1/account/recordings/uploads", string(body), token)
}

func appendRecordingUpload(t *testing.T, base, token, id string, offset int64, body []byte) (int, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodPatch,
		base+"/v1/account/recordings/uploads/"+id, bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	req.Header.Set("X-Upload-Offset", strconv.FormatInt(offset, 10))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, data
}

func recordingUploadStatus(t *testing.T, base, token, id string) (int, []byte) {
	t.Helper()
	return request(t, http.MethodGet, base+"/v1/account/recordings/uploads/"+id, "", token)
}

func completeRecordingUpload(t *testing.T, base, token, id string) (int, []byte) {
	t.Helper()
	return request(t, http.MethodPost, base+"/v1/account/recordings/uploads/"+id+"/complete", "", token)
}

func decodeResumableUpload(t *testing.T, data []byte) resumableUpload {
	t.Helper()
	var reply resumableUploadReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("upload reply: %v (%s)", err, data)
	}
	return reply.Upload
}

func TestRecordingResumableUploadHTTPFlow(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RecordingsDir: t.TempDir()})
	for _, name := range []string{"alice", "bob"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", name, err)
		}
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	body := []byte("abcdefghijklmnopq")
	name := "跨网络会话 2026-11-14.mp4"

	if status, data := beginRecordingUpload(t, s.URL, "", name, int64(len(body)), 1700000000000, 91000); status != http.StatusUnauthorized {
		t.Fatalf("anonymous begin: %d %s", status, data)
	}
	status, data := beginRecordingUpload(t, s.URL, alice.Token, name, int64(len(body)), 1700000000000, 91000)
	if status != http.StatusCreated {
		t.Fatalf("begin: %d %s", status, data)
	}
	upload := decodeResumableUpload(t, data)
	if upload.ID == "" || upload.Offset != 0 || upload.Size != int64(len(body)) ||
		upload.Name != name || upload.PeerID != "devbox" || upload.DurationMS != 91000 {
		t.Fatalf("begin upload = %+v", upload)
	}

	if status, data := recordingUploadStatus(t, s.URL, bob.Token, upload.ID); status != http.StatusNotFound {
		t.Fatalf("cross-account status: %d %s", status, data)
	}
	if status, data := appendRecordingUpload(t, s.URL, bob.Token, upload.ID, 0, body[:1]); status != http.StatusNotFound {
		t.Fatalf("cross-account append: %d %s", status, data)
	}
	if status, data := completeRecordingUpload(t, s.URL, bob.Token, upload.ID); status != http.StatusNotFound {
		t.Fatalf("cross-account complete: %d %s", status, data)
	}
	if status, data := request(t, http.MethodDelete,
		s.URL+"/v1/account/recordings/uploads/"+upload.ID, "", bob.Token); status != http.StatusNotFound {
		t.Fatalf("cross-account delete: %d %s", status, data)
	}

	status, data = appendRecordingUpload(t, s.URL, alice.Token, upload.ID, 0, body[:6])
	if status != http.StatusAccepted {
		t.Fatalf("first chunk: %d %s", status, data)
	}
	upload = decodeResumableUpload(t, data)
	if upload.Offset != 6 {
		t.Fatalf("first chunk offset = %d", upload.Offset)
	}

	// Simulate a lost PATCH response: query the durable offset and continue
	// from it instead of assuming that every acknowledged byte was lost.
	status, data = recordingUploadStatus(t, s.URL, alice.Token, upload.ID)
	if status != http.StatusOK {
		t.Fatalf("status after lost response: %d %s", status, data)
	}
	upload = decodeResumableUpload(t, data)
	if upload.Offset != 6 {
		t.Fatalf("durable offset = %d", upload.Offset)
	}
	if status, data := appendRecordingUpload(t, s.URL, alice.Token, upload.ID, 0, body[:1]); status != http.StatusConflict {
		t.Fatalf("stale append: %d %s", status, data)
	} else {
		var conflict struct {
			Offset int64 `json:"offset"`
		}
		if err := json.Unmarshal(data, &conflict); err != nil || conflict.Offset != 6 {
			t.Fatalf("stale append reply: %v %+v", err, conflict)
		}
	}
	status, data = appendRecordingUpload(t, s.URL, alice.Token, upload.ID, upload.Offset, body[6:])
	if status != http.StatusAccepted {
		t.Fatalf("resumed chunk: %d %s", status, data)
	}
	upload = decodeResumableUpload(t, data)
	if upload.Offset != int64(len(body)) {
		t.Fatalf("complete offset = %d", upload.Offset)
	}

	status, data = completeRecordingUpload(t, s.URL, alice.Token, upload.ID)
	if status != http.StatusCreated {
		t.Fatalf("complete: %d %s", status, data)
	}
	var completed recordingUploadReply
	if err := json.Unmarshal(data, &completed); err != nil {
		t.Fatalf("complete reply: %v", err)
	}
	sum := sha256.Sum256(body)
	if completed.Recording.Name != name || completed.Recording.Size != int64(len(body)) ||
		completed.Recording.SHA256 != hex.EncodeToString(sum[:]) || completed.Recording.PeerID != "devbox" ||
		completed.UsageBytes != int64(len(body)) {
		t.Fatalf("completed recording = %+v", completed)
	}
	status, downloaded, _ := downloadRecording(t, s.URL, alice.Token, completed.Recording.ID)
	if status != http.StatusOK || !bytes.Equal(downloaded, body) {
		t.Fatalf("download: status=%d body=%q", status, downloaded)
	}
	if status, data := recordingUploadStatus(t, s.URL, alice.Token, upload.ID); status != http.StatusNotFound {
		t.Fatalf("completed upload remains: %d %s", status, data)
	}

	events, err := s.Accounts.AuditEvents(50)
	if err != nil {
		t.Fatalf("audit: %v", err)
	}
	var uploads int
	for _, event := range events {
		if event.Success && event.Action == account.AuditRecordingUpload {
			uploads++
		}
	}
	if uploads != 1 {
		t.Fatalf("recording upload audits = %d", uploads)
	}
}

func TestRecordingResumableUploadRejectsIncompleteAndDelete(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RecordingsDir: t.TempDir()})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	session := login(t, s.URL, "alice", "correct horse")

	status, data := beginRecordingUpload(t, s.URL, session.Token, "partial.mp4", 10, 0, 0)
	if status != http.StatusCreated {
		t.Fatalf("begin: %d %s", status, data)
	}
	upload := decodeResumableUpload(t, data)
	if status, data := appendRecordingUpload(t, s.URL, session.Token, upload.ID, 0, []byte("1234")); status != http.StatusAccepted {
		t.Fatalf("partial chunk: %d %s", status, data)
	}
	if status, data := completeRecordingUpload(t, s.URL, session.Token, upload.ID); status != http.StatusBadRequest {
		t.Fatalf("incomplete complete: %d %s", status, data)
	}
	if status, data := request(t, http.MethodDelete,
		s.URL+"/v1/account/recordings/uploads/"+upload.ID, "", session.Token); status != http.StatusNoContent {
		t.Fatalf("delete upload: %d %s", status, data)
	}
	if status, data := recordingUploadStatus(t, s.URL, session.Token, upload.ID); status != http.StatusNotFound {
		t.Fatalf("deleted upload: %d %s", status, data)
	}
}

func TestRecordingResumableUploadLimits(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{
		RecordingsDir:   t.TempDir(),
		RecordingsQuota: 10,
		RecordingsMax:   8,
	})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	session := login(t, s.URL, "alice", "correct horse")

	if status, data := beginRecordingUpload(t, s.URL, session.Token, "empty.mp4", 0, 0, 0); status != http.StatusBadRequest {
		t.Fatalf("empty begin: %d %s", status, data)
	}
	if status, data := beginRecordingUpload(t, s.URL, session.Token, "large.mp4", 9, 0, 0); status != http.StatusRequestEntityTooLarge {
		t.Fatalf("large begin: %d %s", status, data)
	}
	status, data := beginRecordingUpload(t, s.URL, session.Token, "quota.mp4", 6, 0, 0)
	if status != http.StatusCreated {
		t.Fatalf("quota begin: %d %s", status, data)
	}
	upload := decodeResumableUpload(t, data)
	if status, data := beginRecordingUpload(t, s.URL, session.Token, "second.mp4", 6, 0, 0); status != http.StatusRequestEntityTooLarge {
		t.Fatalf("quota reservation: %d %s", status, data)
	}
	if status, data := appendRecordingUpload(t, s.URL, session.Token, upload.ID, 0, make([]byte, (32<<20)+1)); status != http.StatusBadRequest {
		t.Fatalf("oversized chunk: %d %s", status, data)
	}

	status, data = beginRecordingUpload(t, s.URL, session.Token, "incomplete.mp4", 10, 0, 0)
	if status != http.StatusRequestEntityTooLarge {
		t.Fatalf("quota must still count reservation: %d %s", status, data)
	}
	if status, data := request(t, http.MethodDelete,
		s.URL+"/v1/account/recordings/uploads/"+upload.ID, "", session.Token); status != http.StatusNoContent {
		t.Fatalf("delete upload: %d %s", status, data)
	}

	for i := 0; i < 8; i++ {
		status, data := beginRecordingUpload(t, s.URL, session.Token, "active.mp4", 1, 0, 0)
		if status != http.StatusCreated {
			t.Fatalf("active begin %d: %d %s", i, status, data)
		}
	}
	if status, data := beginRecordingUpload(t, s.URL, session.Token, "overflow.mp4", 1, 0, 0); status != http.StatusTooManyRequests {
		t.Fatalf("active upload limit: %d %s", status, data)
	}
}
