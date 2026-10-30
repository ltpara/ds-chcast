package api_test

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type recordingReply struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	PeerID      string `json:"peer_id"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	DurationMS  int64  `json:"duration_ms"`
	StartedAtMS int64  `json:"started_at_ms"`
	CreatedAtMS int64  `json:"created_at_ms"`
}

type recordingUploadReply struct {
	Recording  recordingReply `json:"recording"`
	UsageBytes int64          `json:"usage_bytes"`
	QuotaBytes int64          `json:"quota_bytes"`
}

type recordingsListReply struct {
	Recordings     []recordingReply `json:"recordings"`
	UsageBytes     int64            `json:"usage_bytes"`
	QuotaBytes     int64            `json:"quota_bytes"`
	MaxUploadBytes int64            `json:"max_upload_bytes"`
}

func uploadRecording(t *testing.T, base, token, query string, body []byte, chunked bool) (int, []byte) {
	t.Helper()
	var reader io.Reader
	if chunked {
		reader = io.NopCloser(strings.NewReader(string(body)))
	} else {
		reader = strings.NewReader(string(body))
	}
	req, err := http.NewRequest(http.MethodPost, base+"/v1/account/recordings"+query, reader)
	if err != nil {
		t.Fatal(err)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if chunked {
		req.ContentLength = -1
	}
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

func downloadRecording(t *testing.T, base, token, id string) (int, []byte, http.Header) {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, base+"/v1/account/recordings/"+id, nil)
	if err != nil {
		t.Fatal(err)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, data, resp.Header
}

func TestRecordingArchiveUploadListDownloadDelete(t *testing.T) {
	dir := t.TempDir()
	s := testutil.StartWith(t, testutil.Options{RecordingsDir: dir})
	for _, name := range []string{"alice", "bob"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", name, err)
		}
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	body := []byte("FAKE-MP4-DATA-0123456789-abcdefghij")
	sum := sha256.Sum256(body)
	name := "会话录制 2026-10-28.mp4"
	query := "?" + url.Values{
		"name":        {name},
		"peer_id":     {"devbox"},
		"started_ms":  {"1700000000000"},
		"duration_ms": {"91000"},
	}.Encode()

	status, data := uploadRecording(t, s.URL, alice.Token, query, body, false)
	if status != http.StatusCreated {
		t.Fatalf("upload: %d %s", status, data)
	}
	var uploaded recordingUploadReply
	if err := json.Unmarshal(data, &uploaded); err != nil {
		t.Fatalf("upload reply: %v", err)
	}
	rec := uploaded.Recording
	if len(rec.ID) != 32 || rec.Name != name || rec.PeerID != "devbox" ||
		rec.Size != int64(len(body)) || rec.SHA256 != hex.EncodeToString(sum[:]) ||
		rec.StartedAtMS != 1700000000000 || rec.DurationMS != 91000 {
		t.Fatalf("unexpected recording: %+v", rec)
	}
	if uploaded.UsageBytes != int64(len(body)) || uploaded.QuotaBytes <= 0 {
		t.Fatalf("usage after upload = %d/%d", uploaded.UsageBytes, uploaded.QuotaBytes)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/account/recordings", "", alice.Token)
	if status != http.StatusOK {
		t.Fatalf("list: %d %s", status, data)
	}
	var list recordingsListReply
	if err := json.Unmarshal(data, &list); err != nil {
		t.Fatalf("list reply: %v", err)
	}
	if len(list.Recordings) != 1 || list.Recordings[0].ID != rec.ID ||
		list.UsageBytes != int64(len(body)) || list.MaxUploadBytes <= 0 {
		t.Fatalf("unexpected list: %+v", list)
	}

	status, data, header := downloadRecording(t, s.URL, alice.Token, rec.ID)
	if status != http.StatusOK || string(data) != string(body) {
		t.Fatalf("download: %d %d bytes", status, len(data))
	}
	if ct := header.Get("Content-Type"); !strings.HasPrefix(ct, "video/mp4") {
		t.Fatalf("download content type = %q", ct)
	}
	if cd := header.Get("Content-Disposition"); !strings.Contains(cd, "filename*=UTF-8''") {
		t.Fatalf("download disposition = %q", cd)
	}

	if status, _, _ := downloadRecording(t, s.URL, bob.Token, rec.ID); status != http.StatusNotFound {
		t.Fatalf("cross-account download = %d", status)
	}
	if status, data := request(t, http.MethodDelete,
		s.URL+"/v1/account/recordings/"+rec.ID, "", bob.Token); status != http.StatusNotFound {
		t.Fatalf("cross-account delete: %d %s", status, data)
	}

	// A row whose file vanished must not serve an empty body.
	if err := os.Remove(filepath.Join(dir, ownerID(t, s, "alice"), rec.ID+".mp4")); err != nil {
		t.Fatalf("remove file: %v", err)
	}
	if status, _, _ := downloadRecording(t, s.URL, alice.Token, rec.ID); status != http.StatusNotFound {
		t.Fatalf("missing file download = %d", status)
	}

	if status, data := request(t, http.MethodDelete,
		s.URL+"/v1/account/recordings/"+rec.ID, "", alice.Token); status != http.StatusNoContent {
		t.Fatalf("delete: %d %s", status, data)
	}
	var after recordingsListReply
	status, data = request(t, http.MethodGet, s.URL+"/v1/account/recordings", "", alice.Token)
	if status != http.StatusOK {
		t.Fatalf("list after delete = %d", status)
	}
	if err := json.Unmarshal(data, &after); err != nil {
		t.Fatalf("list reply: %v", err)
	}
	if len(after.Recordings) != 0 || after.UsageBytes != 0 {
		t.Fatalf("archive after delete: %+v", after)
	}
	if status, data := request(t, http.MethodDelete,
		s.URL+"/v1/account/recordings/"+rec.ID, "", alice.Token); status != http.StatusNotFound {
		t.Fatalf("second delete: %d %s", status, data)
	}

	events, err := s.Accounts.AuditEvents(50)
	if err != nil {
		t.Fatalf("audit: %v", err)
	}
	var uploads, deletes int
	for _, event := range events {
		if !event.Success {
			continue
		}
		switch event.Action {
		case account.AuditRecordingUpload:
			uploads++
		case account.AuditRecordingDelete:
			deletes++
		}
	}
	if uploads != 1 || deletes != 1 {
		t.Fatalf("recording audit uploads=%d deletes=%d", uploads, deletes)
	}

	if status, _ := uploadRecording(t, s.URL, "", "", body, false); status != http.StatusUnauthorized {
		t.Fatalf("anonymous upload = %d", status)
	}
}

func TestRecordingArchiveLimits(t *testing.T) {
	dir := t.TempDir()
	s := testutil.StartWith(t, testutil.Options{
		RecordingsDir:   dir,
		RecordingsMax:   8,
		RecordingsQuota: 10,
	})
	user, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	session := login(t, s.URL, "alice", "correct horse")

	if status, data := uploadRecording(t, s.URL, session.Token, "", nil, false); status != http.StatusBadRequest {
		t.Fatalf("empty upload: %d %s", status, data)
	}
	if status, data := uploadRecording(t, s.URL, session.Token, "", []byte("123456789"), false); status != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversized upload: %d %s", status, data)
	}
	// A chunked body has no declared length, so the archive must bound the
	// copy itself instead of trusting Content-Length.
	if status, data := uploadRecording(t, s.URL, session.Token, "", []byte("123456789"), true); status != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversized chunked upload: %d %s", status, data)
	}
	if status, data := uploadRecording(t, s.URL, session.Token, "", []byte("123456"), false); status != http.StatusCreated {
		t.Fatalf("first upload: %d %s", status, data)
	}
	if status, data := uploadRecording(t, s.URL, session.Token, "", []byte("123456"), false); status != http.StatusRequestEntityTooLarge {
		t.Fatalf("quota upload: %d %s", status, data)
	}
	entries, err := os.ReadDir(filepath.Join(dir, user.ID))
	if err != nil {
		t.Fatalf("read archive dir: %v", err)
	}
	var files []string
	for _, entry := range entries {
		files = append(files, entry.Name())
	}
	if len(files) != 1 || !strings.HasSuffix(files[0], ".mp4") {
		t.Fatalf("archive files after refused uploads = %v", files)
	}
}

func TestRecordingArchiveDisabled(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	session := login(t, s.URL, "alice", "correct horse")
	if status, data := uploadRecording(t, s.URL, session.Token, "", []byte("data"), false); status != http.StatusServiceUnavailable {
		t.Fatalf("disabled upload: %d %s", status, data)
	}
	if status, data := request(t, http.MethodGet, s.URL+"/v1/account/recordings", "", session.Token); status != http.StatusServiceUnavailable {
		t.Fatalf("disabled list: %d %s", status, data)
	}
}

func ownerID(t *testing.T, s *testutil.Server, username string) string {
	t.Helper()
	user, err := s.Accounts.FindUser(username)
	if err != nil {
		t.Fatalf("find %s: %v", username, err)
	}
	return user.ID
}
