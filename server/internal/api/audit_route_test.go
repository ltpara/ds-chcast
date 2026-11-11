package api_test

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/geoip"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func geoipServer(t *testing.T) *testutil.Server {
	t.Helper()
	table := filepath.Join(t.TempDir(), "geoip.txt")
	if err := os.WriteFile(table, []byte("127.0.0.0/8\tLocal lab\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	locator, err := geoip.Load(table)
	if err != nil {
		t.Fatalf("load geoip: %v", err)
	}
	return testutil.StartWith(t, testutil.Options{GeoIP: locator})
}

func TestAuditLocationRouteAndBatch(t *testing.T) {
	s := geoipServer(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse")
	now := time.Now().UnixMilli()
	start := now - 60000

	status, body := request(t, http.MethodPost, s.URL+"/v1/account/connections",
		`{"peer_id":"devbox","started_at_ms":`+strconv.FormatInt(start, 10)+
			`,"ended_at_ms":`+strconv.FormatInt(now, 10)+
			`,"audit_ref":"0011aabb","route":"punch"}`,
		alice.Token)
	if status != http.StatusCreated {
		t.Fatalf("report connection: %d %s", status, body)
	}
	var created connectionReply
	if err := json.Unmarshal(body, &created); err != nil {
		t.Fatalf("connection reply: %v", err)
	}
	if created.ClientIP != "127.0.0.1" || created.Location != "Local lab" ||
		created.Route != "punch" || created.AuditRef != "0011aabb" {
		t.Fatalf("created connection = %+v", created)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/account/connections",
		`{"peer_id":"laptop","route":"bogus"}`, alice.Token)
	if status != http.StatusCreated {
		t.Fatalf("report unknown route: %d %s", status, body)
	}
	var unknownRoute connectionReply
	if err := json.Unmarshal(body, &unknownRoute); err != nil {
		t.Fatalf("connection reply: %v", err)
	}
	if unknownRoute.Route != "" || unknownRoute.Location != "Local lab" {
		t.Fatalf("unknown route was kept: %+v", unknownRoute)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/account/file-transfers/batch",
		`{"peer_id":"devbox","audit_ref":"0011aabb","route":"punch","items":[`+
			`{"direction":"upload","name":"report.pdf","size":4096,`+
			`"occurred_at_ms":`+strconv.FormatInt(now, 10)+`},`+
			`{"direction":"download","name":"dump.bin","size":9,"success":false,`+
			`"detail":"disk full","occurred_at_ms":`+strconv.FormatInt(now, 10)+`}]}`,
		alice.Token)
	if status != http.StatusCreated {
		t.Fatalf("report batch: %d %s", status, body)
	}
	var batch fileTransfersReply
	if err := json.Unmarshal(body, &batch); err != nil {
		t.Fatalf("batch reply: %v", err)
	}
	if len(batch.Transfers) != 2 {
		t.Fatalf("batch transfers = %+v", batch.Transfers)
	}
	upload, download := batch.Transfers[0], batch.Transfers[1]
	if upload.Name != "report.pdf" || upload.Size != 4096 || !upload.Success ||
		upload.AuditRef != "0011aabb" || upload.Route != "punch" ||
		upload.Location != "Local lab" {
		t.Fatalf("batch upload = %+v", upload)
	}
	if download.Name != "dump.bin" || download.Success || download.Detail != "disk full" ||
		download.AuditRef != "0011aabb" || download.Route != "punch" {
		t.Fatalf("batch download = %+v", download)
	}

	status, body = request(t, http.MethodGet,
		s.URL+"/v1/account/file-transfers?limit=10", "", alice.Token)
	if status != http.StatusOK {
		t.Fatalf("list own transfers: %d %s", status, body)
	}
	var listed fileTransfersReply
	if err := json.Unmarshal(body, &listed); err != nil {
		t.Fatalf("list reply: %v", err)
	}
	if len(listed.Transfers) != 2 || listed.Transfers[0].Name != "dump.bin" ||
		listed.Transfers[1].Name != "report.pdf" {
		t.Fatalf("listed transfers = %+v", listed.Transfers)
	}
	for _, event := range listed.Transfers {
		if event.AuditRef != "0011aabb" || event.Route != "punch" ||
			event.Location != "Local lab" || event.ClientIP != "127.0.0.1" {
			t.Fatalf("listed transfer lacks session context: %+v", event)
		}
	}

	for _, tc := range []struct {
		name string
		body string
	}{
		{"empty items", `{"peer_id":"devbox","items":[]}`},
		{"missing items", `{"peer_id":"devbox"}`},
		{"bad direction", `{"peer_id":"devbox","items":[{"direction":"viewer","name":"a"}]}`},
		{"bad ref", `{"peer_id":"devbox","audit_ref":"not-hex!","items":[{"direction":"upload","name":"a"}]}`},
	} {
		if status, body := request(t, http.MethodPost,
			s.URL+"/v1/account/file-transfers/batch", tc.body, alice.Token); status != http.StatusBadRequest {
			t.Fatalf("%s: %d %s", tc.name, status, body)
		}
	}
	tooMany := `{"peer_id":"devbox","items":[`
	for i := 0; i < 201; i++ {
		if i > 0 {
			tooMany += ","
		}
		tooMany += `{"direction":"upload","name":"f"}`
	}
	tooMany += `]}`
	if status, body := request(t, http.MethodPost,
		s.URL+"/v1/account/file-transfers/batch", tooMany, alice.Token); status != http.StatusBadRequest {
		t.Fatalf("oversized batch: %d %s", status, body)
	}
}

func TestAuditUserFilter(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	login(t, s.URL, "alice", "correct horse")
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"bob","password":"wrong horse"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("failed bob login: %d %s", status, body)
	}
	login(t, s.URL, "bob", "correct horse")

	type eventReply struct {
		Username string `json:"username"`
		Action   string `json:"action"`
		Success  bool   `json:"success"`
	}
	type auditReply struct {
		Events []eventReply `json:"events"`
	}
	list := func(query string) auditReply {
		t.Helper()
		status, body := request(t, http.MethodGet, s.URL+"/v1/audit"+query, "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("audit%s: %d %s", query, status, body)
		}
		var reply auditReply
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatalf("audit%s reply: %v", query, err)
		}
		return reply
	}

	alice := list("?user=alice")
	if len(alice.Events) != 1 || alice.Events[0].Username != "alice" ||
		alice.Events[0].Action != "login" || !alice.Events[0].Success {
		t.Fatalf("alice events = %+v", alice.Events)
	}
	// A failed attempt and a successful login both hang off bob's trail.
	bob := list("?user=bob")
	if len(bob.Events) != 2 {
		t.Fatalf("bob events = %+v", bob.Events)
	}
	for _, event := range bob.Events {
		if event.Username != "bob" || event.Action != "login" {
			t.Fatalf("bob event leaked another account: %+v", event)
		}
	}
	if !bob.Events[0].Success || bob.Events[1].Success {
		t.Fatalf("bob events out of order: %+v", bob.Events)
	}
	// Whitespace from a hand-built query is trimmed like the other filters.
	if padded := list("?user=+bob+"); len(padded.Events) != 2 {
		t.Fatalf("padded bob events = %+v", padded.Events)
	}
	if combo := list("?user=bob&action=login&limit=1"); len(combo.Events) != 1 {
		t.Fatalf("bob login page = %+v", combo.Events)
	}
	if unknown := list("?user=carol"); len(unknown.Events) != 0 {
		t.Fatalf("unknown user returned %+v", unknown.Events)
	}

	rows := exportCSV(t, s.URL+"/v1/audit?format=csv&user=bob")
	if len(rows) != 3 {
		t.Fatalf("csv rows = %v", rows)
	}
	for _, row := range rows[1:] {
		if row[2] != "bob" {
			t.Fatalf("csv row leaked another account: %v", row)
		}
	}
	rows = exportCSV(t, s.URL+"/v1/audit?format=csv&user=bob&action=login")
	if len(rows) != 3 || rows[1][3] != "login" || rows[2][3] != "login" {
		t.Fatalf("csv action rows = %v", rows)
	}
}

func TestAuditWithoutGeoIPRecordsIPOnly(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse")
	status, body := request(t, http.MethodPost, s.URL+"/v1/account/connections",
		`{"peer_id":"devbox","route":"relay","audit_ref":"00aabbcc"}`, alice.Token)
	if status != http.StatusCreated {
		t.Fatalf("report connection: %d %s", status, body)
	}
	var created connectionReply
	if err := json.Unmarshal(body, &created); err != nil {
		t.Fatalf("connection reply: %v", err)
	}
	if created.ClientIP != "127.0.0.1" || created.Location != "" || created.Route != "relay" {
		t.Fatalf("created connection without geoip = %+v", created)
	}
}
