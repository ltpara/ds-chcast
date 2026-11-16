package api_test

import (
	"encoding/json"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/liveconn"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

const (
	apiLiveRuntime = "11111111111111111111111111111111"
	apiLiveConnA   = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	apiLiveConnB   = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
)

type liveSnapshotReply struct {
	RuntimeID   string                `json:"runtime_id"`
	Sequence    uint64                `json:"sequence"`
	Terminal    bool                  `json:"terminal"`
	Connections []liveconn.Connection `json:"connections"`
}

type liveConnectionReply struct {
	HostID                 string    `json:"host_id"`
	RuntimeID              string    `json:"runtime_id"`
	ConnectionID           string    `json:"connection_id"`
	State                  string    `json:"state"`
	StartedAt              time.Time `json:"started_at"`
	LastHeartbeatAt        time.Time `json:"last_heartbeat_at"`
	CommandID              string    `json:"command_id"`
	CommandStatus          string    `json:"command_status"`
	CommandDeadline        time.Time `json:"command_deadline"`
	ReceiptConfirmedAbsent bool      `json:"receipt_confirmed_absent"`
	ReceiptDetail          string    `json:"receipt_detail"`
}

type liveConnectionListReply struct {
	Connections []liveConnectionReply `json:"connections"`
}

type liveCommandsReply struct {
	Commands  []liveconn.Command `json:"commands"`
	Timestamp int64              `json:"timestamp"`
	Signature string             `json:"signature"`
}

func registerLiveHost(t *testing.T, base, id, secret string) {
	t.Helper()
	status, body := request(t, http.MethodPost, base+"/v1/hosts",
		registerBody(t, id, secret, "AB12", time.Now().Unix()), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register %s: %d %s", id, status, body)
	}
}

func liveSnapshot(sequence uint64, ids ...string) liveconn.Snapshot {
	snapshot := liveconn.Snapshot{
		RuntimeID:   apiLiveRuntime,
		Sequence:    sequence,
		Connections: make([]liveconn.Connection, 0, len(ids)),
	}
	for i, id := range ids {
		snapshot.Connections = append(snapshot.Connections, liveconn.Connection{
			ConnectionID: id,
			PeerAddress:  "192.0.2.20",
			Viewer:       "viewer",
			Mode:         "control",
			Route:        "direct",
			Permissions:  7,
			Controller:   i == 0,
			StartedAtMS:  time.Date(2026, 11, 16, 1, 0, 0, 0, time.UTC).UnixMilli(),
		})
	}
	return snapshot
}

func signedLiveSnapshot(t *testing.T, hostID, secret string, snapshot liveconn.Snapshot) string {
	t.Helper()
	priv, _ := hostKey(secret)
	normalized, err := snapshot.Normalize()
	if err != nil {
		t.Fatal(err)
	}
	digest, err := liveconn.Digest(normalized)
	if err != nil {
		t.Fatal(err)
	}
	timestamp := time.Now().Unix()
	return mustJSON(t, map[string]any{
		"snapshot":  normalized,
		"timestamp": timestamp,
		"signature": identity.Sign(priv, identity.HostLiveSnapshotMessage(
			hostID, normalized.RuntimeID, normalized.Sequence, normalized.Terminal,
			digest, timestamp,
		)),
	})
}

func signedLiveCommandsURL(t *testing.T, base, hostID, secret, runtimeID string, at time.Time) string {
	t.Helper()
	priv, _ := hostKey(secret)
	timestamp := at.Unix()
	query := url.Values{
		"timestamp": {strconv.FormatInt(timestamp, 10)},
		"signature": {identity.Sign(priv,
			identity.HostLiveCommandsFetchMessage(hostID, runtimeID, timestamp))},
	}
	return base + "/v1/hosts/" + url.PathEscape(hostID) +
		"/live-connections/" + url.PathEscape(runtimeID) + "/commands?" + query.Encode()
}

func signedLiveReceipt(t *testing.T, hostID, secret, runtimeID string,
	receipt liveconn.Receipt) string {
	t.Helper()
	priv, _ := hostKey(secret)
	timestamp := time.Now().Unix()
	return mustJSON(t, map[string]any{
		"receipt":   receipt,
		"timestamp": timestamp,
		"signature": identity.Sign(priv, identity.HostLiveReceiptMessage(
			hostID, runtimeID, receipt.ReceiptSequence, receipt.CommandID,
			receipt.Status, receipt.ConfirmedAbsent, receipt.Detail, timestamp,
		)),
	})
}

func decodeLiveConnection(t *testing.T, body []byte) liveConnectionReply {
	t.Helper()
	var reply liveConnectionReply
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("decode live connection: %v (%s)", err, body)
	}
	return reply
}

