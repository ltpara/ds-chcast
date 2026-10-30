package api_test

import (
	"encoding/json"
	"io/fs"
	"net/http"
	"net/url"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type consoleRecordingReply struct {
	ID          string `json:"id"`
	Username    string `json:"username"`
	Name        string `json:"name"`
	Size        int64  `json:"size"`
	CreatedAtMS int64  `json:"created_at_ms"`
}

type consoleRecordingsReply struct {
	Recordings     []consoleRecordingReply `json:"recordings"`
	UsageBytes     int64                   `json:"usage_bytes"`
	Total          int                     `json:"total"`
	NextBeforeID   int64                   `json:"next_before_id"`
	MaxUploadBytes int64                   `json:"max_upload_bytes"`
}

func consoleUpload(t *testing.T, base, token, name string, body []byte) consoleRecordingReply {
	t.Helper()
	query := "?" + url.Values{"name": {name}, "peer_id": {"devbox"}}.Encode()
	status, data := uploadRecording(t, base, token, query, body, false)
	if status != http.StatusCreated {
		t.Fatalf("upload %s: %d %s", name, status, data)
	}
	var reply struct {
		Recording consoleRecordingReply `json:"recording"`
	}
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("upload reply: %v", err)
	}
	return reply.Recording
}

// TestConsoleRecordingsListFilterAndRoles pins the operator surface: readonly
// and up may list and download, only operator and up may delete, viewer
// accounts and unknown tokens are rejected, and the username filter narrows
// the global list.
func TestConsoleRecordingsListFilterAndRoles(t *testing.T) {
	dir := t.TempDir()
	s := testutil.StartWith(t, testutil.Options{RecordingsDir: dir})
	for _, user := range []struct {
		name string
		role string
	}{
		{"alice", account.RoleAdmin},
		{"op", account.RoleOperator},
		{"ro", account.RoleReadonly},
		{"bob", account.RoleUser},
	} {
		if _, err := s.Accounts.CreateUser(user.name, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", user.name, err)
		}
		if user.role != account.RoleUser {
			if _, err := s.Accounts.SetRole(user.name, user.role); err != nil {
				t.Fatalf("role %s: %v", user.name, err)
			}
		}
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	operator := login(t, s.URL, "op", "correct horse").Token
	readonly := login(t, s.URL, "ro", "correct horse").Token
	viewer := login(t, s.URL, "bob", "correct horse").Token

	aliceBody := []byte("ALICE-RECORDING-BYTES")
	bobBody := []byte("BOB-RECORDING-BYTES-0123456789")
	aliceRec := consoleUpload(t, s.URL, alice, "alice 会话.mp4", aliceBody)
	bobRec := consoleUpload(t, s.URL, viewer, "bob 会话.mp4", bobBody)

	status, body := request(t, http.MethodGet, s.URL+"/v1/recordings", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("server-key list: %d %s", status, body)
	}
	var all consoleRecordingsReply
	if err := json.Unmarshal(body, &all); err != nil {
		t.Fatalf("list reply: %v", err)
	}
	if all.Total != 2 || len(all.Recordings) != 2 {
		t.Fatalf("list total = %d rows = %d", all.Total, len(all.Recordings))
	}
	if all.UsageBytes != int64(len(aliceBody)+len(bobBody)) || all.MaxUploadBytes <= 0 {
		t.Fatalf("list usage = %d max = %d", all.UsageBytes, all.MaxUploadBytes)
	}
	owners := map[string]string{}
	for _, rec := range all.Recordings {
		owners[rec.ID] = rec.Username
	}
	if owners[aliceRec.ID] != "alice" || owners[bobRec.ID] != "bob" {
		t.Fatalf("owner attribution = %v", owners)
	}
	if all.Recordings[0].ID != bobRec.ID || all.Recordings[1].ID != aliceRec.ID {
		t.Fatalf("list is not newest first: %+v", all.Recordings)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/recordings?username=bob", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("filtered list: %d %s", status, body)
	}
	var filtered consoleRecordingsReply
	if err := json.Unmarshal(body, &filtered); err != nil {
		t.Fatalf("filtered reply: %v", err)
	}
	if filtered.Total != 1 || len(filtered.Recordings) != 1 || filtered.Recordings[0].ID != bobRec.ID {
		t.Fatalf("filtered list = %+v", filtered)
	}

	for _, tc := range []struct {
		name  string
		token string
		want  int
	}{
		{"readonly", readonly, http.StatusOK},
		{"operator", operator, http.StatusOK},
		{"viewer", viewer, http.StatusForbidden},
		{"junk", "not-a-token", http.StatusUnauthorized},
	} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings", "", tc.token); status != tc.want {
			t.Fatalf("list as %s: %d %s", tc.name, status, body)
		}
	}

	if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings/"+aliceRec.ID, "", readonly); status != http.StatusOK ||
		string(body) != string(aliceBody) {
		t.Fatalf("readonly download: %d %q", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings/"+aliceRec.ID, "", testutil.ServerKey); status != http.StatusOK ||
		string(body) != string(aliceBody) {
		t.Fatalf("server-key download: %d %q", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings/"+aliceRec.ID, "", viewer); status != http.StatusForbidden {
		t.Fatalf("viewer download: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings/"+strings.Repeat("0", 32), "", readonly); status != http.StatusNotFound {
		t.Fatalf("unknown download: %d %s", status, body)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/recordings/"+aliceRec.ID, "", readonly); status != http.StatusForbidden {
		t.Fatalf("readonly delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/recordings/"+aliceRec.ID, "", operator); status != http.StatusNoContent {
		t.Fatalf("operator delete: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/recordings?username=alice", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("alice list after delete: %d %s", status, body)
	}
	var aliceLeft consoleRecordingsReply
	if err := json.Unmarshal(body, &aliceLeft); err != nil {
		t.Fatalf("alice reply: %v", err)
	}
	if aliceLeft.Total != 0 || len(aliceLeft.Recordings) != 0 {
		t.Fatalf("alice list after delete = %+v", aliceLeft)
	}
	files := 0
	err := filepath.WalkDir(dir, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !entry.IsDir() {
			files++
		}
		return nil
	})
	if err != nil {
		t.Fatalf("walk archive: %v", err)
	}
	if files != 1 {
		t.Fatalf("archive holds %d files after one delete, want 1", files)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/audit?action="+account.AuditRecordingDelete, "", alice)
	if status != http.StatusOK {
		t.Fatalf("delete audit: %d %s", status, body)
	}
	if !strings.Contains(string(body), `"username":"alice"`) || !strings.Contains(string(body), "by=op") {
		t.Fatalf("delete audit does not name owner and operator: %s", body)
	}
}

// TestConsoleRecordingsCursor walks the operator page with limit=1 and checks
// the cursor is the last row and that the walk ends with a short page.
func TestConsoleRecordingsCursor(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RecordingsDir: t.TempDir()})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	first := consoleUpload(t, s.URL, alice, "first.mp4", []byte("FIRST"))
	second := consoleUpload(t, s.URL, alice, "second.mp4", []byte("SECOND"))

	seen := []string{}
	before := int64(0)
	for page := 0; page < 4; page++ {
		target := s.URL + "/v1/recordings?limit=1"
		if before > 0 {
			target += "&before_id=" + strconv.FormatInt(before, 10)
		}
		status, body := request(t, http.MethodGet, target, "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("page %d: %d %s", page, status, body)
		}
		var reply consoleRecordingsReply
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatalf("page %d reply: %v", page, err)
		}
		for _, rec := range reply.Recordings {
			seen = append(seen, rec.ID)
		}
		if reply.NextBeforeID == 0 {
			break
		}
		before = reply.NextBeforeID
	}
	if len(seen) != 2 || seen[0] != second.ID || seen[1] != first.ID {
		t.Fatalf("cursor walk = %v", seen)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings?before_id=abc", "", testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("bad cursor: %d %s", status, body)
	}
}

// TestConsoleRecordingsDisabled keeps the archive-off answer at 503 so the
// console can hide the tab instead of showing an empty archive.
func TestConsoleRecordingsDisabled(t *testing.T) {
	s := testutil.Start(t)
	if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings", "", testutil.ServerKey); status != http.StatusServiceUnavailable {
		t.Fatalf("disabled list: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/recordings/"+strings.Repeat("0", 32), "", testutil.ServerKey); status != http.StatusServiceUnavailable {
		t.Fatalf("disabled download: %d %s", status, body)
	}
}
