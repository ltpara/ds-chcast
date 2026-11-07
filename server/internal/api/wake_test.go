package api_test

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type wakeReply struct {
	Queued       bool   `json:"queued"`
	Job          string `json:"job"`
	ExpiresInSec int64  `json:"expires_in_sec"`
}

type wakeJobReply struct {
	Job          string   `json:"job"`
	Target       string   `json:"target"`
	MAC          string   `json:"mac"`
	Broadcasts   []string `json:"broadcasts"`
	ExpiresInSec int64    `json:"expires_in_sec"`
}

// wakeLink is the directed broadcast a test agent reports for its LAN. The
// server never derives a link from the limited broadcast, so tests use an
// explicit value.
const wakeLink = "192.168.50.255:9"

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

func registerWakeAgent(t *testing.T, base, id string, timestamp int64) []wakeJobReply {
	t.Helper()
	return registerWakeAgentLink(t, base, id, timestamp, false, []string{wakeLink})
}

// registerWakeAgentLink registers an agent with an explicit relay flag and
// link. The returned jobs are those the registration delivered.
func registerWakeAgentLink(t *testing.T, base, id string, timestamp int64, relay bool, targets []string) []wakeJobReply {
	t.Helper()
	status, body := request(t, http.MethodPost, base+"/v1/hosts",
		registerBodyWithLink(t, id, "secret "+id, "FP-"+id, timestamp, relay, targets), testutil.ServerKey)
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
	s := testutil.StartWith(t, testutil.Options{WakeRelays: map[string][]string{"targetbox": {"proxy"}}})
	now := time.Now().Unix()
	registerWakeAgent(t, s.URL, "targetbox", now)
	registerWakeAgentLink(t, s.URL, "proxy", now+1, true, []string{wakeLink})

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

	jobs := registerWakeAgentLink(t, s.URL, "proxy", now+2, true, []string{wakeLink})
	if len(jobs) != 1 {
		t.Fatalf("proxy got %d jobs, want 1: %+v", len(jobs), jobs)
	}
	job := jobs[0]
	if job.Job != queued.Job || job.Target != "targetbox" ||
		job.MAC != "AA:BB:CC:DD:EE:FF" || job.ExpiresInSec != 60 {
		t.Fatalf("delivered job = %+v, want job %s", job, queued.Job)
	}
	if len(job.Broadcasts) != 1 || job.Broadcasts[0] != wakeLink {
		t.Fatalf("delivered job broadcasts = %v, want [%s]", job.Broadcasts, wakeLink)
	}

	// The broker must repeat an unexpired job because the previous registration
	// response may have been lost before the agent received it.
	if jobs := registerWakeAgentLink(t, s.URL, "proxy", now+3, true, []string{wakeLink}); len(jobs) != 1 || jobs[0].Job != queued.Job {
		t.Fatalf("proxy retry jobs = %+v, want job %s", jobs, queued.Job)
	}
	if jobs := registerWakeAgent(t, s.URL, "targetbox", now+4); len(jobs) != 0 {
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
	s := testutil.StartWith(t, testutil.Options{WakeRelays: map[string][]string{"targetbox": {"proxy"}, "otherbox": {"proxy"}}})
	now := time.Now().Unix()
	if _, err := s.Accounts.CreateUserWithRole("root", "battery staple", "", "", account.RoleAdmin); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	// The first account becomes an admin, so root must exist before alice.
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	registerWakeAgent(t, s.URL, "targetbox", now)
	registerWakeAgent(t, s.URL, "otherbox", now+1)

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
	s := testutil.StartWith(t, testutil.Options{WakeRelays: map[string][]string{"targetbox": {"otherbox"}}})
	now := time.Now().Unix()
	if _, err := s.Accounts.CreateUserWithRole("root", "battery staple", "", "", account.RoleAdmin); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	for _, username := range []string{"alice", "bob"} {
		if _, err := s.Accounts.CreateUser(username, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", username, err)
		}
	}
	registerWakeAgent(t, s.URL, "targetbox", now)
	registerWakeAgentLink(t, s.URL, "otherbox", now+1, true, []string{wakeLink})

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
	jobs := registerWakeAgentLink(t, s.URL, "otherbox", now+2, true, []string{wakeLink})
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

func TestWakeDeliveryScopedToTargetLink(t *testing.T) {
	node := newStubNode(t, "203.0.113.10", "203.0.113.10:21116")
	s := testutil.StartWith(t, testutil.Options{RelayNodes: []string{node.url},
		WakeRelays: map[string][]string{"targetbox": {"peerrelay", "foreignrelay", "plainpeer"}}})
	now := time.Now().Unix()
	const foreignLink = "192.168.60.255:9"

	registerWakeAgentLink(t, s.URL, "targetbox", now, false, []string{wakeLink})
	registerWakeAgentLink(t, s.URL, "peerrelay", now+1, true, []string{wakeLink})
	registerWakeAgentLink(t, s.URL, "foreignrelay", now+2, true, []string{foreignLink})
	registerWakeAgentLink(t, s.URL, "plainpeer", now+3, false, []string{wakeLink})

	status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", testutil.ServerKey)
	if status != http.StatusAccepted {
		t.Fatalf("wake: %d %s", status, body)
	}
	var queued wakeReply
	if err := json.Unmarshal(body, &queued); err != nil {
		t.Fatalf("wake reply: %v", err)
	}

	if jobs := registerWakeAgentLink(t, s.URL, "foreignrelay", now+4, true, []string{foreignLink}); len(jobs) != 0 {
		t.Fatalf("relay on another link got a job: %+v", jobs)
	}
	if jobs := registerWakeAgentLink(t, s.URL, "plainpeer", now+5, false, []string{wakeLink}); len(jobs) != 0 {
		t.Fatalf("non-relay agent got a job: %+v", jobs)
	}
	if jobs := registerWakeAgentLink(t, s.URL, "peerrelay", now+6, true, []string{foreignLink}); len(jobs) != 0 {
		t.Fatalf("relay got a job after its link changed: %+v", jobs)
	}

	jobs := registerWakeAgentLink(t, s.URL, "peerrelay", now+7, true, []string{wakeLink})
	if len(jobs) != 1 || jobs[0].Job != queued.Job || jobs[0].Target != "targetbox" {
		t.Fatalf("peer relay jobs = %+v, want job %s for targetbox", jobs, queued.Job)
	}
	if len(jobs[0].Broadcasts) != 1 || jobs[0].Broadcasts[0] != wakeLink {
		t.Fatalf("peer relay job broadcasts = %v, want [%s]", jobs[0].Broadcasts, wakeLink)
	}
}

func TestWakeTargetWithoutLinkIsRejected(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "nolink", "secret nolink", "FP-nolink", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}

	status, _, body = postWake(t, s.URL, "nolink", "AA:BB:CC:DD:EE:FF", testutil.ServerKey)
	if status != http.StatusConflict {
		t.Fatalf("wake without a link: %d %s", status, body)
	}
	if !strings.Contains(string(body), "wake link") {
		t.Fatalf("wake error = %s, want a wake link message", body)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditDeviceWake, 10)
	if err != nil {
		t.Fatalf("list wake audit: %v", err)
	}
	if len(events) != 1 || events[0].Success || !strings.Contains(events[0].Detail, "no wake link") {
		t.Fatalf("wake audit = %+v, want one failed no-link event", events)
	}
}

func TestWakeTargetWithoutAuthorizedRelayIsRejected(t *testing.T) {
	s := testutil.Start(t)
	registerWakeAgent(t, s.URL, "targetbox", time.Now().Unix())
	status, _, body := postWake(t, s.URL, "targetbox", "AA:BB:CC:DD:EE:FF", testutil.ServerKey)
	if status != http.StatusConflict || !strings.Contains(string(body), "authorized wake relay") {
		t.Fatalf("wake without policy: %d %s", status, body)
	}
	events, err := s.Accounts.AuditEventsFor(account.AuditDeviceWake, 10)
	if err != nil || len(events) != 1 || events[0].Success || !strings.Contains(events[0].Detail, "no authorized wake relay") {
		t.Fatalf("wake audit = %+v, error = %v", events, err)
	}
}

func TestWakeSameSubnetDoesNotAuthorizeAnonymousRelay(t *testing.T) {
	node := newStubNode(t, "203.0.113.10", "203.0.113.10:21116")
	s := testutil.StartWith(t, testutil.Options{RelayNodes: []string{node.url}})
	srv := httptest.NewServer(api.New(api.Config{
		Registry: s.Registry, Relay: s.Relay, Identity: s.Identity,
		RelayPool: s.Pool,
		Accounts:  s.Accounts, PunchAddr: s.Punch, RelayHost: s.RelayHost,
		TTL: time.Minute, WakeRelays: map[string][]string{"private-target": {"trusted-relay"}},
	}))
	defer srv.Close()
	now := time.Now().Unix()
	register := func(id string, relay bool) []wakeJobReply {
		t.Helper()
		status, body := request(t, http.MethodPost, srv.URL+"/v1/hosts",
			registerBodyWithLink(t, id, "secret "+id, "FP-"+id, now, relay, []string{wakeLink}), "")
		if status != http.StatusOK {
			t.Fatalf("registration: %d %s", status, body)
		}
		var out struct {
			Wake []wakeJobReply `json:"wake"`
		}
		if err := json.Unmarshal(body, &out); err != nil {
			t.Fatal(err)
		}
		return out.Wake
	}
	register("private-target", false)
	register("trusted-relay", true)
	if _, err := s.Accounts.CreateUser("root", "correct horse"); err != nil {
		t.Fatal(err)
	}
	root := login(t, srv.URL, "root", "correct horse")
	status, _, body := postWake(t, srv.URL, "private-target", "AA:BB:CC:DD:EE:FF", root.Token)
	if status != http.StatusAccepted {
		t.Fatalf("wake: %d %s", status, body)
	}
	if jobs := register("unrelated-relay", true); len(jobs) != 0 {
		t.Fatalf("anonymous unrelated host received wake: %+v", jobs)
	}
	now++
	if jobs := register("trusted-relay", true); len(jobs) != 1 || jobs[0].Target != "private-target" {
		t.Fatalf("authorized host did not receive wake: %+v", jobs)
	}
}