func TestLiveConnectionAdminAuthorization(t *testing.T) {
	s := testutil.Start(t)
	_, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateUserWithRole("operator", "operator pass", "", "",
		account.RoleOperator); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateUserWithRole("readonly", "readonly pass", "", "",
		account.RoleReadonly); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateUser("viewer", "viewer pass"); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	operator := login(t, s.URL, "operator", "operator pass").Token
	readonly := login(t, s.URL, "readonly", "readonly pass").Token
	viewer := login(t, s.URL, "viewer", "viewer pass").Token
	registerLiveHost(t, s.URL, "devbox", "devbox secret")
	if status, body := request(t, http.MethodPost,
		s.URL+"/v1/hosts/devbox/live-connections",
		signedLiveSnapshot(t, "devbox", "devbox secret", liveSnapshot(1, apiLiveConnA)),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("seed snapshot: %d %s", status, body)
	}

	for _, tc := range []struct {
		name   string
		token  string
		status int
	}{
		{"anonymous", "", http.StatusUnauthorized},
		{"viewer", viewer, http.StatusForbidden},
		{"readonly", readonly, http.StatusOK},
		{"operator", operator, http.StatusOK},
		{"admin", admin, http.StatusOK},
	} {
		if got, body := request(t, http.MethodGet, s.URL+"/v1/live-connections", "", tc.token); got != tc.status {
			t.Fatalf("list as %s: %d %s", tc.name, got, body)
		}
	}
	for _, tc := range []struct {
		name   string
		token  string
		status int
	}{
		{"anonymous", "", http.StatusUnauthorized},
		{"viewer", viewer, http.StatusForbidden},
		{"readonly", readonly, http.StatusOK},
		{"operator", operator, http.StatusOK},
		{"admin", admin, http.StatusOK},
	} {
		if got, body := request(t, http.MethodGet,
			s.URL+"/v1/live-connections/devbox/"+apiLiveConnA, "", tc.token); got != tc.status {
			t.Fatalf("detail as %s: %d %s", tc.name, got, body)
		}
	}
	for _, tc := range []struct {
		name   string
		token  string
		status int
	}{
		{"anonymous", "", http.StatusUnauthorized},
		{"viewer", viewer, http.StatusForbidden},
		{"operator", operator, http.StatusAccepted},
		{"admin", admin, http.StatusAccepted},
	} {
		key := "authorization-" + strings.ReplaceAll(tc.name, " ", "-")
		if tc.token == "" {
			key = "authorization-anonymous"
		}
		if tc.token == operator {
			key = "authorization-operator"
		}
		if tc.token == admin {
			key = "authorization-admin"
		}
		got, body := request(t, http.MethodPost,
			s.URL+"/v1/live-connections/devbox/"+apiLiveConnA+"/disconnect",
			mustJSON(t, map[string]any{"request_key": key, "reason": "policy check"}), tc.token)
		if got != tc.status {
			t.Fatalf("disconnect as %s: %d %s", tc.name, got, body)
		}
	}

	// The operator owns console write access, while account administration
	// remains admin-only. This keeps the actionable part of the live directory
	// usable without granting unrelated account management.
}

