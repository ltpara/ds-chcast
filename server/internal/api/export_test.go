package api_test

import (
	"encoding/csv"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func exportGet(t *testing.T, url, key string) (int, http.Header, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	if key != "" {
		req.Header.Set("Authorization", "Bearer "+key)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, resp.Header, body
}

func decodeExport(t *testing.T, header http.Header, body []byte, prefix string) [][]string {
	t.Helper()
	if got := header.Get("Content-Type"); got != "text/csv; charset=utf-8" {
		t.Fatalf("export content type = %q\n%s", got, body)
	}
	disposition := header.Get("Content-Disposition")
	if !strings.HasPrefix(disposition, `attachment; filename="`+prefix+"-") ||
		!strings.HasSuffix(disposition, `.csv"`) {
		t.Fatalf("export disposition = %q", disposition)
	}
	rows, err := csv.NewReader(strings.NewReader(string(body))).ReadAll()
	if err != nil {
		t.Fatalf("parse export csv: %v\n%s", err, body)
	}
	return rows
}

func exportRow(t *testing.T, rows [][]string, column int, value string) []string {
	t.Helper()
	for _, row := range rows[1:] {
		if column < len(row) && row[column] == value {
			return row
		}
	}
	t.Fatalf("no csv row with column %d = %q in %v", column, value, rows)
	return nil
}

func exportCSV(t *testing.T, url string) [][]string {
	t.Helper()
	status, header, body := exportGet(t, url, testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("export %s: %d %s", url, status, body)
	}
	prefix := "chast-audit"
	if strings.Contains(url, "/connections") {
		prefix = "chast-connections"
	} else if strings.Contains(url, "/file-transfers") {
		prefix = "chast-file-transfers"
	}
	return decodeExport(t, header, body, prefix)
}

func TestCSVExportPagesPastStorePageCaps(t *testing.T) {
	s := testutil.Start(t)
	user, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	start := time.Date(2026, 10, 28, 0, 0, 0, 0, time.UTC)

	const auditRows = 510
	for i := 0; i < auditRows; i++ {
		if err := s.Accounts.AppendAudit(account.AuditEvent{
			Username: "alice", Action: "export_bulk", Detail: fmt.Sprintf("a%d", i),
			CreatedAt: start.Add(time.Duration(i) * time.Second), Success: true,
		}); err != nil {
			t.Fatalf("append audit %d: %v", i, err)
		}
	}
	rows := exportCSV(t, s.URL+"/v1/audit?format=csv&action=export_bulk")
	if len(rows) != auditRows+1 {
		t.Fatalf("audit export rows = %d, want %d", len(rows)-1, auditRows)
	}
	if rows[1][4] != "a509" || rows[len(rows)-1][4] != "a0" {
		t.Fatalf("audit export order = %q .. %q", rows[1][4], rows[len(rows)-1][4])
	}

	// The connection and file listings cap a page at 200 rows, so these
	// downloads must not stop after the first page.
	const connectionRows = 450
	for i := 0; i < connectionRows; i++ {
		at := start.Add(time.Duration(i) * time.Minute)
		if _, err := s.Accounts.RecordConnection(user.ID, "alice", "peer1", "viewer", "",
			"", at, at.Add(time.Second), true, fmt.Sprintf("c%d", i)); err != nil {
			t.Fatalf("record connection %d: %v", i, err)
		}
	}
	rows = exportCSV(t, s.URL+"/v1/audit/connections?format=csv")
	if len(rows) != connectionRows+1 {
		t.Fatalf("connection export rows = %d, want %d", len(rows)-1, connectionRows)
	}
	if rows[1][12] != "c449" || rows[len(rows)-1][12] != "c0" {
		t.Fatalf("connection export order = %q .. %q", rows[1][12], rows[len(rows)-1][12])
	}

	const transferRows = 250
	for i := 0; i < transferRows; i++ {
		if _, err := s.Accounts.RecordFileTransfer(user.ID, "alice", "peer1", "upload",
			"dump.bin", "", "", 1, 1, start, true, fmt.Sprintf("f%d", i)); err != nil {
			t.Fatalf("record transfer %d: %v", i, err)
		}
	}
	rows = exportCSV(t, s.URL+"/v1/audit/file-transfers?format=csv")
	if len(rows) != transferRows+1 {
		t.Fatalf("file export rows = %d, want %d", len(rows)-1, transferRows)
	}
	if rows[1][13] != "f249" || rows[len(rows)-1][13] != "f0" {
		t.Fatalf("file export order = %q .. %q", rows[1][13], rows[len(rows)-1][13])
	}
}

func TestCSVExportFlushesThroughInstrumentation(t *testing.T) {
	s := testutil.Start(t)
	req, err := http.NewRequest(http.MethodGet, s.URL+"/v1/audit?format=csv", nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+testutil.ServerKey)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("export status = %d", resp.StatusCode)
	}
	// A flush before the handler returns makes the server stream the body
	// instead of buffering it to compute Content-Length.
	if got := resp.Header.Get("Content-Length"); got != "" {
		t.Fatalf("streaming export kept content length %q", got)
	}
	if _, err := io.Copy(io.Discard, resp.Body); err != nil {
		t.Fatalf("read export body: %v", err)
	}
}

func TestOperatorHistoryCSVExport(t *testing.T) {
	s := testutil.Start(t)
	user, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	created := time.Date(2026, 10, 27, 9, 3, 4, 123000000, time.UTC)
	tricky := "said \"hello\", then\nleft"
	for _, event := range []account.AuditEvent{
		{
			Username: "alice", Action: "export_probe", Detail: tricky,
			ClientIP: "203.0.113.5", Location: "Testland", CreatedAt: created,
		},
		{
			Username: "bob", Action: "export_probe", Detail: "plain",
			ClientIP: "203.0.113.6", CreatedAt: created.Add(time.Second), Success: true,
		},
	} {
		if err := s.Accounts.AppendAudit(event); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	ended := created.Add(90 * time.Second)
	if _, err := s.Accounts.RecordHostConnection(
		"devbox7", "0011aabb", created, ended, true, "host side, \"quoted\""); err != nil {
		t.Fatalf("record host connection: %v", err)
	}
	if _, err := s.Accounts.RecordConnection(
		user.ID, "alice", "peer1", "viewer", "198.51.100.7", "",
		created, ended, true, "viewer side"); err != nil {
		t.Fatalf("record connection: %v", err)
	}
	if _, err := s.Accounts.RecordHostFileTransfer(
		"devbox7", "2233ccdd", "upload", "report, final.csv", 42, created,
		true, "host file"); err != nil {
		t.Fatalf("record host file: %v", err)
	}
	if _, err := s.Accounts.RecordFileTransfer(
		user.ID, "alice", "peer1", "download", "notes.txt", "198.51.100.7", "",
		7, 1, created, true, "viewer file"); err != nil {
		t.Fatalf("record file transfer: %v", err)
	}

	rows := exportCSV(t, s.URL+"/v1/audit?format=csv&action=export_probe")
	if want := []string{"id", "created_at", "username", "action", "detail",
		"client_ip", "location", "success"}; !slices.Equal(rows[0], want) {
		t.Fatalf("audit header = %v, want %v", rows[0], want)
	}
	if len(rows) != 3 {
		t.Fatalf("audit rows = %d, want 3\n%v", len(rows), rows)
	}
	quoted := exportRow(t, rows, 4, tricky)
	if quoted[2] != "alice" || quoted[3] != "export_probe" ||
		quoted[5] != "203.0.113.5" || quoted[6] != "Testland" || quoted[7] != "false" {
		t.Fatalf("quoted audit row = %v", quoted)
	}
	if quoted[1] != "2026-10-27T09:03:04.123Z" {
		t.Fatalf("audit timestamp = %q", quoted[1])
	}
	if plain := exportRow(t, rows, 4, "plain"); plain[2] != "bob" || plain[7] != "true" {
		t.Fatalf("plain audit row = %v", plain)
	}

	rows = exportCSV(t, s.URL+"/v1/audit/connections?format=csv")
	if want := []string{"id", "started_at", "ended_at", "duration_ms", "username",
		"peer_id", "host_id", "audit_ref", "source", "direction",
		"client_ip", "success", "detail"}; !slices.Equal(rows[0], want) {
		t.Fatalf("connection header = %v, want %v", rows[0], want)
	}
	if len(rows) != 3 {
		t.Fatalf("connection rows = %d, want 3\n%v", len(rows), rows)
	}
	hostRow := exportRow(t, rows, 12, "host side, \"quoted\"")
	if hostRow[1] != "2026-10-27T09:03:04.123Z" || hostRow[3] != "90000" ||
		hostRow[4] != "-" || hostRow[6] != "devbox7" || hostRow[7] != "0011aabb" ||
		hostRow[8] != "host" || hostRow[9] != "host" || hostRow[11] != "true" {
		t.Fatalf("host connection row = %v", hostRow)
	}
	viewerRow := exportRow(t, rows, 12, "viewer side")
	if viewerRow[4] != "alice" || viewerRow[5] != "peer1" ||
		viewerRow[8] != "viewer" || viewerRow[10] != "198.51.100.7" {
		t.Fatalf("viewer connection row = %v", viewerRow)
	}

	rows = exportCSV(t, s.URL+"/v1/audit/file-transfers?format=csv")
	if want := []string{"id", "occurred_at", "username", "peer_id", "host_id",
		"audit_ref", "source", "direction", "name", "size", "files",
		"client_ip", "success", "detail"}; !slices.Equal(rows[0], want) {
		t.Fatalf("file header = %v, want %v", rows[0], want)
	}
	if len(rows) != 3 {
		t.Fatalf("file rows = %d, want 3\n%v", len(rows), rows)
	}
	hostFile := exportRow(t, rows, 8, "report, final.csv")
	if hostFile[1] != "2026-10-27T09:03:04.123Z" || hostFile[2] != "-" ||
		hostFile[4] != "devbox7" || hostFile[6] != "host" || hostFile[7] != "upload" ||
		hostFile[9] != "42" || hostFile[10] != "1" {
		t.Fatalf("host file row = %v", hostFile)
	}
	viewerFile := exportRow(t, rows, 8, "notes.txt")
	if viewerFile[2] != "alice" || viewerFile[4] != "" ||
		viewerFile[6] != "viewer" || viewerFile[7] != "download" ||
		viewerFile[11] != "198.51.100.7" {
		t.Fatalf("viewer file row = %v", viewerFile)
	}

	rows = exportCSV(t, s.URL+"/v1/audit/connections?format=csv&username=alice")
	if len(rows) != 2 || rows[1][4] != "alice" || rows[1][5] != "peer1" {
		t.Fatalf("connection username filter = %v", rows)
	}
	rows = exportCSV(t, s.URL+"/v1/audit/connections?format=csv&host=devbox7")
	if len(rows) != 2 || rows[1][6] != "devbox7" || rows[1][8] != "host" {
		t.Fatalf("connection host filter = %v", rows)
	}
	rows = exportCSV(t, s.URL+"/v1/audit/file-transfers?format=csv&username=alice")
	if len(rows) != 2 || rows[1][8] != "notes.txt" {
		t.Fatalf("file username filter = %v", rows)
	}
	rows = exportCSV(t, s.URL+"/v1/audit/file-transfers?format=csv&host=devbox7")
	if len(rows) != 2 || rows[1][8] != "report, final.csv" {
		t.Fatalf("file host filter = %v", rows)
	}
	rows = exportCSV(t, s.URL+"/v1/audit/file-transfers?format=csv&username=nobody")
	if len(rows) != 1 {
		t.Fatalf("empty export rows = %v", rows)
	}

	// The paged limit does not truncate a download.
	rows = exportCSV(t, s.URL+"/v1/audit?format=csv&action=export_probe&limit=1")
	if len(rows) != 3 {
		t.Fatalf("export ignored limit? rows = %v", rows)
	}

	// The default response is still the paged JSON view.
	status, body := request(t, http.MethodGet,
		s.URL+"/v1/audit?action=export_probe", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("audit json: %d %s", status, body)
	}
	var page auditEventsPage
	if err := json.Unmarshal(body, &page); err != nil {
		t.Fatalf("audit json: %v", err)
	}
	if len(page.Events) != 2 {
		t.Fatalf("audit json events = %+v", page.Events)
	}

	for _, path := range []string{
		"/v1/audit?format=xml",
		"/v1/audit/connections?format=xml",
		"/v1/audit/file-transfers?format=xml",
	} {
		status, _, body := exportGet(t, s.URL+path, testutil.ServerKey)
		if status != http.StatusBadRequest || !strings.Contains(string(body), "format must be csv or json") {
			t.Fatalf("%s: %d %s", path, status, body)
		}
	}

	token := login(t, s.URL, "alice", "correct horse").Token
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	viewerToken := login(t, s.URL, "bob", "battery staple").Token
	for _, path := range []string{
		"/v1/audit?format=csv",
		"/v1/audit/connections?format=csv",
		"/v1/audit/file-transfers?format=csv",
	} {
		status, header, body := exportGet(t, s.URL+path, "")
		if status != http.StatusUnauthorized {
			t.Fatalf("%s anonymous: %d %s", path, status, body)
		}
		if strings.HasPrefix(header.Get("Content-Type"), "text/csv") {
			t.Fatalf("%s anonymous leaked a csv body", path)
		}
		status, header, body = exportGet(t, s.URL+path, viewerToken)
		if status != http.StatusForbidden {
			t.Fatalf("%s viewer: %d %s", path, status, body)
		}
		if strings.HasPrefix(header.Get("Content-Type"), "text/csv") {
			t.Fatalf("%s viewer leaked a csv body", path)
		}
		status, header, body = exportGet(t, s.URL+path, token)
		if status != http.StatusOK {
			t.Fatalf("%s admin: %d %s", path, status, body)
		}
		if !strings.HasPrefix(header.Get("Content-Type"), "text/csv") {
			t.Fatalf("%s admin content type = %q", path, header.Get("Content-Type"))
		}
	}
}
