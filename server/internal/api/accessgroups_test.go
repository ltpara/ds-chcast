package api_test

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type accessGroupReply struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Role        string `json:"role"`
	MaxDevices  int    `json:"max_devices"`
	MaxMembers  int    `json:"max_members"`
	DeviceCount int    `json:"device_count"`
	MemberCount int    `json:"member_count"`
}

type accessGroupListReply struct {
	Groups []accessGroupReply `json:"groups"`
}

type accessGroupMemberReply struct {
	Username string `json:"username"`
	Role     string `json:"role"`
}

type accessGroupDeviceReply struct {
	DeviceID string `json:"device_id"`
	Added    bool   `json:"added"`
}

func TestAccessGroupHTTPAuthorizationAndQuotas(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := s.Accounts.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.CreateUser("carol", "silent river"); err != nil {
		t.Fatalf("create carol: %v", err)
	}
	if _, err := s.Accounts.CreateUser("dave", "quiet meadow"); err != nil {
		t.Fatalf("create dave: %v", err)
	}
	adminToken := login(t, s.URL, "alice", "correct horse").Token
	bobToken := login(t, s.URL, "bob", "battery staple").Token
	carolToken := login(t, s.URL, "carol", "silent river").Token

	status, data := request(t, http.MethodPost, s.URL+"/v1/access-groups",
		`{"name":"ops","description":"core hosts","max_devices":1,"max_members":2,"admin":"bob"}`,
		testutil.ServerKey)
	if status != http.StatusCreated {
		t.Fatalf("server-key create: %d %s", status, data)
	}
	var group accessGroupReply
	if err := json.Unmarshal(data, &group); err != nil {
		t.Fatal(err)
	}
	if group.ID == "" || group.MemberCount != 1 || group.MaxDevices != 1 || group.MaxMembers != 2 {
		t.Fatalf("created group = %+v", group)
	}
	groupURL := s.URL + "/v1/access-groups/" + group.ID

	if status, body := request(t, http.MethodGet, groupURL, "", carolToken); status != http.StatusForbidden {
		t.Fatalf("non-member read: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, groupURL, "", ""); status != http.StatusUnauthorized {
		t.Fatalf("anonymous read: %d %s", status, body)
	}
	status, data = request(t, http.MethodGet, groupURL, "", bobToken)
	if status != http.StatusOK {
		t.Fatalf("member read: %d %s", status, data)
	}
	var view accessGroupReply
	if err := json.Unmarshal(data, &view); err != nil {
		t.Fatal(err)
	}
	if view.Role != account.GroupRoleAdmin {
		t.Fatalf("bob group role = %q", view.Role)
	}

	if status, body := request(t, http.MethodPut, groupURL+"/members/carol", `{"role":"readonly"}`, bobToken); status != http.StatusOK {
		t.Fatalf("add carol: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPut, groupURL+"/members/dave", `{"role":"readonly"}`, adminToken); status != http.StatusConflict {
		t.Fatalf("member quota: %d %s", status, body)
	} else if !strings.Contains(string(body), "member quota") {
		t.Fatalf("member quota error is not explicit: %s", body)
	}
	if status, body := request(t, http.MethodPut, groupURL+"/members/carol", `{"role":"root"}`, bobToken); status != http.StatusBadRequest {
		t.Fatalf("invalid group role: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPut, groupURL+"/members/bob", `{"role":"write"}`, bobToken); status != http.StatusConflict {
		t.Fatalf("demote last admin: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPost, groupURL+"/devices", `{"device_id":"host-a"}`, carolToken); status != http.StatusForbidden {
		t.Fatalf("readonly add device: %d %s", status, body)
	}
	status, data = request(t, http.MethodPost, groupURL+"/devices", `{"device_id":"host-a"}`, bobToken)
	if status != http.StatusCreated {
		t.Fatalf("add host-a: %d %s", status, data)
	}
	var device accessGroupDeviceReply
	if err := json.Unmarshal(data, &device); err != nil || !device.Added || device.DeviceID != "host-a" {
		t.Fatalf("device reply = %+v %v", device, err)
	}
	if status, body := request(t, http.MethodPost, groupURL+"/devices", `{"device_id":"host-a"}`, bobToken); status != http.StatusOK {
		t.Fatalf("re-add host-a: %d %s", status, body)
	} else {
		var again accessGroupDeviceReply
		if err := json.Unmarshal(body, &again); err != nil || again.Added {
			t.Fatalf("re-add reply = %+v %v", again, err)
		}
	}
	if status, body := request(t, http.MethodPost, groupURL+"/devices", `{"device_id":"host-b"}`, bobToken); status != http.StatusConflict {
		t.Fatalf("device quota: %d %s", status, body)
	} else if !strings.Contains(string(body), "device quota") {
		t.Fatalf("device quota error is not explicit: %s", body)
	}
	if status, body := request(t, http.MethodPatch, groupURL, `{"max_devices":2}`, bobToken); status != http.StatusOK {
		t.Fatalf("raise device quota: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, groupURL+"/devices", `{"device_id":"host-b"}`, bobToken); status != http.StatusCreated {
		t.Fatalf("add host-b after raising quota: %d %s", status, body)
	}

	now := time.Now().UTC()
	for _, peer := range []string{"host-a", "host-b", "host-c"} {
		if _, err := s.Accounts.RecordConnection(bob.ID, bob.Username, peer, "viewer", "127.0.0.1",
			"", "", "", now, now.Add(time.Second), true, ""); err != nil {
			t.Fatalf("record connection %s: %v", peer, err)
		}
		if _, err := s.Accounts.RecordFileTransfer(bob.ID, bob.Username, peer, "download", "file.bin",
			"127.0.0.1", "", "", "", 10, 1, now, true, ""); err != nil {
			t.Fatalf("record transfer %s: %v", peer, err)
		}
	}
	status, data = request(t, http.MethodGet, groupURL+"/connections", "", carolToken)
	if status != http.StatusOK {
		t.Fatalf("group connections: %d %s", status, data)
	}
	var connections struct {
		Connections []struct {
			PeerID string `json:"peer_id"`
		} `json:"connections"`
	}
	if err := json.Unmarshal(data, &connections); err != nil {
		t.Fatal(err)
	}
	if len(connections.Connections) != 2 {
		t.Fatalf("group connections = %+v", connections.Connections)
	}
	for _, event := range connections.Connections {
		if event.PeerID == "host-c" {
			t.Fatalf("host-c leaked into group connections: %+v", event)
		}
	}
	status, data = request(t, http.MethodGet, groupURL+"/file-transfers", "", carolToken)
	if status != http.StatusOK {
		t.Fatalf("group transfers: %d %s", status, data)
	}
	var transfers struct {
		Transfers []struct {
			PeerID string `json:"peer_id"`
		} `json:"transfers"`
	}
	if err := json.Unmarshal(data, &transfers); err != nil {
		t.Fatal(err)
	}
	if len(transfers.Transfers) != 2 {
		t.Fatalf("group transfers = %+v", transfers.Transfers)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/access-groups", "", bobToken)
	if status != http.StatusOK {
		t.Fatalf("bob list groups: %d %s", status, data)
	}
	var bobGroups accessGroupListReply
	if err := json.Unmarshal(data, &bobGroups); err != nil || len(bobGroups.Groups) != 1 ||
		bobGroups.Groups[0].Role != account.GroupRoleAdmin {
		t.Fatalf("bob groups = %+v %v", bobGroups, err)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/access-groups", "", adminToken)
	if status != http.StatusOK {
		t.Fatalf("admin list groups: %d %s", status, data)
	}
	var allGroups accessGroupListReply
	if err := json.Unmarshal(data, &allGroups); err != nil || len(allGroups.Groups) != 1 {
		t.Fatalf("admin groups = %+v %v", allGroups, err)
	}

	if status, body := request(t, http.MethodDelete, groupURL, "", bobToken); status != http.StatusForbidden {
		t.Fatalf("group admin delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/access-groups",
		`{"name":"OPS"}`, adminToken); status != http.StatusConflict {
		t.Fatalf("duplicate group name: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/access-groups",
		`{"name":"bad","max_devices":-1}`, adminToken); status != http.StatusBadRequest {
		t.Fatalf("negative quota: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, groupURL, "", adminToken); status != http.StatusNoContent {
		t.Fatalf("admin delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, groupURL, "", bobToken); status != http.StatusNotFound {
		t.Fatalf("deleted group read: %d %s", status, body)
	}
}