func TestLiveConnectionHostSigningAndBodyLimits(t *testing.T) {
	s := testutil.Start(t)
	registerLiveHost(t, s.URL, "devbox", "devbox secret")
	path := s.URL + "/v1/hosts/devbox/live-connections"
	body := signedLiveSnapshot(t, "devbox", "devbox secret", liveSnapshot(1, apiLiveConnA))

	if status, reply := request(t, http.MethodPost, path, body, ""); status != http.StatusUnauthorized {
		t.Fatalf("missing host credential: %d %s", status, reply)
	}
	if status, reply := request(t, http.MethodPost, path,
		signedLiveSnapshot(t, "devbox", "wrong secret", liveSnapshot(1, apiLiveConnA)),
		testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("wrong key: %d %s", status, reply)
	}

	priv, _ := hostKey("devbox secret")
	normalized, _ := liveSnapshot(1, apiLiveConnA).Normalize()
	digest, _ := liveconn.Digest(normalized)
	oldTimestamp := time.Now().Add(-maxLiveTestSkew).Unix()
	expired := mustJSON(t, map[string]any{
		"snapshot":  normalized,
		"timestamp": oldTimestamp,
		"signature": identity.Sign(priv, identity.HostLiveSnapshotMessage(
			"devbox", normalized.RuntimeID, normalized.Sequence, normalized.Terminal,
			digest, oldTimestamp,
		)),
	})
	if status, reply := request(t, http.MethodPost, path, expired, testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("expired signature: %d %s", status, reply)
	}

	disabled, _, found, err := s.Registry.SetDisabled("devbox", true)
	if err != nil || !found || !disabled.Disabled {
		t.Fatalf("disable host: %+v found=%v err=%v", disabled, found, err)
	}
	if status, reply := request(t, http.MethodPost, path, body, testutil.ServerKey); status != http.StatusForbidden {
		t.Fatalf("disabled host: %d %s", status, reply)
	}

	s2 := testutil.Start(t)
	registerLiveHost(t, s2.URL, "devbox", "devbox secret")
	oversized := `{"snapshot":` + strings.Repeat(" ", (128<<10)+1) + `}`
	if status, reply := request(t, http.MethodPost,
		s2.URL+"/v1/hosts/devbox/live-connections", oversized, testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("oversized snapshot: %d %s", status, reply)
	}
}

const maxLiveTestSkew = 3 * time.Minute

