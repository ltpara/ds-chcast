package api_test

import (
	"encoding/json"
	"net/http"
	"net/url"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type hostEventView struct {
	Seq               uint64 `json:"seq"`
	Kind              string `json:"kind"`
	Mode              string `json:"mode"`
	PermissionCeiling uint32 `json:"permission_ceiling"`
	PolicyVersion     uint32 `json:"policy_version"`
	UpdatedAtMS       int64  `json:"updated_at_ms"`
	Managed           *bool  `json:"managed"`
	Kid               string `json:"kid"`
	Reason            string `json:"reason"`
	AtMS              int64  `json:"at_ms"`
}

type hostEventsReply struct {
	Events []hostEventView `json:"events"`
	Cursor uint64          `json:"cursor"`
}

func hostEventsURL(t *testing.T, base, hostID, secret string, after uint64, timestamp int64,
	waitMS int) string {
	t.Helper()
	priv, _ := hostKey(secret)
	query := url.Values{}
	query.Set("after", strconv.FormatUint(after, 10))
	query.Set("timestamp", strconv.FormatInt(timestamp, 10))
	query.Set("wait_ms", strconv.Itoa(waitMS))
	query.Set("signature",
		identity.Sign(priv, identity.HostEventsSubscribeMessage(hostID, after, timestamp)))
	return base + "/v1/hosts/" + url.PathEscape(hostID) + "/events?" + query.Encode()
}

func pollHostEvents(t *testing.T, srv, hostID, secret string, after uint64) hostEventsReply {
	t.Helper()
	status, body := request(t, http.MethodGet,
		hostEventsURL(t, srv, hostID, secret, after, time.Now().Unix(), 0), "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("host events = %d %s", status, body)
	}
	var reply hostEventsReply
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("host events reply: %v (%s)", err, body)
	}
	return reply
}

// TestHostEventStream covers the authenticated long poll and the two pushed
// statements: a policy edit reaches the host as a versioned statement and a
// retired issuer key reaches it as a revoke event. Signatures bind the cursor
// to the device key, so a captured request cannot read another host's stream.
func TestHostEventStream(t *testing.T) {
	issuer := newTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}

	if status, _ := request(t, http.MethodGet,
		hostEventsURL(t, s.URL, "devbox", "devbox secret", 0, now, 0), "", ""); status != http.StatusUnauthorized {
		t.Fatalf("anonymous poll = %d", status)
	}
	if status, _ := request(t, http.MethodGet,
		hostEventsURL(t, s.URL, "devbox", "another secret", 0, now, 0), "", testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("foreign signature = %d", status)
	}
	if status, _ := request(t, http.MethodGet,
		hostEventsURL(t, s.URL, "ghost", "ghost secret", 0, now, 0), "", testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("unknown host poll = %d", status)
	}
	// A timestamp outside the replay window is refused before any wait.
	stale := now - 10*60
	if status, _ := request(t, http.MethodGet,
		hostEventsURL(t, s.URL, "devbox", "devbox secret", 0, stale, 0), "", testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("stale poll = %d", status)
	}

	first := pollHostEvents(t, s.URL, "devbox", "devbox secret", 0)
	if len(first.Events) != 0 {
		t.Fatalf("first poll = %+v", first)
	}

	if status, body := request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		`{"mode":"open"}`, admin); status != http.StatusOK {
		t.Fatalf("set policy: %d %s", status, body)
	}
	second := pollHostEvents(t, s.URL, "devbox", "devbox secret", first.Cursor)
	if len(second.Events) != 1 {
		t.Fatalf("policy poll = %+v", second)
	}
	policy := second.Events[0]
	if policy.Kind != "policy" || policy.Managed == nil || !*policy.Managed ||
		policy.PolicyVersion != 1 || policy.Mode != "open" {
		t.Fatalf("policy event = %+v", policy)
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/access-kids",
		`{"kid":"kid-2026-11","reason":"rotation"}`, admin); status != http.StatusOK {
		t.Fatalf("revoke kid: %d %s", status, body)
	}
	status, body := request(t, http.MethodGet, s.URL+"/v1/access-kids", "", admin)
	if status != http.StatusOK {
		t.Fatalf("list kids: %d %s", status, body)
	}
	var kids struct {
		Kids []struct {
			Kid    string `json:"kid"`
			Reason string `json:"reason"`
		} `json:"kids"`
	}
	if err := json.Unmarshal(body, &kids); err != nil {
		t.Fatalf("kids reply: %v", err)
	}
	if len(kids.Kids) != 1 || kids.Kids[0].Kid != "kid-2026-11" || kids.Kids[0].Reason != "rotation" {
		t.Fatalf("kids = %+v", kids)
	}

	third := pollHostEvents(t, s.URL, "devbox", "devbox secret", second.Cursor)
	if len(third.Events) != 1 {
		t.Fatalf("revoke poll = %+v", third)
	}
	revoke := third.Events[0]
	if revoke.Kind != "revoke" || revoke.Kid != "kid-2026-11" || revoke.Reason != "rotation" {
		t.Fatalf("revoke event = %+v", revoke)
	}

	// The retired signer can no longer issue tickets, so the push only
	// shortens the window for sessions that are already running.
	clientKey := make([]byte, 32)
	status, body = request(t, http.MethodPost, s.URL+"/v1/access-tickets",
		ticketRequestBody(t, "devbox", clientKey, 1), admin)
	if status != http.StatusServiceUnavailable {
		t.Fatalf("revoked issuer ticket = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/access-kids",
		`{"kid":"bad kid"}`, admin); status != http.StatusBadRequest {
		t.Fatalf("invalid kid = %d %s", status, body)
	}
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/access-kids", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("anonymous kid list = %d", status)
	}
}
