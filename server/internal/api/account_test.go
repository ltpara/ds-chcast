package api_test

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/geoip"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type loginReply struct {
	Token              string `json:"token"`
	ExpiresInMS        int64  `json:"expires_in_ms"`
	RefreshToken       string `json:"refresh_token"`
	RefreshExpiresInMS int64  `json:"refresh_expires_in_ms"`
	User               struct {
		ID       string `json:"id"`
		Username string `json:"username"`
	} `json:"user"`
}

type peerReply struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Tags        []string `json:"tags"`
	GroupID     string   `json:"group_id"`
	UpdatedAtMS int64    `json:"updated_at_ms"`
	Revision    int64    `json:"revision"`
	Deleted     bool     `json:"deleted"`
}

type groupReply struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	UpdatedAtMS int64  `json:"updated_at_ms"`
	Revision    int64  `json:"revision"`
	Deleted     bool   `json:"deleted"`
}

type addressBookReply struct {
	Revision     int64        `json:"revision"`
	Peers        []peerReply  `json:"peers"`
	Changes      []peerReply  `json:"changes"`
	Groups       []groupReply `json:"groups"`
	GroupChanges []groupReply `json:"group_changes"`
}

func login(t *testing.T, base, username, password string) loginReply {
	t.Helper()
	body, err := json.Marshal(map[string]string{"username": username, "password": password})
	if err != nil {
		t.Fatal(err)
	}
	status, data := request(t, http.MethodPost, base+"/v1/account/login", string(body), "")
	if status != http.StatusOK {
		t.Fatalf("login: %d %s", status, data)
	}
	var reply loginReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("login reply: %v", err)
	}
	if reply.Token == "" || reply.ExpiresInMS <= 0 || reply.User.Username != username {
		t.Fatalf("unexpected login reply: %+v", reply)
	}
	if reply.RefreshToken == "" || reply.RefreshExpiresInMS <= 0 {
		t.Fatalf("login reply has no refresh token: %+v", reply)
	}
	return reply
}

