package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestAuditReportsCombineGroupUserHostAndCSV(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := s.Accounts.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.CreateUser("carol", "carol password"); err != nil {
		t.Fatalf("create carol: %v", err)
	}
	if _, err := s.Accounts.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create group: %v", err)
	}
	if _, err := s.Accounts.AddGroupMember("lab", alice.Username, account.RoleReadonly); err != nil {
		t.Fatalf("add alice: %v", err)
	}
	if _, err := s.Accounts.AddGroupMember("lab", bob.Username, account.RoleAdmin); err != nil {
		t.Fatalf("add bob: %v", err)
	}
	now := time.Now().UTC()
	for _, event := range []account.AuditEvent{
		{Username: "alice", Action: "filter_probe", HostID: "alpha", Detail: "alice-alpha"},
		{Username: "alice", Action: "filter_probe", HostID: "beta", Detail: "alice-beta"},
		{Username: "alice", Action: "other_probe", HostID: "alpha", Detail: "alice-other-action"},
		{Username: "bob", Action: "filter_probe", HostID: "alpha", Detail: "bob-alpha"},
		{Username: "carol", Action: "filter_probe", HostID: "alpha", Detail: "carol-alpha"},
	} {
		event.CreatedAt = now
		if err := s.Accounts.AppendAudit(event); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	for _, item := range []struct {
		user account.User
		peer string
	}{
		{alice, "alpha"},
		{bob, "alpha"},
		{alice, "beta"},
	} {
		if _, err := s.Accounts.RecordConnection(item.user.ID, item.user.Username,
			item.peer, "viewer", "127.0.0.1", "", "", "",
			now.Add(-time.Minute), now, true, item.user.Username+"-"+item.peer); err != nil {
			t.Fatalf("record connection: %v", err)
		}
		if _, err := s.Accounts.RecordFileTransfer(item.user.ID, item.user.Username,
			item.peer, "upload", item.user.Username+"-"+item.peer+".txt",
			"127.0.0.1", "", "", "", 1, 1, now, true, ""); err != nil {
			t.Fatalf("record transfer: %v", err)
		}
	}

	page := fetchAuditPage(t, s.URL,
		"?group=lab&action=filter_probe&user=alice&host=ALPHA")
	if len(page.Events) != 1 || page.Events[0].Detail != "alice-alpha" {
		t.Fatalf("combined account audit = %+v", page.Events)
	}
	page = fetchAuditPage(t, s.URL, "?group=lab&action=filter_probe&host=alpha")
	if len(page.Events) != 2 {
		t.Fatalf("group host account audit = %+v", page.Events)
	}
	rows := exportCSV(t, s.URL+
		"/v1/audit?format=csv&group=lab&action=filter_probe&user=alice&host=alpha")
	if len(rows) != 2 || rows[1][2] != "alice" || rows[1][4] != "alice-alpha" || rows[1][8] != "alpha" {
		t.Fatalf("combined account audit csv = %v", rows)
	}

	status, body := request(t, http.MethodGet,
		s.URL+"/v1/audit/connections?username=alice&host=ALPHA", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("combined connections: %d %s", status, body)
	}
	var connections struct {
		Connections []struct {
			Username string `json:"username"`
			Detail   string `json:"detail"`
		} `json:"connections"`
	}
	if err := json.Unmarshal(body, &connections); err != nil {
		t.Fatalf("decode combined connections: %v", err)
	}
	if len(connections.Connections) != 1 || connections.Connections[0].Detail != "alice-alpha" {
		t.Fatalf("combined connections = %+v", connections.Connections)
	}
	rows = exportCSV(t, s.URL+"/v1/audit/connections?format=csv&username=alice&host=alpha")
	if len(rows) != 2 || rows[1][4] != "alice" || rows[1][14] != "alice-alpha" {
		t.Fatalf("combined connection csv = %v", rows)
	}

	status, body = request(t, http.MethodGet,
		s.URL+"/v1/audit/file-transfers?username=alice&host=ALPHA", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("combined transfers: %d %s", status, body)
	}
	var transfers struct {
		Transfers []struct {
			Username string `json:"username"`
			Name     string `json:"name"`
		} `json:"transfers"`
	}
	if err := json.Unmarshal(body, &transfers); err != nil {
		t.Fatalf("decode combined transfers: %v", err)
	}
	if len(transfers.Transfers) != 1 || transfers.Transfers[0].Name != "alice-alpha.txt" {
		t.Fatalf("combined transfers = %+v", transfers.Transfers)
	}
	rows = exportCSV(t, s.URL+"/v1/audit/file-transfers?format=csv&username=alice&host=alpha")
	if len(rows) != 2 || rows[1][2] != "alice" || rows[1][8] != "alice-alpha.txt" {
		t.Fatalf("combined transfer csv = %v", rows)
	}
}
