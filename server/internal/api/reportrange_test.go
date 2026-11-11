package api_test

import (
	"encoding/json"
	"net/http"
	"net/url"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// TestReportTimeRangeFiltering covers the since/until window shared by the
// audit, connection and file-transfer reports: unix milliseconds or RFC3339,
// inclusive edges, the group-scoped variants and the CSV exporters.
func TestReportTimeRangeFiltering(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.CreateGroup("range-lab", "Range Lab", "", 0, 0); err != nil {
		t.Fatalf("create group: %v", err)
	}
	if _, err := s.Accounts.AddGroupMember("range-lab", "alice", account.RoleReadonly); err != nil {
		t.Fatalf("add member: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")

	now := time.Now().UnixMilli()
	old := now - 3*60*60*1000
	recent := now - 10*60*1000

	reportConnection := func(token string, started, ended int64) {
		t.Helper()
		body := `{"peer_id":"range-box","started_at_ms":` + strconv.FormatInt(started, 10) +
			`,"ended_at_ms":` + strconv.FormatInt(ended, 10) + `}`
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/connections", body, token); status != http.StatusCreated {
			t.Fatalf("report connection: %d %s", status, body)
		}
	}
	reportConnection(alice.Token, old, old+60000)
	reportConnection(bob.Token, recent, recent+60000)

	reportTransfer := func(token string, occurred int64, name string) {
		t.Helper()
		body := `{"peer_id":"range-box","direction":"upload","name":"` + name +
			`","size":1024,"occurred_at_ms":` + strconv.FormatInt(occurred, 10) + `}`
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/file-transfers", body, token); status != http.StatusCreated {
			t.Fatalf("report transfer: %d %s", status, body)
		}
	}
	reportTransfer(alice.Token, old, "old.bin")
	reportTransfer(bob.Token, recent, "recent.bin")

	connections := func(query string) connectionsReply {
		t.Helper()
		status, body := request(t, http.MethodGet, s.URL+"/v1/audit/connections"+query, "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("connections%s: %d %s", query, status, body)
		}
		var reply connectionsReply
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatalf("connections%s reply: %v", query, err)
		}
		return reply
	}
	transfers := func(query string) fileTransfersReply {
		t.Helper()
		status, body := request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers"+query, "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("transfers%s: %d %s", query, status, body)
		}
		var reply fileTransfersReply
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatalf("transfers%s reply: %v", query, err)
		}
		return reply
	}
	auditEvents := func(query string) int {
		t.Helper()
		status, body := request(t, http.MethodGet, s.URL+"/v1/audit"+query, "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("audit%s: %d %s", query, status, body)
		}
		var reply struct {
			Events []json.RawMessage `json:"events"`
		}
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatalf("audit%s reply: %v", query, err)
		}
		return len(reply.Events)
	}
	millis := func(value int64) string { return strconv.FormatInt(value, 10) }
	rfc := func(value int64) string {
		return url.QueryEscape(time.UnixMilli(value).UTC().Format(time.RFC3339))
	}

	// An open window keeps every row; each edge alone cuts the other side.
	if got := connections("?since=" + millis(old)); len(got.Connections) != 2 {
		t.Fatalf("since=old connections = %+v", got.Connections)
	}
	if got := connections("?since=" + millis(recent)); len(got.Connections) != 1 ||
		got.Connections[0].Username != "bob" {
		t.Fatalf("since=recent connections = %+v", got.Connections)
	}
	// The edges are inclusive: the row that starts exactly at since stays in.
	if got := connections("?since=" + millis(recent)); len(got.Connections) != 1 {
		t.Fatalf("inclusive since lost the boundary row: %+v", got.Connections)
	}
	if got := connections("?until=" + millis(old+60000)); len(got.Connections) != 1 ||
		got.Connections[0].Username != "alice" {
		t.Fatalf("until=old connections = %+v", got.Connections)
	}
	if got := connections("?since=" + millis(old+120000) + "&until=" + millis(recent-1000)); len(got.Connections) != 0 {
		t.Fatalf("narrow window connections = %+v", got.Connections)
	}
	// RFC3339 is accepted as an alternative to unix milliseconds.
	if got := connections("?since=" + rfc(recent) + "&until=" + rfc(recent+60000)); len(got.Connections) != 1 ||
		got.Connections[0].Username != "bob" {
		t.Fatalf("rfc3339 connections = %+v", got.Connections)
	}
	// The group view applies the same bounds.
	if got := connections("?group=range-lab&since=" + millis(old) + "&until=" + millis(old+60000)); len(got.Connections) != 1 ||
		got.Connections[0].Username != "alice" {
		t.Fatalf("group connections = %+v", got.Connections)
	}
	if got := connections("?group=range-lab&since=" + millis(recent)); len(got.Connections) != 0 {
		t.Fatalf("group window leaked rows: %+v", got.Connections)
	}

	// File transfers use occurred_at with the same semantics.
	if got := transfers("?since=" + millis(recent)); len(got.Transfers) != 1 ||
		got.Transfers[0].Name != "recent.bin" {
		t.Fatalf("since=recent transfers = %+v", got.Transfers)
	}
	if got := transfers("?until=" + millis(old+1)); len(got.Transfers) != 1 ||
		got.Transfers[0].Name != "old.bin" {
		t.Fatalf("until=old transfers = %+v", got.Transfers)
	}
	if got := transfers("?group=range-lab&until=" + millis(old+1)); len(got.Transfers) != 1 ||
		got.Transfers[0].Username != "alice" {
		t.Fatalf("group transfers = %+v", got.Transfers)
	}
	if got := transfers("?group=range-lab&since=" + millis(recent)); len(got.Transfers) != 0 {
		t.Fatalf("group window leaked transfers: %+v", got.Transfers)
	}

	// The audit trail is written at login time, so anchor its window on now.
	loginsSince := now - 5*60*1000
	if got := auditEvents("?since=" + millis(loginsSince) + "&until=" + millis(now+5*60*1000)); got != 2 {
		t.Fatalf("audit window events = %d, want 2", got)
	}
	if got := auditEvents("?until=" + millis(loginsSince)); got != 0 {
		t.Fatalf("audit until=past events = %d", got)
	}
	if got := auditEvents("?group=range-lab&since=" + millis(loginsSince)); got != 1 {
		t.Fatalf("group audit events = %d, want 1", got)
	}

	// CSV exports honour the same window on all three tables.
	if rows := exportCSV(t, s.URL+"/v1/audit/connections?format=csv&since="+millis(recent)); len(rows) != 2 ||
		rows[1][4] != "bob" {
		t.Fatalf("csv connections window = %v", rows)
	}
	if rows := exportCSV(t, s.URL+"/v1/audit/file-transfers?format=csv&until="+millis(old+1)); len(rows) != 2 ||
		rows[1][2] != "alice" {
		t.Fatalf("csv transfers window = %v", rows)
	}
	if rows := exportCSV(t, s.URL+"/v1/audit?format=csv&until="+millis(loginsSince)); len(rows) != 1 {
		t.Fatalf("csv audit window = %v", rows)
	}

	// Bad windows are refused before any query runs.
	for _, path := range []string{"/v1/audit", "/v1/audit/connections", "/v1/audit/file-transfers"} {
		for _, query := range []string{
			"?since=-1",
			"?since=yesterday",
			"?since=" + millis(recent) + "&until=" + millis(old),
		} {
			if status, body := request(t, http.MethodGet, s.URL+path+query, "", testutil.ServerKey); status != http.StatusBadRequest {
				t.Fatalf("%s%s: %d %s", path, query, status, body)
			}
		}
	}
}
