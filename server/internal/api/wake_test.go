package api_test

import (
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type wakeReply struct {
	Queued       bool   `json:"queued"`
	Job          string `json:"job"`
	ExpiresInSec int64  `json:"expires_in_sec"`
}

type wakeJobReply struct {
	Job          string `json:"job"`
	Target       string `json:"target"`
	MAC          string `json:"mac"`
	ExpiresInSec int64  `json:"expires_in_sec"`
}

func postWake(t *testing.T, base, id, mac, key string) (int, http.Header, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, base+"/v1/hosts/"+id+"/wake",
		strings.NewReader(`{"mac":"`+mac+`"}`))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	if key != "" {
		req.Header.Set("Authorization", "Bearer "+key)
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
	return resp.StatusCode, resp.Header, data
}

func registerAgent(t *testing.T, base, id string, timestamp int64) []wakeJobReply {
	t.Helper()
	status, body := request(t, http.MethodPost, base+"/v1/hosts",
		registerBody(t, id, "secret "+id, "FP-"+id, timestamp), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register %s: %d %s", id, status, body)
	}
	var reply struct {
		Wake []wakeJobReply `json:"wake"`
	}
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("register %s reply: %v", id, err)
	}
	return reply.Wake
}

func TestWakeQueuesForProxyAgent(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	registerAgent(t, s.URL, "targetbox", now)
	registerAgent(t, s.URL, "proxy", now+1)

	status, _, body := postWake(t, s.URL, "targetbox", "aa-bb-cc-dd-ee-ff", testutil.ServerKey)
	if status != http.StatusAccepted {
		t.Fatalf("wake: %d %s", status, body)
	}
	var queued wakeReply
	if err := json.Unmarshal(body, &queued); err != nil {
		t.Fatalf("wake reply: %v", err)
	}
	if !queued.Queued || queued.Job == "" || queued.ExpiresInSec != 60 {
		t.Fatalf("unexpected wake reply: %+v", queued)
	}

	jobs := registerAgent(t, s.URL, "proxy", now+2)
	if len(jobs) != 1 {
		t.Fatalf("proxy got %d jobs, want 1: %+v", len(jobs), jobs)
	}
	job := jobs[0]
	if job.Job != queued.Job || job.Target != "targetbox" ||
		job.MAC != "AA:BB:CC:DD:EE:FF" || job.ExpiresInSec != 60 {
		t.Fatalf("delivered job = %+v, want job %s", job, queued.Job)
	}

	if jobs := registerAgent(t, s.URL, "proxy", now+3); len(jobs) != 0 {
		t.Fatalf("proxy got a job twice: %+v", jobs)
	}
	if jobs := registerAgent(t, s.URL, "targetbox", now+4); len(jobs) != 0 {
		t.Fatalf("target agent relayed its own wake: %+v", jobs)
	}

	status, headers, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", testutil.ServerKey)
	if status != http.StatusTooManyRequests {
		t.Fatalf("cooldown wake: %d %s", status, body)
	}
	if retry, err := strconv.Atoi(headers.Get("Retry-After")); err != nil || retry < 1 || retry > 30 {
		t.Fatalf("Retry-After = %q, want 1..30 seconds", headers.Get("Retry-After"))
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditDeviceWake, 10)
	if err != nil {
		t.Fatalf("list wake audit: %v", err)
	}
	if len(events) != 2 || events[0].Success || !events[1].Success {
		t.Fatalf("wake audit = %+v, want one failure then one success", events)
	}
	if events[0].Username != "server-key" || !strings.Contains(events[0].Detail, "targetbox") {
		t.Fatalf("wake audit event = %+v", events[0])
	}
}

func TestWakeAuthorizationAndValidation(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if _, err := s.Accounts.CreateUserWithRole("root", "battery staple", "", "", account.RoleAdmin); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	// The first account becomes an admin, so root must exist before alice.
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	registerAgent(t, s.URL, "targetbox", now)
	registerAgent(t, s.URL, "otherbox", now+1)

	alice := login(t, s.URL, "alice", "correct horse")
	root := login(t, s.URL, "root", "battery staple")

	if status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", ""); status != http.StatusUnauthorized {
		t.Fatalf("anonymous wake: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", "not-a-token"); status != http.StatusUnauthorized {
		t.Fatalf("invalid token wake: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", alice.Token); status != http.StatusForbidden {
		t.Fatalf("wake outside address book: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/targetbox",
		`{"name":"Target"}`, alice.Token); status != http.StatusOK {
		t.Fatalf("add address book peer: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", alice.Token); status != http.StatusAccepted {
		t.Fatalf("address book wake: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", alice.Token); status != http.StatusTooManyRequests {
		t.Fatalf("address book cooldown wake: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "otherbox", "AA:BB:CC:DD:EE:FF", alice.Token); status != http.StatusForbidden {
		t.Fatalf("wake of unknown peer: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "otherbox", "AA:BB:CC:DD:EE:FF", root.Token); status != http.StatusAccepted {
		t.Fatalf("admin wake: %d %s", status, body)
	}

	if status, _, body := postWake(t, s.URL, "ghost", "AA:BB:CC:DD:EE:FF", testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("unknown host wake: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "bad%20id", "AA:BB:CC:DD:EE:FF", testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("invalid host id wake: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "nope", testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("invalid mac wake: %d %s", status, body)
	}
}

func TestWakeSharedAddressBookPermission(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if _, err := s.Accounts.CreateUserWithRole("root", "battery staple", "", "", account.RoleAdmin); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	for _, username := range []string{"alice", "bob"} {
		if _, err := s.Accounts.CreateUser(username, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", username, err)
		}
	}
	registerAgent(t, s.URL, "targetbox", now)
	registerAgent(t, s.URL, "otherbox", now+1)

	alice := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "correct horse")
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/addressbook/peers/targetbox",
		`{"name":"Target"}`, alice.Token); status != http.StatusOK {
		t.Fatalf("seed alice peer: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", bob.Token); status != http.StatusForbidden {
		t.Fatalf("wake before share: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/grants",
		`{"username":"bob"}`, alice.Token); status != http.StatusOK {
		t.Fatalf("share address book: %d %s", status, body)
	}

	status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", bob.Token)
	if status != http.StatusAccepted {
		t.Fatalf("wake through shared book: %d %s", status, body)
	}
	var queued wakeReply
	if err := json.Unmarshal(body, &queued); err != nil {
		t.Fatalf("wake reply: %v", err)
	}
	if status, _, body := postWake(t, s.URL, "otherbox", "AA:BB:CC:DD:EE:FF", bob.Token); status != http.StatusForbidden {
		t.Fatalf("wake outside shared book: %d %s", status, body)
	}
	jobs := registerAgent(t, s.URL, "otherbox", now+2)
	if len(jobs) != 1 || jobs[0].Job != queued.Job || jobs[0].Target != "targetbox" {
		t.Fatalf("otherbox jobs = %+v, want job %s for targetbox", jobs, queued.Job)
	}

	if status, body := request(t, http.MethodDelete,
		s.URL+"/v1/addressbook/grants/bob", "", alice.Token); status != http.StatusNoContent {
		t.Fatalf("revoke share: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", bob.Token); status != http.StatusForbidden {
		t.Fatalf("wake after revoke: %d %s", status, body)
	}
}
