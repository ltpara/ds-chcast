package api_test

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type auditEventsPage struct {
	Events []struct {
		ID     int64  `json:"id"`
		Detail string `json:"detail"`
	} `json:"events"`
	NextBeforeID int64 `json:"next_before_id"`
}

func fetchAuditPage(t *testing.T, url, query string) auditEventsPage {
	t.Helper()
	status, body := request(t, http.MethodGet, url+"/v1/audit"+query, "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("audit%s: %d %s", query, status, body)
	}
	var page auditEventsPage
	if err := json.Unmarshal(body, &page); err != nil {
		t.Fatalf("audit page: %v", err)
	}
	return page
}

func fetchConnectionsPage(t *testing.T, url, token, query string) connectionsReply {
	t.Helper()
	status, body := request(t, http.MethodGet, url+"/v1/account/connections"+query, "", token)
	if status != http.StatusOK {
		t.Fatalf("connections%s: %d %s", query, status, body)
	}
	var page connectionsReply
	if err := json.Unmarshal(body, &page); err != nil {
		t.Fatalf("connections page: %v", err)
	}
	return page
}

func fetchTransfersPage(t *testing.T, url, token, query string) fileTransfersReply {
	t.Helper()
	status, body := request(t, http.MethodGet, url+"/v1/account/file-transfers"+query, "", token)
	if status != http.StatusOK {
		t.Fatalf("file transfers%s: %d %s", query, status, body)
	}
	var page fileTransfersReply
	if err := json.Unmarshal(body, &page); err != nil {
		t.Fatalf("file transfers page: %v", err)
	}
	return page
}

func TestHistoryCursorPagination(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	token := login(t, s.URL, "alice", "correct horse").Token

	for _, detail := range []string{"one", "two", "three", "four", "five"} {
		if err := s.Accounts.AppendAudit(account.AuditEvent{
			Username: "alice", Action: "pagination_probe", Detail: detail,
			CreatedAt: time.Now(), Success: true,
		}); err != nil {
			t.Fatalf("append %s: %v", detail, err)
		}
	}
	first := fetchAuditPage(t, s.URL, "?action=pagination_probe&limit=2")
	if len(first.Events) != 2 || first.Events[0].Detail != "five" || first.Events[1].Detail != "four" {
		t.Fatalf("audit first page = %+v", first.Events)
	}
	if first.NextBeforeID != first.Events[1].ID {
		t.Fatalf("cursor = %d, want %d", first.NextBeforeID, first.Events[1].ID)
	}
	second := fetchAuditPage(t, s.URL,
		fmt.Sprintf("?action=pagination_probe&limit=2&before_id=%d", first.NextBeforeID))
	if len(second.Events) != 2 || second.Events[0].Detail != "three" || second.Events[1].Detail != "two" {
		t.Fatalf("audit second page = %+v", second.Events)
	}
	last := fetchAuditPage(t, s.URL,
		fmt.Sprintf("?action=pagination_probe&limit=2&before_id=%d", second.NextBeforeID))
	if len(last.Events) != 1 || last.Events[0].Detail != "one" || last.NextBeforeID != 0 {
		t.Fatalf("audit last page = %+v, cursor %d", last.Events, last.NextBeforeID)
	}

	started := time.Now().Add(-time.Minute).UnixMilli()
	ended := time.Now().UnixMilli()
	for i, peer := range []string{"devbox1", "devbox2", "devbox3"} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/connections",
			viewerConnectionBody(t, peer, "", started, ended, true, fmt.Sprintf("conn %d", i)),
			token); status != http.StatusCreated {
			t.Fatalf("connection %s: %d %s", peer, status, body)
		}
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/file-transfers",
			fmt.Sprintf(`{"peer_id":%q,"direction":"upload","name":"file-%d.pdf","size":10,"files":1}`,
				peer, i), token); status != http.StatusCreated {
			t.Fatalf("transfer %s: %d %s", peer, status, body)
		}
	}

	connections := fetchConnectionsPage(t, s.URL, token, "?limit=2")
	if len(connections.Connections) != 2 || connections.NextBeforeID == 0 {
		t.Fatalf("connection first page = %+v, cursor %d", connections.Connections, connections.NextBeforeID)
	}
	older := fetchConnectionsPage(t, s.URL, token,
		fmt.Sprintf("?limit=2&before_id=%d", connections.NextBeforeID))
	if len(older.Connections) != 1 || older.NextBeforeID != 0 ||
		older.Connections[0].PeerID != "devbox1" {
		t.Fatalf("connection last page = %+v, cursor %d", older.Connections, older.NextBeforeID)
	}

	status, body := request(t, http.MethodGet,
		s.URL+fmt.Sprintf("/v1/audit/connections?limit=2&before_id=%d", connections.NextBeforeID),
		"", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("operator connections page: %d %s", status, body)
	}
	var operator connectionsReply
	if err := json.Unmarshal(body, &operator); err != nil {
		t.Fatalf("operator connections page: %v", err)
	}
	if len(operator.Connections) != 1 || operator.NextBeforeID != 0 ||
		operator.Connections[0].PeerID != "devbox1" {
		t.Fatalf("operator connections page = %+v, cursor %d", operator.Connections, operator.NextBeforeID)
	}

	transfers := fetchTransfersPage(t, s.URL, token, "?limit=2")
	if len(transfers.Transfers) != 2 || transfers.NextBeforeID == 0 {
		t.Fatalf("transfer first page = %+v, cursor %d", transfers.Transfers, transfers.NextBeforeID)
	}
	olderTransfers := fetchTransfersPage(t, s.URL, token,
		fmt.Sprintf("?limit=2&before_id=%d", transfers.NextBeforeID))
	if len(olderTransfers.Transfers) != 1 || olderTransfers.NextBeforeID != 0 ||
		olderTransfers.Transfers[0].Name != "file-0.pdf" {
		t.Fatalf("transfer last page = %+v, cursor %d",
			olderTransfers.Transfers, olderTransfers.NextBeforeID)
	}

	status, body = request(t, http.MethodGet,
		s.URL+fmt.Sprintf("/v1/audit/file-transfers?limit=2&before_id=%d", transfers.NextBeforeID),
		"", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("operator transfers page: %d %s", status, body)
	}
	var operatorTransfers fileTransfersReply
	if err := json.Unmarshal(body, &operatorTransfers); err != nil {
		t.Fatalf("operator transfers page: %v", err)
	}
	if len(operatorTransfers.Transfers) != 1 || operatorTransfers.NextBeforeID != 0 ||
		operatorTransfers.Transfers[0].Name != "file-0.pdf" {
		t.Fatalf("operator transfers page = %+v, cursor %d",
			operatorTransfers.Transfers, operatorTransfers.NextBeforeID)
	}

	for _, endpoint := range []struct {
		path  string
		token string
	}{
		{"/v1/audit", testutil.ServerKey},
		{"/v1/audit/connections", testutil.ServerKey},
		{"/v1/audit/file-transfers", testutil.ServerKey},
		{"/v1/account/connections", token},
		{"/v1/account/file-transfers", token},
	} {
		for _, cursor := range []string{"0", "-1", "abc"} {
			status, body := request(t, http.MethodGet,
				s.URL+endpoint.path+"?before_id="+cursor, "", endpoint.token)
			if status != http.StatusBadRequest {
				t.Fatalf("%s before_id=%s: %d %s", endpoint.path, cursor, status, body)
			}
		}
	}
}
