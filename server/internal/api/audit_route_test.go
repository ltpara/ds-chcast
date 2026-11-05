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