func TestLiveConnectionGroupScopePrecedesLimit(t *testing.T) {
	s := testutil.Start(t)
	registerLiveHost(t, s.URL, "eastbox", "east secret")
	registerLiveHost(t, s.URL, "westbox", "west secret")
	if _, err := s.Accounts.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateGroup("west", "West", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.AddGroupDevice("east", "eastbox", "alice"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.AddGroupDevice("west", "westbox", "alice"); err != nil {
		t.Fatal(err)
	}
	_, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	created := createToken(t, s.URL, admin,
		`{"name":"east reader","role":"readonly","resources":["devices"],"group_id":"east","expires_in_seconds":3600}`)
	eastSecret := created.Secret

	// Both snapshots have the same server timestamp. The deterministic host
	// tie-break would otherwise put each first on repeated runs; the API must
	// filter by group before its limit regardless.
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/eastbox/live-connections",
		signedLiveSnapshot(t, "eastbox", "east secret", liveSnapshot(1, apiLiveConnA)),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("east snapshot: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/westbox/live-connections",
		signedLiveSnapshot(t, "westbox", "west secret", liveSnapshot(1, apiLiveConnB)),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("west snapshot: %d %s", status, body)
	}
	status, body := request(t, http.MethodGet,
		s.URL+"/v1/live-connections?limit=1", "", eastSecret)
	if status != http.StatusOK {
		t.Fatalf("east list: %d %s", status, body)
	}
	var list liveConnectionListReply
	if err := json.Unmarshal(body, &list); err != nil {
		t.Fatal(err)
	}
	if len(list.Connections) != 1 || list.Connections[0].HostID != "eastbox" {
		t.Fatalf("east list escaped scope: %+v", list)
	}
	if status, body := request(t, http.MethodGet,
		s.URL+"/v1/live-connections/westbox/"+apiLiveConnB, "", eastSecret); status != http.StatusForbidden {
		t.Fatalf("cross-group detail: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost,
		s.URL+"/v1/live-connections/westbox/"+apiLiveConnB+"/disconnect",
		`{"request_key":"cross-group-disconnect"}`, eastSecret); status != http.StatusForbidden {
		t.Fatalf("cross-group disconnect: %d %s", status, body)
	}
}

func TestLiveConnectionDisconnectPollAndReceiptHTTP(t *testing.T) {
	s := testutil.Start(t)
	registerLiveHost(t, s.URL, "devbox", "devbox secret")
	snapshotBody := signedLiveSnapshot(t, "devbox", "devbox secret",
		liveSnapshot(1, apiLiveConnA, apiLiveConnB))
	if status, body := request(t, http.MethodPost,
		s.URL+"/v1/hosts/devbox/live-connections", snapshotBody, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("snapshot: %d %s", status, body)
	}
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	disconnectPath := s.URL + "/v1/live-connections/devbox/" + apiLiveConnA + "/disconnect"
	if status, body := request(t, http.MethodPost, disconnectPath,
		`{"reason":"missing key"}`, admin); status != http.StatusBadRequest {
		t.Fatalf("missing request key: %d %s", status, body)
	}
	status, body := request(t, http.MethodPost, disconnectPath,
		`{"request_key":"exact-request-0001","reason":"support"}`, admin)
	if status != http.StatusAccepted {
		t.Fatalf("disconnect: %d %s", status, body)
	}
	var command liveconn.Command
	if err := json.Unmarshal(body, &command); err != nil {
		t.Fatal(err)
	}
	status, body = request(t, http.MethodPost, disconnectPath,
		`{"request_key":"exact-request-0001","reason":"support"}`, admin)
	if status != http.StatusAccepted {
		t.Fatalf("idempotent disconnect: %d %s", status, body)
	}
	var retried liveconn.Command
	if err := json.Unmarshal(body, &retried); err != nil {
		t.Fatal(err)
	}
	if retried.CommandID != command.CommandID {
		t.Fatalf("idempotent command changed: %+v vs %+v", command, retried)
	}
	if status, body := request(t, http.MethodPost,
		s.URL+"/v1/live-connections/devbox/"+apiLiveConnB+"/disconnect",
		`{"request_key":"exact-request-0001"}`, admin); status != http.StatusConflict {
		t.Fatalf("request key reused: %d %s", status, body)
	}

	pollURL := signedLiveCommandsURL(t, s.URL, "devbox", "devbox secret",
		apiLiveRuntime, time.Now())
	status, body = request(t, http.MethodGet, pollURL, "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("poll: %d %s", status, body)
	}
	var polled liveCommandsReply
	if err := json.Unmarshal(body, &polled); err != nil {
		t.Fatal(err)
	}
	if len(polled.Commands) != 1 || polled.Commands[0].CommandID != command.CommandID {
		t.Fatalf("first poll = %+v", polled)
	}
	commandsDigest, err := liveconn.CommandsDigest(polled.Commands)
	if err != nil {
		t.Fatal(err)
	}
	if serverPub, err := identity.ParsePublicKey(s.Identity.PublicKeyText()); err != nil ||
		!identity.Verify(serverPub, identity.ServerLiveCommandsResponseMessage(
			"devbox", apiLiveRuntime, polled.Timestamp, commandsDigest),
			polled.Signature) {
		t.Fatalf("signed command response did not verify: %+v", polled)
	}
	pollURL = signedLiveCommandsURL(t, s.URL, "devbox", "devbox secret",
		apiLiveRuntime, time.Now())
	status, body = request(t, http.MethodGet, pollURL, "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("second poll: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &polled); err != nil {
		t.Fatal(err)
	}
	if len(polled.Commands) != 1 || polled.Commands[0].CommandID != command.CommandID {
		t.Fatalf("lost command response was not redelivered: %+v", polled)
	}
	commandsDigest, err = liveconn.CommandsDigest(polled.Commands)
	if err != nil {
		t.Fatal(err)
	}
	if serverPub, err := identity.ParsePublicKey(s.Identity.PublicKeyText()); err != nil ||
		!identity.Verify(serverPub, identity.ServerLiveCommandsResponseMessage(
			"devbox", apiLiveRuntime, polled.Timestamp, commandsDigest),
			polled.Signature) {
		t.Fatalf("redelivered command response did not verify: %+v", polled)
	}

	receiptPath := s.URL + "/v1/hosts/devbox/live-connections/" +
		apiLiveRuntime + "/receipts"
	if status, body := request(t, http.MethodPost, receiptPath,
		signedLiveReceipt(t, "devbox", "devbox secret", apiLiveRuntime,
			liveconn.Receipt{
				ReceiptSequence: 1,
				CommandID:       command.CommandID,
				Status:          liveconn.CommandApplied,
			}), testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("applied without absence: %d %s", status, body)
	}
	otherRuntime := strings.Repeat("2", 32)
	wrongRuntime := s.URL + "/v1/hosts/devbox/live-connections/" +
		otherRuntime + "/receipts"
	if status, body := request(t, http.MethodPost, wrongRuntime,
		signedLiveReceipt(t, "devbox", "devbox secret", otherRuntime,
			liveconn.Receipt{
				ReceiptSequence: 1,
				CommandID:       command.CommandID,
				Status:          liveconn.CommandFailed,
			}), testutil.ServerKey); status != http.StatusConflict {
		t.Fatalf("wrong runtime receipt: %d %s", status, body)
	}
	receipt := liveconn.Receipt{
		ReceiptSequence: 1,
		CommandID:       command.CommandID,
		Status:          liveconn.CommandApplied,
		ConfirmedAbsent: true,
		Detail:          "native session absent",
	}
	for i := 0; i < 2; i++ {
		status, body = request(t, http.MethodPost, receiptPath,
			signedLiveReceipt(t, "devbox", "devbox secret", apiLiveRuntime, receipt),
			testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("receipt %d: %d %s", i, status, body)
		}
	}
	status, body = request(t, http.MethodGet,
		s.URL+"/v1/live-connections/devbox/"+apiLiveConnA, "", admin)
	if status != http.StatusOK {
		t.Fatalf("closed detail: %d %s", status, body)
	}
	detail := decodeLiveConnection(t, body)
	if detail.State != liveconn.ConnectionClosed ||
		detail.CommandStatus != liveconn.CommandApplied ||
		!detail.ReceiptConfirmedAbsent || detail.ReceiptDetail != receipt.Detail {
		t.Fatalf("closed detail = %+v", detail)
	}
	status, body = request(t, http.MethodGet,
		s.URL+"/v1/live-connections/devbox/"+apiLiveConnB, "", admin)
	if status != http.StatusOK {
		t.Fatalf("other detail: %d %s", status, body)
	}
	other := decodeLiveConnection(t, body)
	if other.State != liveconn.ConnectionActive || other.CommandStatus != "" {
		t.Fatalf("other connection changed = %+v", other)
	}
}

func TestLiveConnectionStaleDisconnectHTTP(t *testing.T) {
	s := testutil.Start(t)
	registerLiveHost(t, s.URL, "devbox", "devbox secret")
	if status, body := request(t, http.MethodPost,
		s.URL+"/v1/hosts/devbox/live-connections",
		signedLiveSnapshot(t, "devbox", "devbox secret", liveSnapshot(1, apiLiveConnA)),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("snapshot: %d %s", status, body)
	}
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token

	// Store time controls freshness. The signed HTTP requests continue to use
	// the real clock and therefore stay inside their independent replay window.
	now := time.Now().UTC()
	s.Accounts.SetClock(func() time.Time { return now })
	now = now.Add(liveconn.Freshness + time.Second)
	status, body := request(t, http.MethodPost,
		s.URL+"/v1/live-connections/devbox/"+apiLiveConnA+"/disconnect",
		`{"request_key":"stale-request-0001"}`, admin)
	if status != http.StatusConflict || !strings.Contains(string(body), "stale") {
		t.Fatalf("stale disconnect: %d %s", status, body)
	}
}
