package api_test

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func hostAuditSessionBody(t *testing.T, secret, hostID, ref string, startedAt, endedAt int64, success bool, detail string, timestamp int64) string {
	t.Helper()
	priv, _ := hostKey(secret)
	body, err := json.Marshal(map[string]any{
		"kind":          "session",
		"audit_ref":     ref,
		"started_at_ms": startedAt,
		"ended_at_ms":   endedAt,
		"success":       success,
		"detail":        detail,
		"timestamp":     timestamp,
		"signature": identity.Sign(priv, identity.HostAuditSessionMessage(
			hostID, ref, startedAt, endedAt, success, detail)),
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(body)
}

func hostAuditFileBody(t *testing.T, secret, hostID, ref, direction, name string, size, occurredAt int64, success bool, detail string, timestamp int64) string {
	t.Helper()
	priv, _ := hostKey(secret)
	body, err := json.Marshal(map[string]any{
		"kind":           "file",
		"audit_ref":      ref,
		"direction":      direction,
		"name":           name,
		"size":           size,
		"occurred_at_ms": occurredAt,
		"success":        success,
		"detail":         detail,
		"timestamp":      timestamp,
		"signature": identity.Sign(priv, identity.HostAuditFileMessage(
			hostID, ref, direction, name, size, occurredAt, success, detail)),
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(body)
}

func viewerConnectionBody(t *testing.T, peerID, ref string, startedAt, endedAt int64, success bool, detail string) string {
	t.Helper()
	body, err := json.Marshal(map[string]any{
		"peer_id":       peerID,
		"started_at_ms": startedAt,
		"ended_at_ms":   endedAt,
		"success":       success,
		"detail":        detail,
		"audit_ref":     ref,
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(body)
}

func listConnections(t *testing.T, url, token string) connectionsReply {
	t.Helper()
	status, body := request(t, http.MethodGet, url+"/v1/account/connections?limit=20", "", token)
	if status != http.StatusOK {
		t.Fatalf("list connections: %d %s", status, body)
	}
	var reply connectionsReply
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("connections reply: %v", err)
	}
	return reply
}

func TestHostAuditMergesViewerSessions(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "host secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	token := login(t, s.URL, "alice", "correct horse").Token
	started := time.Now().Add(-time.Minute).UnixMilli()
	ended := time.Now().UnixMilli()

	// Viewer report first; the host report with the same reference completes it.
	ref := "00112233aabbccdd"
	status, body := request(t, http.MethodPost, s.URL+"/v1/account/connections",
		viewerConnectionBody(t, "devbox", ref, started, ended, true, "viewer closed"), token)
	if status != http.StatusCreated {
		t.Fatalf("viewer report: %d %s", status, body)
	}
	var viewerRow connectionReply
	if err := json.Unmarshal(body, &viewerRow); err != nil {
		t.Fatalf("viewer reply: %v", err)
	}
	if viewerRow.Source != "viewer" || viewerRow.AuditRef != ref || viewerRow.HostID != "" {
		t.Fatalf("unexpected viewer row: %+v", viewerRow)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		hostAuditSessionBody(t, "host secret", "devbox", ref,
			started-2000, ended+1000, true, "host closed", time.Now().Unix()), testutil.ServerKey)
	if status != http.StatusCreated {
		t.Fatalf("host report: %d %s", status, body)
	}
	var merged connectionReply
	if err := json.Unmarshal(body, &merged); err != nil {
		t.Fatalf("host reply: %v", err)
	}
	if merged.ID != viewerRow.ID || merged.Source != "both" || merged.Username != "alice" ||
		merged.HostID != "devbox" || merged.PeerID != "devbox" ||
		merged.StartedAtMS != started-2000 || merged.EndedAtMS != ended+1000 ||
		merged.Detail != "viewer closed | host closed" {
		t.Fatalf("merged row = %+v", merged)
	}
	if events := listConnections(t, s.URL, token); len(events.Connections) != 1 {
		t.Fatalf("expected one merged row, got %+v", events.Connections)
	}

	// Host report first; the viewer report completes the account identity.
	ref2 := "a1b2c3d4e5f60718"
	status, body = request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		hostAuditSessionBody(t, "host secret", "devbox", ref2,
			started, ended, false, "approval denied", time.Now().Unix()), testutil.ServerKey)
	if status != http.StatusCreated {
		t.Fatalf("host-first report: %d %s", status, body)
	}
	var hostRow connectionReply
	if err := json.Unmarshal(body, &hostRow); err != nil {
		t.Fatalf("host-first reply: %v", err)
	}
	if hostRow.Source != "host" || hostRow.Username != "-" || hostRow.Success {
		t.Fatalf("host-only row = %+v", hostRow)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/account/connections",
		viewerConnectionBody(t, "devbox", ref2, started, ended, true, ""), token)
	if status != http.StatusCreated {
		t.Fatalf("viewer report after host: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &merged); err != nil {
		t.Fatalf("merged reply: %v", err)
	}
	if merged.ID != hostRow.ID || merged.Source != "both" || merged.Username != "alice" ||
		merged.Success || merged.Detail != "approval denied" {
		t.Fatalf("merge over host row = %+v", merged)
	}

	// An inbound session with no viewer report stays visible to the operator.
	ref3 := "ffeeddccbbaa0099"
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		hostAuditSessionBody(t, "host secret", "devbox", ref3,
			started, ended+500, true, "", time.Now().Unix()), testutil.ServerKey); status != http.StatusCreated {
		t.Fatalf("host-only report: %d %s", status, body)
	}
	events := listConnections(t, s.URL, token)
	if len(events.Connections) != 2 {
		t.Fatalf("account list unexpectedly includes host-only rows: %+v", events.Connections)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/connections?host=devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("audit by host: %d %s", status, body)
	}
	var hostEvents connectionsReply
	if err := json.Unmarshal(body, &hostEvents); err != nil {
		t.Fatalf("audit reply: %v", err)
	}
	if len(hostEvents.Connections) != 3 || hostEvents.Connections[0].AuditRef != ref3 ||
		hostEvents.Connections[0].Source != "host" {
		t.Fatalf("host audit rows = %+v", hostEvents.Connections)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/audit/connections?username=alice", "", testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("audit by username: %d %s", status, body)
	}

	for _, tc := range []struct {
		name   string
		body   string
		status int
	}{
		{
			"bad signature",
			hostAuditSessionBody(t, "other secret", "devbox", "00112233aabbccdd",
				started, ended, true, "", time.Now().Unix()),
			http.StatusUnauthorized,
		},
		{
			"stale timestamp",
			hostAuditSessionBody(t, "host secret", "devbox", "00112233aabbccdd",
				started, ended, true, "", time.Now().Add(-time.Hour).Unix()),
			http.StatusUnauthorized,
		},
		{
			"unknown kind",
			fmt.Sprintf(`{"kind":"network","timestamp":%d,"signature":"AAAA"}`, time.Now().Unix()),
			http.StatusBadRequest,
		},
		{
			"bad audit ref",
			hostAuditSessionBody(t, "host secret", "devbox", "not-hex!",
				started, ended, true, "", time.Now().Unix()),
			http.StatusBadRequest,
		},
	} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit", tc.body, testutil.ServerKey); status != tc.status {
			t.Fatalf("%s: %d %s, want %d", tc.name, status, body, tc.status)
		}
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/ghost/audit",
		hostAuditSessionBody(t, "host secret", "ghost", "00112233aabbccdd",
			started, ended, true, "", time.Now().Unix()), testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("unknown host: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		hostAuditSessionBody(t, "host secret", "devbox", "00112233aabbccdd",
			started, ended, true, "", time.Now().Unix()), ""); status != http.StatusUnauthorized {
		t.Fatalf("missing server key: %d %s", status, body)
	}
}

func TestHostAuditFileTransfers(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "host secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	token := login(t, s.URL, "alice", "correct horse").Token
	ref := "00112233aabbccdd"
	occurred := time.Now().UnixMilli()

	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		hostAuditFileBody(t, "host secret", "devbox", ref, "upload", "report.pdf",
			4096, occurred, true, "", time.Now().Unix()), testutil.ServerKey)
	if status != http.StatusCreated {
		t.Fatalf("host file report: %d %s", status, body)
	}
	var hostRow fileTransferReply
	if err := json.Unmarshal(body, &hostRow); err != nil {
		t.Fatalf("host file reply: %v", err)
	}
	if hostRow.Username != "-" || hostRow.HostID != "devbox" || hostRow.Source != "host" ||
		hostRow.PeerID != "devbox" || hostRow.Files != 1 || hostRow.Name != "report.pdf" ||
		hostRow.Size != 4096 || !hostRow.Success || hostRow.AuditRef != ref {
		t.Fatalf("host file row = %+v", hostRow)
	}

	// Viewer windows aggregate files, host rows are per file, so they stay
	// separate even when they share the session reference.
	viewerBody := fmt.Sprintf(
		`{"peer_id":"devbox","direction":"upload","name":"report.pdf","size":4096,"files":2,"audit_ref":%q}`, ref)
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/file-transfers", viewerBody, token); status != http.StatusCreated {
		t.Fatalf("viewer file report: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers?host=devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("file audit by host: %d %s", status, body)
	}
	var hostFiles fileTransfersReply
	if err := json.Unmarshal(body, &hostFiles); err != nil {
		t.Fatalf("file audit reply: %v", err)
	}
	if len(hostFiles.Transfers) != 2 {
		t.Fatalf("host file rows = %+v", hostFiles.Transfers)
	}
	sources := map[string]bool{}
	for _, row := range hostFiles.Transfers {
		sources[row.Source] = true
	}
	if !sources["host"] || !sources["viewer"] {
		t.Fatalf("missing sources: %+v", hostFiles.Transfers)
	}

	for _, tc := range []struct {
		name   string
		body   string
		status int
	}{
		{
			"bad direction",
			hostAuditFileBody(t, "host secret", "devbox", ref, "copy", "a", 1,
				occurred, true, "", time.Now().Unix()),
			http.StatusBadRequest,
		},
		{
			"bad signature",
			hostAuditFileBody(t, "other secret", "devbox", ref, "upload", "a", 1,
				occurred, true, "", time.Now().Unix()),
			http.StatusUnauthorized,
		},
		{
			"negative size",
			hostAuditFileBody(t, "host secret", "devbox", ref, "upload", "a", -1,
				occurred, true, "", time.Now().Unix()),
			http.StatusBadRequest,
		},
	} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit", tc.body, testutil.ServerKey); status != tc.status {
			t.Fatalf("%s: %d %s, want %d", tc.name, status, body, tc.status)
		}
	}
}

func TestHostAuditNeedsAccounts(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{NoAccounts: true})
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "host secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		hostAuditSessionBody(t, "host secret", "devbox", "00112233aabbccdd",
			now*1000-60000, now*1000, true, "", now), testutil.ServerKey)
	if status != http.StatusServiceUnavailable {
		t.Fatalf("audit without accounts: %d %s", status, body)
	}
}