func TestAccountRefreshRotatesAndDetectsReplay(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	first := login(t, s.URL, "alice", "correct horse")
	refreshBody := func(token string) string {
		body, err := json.Marshal(map[string]string{"refresh_token": token})
		if err != nil {
			t.Fatal(err)
		}
		return string(body)
	}

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/refresh",
		refreshBody(first.RefreshToken), "")
	if status != http.StatusOK {
		t.Fatalf("refresh: %d %s", status, data)
	}
	var second loginReply
	if err := json.Unmarshal(data, &second); err != nil {
		t.Fatalf("refresh reply: %v", err)
	}
	if second.Token == "" || second.Token == first.Token ||
		second.RefreshToken == "" || second.RefreshToken == first.RefreshToken {
		t.Fatalf("refresh did not rotate the pair: %+v", second)
	}
	if second.RefreshExpiresInMS <= second.ExpiresInMS {
		t.Fatalf("refresh lifetime should outlive the access token: %+v", second)
	}
	if status, data := request(t, http.MethodGet, s.URL+"/v1/account", "", first.Token); status != http.StatusUnauthorized {
		t.Fatalf("old access token after refresh = %d %s", status, data)
	}
	if status, data := request(t, http.MethodGet, s.URL+"/v1/account", "", second.Token); status != http.StatusOK {
		t.Fatalf("new access token = %d %s", status, data)
	}

	status, data = request(t, http.MethodPost, s.URL+"/v1/account/refresh",
		refreshBody(first.RefreshToken), "")
	if status != http.StatusUnauthorized {
		t.Fatalf("replayed refresh = %d %s", status, data)
	}
	if status, data := request(t, http.MethodGet, s.URL+"/v1/account", "", second.Token); status != http.StatusUnauthorized {
		t.Fatalf("replay kept the session alive: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/refresh",
		refreshBody(second.RefreshToken), ""); status != http.StatusUnauthorized {
		t.Fatalf("rotated refresh after replay = %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/refresh", `{}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("empty refresh = %d %s", status, data)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditRefresh, 10)
	if err != nil {
		t.Fatalf("refresh audit: %v", err)
	}
	var sawRotation, sawReplay bool
	for _, event := range events {
		if event.Success && event.Username == "alice" && event.Detail == "rotated" {
			sawRotation = true
		}
		if !event.Success && event.Detail == "replay; session revoked" {
			sawReplay = true
		}
	}
	if !sawRotation || !sawReplay {
		t.Fatalf("refresh audit missing: rotation=%v replay=%v events=%+v", sawRotation, sawReplay, events)
	}
}

func TestAccountLoginAndAddressBook(t *testing.T) {
	s := testutil.Start(t)
	user, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}

	reply := login(t, s.URL, "alice", "correct horse")
	if reply.User.ID != user.ID {
		t.Fatalf("login user = %+v, want id %s", reply.User, user.ID)
	}
	for _, tc := range []struct {
		name string
		body string
		want int
	}{
		{"wrong password", `{"username":"alice","password":"wrong horse"}`, http.StatusUnauthorized},
		{"unknown user", `{"username":"bob","password":"correct horse"}`, http.StatusUnauthorized},
		{"empty fields", `{"username":"","password":""}`, http.StatusUnauthorized},
		{"malformed body", `{"username":`, http.StatusBadRequest},
	} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", tc.body, ""); status != tc.want {
			t.Fatalf("%s: %d %s, want %d", tc.name, status, body, tc.want)
		}
	}

	status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", reply.Token)
	if status != http.StatusOK || !strings.Contains(string(body), `"username":"alice"`) {
		t.Fatalf("account info: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("account info without token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("server key accepted as account token: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook", "", reply.Token)
	if status != http.StatusOK || !strings.Contains(string(body), `"peers":[]`) ||
		!strings.Contains(string(body), `"revision":0`) {
		t.Fatalf("empty address book: %d %s", status, body)
	}
	for _, since := range []string{"abc", "-1"} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook?since="+since, "", reply.Token); status != http.StatusBadRequest {
			t.Fatalf("since=%s: %d %s", since, status, body)
		}
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook", "", testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("server key read the address book: %d %s", status, body)
	}

	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/studio",
		`{"name":" 工作室 ","tags":["office","office","","prod"]}`, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("create address book peer: %d %s", status, body)
	}
	var studio peerReply
	if err := json.Unmarshal(body, &studio); err != nil {
		t.Fatalf("peer reply: %v", err)
	}
	if studio.ID != "studio" || studio.Name != "工作室" ||
		len(studio.Tags) != 2 || studio.Tags[0] != "office" || studio.Tags[1] != "prod" ||
		studio.UpdatedAtMS == 0 || studio.Revision != 1 {
		t.Fatalf("unexpected peer: %+v", studio)
	}

	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/studio",
		`{"tags":["lab"]}`, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("partial address book update: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &studio); err != nil {
		t.Fatalf("partial peer reply: %v", err)
	}
	if studio.Name != "工作室" || len(studio.Tags) != 1 || studio.Tags[0] != "lab" ||
		studio.Revision != 2 {
		t.Fatalf("partial update lost fields: %+v", studio)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/studio", `{}`, reply.Token); status != http.StatusBadRequest {
		t.Fatalf("empty update: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/bad%20id", `{"name":"x"}`, reply.Token); status != http.StatusBadRequest {
		t.Fatalf("invalid peer id: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/lab", `{"name":"Lab"}`, reply.Token); status != http.StatusOK {
		t.Fatalf("add lab: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook", "", reply.Token)
	if status != http.StatusOK {
		t.Fatalf("read address book: %d %s", status, body)
	}
	var book addressBookReply
	if err := json.Unmarshal(body, &book); err != nil {
		t.Fatalf("address book reply: %v", err)
	}
	if len(book.Peers) != 2 || book.Peers[0].ID != "lab" || book.Peers[1].ID != "studio" ||
		book.Peers[0].Revision != 3 || book.Peers[1].Revision != 2 || book.Revision != 3 {
		t.Fatalf("address book = %+v revision=%d", book.Peers, book.Revision)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/peers/studio", "", reply.Token); status != http.StatusNoContent {
		t.Fatalf("delete peer: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook?since=3", "", reply.Token)
	if status != http.StatusOK {
		t.Fatalf("incremental address book: %d %s", status, body)
	}
	var incremental addressBookReply
	if err := json.Unmarshal(body, &incremental); err != nil {
		t.Fatalf("incremental reply: %v", err)
	}
	if incremental.Revision != 4 || len(incremental.Peers) != 1 || incremental.Peers[0].ID != "lab" {
		t.Fatalf("incremental peers = %+v revision=%d", incremental.Peers, incremental.Revision)
	}
	if len(incremental.Changes) != 1 || incremental.Changes[0].ID != "studio" ||
		!incremental.Changes[0].Deleted || incremental.Changes[0].Revision != 4 {
		t.Fatalf("incremental changes = %+v", incremental.Changes)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook?since=4", "", reply.Token)
	if status != http.StatusOK {
		t.Fatalf("up-to-date address book: %d %s", status, body)
	}
	incremental = addressBookReply{}
	if err := json.Unmarshal(body, &incremental); err != nil {
		t.Fatalf("up-to-date reply: %v", err)
	}
	if incremental.Revision != 4 || len(incremental.Changes) != 0 {
		t.Fatalf("up-to-date changes = %+v revision=%d", incremental.Changes, incremental.Revision)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/peers/studio", "", reply.Token); status != http.StatusNotFound {
		t.Fatalf("delete missing peer: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/peers/bad%20id", "", reply.Token); status != http.StatusBadRequest {
		t.Fatalf("delete invalid peer: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/logout", "", reply.Token); status != http.StatusNoContent {
		t.Fatalf("logout: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", reply.Token); status != http.StatusUnauthorized {
		t.Fatalf("token survived logout: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/logout", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("logout without token: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	if status != http.StatusOK || !strings.Contains(string(body), `"accounts":true`) {
		t.Fatalf("server info accounts flag: %d %s", status, body)
	}
}

func TestAddressBookGroupAPI(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	reply := login(t, s.URL, "alice", "correct horse")

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/bad%20id", `{"name":"工作室"}`, reply.Token); status != http.StatusBadRequest {
		t.Fatalf("invalid group id: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio", `{"name":"   "}`, reply.Token); status != http.StatusBadRequest {
		t.Fatalf("empty group name: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio", `{"name":"工作室"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("group write without token: %d %s", status, body)
	}

	status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio", `{"name":" 工作室 "}`, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("create group: %d %s", status, body)
	}
	var group groupReply
	if err := json.Unmarshal(body, &group); err != nil {
		t.Fatalf("group reply: %v", err)
	}
	if group.ID != "studio" || group.Name != "工作室" || group.Revision != 1 ||
		group.UpdatedAtMS == 0 || group.Deleted {
		t.Fatalf("unexpected group: %+v", group)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/lab", `{"name":"Lab"}`, reply.Token); status != http.StatusOK {
		t.Fatalf("second group: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/lab", `{"group_id":"missing"}`, reply.Token); status != http.StatusBadRequest {
		t.Fatalf("unknown group assignment: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/lab", `{"group_id":"bad id"}`, reply.Token); status != http.StatusBadRequest {
		t.Fatalf("invalid group assignment: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/missing", `{"group_id":"studio"}`, reply.Token); status != http.StatusNotFound {
		t.Fatalf("group move for missing peer: %d %s", status, body)
	}

	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/studio",
		`{"name":"Studio","group_id":"studio"}`, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("create grouped peer: %d %s", status, body)
	}
	var studio peerReply
	if err := json.Unmarshal(body, &studio); err != nil {
		t.Fatalf("peer reply: %v", err)
	}
	if studio.GroupID != "studio" || studio.Revision != 3 {
		t.Fatalf("grouped peer = %+v", studio)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/lab", `{"name":"Lab box"}`, reply.Token); status != http.StatusOK {
		t.Fatalf("create lab peer: %d %s", status, body)
	}
	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/lab", `{"group_id":"studio"}`, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("move lab peer: %d %s", status, body)
	}
	var lab peerReply
	if err := json.Unmarshal(body, &lab); err != nil {
		t.Fatalf("moved peer reply: %v", err)
	}
	if lab.GroupID != "studio" || lab.Name != "Lab box" || lab.Revision != 5 {
		t.Fatalf("moved peer = %+v", lab)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/lab", `{"group_id":"studio"}`, reply.Token); status != http.StatusOK ||
		!strings.Contains(string(body), `"revision":5`) {
		t.Fatalf("no-op group move: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook", "", reply.Token)
	if status != http.StatusOK {
		t.Fatalf("read grouped book: %d %s", status, body)
	}
	var book addressBookReply
	if err := json.Unmarshal(body, &book); err != nil {
		t.Fatalf("grouped book reply: %v", err)
	}
	if book.Revision != 5 || len(book.Peers) != 2 || len(book.Groups) != 2 ||
		len(book.GroupChanges) != 2 ||
		book.Groups[0].ID != "lab" || book.Groups[1].ID != "studio" {
		t.Fatalf("grouped book = %+v", book)
	}
	if status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook?since=2", "", reply.Token); status != http.StatusOK {
		t.Fatalf("incremental grouped book: %d %s", status, body)
	}
	var incremental addressBookReply
	if err := json.Unmarshal(body, &incremental); err != nil {
		t.Fatalf("incremental reply: %v", err)
	}
	if incremental.Revision != 5 || len(incremental.Changes) != 2 ||
		len(incremental.GroupChanges) != 0 {
		t.Fatalf("incremental grouped book = %+v", incremental)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/groups/studio", "", reply.Token); status != http.StatusNoContent {
		t.Fatalf("delete group: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook?since=5", "", reply.Token)
	if status != http.StatusOK {
		t.Fatalf("book after group delete: %d %s", status, body)
	}
	incremental = addressBookReply{}
	if err := json.Unmarshal(body, &incremental); err != nil {
		t.Fatalf("post-delete reply: %v", err)
	}
	if incremental.Revision != 8 || len(incremental.Peers) != 2 ||
		len(incremental.Groups) != 1 || incremental.Groups[0].ID != "lab" {
		t.Fatalf("book after group delete = %+v", incremental)
	}
	if len(incremental.GroupChanges) != 1 || incremental.GroupChanges[0].ID != "studio" ||
		!incremental.GroupChanges[0].Deleted || incremental.GroupChanges[0].Revision != 6 ||
		incremental.GroupChanges[0].Name != "" {
		t.Fatalf("group tombstone = %+v", incremental.GroupChanges)
	}
	if len(incremental.Changes) != 2 || incremental.Changes[0].ID != "lab" ||
		incremental.Changes[1].ID != "studio" ||
		incremental.Changes[0].GroupID != "" || incremental.Changes[1].GroupID != "" ||
		incremental.Changes[0].Revision != 7 || incremental.Changes[1].Revision != 8 {
		t.Fatalf("ungrouped peer changes = %+v", incremental.Changes)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/groups/studio", "", reply.Token); status != http.StatusNotFound {
		t.Fatalf("delete missing group: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/groups/bad%20id", "", reply.Token); status != http.StatusBadRequest {
		t.Fatalf("delete invalid group: %d %s", status, body)
	}
	wrong := login(t, s.URL, "alice", "correct horse")
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/groups/lab", "", wrong.Token); status != http.StatusNoContent {
		t.Fatalf("delete own group with second session: %d %s", status, body)
	}
}

func TestAddressBookGroupOptimisticConcurrencyAPI(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	reply := login(t, s.URL, "alice", "correct horse")
	status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio", `{"name":"Studio"}`, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("create group: %d %s", status, body)
	}
	var group groupReply
	if err := json.Unmarshal(body, &group); err != nil {
		t.Fatalf("group reply: %v", err)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio", `{"name":"Lab","if_revision":0}`, reply.Token); status != http.StatusBadRequest {
		t.Fatalf("non-positive group if_revision: %d %s", status, body)
	}
	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio",
		fmt.Sprintf(`{"name":"Lab","if_revision":%d}`, group.Revision), reply.Token)
	if status != http.StatusOK {
		t.Fatalf("matching group rename: %d %s", status, body)
	}
	var renamed groupReply
	if err := json.Unmarshal(body, &renamed); err != nil {
		t.Fatalf("renamed group reply: %v", err)
	}
	if renamed.Name != "Lab" || renamed.Revision != 2 {
		t.Fatalf("renamed group = %+v", renamed)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio",
		fmt.Sprintf(`{"name":"Stale","if_revision":%d}`, group.Revision), reply.Token); status != http.StatusConflict {
		t.Fatalf("stale group rename: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/groups/studio?if_revision=0", "", reply.Token); status != http.StatusBadRequest {
		t.Fatalf("non-positive delete if_revision: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/groups/studio?if_revision=1", "", reply.Token); status != http.StatusConflict {
		t.Fatalf("stale group delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete,
		fmt.Sprintf("%s/v1/addressbook/groups/studio?if_revision=%d", s.URL, renamed.Revision),
		"", reply.Token); status != http.StatusNoContent {
		t.Fatalf("matching group delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio",
		fmt.Sprintf(`{"name":"Revive","if_revision":%d}`, renamed.Revision), reply.Token); status != http.StatusConflict {
		t.Fatalf("tombstoned group rename: %d %s", status, body)
	}
	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/studio", `{"name":"Revive"}`, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("legacy group revive: %d %s", status, body)
	}
	var revived groupReply
	if err := json.Unmarshal(body, &revived); err != nil {
		t.Fatalf("revived group reply: %v", err)
	}
	if revived.Revision != 4 || revived.Deleted {
		t.Fatalf("revived group = %+v", revived)
	}
}

func TestAddressBookIsPerUser(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/studio", `{"name":"Studio"}`, alice.Token); status != http.StatusOK {
		t.Fatalf("alice add peer: %d %s", status, body)
	}
	status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook", "", bob.Token)
	if status != http.StatusOK || !strings.Contains(string(body), `"peers":[]`) {
		t.Fatalf("bob sees alice's book: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/peers/studio", "", bob.Token); status != http.StatusNotFound {
		t.Fatalf("bob deleted alice's peer: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook", "", alice.Token)
	if status != http.StatusOK || !strings.Contains(string(body), `"id":"studio"`) {
		t.Fatalf("alice lost her peer: %d %s", status, body)
	}
}

type grantReply struct {
	Username    string `json:"username"`
	Permission  string `json:"permission"`
	GroupID     string `json:"group_id"`
	CreatedAtMS int64  `json:"created_at_ms"`
}

type grantsReply struct {
	Outgoing []grantReply `json:"outgoing"`
	Incoming []grantReply `json:"incoming"`
}

type sharedScopeReply struct {
	GroupID    string `json:"group_id"`
	Permission string `json:"permission"`
}

type sharedBookReply struct {
	Owner      string             `json:"owner"`
	Permission string             `json:"permission"`
	Revision   int64              `json:"revision"`
	Peers      []peerReply        `json:"peers"`
	Groups     []groupReply       `json:"groups"`
	Scopes     []sharedScopeReply `json:"scopes"`
}

type sharedBooksReply struct {
	Books []sharedBookReply `json:"books"`
}

func TestSharedAddressBookGroupScopeAPI(t *testing.T) {
	s := testutil.Start(t)
	for _, username := range []string{"alice", "bob", "carol"} {
		if _, err := s.Accounts.CreateUser(username, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", username, err)
		}
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	for _, group := range []string{"lab", "work"} {
		if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/groups/"+group,
			`{"name":"`+group+`"}`, alice.Token); status != http.StatusOK {
			t.Fatalf("create group %s: %d %s", group, status, body)
		}
	}
	for _, peer := range []struct{ id, name, group string }{
		{"devbox", "开发机", "lab"},
		{"office", "办公机", "work"},
		{"loose", "散装", ""},
	} {
		body := fmt.Sprintf(`{"name":%q,"group_id":%q}`, peer.name, peer.group)
		if status, reply := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/"+peer.id, body, alice.Token); status != http.StatusOK {
			t.Fatalf("seed %s: %d %s", peer.id, status, reply)
		}
	}

	for _, tc := range []struct {
		name string
		body string
		want int
	}{
		{"invalid group id", `{"username":"bob","group_id":"bad id"}`, http.StatusBadRequest},
		{"unknown group", `{"username":"bob","group_id":"ghost"}`, http.StatusNotFound},
	} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/grants", tc.body, alice.Token); status != tc.want {
			t.Fatalf("%s: %d %s, want %d", tc.name, status, body, tc.want)
		}
	}
	status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/grants",
		`{"username":"bob","permission":"write","group_id":"lab"}`, alice.Token)
	var grant grantReply
	if err := json.Unmarshal(body, &grant); err != nil || status != http.StatusOK ||
		grant.Username != "bob" || grant.GroupID != "lab" || grant.Permission != "write" {
		t.Fatalf("scoped share: %d %s (%v)", status, body, err)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/shared", "", bob.Token)
	var shared sharedBooksReply
	if err := json.Unmarshal(body, &shared); err != nil || status != http.StatusOK ||
		len(shared.Books) != 1 || shared.Books[0].Permission != "write" ||
		len(shared.Books[0].Peers) != 1 || shared.Books[0].Peers[0].ID != "devbox" ||
		len(shared.Books[0].Groups) != 1 || shared.Books[0].Groups[0].ID != "lab" ||
		len(shared.Books[0].Scopes) != 1 || shared.Books[0].Scopes[0].GroupID != "lab" {
		t.Fatalf("bob scoped books: %d %s (%v)", status, body, err)
	}

	sharedPeerURL := s.URL + "/v1/addressbook/shared/alice/peers/"
	for _, tc := range []struct {
		name   string
		method string
		url    string
		body   string
		want   int
	}{
		{"write outside the group", http.MethodPatch, sharedPeerURL + "office", `{"name":"越权"}`, http.StatusForbidden},
		{"add outside the group", http.MethodPost, s.URL + "/v1/addressbook/shared/alice/peers",
			`{"id":"escaped","name":"越权","group_id":"work"}`, http.StatusForbidden},
		{"add with invalid group", http.MethodPost, s.URL + "/v1/addressbook/shared/alice/peers",
			`{"id":"escaped","name":"坏组","group_id":"bad id"}`, http.StatusBadRequest},
		{"add with unknown group", http.MethodPost, s.URL + "/v1/addressbook/shared/alice/peers",
			`{"id":"escaped","name":"坏组","group_id":"ghost"}`, http.StatusNotFound},
		{"add into the granted group", http.MethodPost, s.URL + "/v1/addressbook/shared/alice/peers",
			`{"id":"newbox","name":"新增","group_id":"lab"}`, http.StatusOK},
		{"write inside the group", http.MethodPatch, sharedPeerURL + "devbox", `{"name":"共享改名"}`, http.StatusOK},
	} {
		if status, body := request(t, tc.method, tc.url, tc.body, bob.Token); status != tc.want {
			t.Fatalf("%s: %d %s, want %d", tc.name, status, body, tc.want)
		}
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/grants", "", alice.Token)
	var grants grantsReply
	if err := json.Unmarshal(body, &grants); err != nil || status != http.StatusOK ||
		len(grants.Outgoing) != 1 || grants.Outgoing[0].GroupID != "lab" {
		t.Fatalf("alice scoped grants: %d %s (%v)", status, body, err)
	}

	if status, body := request(t, http.MethodDelete,
		s.URL+"/v1/addressbook/grants/bob?group_id=bad%20id", "", alice.Token); status != http.StatusBadRequest {
		t.Fatalf("revoke with invalid group: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete,
		s.URL+"/v1/addressbook/grants/bob?group_id=lab", "", alice.Token); status != http.StatusNoContent {
		t.Fatalf("revoke scoped grant: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete,
		s.URL+"/v1/addressbook/grants/bob?group_id=lab", "", alice.Token); status != http.StatusNotFound {
		t.Fatalf("repeated scoped revoke: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/shared", "", bob.Token)
	if status != http.StatusOK || !strings.Contains(string(body), `"books":[]`) {
		t.Fatalf("books after scoped revoke: %d %s", status, body)
	}
}

func TestSharedAddressBookAPI(t *testing.T) {
	s := testutil.Start(t)
	for _, username := range []string{"alice", "bob", "carol"} {
		if _, err := s.Accounts.CreateUser(username, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", username, err)
		}
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	carol := login(t, s.URL, "carol", "correct horse")
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/studio",
		`{"name":"工作室","tags":["lab"]}`, alice.Token); status != http.StatusOK {
		t.Fatalf("seed alice peer: %d %s", status, body)
	}

	status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook/shared", "", bob.Token)
	if status != http.StatusOK || !strings.Contains(string(body), `"books":[]`) {
		t.Fatalf("initial shared books: %d %s", status, body)
	}
	for _, tc := range []struct {
		name string
		body string
		want int
	}{
		{"unknown grantee", `{"username":"nobody"}`, http.StatusNotFound},
		{"self share", `{"username":"alice"}`, http.StatusBadRequest},
		{"empty username", `{"username":"  "}`, http.StatusBadRequest},
		{"invalid permission", `{"username":"bob","permission":"owner"}`, http.StatusBadRequest},
		{"malformed body", `{"username":`, http.StatusBadRequest},
	} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/grants", tc.body, alice.Token); status != tc.want {
			t.Fatalf("%s: %d %s, want %d", tc.name, status, body, tc.want)
		}
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/addressbook/grants", `{"username":"Bob"}`, alice.Token)
	if status != http.StatusOK {
		t.Fatalf("share with bob: %d %s", status, body)
	}
	var grant grantReply
	if err := json.Unmarshal(body, &grant); err != nil || grant.Username != "bob" ||
		grant.Permission != "read" || grant.CreatedAtMS == 0 {
		t.Fatalf("share reply = %+v, %v", grant, err)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/addressbook/grants",
		`{"username":"bob","permission":"write"}`, alice.Token)
	if err := json.Unmarshal(body, &grant); err != nil || status != http.StatusOK ||
		grant.Permission != "write" {
		t.Fatalf("upgrade share: %d %s (%v)", status, body, err)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/grants", "", bob.Token)
	var grants grantsReply
	if err := json.Unmarshal(body, &grants); err != nil || status != http.StatusOK {
		t.Fatalf("bob grants: %d %s (%v)", status, body, err)
	}
	if len(grants.Outgoing) != 0 || len(grants.Incoming) != 1 ||
		grants.Incoming[0].Username != "alice" || grants.Incoming[0].Permission != "write" {
		t.Fatalf("bob grants = %+v", grants)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook/grants", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("grants without token: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/shared", "", bob.Token)
	var shared sharedBooksReply
	if err := json.Unmarshal(body, &shared); err != nil || status != http.StatusOK {
		t.Fatalf("bob shared books: %d %s (%v)", status, body, err)
	}
	if len(shared.Books) != 1 || shared.Books[0].Owner != "alice" ||
		shared.Books[0].Permission != "write" ||
		shared.Books[0].Revision == 0 || len(shared.Books[0].Peers) != 1 {
		t.Fatalf("bob shared books = %+v", shared)
	}
	if peer := shared.Books[0].Peers[0]; peer.ID != "studio" || peer.Name != "工作室" ||
		len(peer.Tags) != 1 || peer.Tags[0] != "lab" {
		t.Fatalf("shared peer = %+v", peer)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook", "", bob.Token); status != http.StatusOK ||
		!strings.Contains(string(body), `"peers":[]`) {
		t.Fatalf("shared book leaked into bob's book: %d %s", status, body)
	}

	if _, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/grants",
		`{"username":"bob","permission":"read"}`, alice.Token); !strings.Contains(string(body), `"permission":"read"`) {
		t.Fatalf("downgrade share: %s", body)
	}
	sharedPeerURL := s.URL + "/v1/addressbook/shared/alice/peers/studio"
	for _, tc := range []struct {
		name   string
		method string
		url    string
		body   string
	}{
		{"read-only update", http.MethodPatch, sharedPeerURL, `{"name":"篡改"}`},
		{"read-only add", http.MethodPost, s.URL + "/v1/addressbook/shared/alice/peers", `{"id":"newbox","name":"只读新增"}`},
		{"read-only delete", http.MethodDelete, sharedPeerURL, ""},
		{"read-only grants", http.MethodGet, s.URL + "/v1/addressbook/shared/alice/grants", ""},
		{"read-only grant", http.MethodPost, s.URL + "/v1/addressbook/shared/alice/grants", `{"username":"carol"}`},
		{"read-only revoke", http.MethodDelete, s.URL + "/v1/addressbook/shared/alice/grants/carol", ""},
	} {
		if status, body := request(t, tc.method, tc.url, tc.body, bob.Token); status != http.StatusForbidden {
			t.Fatalf("%s: %d %s, want 403", tc.name, status, body)
		}
	}

	if _, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/grants",
		`{"username":"bob","permission":"write"}`, alice.Token); !strings.Contains(string(body), `"permission":"write"`) {
		t.Fatalf("restore write share: %s", body)
	}
	status, body = request(t, http.MethodPatch, sharedPeerURL, `{"name":"共享编辑","tags":["vip"]}`, bob.Token)
	var edited peerReply
	if err := json.Unmarshal(body, &edited); err != nil || status != http.StatusOK ||
		edited.Name != "共享编辑" || len(edited.Tags) != 1 || edited.Tags[0] != "vip" {
		t.Fatalf("shared peer update: %d %s (%v)", status, body, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook", "", alice.Token)
	var ownerBook addressBookReply
	if err := json.Unmarshal(body, &ownerBook); err != nil || status != http.StatusOK ||
		len(ownerBook.Peers) != 1 || ownerBook.Peers[0].Name != "共享编辑" {
		t.Fatalf("owner book after shared update: %d %s (%v)", status, body, err)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook", "", bob.Token); status != http.StatusOK ||
		!strings.Contains(string(body), `"peers":[]`) {
		t.Fatalf("shared write leaked into bob's book: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, sharedPeerURL,
		`{"name":"过期","if_revision":1}`, bob.Token); status != http.StatusConflict ||
		!strings.Contains(string(body), "changed elsewhere") {
		t.Fatalf("stale shared update: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, sharedPeerURL,
		`{"name":"过期","if_revision":0}`, bob.Token); status != http.StatusBadRequest {
		t.Fatalf("non-positive shared if_revision: %d %s", status, body)
	}
	status, body = request(t, http.MethodPatch, sharedPeerURL,
		fmt.Sprintf(`{"name":"共享编辑二","if_revision":%d}`, edited.Revision), bob.Token)
	if err := json.Unmarshal(body, &edited); err != nil || status != http.StatusOK ||
		edited.Name != "共享编辑二" {
		t.Fatalf("shared update with matching revision: %d %s (%v)", status, body, err)
	}
	if status, body := request(t, http.MethodDelete, sharedPeerURL+"?if_revision=1", "", bob.Token); status != http.StatusConflict {
		t.Fatalf("stale shared delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete,
		fmt.Sprintf("%s?if_revision=%d", sharedPeerURL, edited.Revision), "", bob.Token); status != http.StatusNoContent {
		t.Fatalf("shared peer delete with matching revision: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, sharedPeerURL, "", bob.Token); status != http.StatusNotFound {
		t.Fatalf("repeated shared delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook", "", alice.Token); status != http.StatusOK ||
		!strings.Contains(string(body), `"peers":[]`) {
		t.Fatalf("owner book after shared delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/studio",
		`{"name":"工作室","tags":["lab"]}`, alice.Token); status != http.StatusOK {
		t.Fatalf("restore alice peer: %d %s", status, body)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/addressbook/shared/alice/peers",
		`{"id":"addedbox","name":"共享新增","tags":["added","added"]}`, bob.Token)
	var added peerReply
	if err := json.Unmarshal(body, &added); err != nil || status != http.StatusOK ||
		added.ID != "addedbox" || added.Name != "共享新增" ||
		len(added.Tags) != 1 || added.Tags[0] != "added" {
		t.Fatalf("shared peer add: %d %s (%v)", status, body, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook", "", alice.Token)
	if err := json.Unmarshal(body, &ownerBook); err != nil || status != http.StatusOK ||
		len(ownerBook.Peers) != 2 {
		t.Fatalf("owner book after shared add: %d %s (%v)", status, body, err)
	}
	foundAdded := false
	for _, peer := range ownerBook.Peers {
		if peer.ID == "addedbox" && peer.Name == "共享新增" &&
			len(peer.Tags) == 1 && peer.Tags[0] == "added" {
			foundAdded = true
		}
	}
	if !foundAdded {
		t.Fatalf("owner book is missing the shared add: %+v", ownerBook.Peers)
	}
	for _, tc := range []struct {
		name string
		body string
		want int
	}{
		{"invalid shared peer id", `{"id":"bad id","name":"坏 ID"}`, http.StatusBadRequest},
		{"malformed shared peer", `{"id":`, http.StatusBadRequest},
	} {
		if status, body := request(t, http.MethodPost,
			s.URL+"/v1/addressbook/shared/alice/peers", tc.body, bob.Token); status != tc.want {
			t.Fatalf("%s: %d %s, want %d", tc.name, status, body, tc.want)
		}
	}
	if status, body := request(t, http.MethodPost,
		s.URL+"/v1/addressbook/shared/nobody/peers",
		`{"id":"newbox"}`, bob.Token); status != http.StatusNotFound {
		t.Fatalf("add to unknown shared owner: %d %s", status, body)
	}

	if _, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/grants",
		`{"username":"bob","permission":"control"}`, alice.Token); !strings.Contains(string(body), `"permission":"control"`) {
		t.Fatalf("grant control: %s", body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/shared/alice/grants", "", bob.Token)
	if err := json.Unmarshal(body, &grants); err != nil || status != http.StatusOK ||
		len(grants.Outgoing) != 1 || grants.Outgoing[0].Username != "bob" ||
		grants.Outgoing[0].Permission != "control" {
		t.Fatalf("shared grants for control: %d %s (%v)", status, body, err)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/addressbook/shared/alice/grants",
		`{"username":"carol","permission":"write"}`, bob.Token)
	if err := json.Unmarshal(body, &grant); err != nil || status != http.StatusOK ||
		grant.Username != "carol" || grant.Permission != "write" {
		t.Fatalf("control grant: %d %s (%v)", status, body, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/shared", "", carol.Token)
	if err := json.Unmarshal(body, &shared); err != nil || status != http.StatusOK ||
		len(shared.Books) != 1 || shared.Books[0].Owner != "alice" ||
		shared.Books[0].Permission != "write" {
		t.Fatalf("carol shared books: %d %s (%v)", status, body, err)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook/shared/alice/grants", "", carol.Token); status != http.StatusForbidden {
		t.Fatalf("write-only grant list: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/shared/alice/grants/carol", "", bob.Token); status != http.StatusNoContent {
		t.Fatalf("control revoke: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/shared/alice/grants/carol", "", bob.Token); status != http.StatusNotFound {
		t.Fatalf("repeated control revoke: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook/shared/nobody/grants", "", bob.Token); status != http.StatusNotFound {
		t.Fatalf("unknown shared owner: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook/shared/alice/grants", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("shared grants without token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/grants/alice", "", bob.Token); status != http.StatusNotFound {
		t.Fatalf("grantee revoked a grant: %d %s", status, body)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/grants/BOB", "", alice.Token); status != http.StatusNoContent {
		t.Fatalf("revoke share: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/addressbook/grants/bob", "", alice.Token); status != http.StatusNotFound {
		t.Fatalf("repeated revoke: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, sharedPeerURL, `{"name":"迟到写入"}`, bob.Token); status != http.StatusNotFound {
		t.Fatalf("write after revoke: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/addressbook/shared", "", bob.Token)
	if status != http.StatusOK || !strings.Contains(string(body), `"books":[]`) {
		t.Fatalf("shared books after revoke: %d %s", status, body)
	}

	// Address-book edits can carry the revision they are based on. A matching
	// base applies; reusing it afterwards conflicts instead of overwriting.
	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/occbox",
		`{"name":"OCC box"}`, alice.Token)
	if status != http.StatusOK {
		t.Fatalf("create occ peer: %d %s", status, body)
	}
	var occ peerReply
	if err := json.Unmarshal(body, &occ); err != nil || occ.Revision == 0 {
		t.Fatalf("occ peer reply: %+v, %v", occ, err)
	}
	revision := strconv.FormatInt(occ.Revision, 10)
	status, body = request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/occbox",
		`{"name":"OCC box 2","if_revision":`+revision+`}`, alice.Token)
	if status != http.StatusOK {
		t.Fatalf("matching if_revision update: %d %s", status, body)
	}
	var occNext peerReply
	if err := json.Unmarshal(body, &occNext); err != nil || occNext.Revision != occ.Revision+1 || occNext.Name != "OCC box 2" {
		t.Fatalf("matching if_revision reply: %+v, %v", occNext, err)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/occbox",
		`{"tags":["lab"],"if_revision":`+revision+`}`, alice.Token); status != http.StatusConflict ||
		!strings.Contains(string(body), "changed elsewhere") {
		t.Fatalf("stale if_revision update: %d %s", status, body)
	}
	nextRevision := strconv.FormatInt(occNext.Revision, 10)
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/occbox",
		`{"group_id":"","if_revision":`+nextRevision+`}`, alice.Token); status != http.StatusOK {
		t.Fatalf("matching if_revision group move: %d %s", status, body)
	}
	for _, invalid := range []string{"0", "-2"} {
		if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/occbox",
			`{"name":"x","if_revision":`+invalid+`}`, alice.Token); status != http.StatusBadRequest {
			t.Fatalf("if_revision=%s: %d %s", invalid, status, body)
		}
	}
}

func TestAccountRoutesNeedPersistentState(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{NoAccounts: true})
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse"}`, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("login without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook", "", "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("address book without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("sessions without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/sessions/revoke-others", "", "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("revoke others without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/account/sessions/abc",
		`{"device_name":"Laptop"}`, "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("rename session without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"a","new_password":"new correct horse"}`, "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("password change without accounts: %d %s", status, body)
	}
	for _, path := range []string{"/v1/account/2fa/start", "/v1/account/2fa/enable", "/v1/account/2fa/disable", "/v1/account/2fa/recovery"} {
		if status, body := request(t, http.MethodPost, s.URL+path, `{"code":"000000"}`, "token"); status != http.StatusServiceUnavailable {
			t.Fatalf("%s without accounts: %d %s", path, status, body)
		}
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/server", "", ""); status != http.StatusOK ||
		!strings.Contains(string(body), `"accounts":false`) {
		t.Fatalf("server info accounts flag: %d %s", status, body)
	}
}

type accountSessionReply struct {
	ID          string `json:"id"`
	DeviceID    string `json:"device_id"`
	DeviceName  string `json:"device_name"`
	CreatedAtMS int64  `json:"created_at_ms"`
	ExpiresAtMS int64  `json:"expires_at_ms"`
	Current     bool   `json:"current"`
}

type accountSessionsReply struct {
	Sessions []accountSessionReply `json:"sessions"`
}

func TestAccountSessionsAndRevoke(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	first := login(t, s.URL, "alice", "correct horse")

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse","device_id":"device-b","device_name":"Office PC"}`, "")
	if status != http.StatusOK {
		t.Fatalf("device login: %d %s", status, data)
	}
	var second loginReply
	if err := json.Unmarshal(data, &second); err != nil {
		t.Fatalf("device login reply: %v", err)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", second.Token)
	if status != http.StatusOK {
		t.Fatalf("sessions: %d %s", status, data)
	}
	var list accountSessionsReply
	if err := json.Unmarshal(data, &list); err != nil {
		t.Fatalf("sessions reply: %v", err)
	}
	if len(list.Sessions) != 2 {
		t.Fatalf("sessions = %+v, want 2", list.Sessions)
	}
	var current, other accountSessionReply
	for _, session := range list.Sessions {
		if session.ID == "" || session.ExpiresAtMS <= session.CreatedAtMS {
			t.Fatalf("malformed session %+v", session)
		}
		if session.Current {
			current = session
		} else {
			other = session
		}
	}
	if current.DeviceID != "device-b" || current.DeviceName != "Office PC" {
		t.Fatalf("current session = %+v", current)
	}
	if other.DeviceID != "" || other.DeviceName != "" {
		t.Fatalf("token-only session = %+v", other)
	}

	status, data = request(t, http.MethodDelete, s.URL+"/v1/account/sessions/"+other.ID, "", second.Token)
	if status != http.StatusNoContent {
		t.Fatalf("revoke other session: %d %s", status, data)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", first.Token); status != http.StatusUnauthorized {
		t.Fatalf("revoked token still works: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", second.Token); status != http.StatusOK {
		t.Fatalf("current session was revoked: %d %s", status, body)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", second.Token)
	if err := json.Unmarshal(data, &list); err != nil || status != http.StatusOK {
		t.Fatalf("sessions after revoke: %d %s (%v)", status, data, err)
	}
	if len(list.Sessions) != 1 || !list.Sessions[0].Current {
		t.Fatalf("sessions after revoke = %+v", list.Sessions)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/account/sessions/"+other.ID, "", second.Token); status != http.StatusNotFound {
		t.Fatalf("repeated revoke: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/account/sessions/unknown", "", second.Token); status != http.StatusNotFound {
		t.Fatalf("unknown session revoke: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("sessions without token: %d %s", status, body)
	}
}

func TestAccountRevokeOtherSessions(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	main := login(t, s.URL, "alice", "correct horse")
	second := login(t, s.URL, "alice", "correct horse")
	third := login(t, s.URL, "alice", "correct horse")

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/sessions/revoke-others", "", main.Token)
	if status != http.StatusOK || !strings.Contains(string(data), `"revoked":2`) {
		t.Fatalf("revoke other sessions: %d %s", status, data)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", main.Token); status != http.StatusOK {
		t.Fatalf("current session was revoked: %d %s", status, body)
	}
	for name, token := range map[string]string{"second": second.Token, "third": third.Token} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", token); status != http.StatusUnauthorized {
			t.Fatalf("%s session survived: %d %s", name, status, body)
		}
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", main.Token)
	if status != http.StatusOK {
		t.Fatalf("sessions: %d %s", status, data)
	}
	var list accountSessionsReply
	if err := json.Unmarshal(data, &list); err != nil || len(list.Sessions) != 1 || !list.Sessions[0].Current {
		t.Fatalf("sessions after revoke-others = %+v, %v", list.Sessions, err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/sessions/revoke-others", "", main.Token)
	if status != http.StatusOK || !strings.Contains(string(data), `"revoked":0`) {
		t.Fatalf("repeated revoke other sessions: %d %s", status, data)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/sessions/revoke-others", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("revoke others without token: %d %s", status, body)
	}
	events, err := s.Accounts.AuditEvents(50)
	if err != nil {
		t.Fatalf("list audit: %v", err)
	}
	var sawTwo, sawZero bool
	for _, event := range events {
		if event.Action != account.AuditSessionRevoke || !event.Success {
			continue
		}
		switch event.Detail {
		case "revoked 2 other sessions":
			sawTwo = true
		case "revoked 0 other sessions":
			sawZero = true
		}
	}
	if !sawTwo || !sawZero {
		t.Fatalf("revoke-others audit missing: two=%v zero=%v events=%+v", sawTwo, sawZero, events)
	}
}

func TestAccountRenameSession(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	status, data := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse","device_id":"device-b","device_name":"Office PC"}`, "")
	if status != http.StatusOK {
		t.Fatalf("device login: %d %s", status, data)
	}
	var second loginReply
	if err := json.Unmarshal(data, &second); err != nil {
		t.Fatalf("device login reply: %v", err)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", alice.Token)
	if status != http.StatusOK {
		t.Fatalf("sessions: %d %s", status, data)
	}
	var list accountSessionsReply
	if err := json.Unmarshal(data, &list); err != nil || len(list.Sessions) != 2 {
		t.Fatalf("sessions = %s, %v", data, err)
	}
	var target accountSessionReply
	for _, session := range list.Sessions {
		if session.Current {
			target = session
		}
	}
	if target.ID == "" {
		t.Fatalf("current session missing from %+v", list.Sessions)
	}

	status, body := request(t, http.MethodPatch, s.URL+"/v1/account/sessions/"+target.ID,
		`{"device_name":"  书房 PC  "}`, second.Token)
	if status != http.StatusNoContent {
		t.Fatalf("rename session: %d %s", status, body)
	}
	status, body = request(t, http.MethodPatch, s.URL+"/v1/account/sessions/"+target.ID,
		`{"device_name":"  "}`, second.Token)
	if status != http.StatusBadRequest {
		t.Fatalf("empty name: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/account/sessions/unknown",
		`{"device_name":"Laptop"}`, second.Token); status != http.StatusNotFound {
		t.Fatalf("unknown session rename: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/account/sessions/"+target.ID,
		`{"device_name":"Hijacked"}`, bob.Token); status != http.StatusNotFound {
		t.Fatalf("cross-user rename: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/account/sessions/"+target.ID,
		`{"device_name":"Laptop"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("rename without token: %d %s", status, body)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", alice.Token)
	if status != http.StatusOK {
		t.Fatalf("sessions after rename: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &list); err != nil {
		t.Fatalf("sessions after rename reply: %v", err)
	}
	if len(list.Sessions) != 2 {
		t.Fatalf("sessions after rename = %+v", list.Sessions)
	}
	for _, session := range list.Sessions {
		if session.ID == target.ID && session.DeviceName != "书房 PC" {
			t.Fatalf("renamed session = %+v", session)
		}
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditSessionRename, 50)
	if err != nil {
		t.Fatalf("list rename audit: %v", err)
	}
	var sawSuccess, sawBadName, sawUnknown bool
	for _, event := range events {
		switch {
		case event.Success && event.Detail == "session="+target.ID:
			sawSuccess = true
		case !event.Success && event.Detail == "invalid device name":
			sawBadName = true
		case !event.Success && event.Detail == "unknown session":
			sawUnknown = true
		}
	}
	if !sawSuccess || !sawBadName || !sawUnknown {
		t.Fatalf("rename audit missing: success=%v bad=%v unknown=%v events=%+v",
			sawSuccess, sawBadName, sawUnknown, events)
	}
}

func TestAccountChangePassword(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	first := login(t, s.URL, "alice", "correct horse")
	status, data := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse","device_name":"Laptop"}`, "")
	if status != http.StatusOK {
		t.Fatalf("second login: %d %s", status, data)
	}
	var second loginReply
	if err := json.Unmarshal(data, &second); err != nil {
		t.Fatalf("second login reply: %v", err)
	}

	status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"wrong horse","new_password":"new correct horse"}`, first.Token)
	if status != http.StatusForbidden {
		t.Fatalf("wrong current password: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"correct horse","new_password":"short"}`, first.Token); status != http.StatusBadRequest {
		t.Fatalf("weak new password: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"correct horse","new_password":"new correct horse"}`, first.Token); status != http.StatusNoContent {
		t.Fatalf("change password: %d %s", status, body)
	}

	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", first.Token); status != http.StatusOK {
		t.Fatalf("current session was revoked: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", second.Token); status != http.StatusUnauthorized {
		t.Fatalf("other session survived: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("old password still logs in: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"new correct horse"}`, ""); status != http.StatusOK {
		t.Fatalf("new password login: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"new correct horse","new_password":"another password"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("password change without token: %d %s", status, body)
	}
}

type accountInfoReply struct {
	User struct {
		ID       string `json:"id"`
		Username string `json:"username"`
	} `json:"user"`
	TwoFactor     bool `json:"two_factor"`
	RecoveryCodes int  `json:"recovery_codes"`
}

type twoFactorStartReply struct {
	Secret     string `json:"secret"`
	OTPAuthURI string `json:"otpauth_uri"`
}

func TestAccountTwoFactorLogin(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	s.Accounts.SetClock(func() time.Time { return now })
	first := login(t, s.URL, "alice", "correct horse")

	readInfo := func(token string) accountInfoReply {
		t.Helper()
		status, data := request(t, http.MethodGet, s.URL+"/v1/account", "", token)
		if status != http.StatusOK {
			t.Fatalf("account info: %d %s", status, data)
		}
		var reply accountInfoReply
		if err := json.Unmarshal(data, &reply); err != nil {
			t.Fatalf("account info reply: %v", err)
		}
		return reply
	}
	if info := readInfo(first.Token); info.TwoFactor {
		t.Fatal("new account reports two-factor enabled")
	}

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/2fa/start", "", first.Token)
	if status != http.StatusOK {
		t.Fatalf("start two-factor: %d %s", status, data)
	}
	var start twoFactorStartReply
	if err := json.Unmarshal(data, &start); err != nil {
		t.Fatalf("start reply: %v", err)
	}
	if start.Secret == "" || !strings.Contains(start.OTPAuthURI, "secret="+start.Secret) ||
		!strings.Contains(start.OTPAuthURI, "issuer=chast") {
		t.Fatalf("start reply = %+v", start)
	}
	code, err := account.TOTPCode(start.Secret, now)
	if err != nil {
		t.Fatalf("generate code: %v", err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/enable",
		`{"code":"000000"}`, first.Token); status != http.StatusForbidden {
		t.Fatalf("enable with wrong code: %d %s", status, body)
	}
	enableBody, err := json.Marshal(map[string]string{"code": code})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/enable",
		string(enableBody), first.Token); status != http.StatusNoContent {
		t.Fatalf("enable two-factor: %d %s", status, body)
	}
	if info := readInfo(first.Token); !info.TwoFactor || info.User.Username != "alice" {
		t.Fatalf("account info after enable = %+v", info)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/start", "", first.Token); status != http.StatusConflict {
		t.Fatalf("restart enabled two-factor: %d %s", status, body)
	}

	loginBody := func(code string) string {
		payload := map[string]string{"username": "alice", "password": "correct horse"}
		if code != "" {
			payload["totp_code"] = code
		}
		body, err := json.Marshal(payload)
		if err != nil {
			t.Fatal(err)
		}
		return string(body)
	}
	status, body, headers := requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", loginBody(""))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "required" {
		t.Fatalf("login without code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", loginBody("000000"))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("login with wrong code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}
	// The enrollment code belongs to a step that was already spent.
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", loginBody(code))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("replayed enrollment code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}

	now = now.Add(30 * time.Second)
	nextCode, err := account.TOTPCode(start.Secret, now)
	if err != nil {
		t.Fatalf("next code: %v", err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", loginBody(nextCode), "")
	if status != http.StatusOK {
		t.Fatalf("login with two-factor: %d %s", status, data)
	}
	var second loginReply
	if err := json.Unmarshal(data, &second); err != nil || second.Token == "" {
		t.Fatalf("two-factor login reply: %s (%v)", data, err)
	}
	if _, _, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", loginBody(nextCode)); headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("replayed login code second-factor=%q", headers.Get("X-Chast-2FA"))
	}
	if status, body = request(t, http.MethodPost, s.URL+"/v1/account/login", loginBody(""), ""); status != http.StatusUnauthorized {
		t.Fatalf("missing code after a success: %d %s", status, body)
	}

	disableBody, err := json.Marshal(map[string]string{"password": "wrong horse", "code": nextCode})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/disable",
		string(disableBody), second.Token); status != http.StatusForbidden {
		t.Fatalf("disable with wrong password: %d %s", status, body)
	}
	disableBody, err = json.Marshal(map[string]string{"password": "correct horse", "code": "000000"})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/disable",
		string(disableBody), second.Token); status != http.StatusForbidden {
		t.Fatalf("disable with wrong code: %d %s", status, body)
	}
	disableBody, err = json.Marshal(map[string]string{"password": "correct horse", "code": nextCode})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/disable",
		string(disableBody), second.Token); status != http.StatusNoContent {
		t.Fatalf("disable two-factor: %d %s", status, body)
	}
	if info := readInfo(second.Token); info.TwoFactor {
		t.Fatal("two-factor still enabled after disable")
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/disable",
		string(disableBody), second.Token); status != http.StatusBadRequest {
		t.Fatalf("second disable: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", loginBody(""), ""); status != http.StatusOK {
		t.Fatalf("login after disable: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/start", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("start without token: %d %s", status, body)
	}
}

type recoveryCodesReply struct {
	Codes []string `json:"codes"`
}

func TestAccountRecoveryCodes(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	s.Accounts.SetClock(func() time.Time { return now })
	first := login(t, s.URL, "alice", "correct horse")

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/2fa/start", "", first.Token)
	if status != http.StatusOK {
		t.Fatalf("start two-factor: %d %s", status, data)
	}
	var start twoFactorStartReply
	if err := json.Unmarshal(data, &start); err != nil {
		t.Fatalf("start reply: %v", err)
	}
	code, err := account.TOTPCode(start.Secret, now)
	if err != nil {
		t.Fatal(err)
	}
	enableBody, err := json.Marshal(map[string]string{"code": code})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/enable",
		string(enableBody), first.Token); status != http.StatusNoContent {
		t.Fatalf("enable two-factor: %d %s", status, body)
	}

	readInfo := func(token string) accountInfoReply {
		t.Helper()
		status, data := request(t, http.MethodGet, s.URL+"/v1/account", "", token)
		if status != http.StatusOK {
			t.Fatalf("account info: %d %s", status, data)
		}
		var reply accountInfoReply
		if err := json.Unmarshal(data, &reply); err != nil {
			t.Fatalf("account info reply: %v", err)
		}
		return reply
	}
	if info := readInfo(first.Token); !info.TwoFactor || info.RecoveryCodes != 0 {
		t.Fatalf("account info before codes = %+v", info)
	}

	recoveryBody := func(password, totp string) string {
		body, err := json.Marshal(map[string]string{"password": password, "code": totp})
		if err != nil {
			t.Fatal(err)
		}
		return string(body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/recovery",
		recoveryBody("correct horse", code), ""); status != http.StatusUnauthorized {
		t.Fatalf("recovery codes without token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/recovery",
		recoveryBody("wrong horse", code), first.Token); status != http.StatusForbidden {
		t.Fatalf("recovery codes with wrong password: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/recovery",
		recoveryBody("correct horse", "000000"), first.Token); status != http.StatusForbidden {
		t.Fatalf("recovery codes with wrong code: %d %s", status, body)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/2fa/recovery",
		recoveryBody("correct horse", code), first.Token)
	if status != http.StatusOK {
		t.Fatalf("generate recovery codes: %d %s", status, data)
	}
	var issued recoveryCodesReply
	if err := json.Unmarshal(data, &issued); err != nil {
		t.Fatalf("recovery codes reply: %v", err)
	}
	if len(issued.Codes) != 10 {
		t.Fatalf("recovery codes = %d, want 10", len(issued.Codes))
	}
	for _, generated := range issued.Codes {
		if len(generated) != 19 || strings.Count(generated, "-") != 3 {
			t.Fatalf("recovery code %q is not grouped base32", generated)
		}
	}
	if info := readInfo(first.Token); info.RecoveryCodes != 10 {
		t.Fatalf("recovery code count = %d, want 10", info.RecoveryCodes)
	}

	recoveryLogin := func(recovery string) string {
		body, err := json.Marshal(map[string]string{
			"username":      "alice",
			"password":      "correct horse",
			"recovery_code": recovery,
		})
		if err != nil {
			t.Fatal(err)
		}
		return string(body)
	}
	typed := strings.ToLower(strings.ReplaceAll(issued.Codes[0], "-", " "))
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", recoveryLogin(typed), "")
	if status != http.StatusOK {
		t.Fatalf("login with recovery code: %d %s", status, data)
	}
	var recovered loginReply
	if err := json.Unmarshal(data, &recovered); err != nil || recovered.Token == "" {
		t.Fatalf("recovery login reply: %s (%v)", data, err)
	}
	if info := readInfo(recovered.Token); info.RecoveryCodes != 9 {
		t.Fatalf("recovery code count after use = %d, want 9", info.RecoveryCodes)
	}

	status, body, headers := requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", recoveryLogin(issued.Codes[0]))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("replayed recovery code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}
	if status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse"}`); status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "required" {
		t.Fatalf("login still needs a second factor: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}

	now = now.Add(30 * time.Second)
	nextCode, err := account.TOTPCode(start.Secret, now)
	if err != nil {
		t.Fatal(err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/2fa/recovery",
		recoveryBody("correct horse", nextCode), recovered.Token)
	if status != http.StatusOK {
		t.Fatalf("regenerate recovery codes: %d %s", status, data)
	}
	var replacement recoveryCodesReply
	if err := json.Unmarshal(data, &replacement); err != nil || len(replacement.Codes) != 10 {
		t.Fatalf("replacement recovery codes = %s (%v)", data, err)
	}
	if info := readInfo(recovered.Token); info.RecoveryCodes != 10 {
		t.Fatalf("recovery code count after regenerate = %d, want 10", info.RecoveryCodes)
	}
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", recoveryLogin(issued.Codes[1]))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("old recovery code after regenerate: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditTwoFactorRecovery, 20)
	if err != nil {
		t.Fatalf("recovery audit: %v", err)
	}
	var successes, failures int
	for _, event := range events {
		if event.Username != "alice" {
			t.Fatalf("recovery audit username = %q", event.Username)
		}
		if event.Success {
			successes++
		} else {
			failures++
		}
	}
	if successes != 2 || failures != 2 {
		t.Fatalf("recovery audit successes=%d failures=%d, want 2 and 2", successes, failures)
	}
}

func requestWithHeaders(t *testing.T, method, url, body string) (int, []byte, http.Header) {
	t.Helper()
	req, err := http.NewRequest(method, url, strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
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

func TestAccountLoginAudit(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"wrong horse","device_name":"书房-PC"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("failed login: %d %s", status, body)
	}
	login(t, s.URL, "alice", "correct horse")
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"bob","password":"correct horse"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("unknown user: %d %s", status, body)
	}

	events, err := s.Accounts.AuditEvents(10)
	if err != nil {
		t.Fatalf("list audit: %v", err)
	}
	if len(events) != 3 {
		t.Fatalf("audit events = %d, want 3: %+v", len(events), events)
	}
	if events[0].Username != "bob" || events[0].Success ||
		events[0].Detail != "invalid credentials" {
		t.Fatalf("unknown user audit = %+v", events[0])
	}
	if events[1].Username != "alice" || !events[1].Success ||
		events[1].Detail != "password" || events[1].Action != account.AuditLogin {
		t.Fatalf("successful login audit = %+v", events[1])
	}
	if events[2].Username != "alice" || events[2].Success ||
		events[2].Detail != "invalid credentials" {
		t.Fatalf("failed login audit = %+v", events[2])
	}
	for _, event := range events {
		if event.ClientIP != "127.0.0.1" {
			t.Fatalf("audit client ip = %q, want loopback", event.ClientIP)
		}
		if event.CreatedAt.IsZero() {
			t.Fatalf("audit event has no timestamp: %+v", event)
		}
	}
}

func TestAccountLoginLockout(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	limiter := authlimit.New(authlimit.Config{
		Window:      time.Minute,
		MaxFailures: 3,
		Lockout:     30 * time.Second,
		Now:         func() time.Time { return now },
	})
	s := testutil.StartWith(t, testutil.Options{LoginLimiter: limiter})
	for _, name := range []string{"alice", "bob"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", name, err)
		}
	}

	wrong := `{"username":"alice","password":"wrong horse"}`
	for i := 0; i < 2; i++ {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", wrong, ""); status != http.StatusUnauthorized {
			t.Fatalf("failure %d: %d %s", i+1, status, body)
		}
	}
	status, body, headers := requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", wrong)
	if status != http.StatusTooManyRequests || headers.Get("Retry-After") != "30" {
		t.Fatalf("locking attempt: %d %s retry=%q", status, body, headers.Get("Retry-After"))
	}
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse"}`)
	if status != http.StatusTooManyRequests || headers.Get("Retry-After") == "" {
		t.Fatalf("correct password while locked: %d %s retry=%q", status, body, headers.Get("Retry-After"))
	}
	// The lockout is scoped to the username, not the client IP.
	status, body = request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"bob","password":"correct horse"}`, "")
	if status != http.StatusOK {
		t.Fatalf("other user while one key is locked: %d %s", status, body)
	}

	now = now.Add(31 * time.Second)
	status, body = request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse"}`, "")
	if status != http.StatusOK {
		t.Fatalf("login after lockout: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", wrong, ""); status != http.StatusUnauthorized {
		t.Fatalf("failure after a successful login: %d %s", status, body)
	}
	events, err := s.Accounts.AuditEvents(50)
	if err != nil {
		t.Fatalf("list audit: %v", err)
	}
	var sawTriggered, sawLocked bool
	for _, event := range events {
		if event.Username != "alice" {
			continue
		}
		switch event.Detail {
		case "invalid credentials; locked out":
			sawTriggered = true
		case "locked out":
			sawLocked = true
		}
	}
	if !sawTriggered || !sawLocked {
		t.Fatalf("lockout audit missing: triggering=%v locked=%v events=%+v",
			sawTriggered, sawLocked, events)
	}
}

func TestPersistentLoginLockout(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{LoginPersistent: true})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	wrong := `{"username":"alice","password":"wrong horse"}`
	right := `{"username":"alice","password":"correct horse"}`

	for i := 0; i < 2; i++ {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", wrong, ""); status != http.StatusUnauthorized {
			t.Fatalf("failure %d: %d %s", i+1, status, body)
		}
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", right, ""); status != http.StatusOK {
		t.Fatalf("login after two failures: %d %s", status, body)
	}
	// The success above must clear the persisted counter.
	for i := 0; i < 2; i++ {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", wrong, ""); status != http.StatusUnauthorized {
			t.Fatalf("failure after success %d: %d %s", i+1, status, body)
		}
	}
	status, body, headers := requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login", wrong)
	if status != http.StatusTooManyRequests || headers.Get("Retry-After") == "" {
		t.Fatalf("locking attempt: %d %s retry=%q", status, body, headers.Get("Retry-After"))
	}

	// A limiter built over the same database sees the lockout, like a server
	// restart. The API's own limiter is the persistent one from StartWith.
	restarted := authlimit.NewPersistent(authlimit.Config{
		Window:      time.Minute,
		MaxFailures: 3,
		Lockout:     time.Minute,
	}, s.Accounts, nil)
	if retry, ok := restarted.Allow("alice|127.0.0.1"); ok || retry <= 0 {
		t.Fatalf("restarted limiter allow = (%v, %v), want locked", retry, ok)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", right, ""); status != http.StatusTooManyRequests {
		t.Fatalf("correct password while persisted lockout: %d %s", status, body)
	}
}

func TestAccountMutationAudit(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	s.Accounts.SetClock(func() time.Time { return now })
	main := login(t, s.URL, "alice", "correct horse")
	second := login(t, s.URL, "alice", "correct horse")

	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"wrong horse","new_password":"new correct horse"}`, main.Token); status != http.StatusForbidden {
		t.Fatalf("wrong current password: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"correct horse","new_password":"short"}`, main.Token); status != http.StatusBadRequest {
		t.Fatalf("weak new password: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/password",
		`{"current_password":"correct horse","new_password":"new correct horse"}`, main.Token); status != http.StatusNoContent {
		t.Fatalf("change password: %d %s", status, body)
	}

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/2fa/start", "", main.Token)
	if status != http.StatusOK {
		t.Fatalf("start two-factor: %d %s", status, data)
	}
	var start twoFactorStartReply
	if err := json.Unmarshal(data, &start); err != nil {
		t.Fatalf("start reply: %v", err)
	}
	code, err := account.TOTPCode(start.Secret, now)
	if err != nil {
		t.Fatalf("generate code: %v", err)
	}
	enableBody, err := json.Marshal(map[string]string{"code": code})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/enable",
		`{"code":"000000"}`, main.Token); status != http.StatusForbidden {
		t.Fatalf("enable with wrong code: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/enable",
		string(enableBody), main.Token); status != http.StatusNoContent {
		t.Fatalf("enable two-factor: %d %s", status, body)
	}

	now = now.Add(30 * time.Second)
	code, err = account.TOTPCode(start.Secret, now)
	if err != nil {
		t.Fatalf("generate disable code: %v", err)
	}
	disableBody, err := json.Marshal(map[string]string{"password": "wrong horse", "code": code})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/disable",
		string(disableBody), main.Token); status != http.StatusForbidden {
		t.Fatalf("disable with wrong password: %d %s", status, body)
	}
	disableBody, err = json.Marshal(map[string]string{"password": "new correct horse", "code": code})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/disable",
		string(disableBody), main.Token); status != http.StatusNoContent {
		t.Fatalf("disable two-factor: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/2fa/disable",
		string(disableBody), main.Token); status != http.StatusBadRequest {
		t.Fatalf("second disable: %d %s", status, body)
	}

	third := login(t, s.URL, "alice", "new correct horse")
	status, data = request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", main.Token)
	if status != http.StatusOK {
		t.Fatalf("sessions: %d %s", status, data)
	}
	var list accountSessionsReply
	if err := json.Unmarshal(data, &list); err != nil {
		t.Fatalf("sessions reply: %v", err)
	}
	var target string
	for _, session := range list.Sessions {
		if !session.Current {
			target = session.ID
		}
	}
	if target == "" {
		t.Fatalf("no other session to revoke: %+v", list.Sessions)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/account/sessions/"+target, "", main.Token); status != http.StatusNoContent {
		t.Fatalf("revoke session: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/account/sessions/unknown", "", main.Token); status != http.StatusNotFound {
		t.Fatalf("unknown session: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", third.Token); status != http.StatusUnauthorized {
		t.Fatalf("revoked session still accepted: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/logout", "", second.Token); status != http.StatusUnauthorized {
		t.Fatalf("logout with revoked token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/logout", "", main.Token); status != http.StatusNoContent {
		t.Fatalf("logout: %d %s", status, body)
	}

	events, err := s.Accounts.AuditEvents(100)
	if err != nil {
		t.Fatalf("list audit: %v", err)
	}
	wantEvent := func(action, detail string, success bool) {
		t.Helper()
		for _, event := range events {
			if event.Action == action && event.Detail == detail && event.Success == success {
				return
			}
		}
		t.Fatalf("audit missing action=%s detail=%q success=%v in %+v", action, detail, success, events)
	}
	wantEvent(account.AuditPasswordChange, "current password rejected", false)
	wantEvent(account.AuditPasswordChange, "weak password", false)
	wantEvent(account.AuditPasswordChange, "other sessions revoked", true)
	wantEvent(account.AuditTwoFactorEnable, "invalid code", false)
	wantEvent(account.AuditTwoFactorEnable, "", true)
	wantEvent(account.AuditTwoFactorDisable, "invalid credentials or code", false)
	wantEvent(account.AuditTwoFactorDisable, "", true)
	wantEvent(account.AuditTwoFactorDisable, "not enabled", false)
	wantEvent(account.AuditSessionRevoke, "unknown session", false)
	wantEvent(account.AuditSessionRevoke, "session="+target, true)
	wantEvent(account.AuditLogout, "invalid token", false)
	wantEvent(account.AuditLogout, "", true)
}

func TestAuditLogEndpoint(t *testing.T) {
	type auditEventReply struct {
		ID          int64  `json:"id"`
		CreatedAtMS int64  `json:"created_at_ms"`
		Username    string `json:"username"`
		Action      string `json:"action"`
		Detail      string `json:"detail"`
		ClientIP    string `json:"client_ip"`
		Success     bool   `json:"success"`
	}
	type auditReply struct {
		Events []auditEventReply `json:"events"`
	}
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"wrong horse"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("failed login: %d %s", status, body)
	}
	token := login(t, s.URL, "alice", "correct horse").Token

	if status, body := request(t, http.MethodGet, s.URL+"/v1/audit", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("audit without operator key: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/audit", "", token); status != http.StatusUnauthorized {
		t.Fatalf("audit with viewer token: %d %s", status, body)
	}
	status, data := request(t, http.MethodGet, s.URL+"/v1/audit", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("audit: %d %s", status, data)
	}
	var reply auditReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("audit reply: %v", err)
	}
	if len(reply.Events) != 2 {
		t.Fatalf("audit events = %d, want 2: %s", len(reply.Events), data)
	}
	newest := reply.Events[0]
	if newest.ID == 0 || newest.CreatedAtMS == 0 || newest.Username != "alice" ||
		newest.Action != account.AuditLogin || !newest.Success || newest.ClientIP != "127.0.0.1" {
		t.Fatalf("newest audit event = %+v", newest)
	}
	if reply.Events[0].ID < reply.Events[1].ID {
		t.Fatalf("audit is not newest-first: %+v", reply.Events)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/audit?action=login&limit=1", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("filtered audit: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("filtered audit reply: %v", err)
	}
	if len(reply.Events) != 1 || !reply.Events[0].Success {
		t.Fatalf("filtered audit = %+v", reply.Events)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/audit?action=logout", "", testutil.ServerKey)
	if status != http.StatusOK || !strings.Contains(string(data), `"events":[]`) {
		t.Fatalf("empty action audit: %d %s", status, data)
	}
	for _, query := range []string{"?limit=0", "?limit=201", "?limit=-1", "?limit=abc"} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/audit"+query, "", testutil.ServerKey); status != http.StatusBadRequest {
			t.Fatalf("%s: %d %s", query, status, body)
		}
	}

	noStore := testutil.StartWith(t, testutil.Options{NoAccounts: true})
	if status, body := request(t, http.MethodGet, noStore.URL+"/v1/audit", "", testutil.ServerKey); status != http.StatusServiceUnavailable {
		t.Fatalf("audit without accounts: %d %s", status, body)
	}
}

type connectionReply struct {
	ID          int64  `json:"id"`
	Username    string `json:"username"`
	PeerID      string `json:"peer_id"`
	Direction   string `json:"direction"`
	HostID      string `json:"host_id"`
	AuditRef    string `json:"audit_ref"`
	Source      string `json:"source"`
	ClientIP    string `json:"client_ip"`
	StartedAtMS int64  `json:"started_at_ms"`
	EndedAtMS   int64  `json:"ended_at_ms"`
	DurationMS  int64  `json:"duration_ms"`
	Success     bool   `json:"success"`
	Detail      string `json:"detail"`
}

type connectionsReply struct {
	Connections []connectionReply `json:"connections"`
}

func TestConnectionAudit(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	now := time.Now().UnixMilli()
	start := now - 60000

	status, body := request(t, http.MethodPost, s.URL+"/v1/account/connections",
		`{"peer_id":"devbox"}`, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("report without token: %d %s", status, body)
	}
	for _, tc := range []struct {
		name string
		body string
	}{
		{"malformed", `{"peer_id":`},
		{"empty peer", `{"peer_id":"  "}`},
		{"unknown direction", `{"peer_id":"devbox","direction":"host"}`},
		{"ends before start", `{"peer_id":"devbox","started_at_ms":2000,"ended_at_ms":1000}`},
	} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/connections", tc.body, alice.Token); status != http.StatusBadRequest {
			t.Fatalf("%s: %d %s", tc.name, status, body)
		}
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/account/connections",
		`{"peer_id":" devbox ","started_at_ms":`+strconv.FormatInt(start, 10)+
			`,"ended_at_ms":`+strconv.FormatInt(now, 10)+`,"detail":"ended normally"}`,
		alice.Token)
	if status != http.StatusCreated {
		t.Fatalf("report connection: %d %s", status, body)
	}
	var created connectionReply
	if err := json.Unmarshal(body, &created); err != nil {
		t.Fatalf("report reply: %v", err)
	}
	if created.ID == 0 || created.Username != "alice" || created.PeerID != "devbox" ||
		created.Direction != "viewer" || !created.Success ||
		created.DurationMS != 60000 || created.ClientIP != "127.0.0.1" {
		t.Fatalf("created connection = %+v", created)
	}
	if created.StartedAtMS != start || created.EndedAtMS != now {
		t.Fatalf("connection timestamps = %d..%d, want %d..%d",
			created.StartedAtMS, created.EndedAtMS, start, now)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/account/connections",
		`{"peer_id":"laptop","started_at_ms":`+strconv.FormatInt(start, 10)+
			`,"ended_at_ms":`+strconv.FormatInt(now, 10)+`,"success":false,"detail":"host refused"}`,
		bob.Token)
	if status != http.StatusCreated {
		t.Fatalf("report bob connection: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/account/connections", "", alice.Token)
	if status != http.StatusOK {
		t.Fatalf("list own connections: %d %s", status, body)
	}
	var own connectionsReply
	if err := json.Unmarshal(body, &own); err != nil {
		t.Fatalf("own connections reply: %v", err)
	}
	if len(own.Connections) != 1 || own.Connections[0].PeerID != "devbox" ||
		own.Connections[0].Detail != "ended normally" {
		t.Fatalf("own connections = %+v", own.Connections)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/connections", "", "")
	if status != http.StatusUnauthorized {
		t.Fatalf("operator view without key: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/connections", "", alice.Token)
	if status != http.StatusUnauthorized {
		t.Fatalf("account token accepted by operator view: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/connections", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("operator connections: %d %s", status, body)
	}
	var all connectionsReply
	if err := json.Unmarshal(body, &all); err != nil {
		t.Fatalf("operator connections reply: %v", err)
	}
	if len(all.Connections) != 2 || all.Connections[0].Username != "bob" ||
		all.Connections[0].Success || all.Connections[0].Detail != "host refused" ||
		all.Connections[1].Username != "alice" {
		t.Fatalf("operator connections = %+v", all.Connections)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/connections?username=ALICE", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("filtered connections: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &all); err != nil || len(all.Connections) != 1 ||
		all.Connections[0].Username != "alice" {
		t.Fatalf("filtered connections = %+v, %v", all.Connections, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/connections?username=nobody", "", testutil.ServerKey)
	if status != http.StatusOK || !strings.Contains(string(body), `"connections":[]`) {
		t.Fatalf("unknown user connections: %d %s", status, body)
	}
	for _, query := range []string{"?limit=0", "?limit=201", "?limit=nope"} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/audit/connections"+query, "", testutil.ServerKey); status != http.StatusBadRequest {
			t.Fatalf("operator %s: %d %s", query, status, body)
		}
		if status, body := request(t, http.MethodGet, s.URL+"/v1/account/connections"+query, "", alice.Token); status != http.StatusBadRequest {
			t.Fatalf("account %s: %d %s", query, status, body)
		}
	}

	noStore := testutil.StartWith(t, testutil.Options{NoAccounts: true})
	if status, body := request(t, http.MethodPost, noStore.URL+"/v1/account/connections",
		`{"peer_id":"devbox"}`, "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("report without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, noStore.URL+"/v1/account/connections", "", "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("own connections without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, noStore.URL+"/v1/audit/connections", "", testutil.ServerKey); status != http.StatusServiceUnavailable {
		t.Fatalf("operator connections without accounts: %d %s", status, body)
	}
}

type fileTransferReply struct {
	ID           int64  `json:"id"`
	Username     string `json:"username"`
	PeerID       string `json:"peer_id"`
	Direction    string `json:"direction"`
	HostID       string `json:"host_id"`
	AuditRef     string `json:"audit_ref"`
	Source       string `json:"source"`
	Name         string `json:"name"`
	Size         int64  `json:"size"`
	Files        int    `json:"files"`
	ClientIP     string `json:"client_ip"`
	OccurredAtMS int64  `json:"occurred_at_ms"`
	Success      bool   `json:"success"`
	Detail       string `json:"detail"`
}

type fileTransfersReply struct {
	Transfers []fileTransferReply `json:"transfers"`
}

func TestFileTransferAudit(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	now := time.Now().UnixMilli()
	occurred := now - 30000

	status, body := request(t, http.MethodPost, s.URL+"/v1/account/file-transfers",
		`{"peer_id":"devbox","direction":"upload"}`, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("report without token: %d %s", status, body)
	}
	for _, tc := range []struct {
		name string
		body string
	}{
		{"malformed", `{"peer_id":`},
		{"empty peer", `{"peer_id":"  "}`},
		{"unknown direction", `{"peer_id":"devbox","direction":"viewer"}`},
		{"negative size", `{"peer_id":"devbox","direction":"upload","size":-1}`},
		{"too many files", `{"peer_id":"devbox","direction":"upload","files":2000}`},
	} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/account/file-transfers", tc.body, alice.Token); status != http.StatusBadRequest {
			t.Fatalf("%s: %d %s", tc.name, status, body)
		}
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/account/file-transfers",
		`{"peer_id":" devbox ","direction":"upload","name":" \u0000report.pdf ",`+
			`"size":4096,"occurred_at_ms":`+strconv.FormatInt(occurred, 10)+`}`,
		alice.Token)
	if status != http.StatusCreated {
		t.Fatalf("report upload: %d %s", status, body)
	}
	var created fileTransferReply
	if err := json.Unmarshal(body, &created); err != nil {
		t.Fatalf("report reply: %v", err)
	}
	if created.ID == 0 || created.Username != "alice" || created.PeerID != "devbox" ||
		created.Direction != "upload" || created.Name != "report.pdf" ||
		created.Size != 4096 || created.Files != 1 || !created.Success ||
		created.ClientIP != "127.0.0.1" || created.OccurredAtMS != occurred {
		t.Fatalf("created transfer = %+v", created)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/account/file-transfers",
		`{"peer_id":"laptop","direction":"download","name":"photos.zip","size":900,`+
			`"files":3,"success":false,"detail":"disk full"}`,
		bob.Token)
	if status != http.StatusCreated {
		t.Fatalf("report bob transfer: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/account/file-transfers", "", alice.Token)
	if status != http.StatusOK {
		t.Fatalf("list own transfers: %d %s", status, body)
	}
	var own fileTransfersReply
	if err := json.Unmarshal(body, &own); err != nil {
		t.Fatalf("own transfers reply: %v", err)
	}
	if len(own.Transfers) != 1 || own.Transfers[0].PeerID != "devbox" ||
		own.Transfers[0].Name != "report.pdf" {
		t.Fatalf("own transfers = %+v", own.Transfers)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers", "", "")
	if status != http.StatusUnauthorized {
		t.Fatalf("operator view without key: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers", "", alice.Token)
	if status != http.StatusUnauthorized {
		t.Fatalf("account token accepted by operator view: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("operator transfers: %d %s", status, body)
	}
	var all fileTransfersReply
	if err := json.Unmarshal(body, &all); err != nil {
		t.Fatalf("operator transfers reply: %v", err)
	}
	if len(all.Transfers) != 2 || all.Transfers[0].Username != "bob" ||
		all.Transfers[0].Success || all.Transfers[0].Files != 3 ||
		all.Transfers[0].Detail != "disk full" || all.Transfers[1].Username != "alice" {
		t.Fatalf("operator transfers = %+v", all.Transfers)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers?username=ALICE", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("filtered transfers: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &all); err != nil || len(all.Transfers) != 1 ||
		all.Transfers[0].Username != "alice" {
		t.Fatalf("filtered transfers = %+v, %v", all.Transfers, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers?username=nobody", "", testutil.ServerKey)
	if status != http.StatusOK || !strings.Contains(string(body), `"transfers":[]`) {
		t.Fatalf("unknown user transfers: %d %s", status, body)
	}
	for _, query := range []string{"?limit=0", "?limit=201", "?limit=nope"} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/audit/file-transfers"+query, "", testutil.ServerKey); status != http.StatusBadRequest {
			t.Fatalf("operator %s: %d %s", query, status, body)
		}
		if status, body := request(t, http.MethodGet, s.URL+"/v1/account/file-transfers"+query, "", alice.Token); status != http.StatusBadRequest {
			t.Fatalf("account %s: %d %s", query, status, body)
		}
	}

	noStore := testutil.StartWith(t, testutil.Options{NoAccounts: true})
	if status, body := request(t, http.MethodPost, noStore.URL+"/v1/account/file-transfers",
		`{"peer_id":"devbox","direction":"upload"}`, "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("report without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, noStore.URL+"/v1/account/file-transfers", "", "token"); status != http.StatusServiceUnavailable {
		t.Fatalf("own transfers without accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, noStore.URL+"/v1/audit/file-transfers", "", testutil.ServerKey); status != http.StatusServiceUnavailable {
		t.Fatalf("operator transfers without accounts: %d %s", status, body)
	}
}

func TestLoginRecordsGeoIPLocation(t *testing.T) {
	table := filepath.Join(t.TempDir(), "geoip.txt")
	if err := os.WriteFile(table, []byte("127.0.0.0/8\tLocal lab\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	locator, err := geoip.Load(table)
	if err != nil {
		t.Fatal(err)
	}
	s := testutil.StartWith(t, testutil.Options{GeoIP: locator})
	user, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse"}`, "")
	if status != http.StatusOK {
		t.Fatalf("login: %d %s", status, body)
	}
	var reply loginReply
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatal(err)
	}

	sessions, err := s.Accounts.Sessions(user.ID, reply.Token)
	if err != nil || len(sessions) != 1 {
		t.Fatalf("sessions = %+v, %v", sessions, err)
	}
	if sessions[0].LoginIP != "127.0.0.1" || sessions[0].Location != "Local lab" {
		t.Fatalf("session origin = %+v", sessions[0])
	}
	events, err := s.Accounts.AuditEvents(10)
	if err != nil || len(events) == 0 {
		t.Fatalf("audit events = %+v, %v", events, err)
	}
	if events[0].Action != account.AuditLogin || !events[0].Success ||
		events[0].ClientIP != "127.0.0.1" || events[0].Location != "Local lab" {
		t.Fatalf("login audit = %+v", events[0])
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/account/sessions", "", reply.Token)
	if status != http.StatusOK {
		t.Fatalf("sessions api: %d %s", status, body)
	}
	var view struct {
		Sessions []struct {
			LoginIP  string `json:"login_ip"`
			Location string `json:"location"`
		} `json:"sessions"`
	}
	if err := json.Unmarshal(body, &view); err != nil || len(view.Sessions) != 1 {
		t.Fatalf("sessions view = %+v, %v", view, err)
	}
	if view.Sessions[0].LoginIP != "127.0.0.1" || view.Sessions[0].Location != "Local lab" {
		t.Fatalf("sessions view origin = %+v", view.Sessions[0])
	}
}
