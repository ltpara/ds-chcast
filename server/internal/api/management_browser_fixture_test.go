package api_test

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/liveconn"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// Optional browser-only simulated host. It uses signed production HTTP routes,
// but does not run a native host or prove that an actual viewer disconnected.
// Markers: poll-commands, ack-applied/ack-failed, pause-hosts,
// strategy-applied/strategy-pending/strategy-failed. Remove markers to pause.
func setupManagementBrowserFixture(t *testing.T, s *testutil.Server, dir string) func(uint64) {
	t.Helper()
	exists := func(name string) bool { _, err := os.Stat(filepath.Join(dir, name)); return err == nil }
	policy, err := s.Accounts.CreatePolicyObject("browser-managed", "open", 0, nil, nil, "fixture")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreatePolicyAssignment(policy.ID, "group", "east", 10, "fixture"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.SetClientStrategy(policy.ID, 0, strategy.Settings{
		"host.keep_awake":    {Mode: "locked", Value: json.RawMessage(`true`)},
		"viewer.tunnel_mode": {Mode: "default", Value: json.RawMessage(`"relay"`)},
	}, "fixture"); err != nil {
		t.Fatal(err)
	}
	reader, err := s.Accounts.AdminRole("console-reader")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.UpdateAdminRole(reader.ID, reader.Name, reader.Description,
		append(reader.Permissions, account.PermissionDevicesPolicyRead), reader.Revision); err != nil {
		t.Fatal(err)
	}
	active := map[string][]string{"eastbox": {apiLiveConnA, apiLiveConnB}, "westbox": {apiLiveConnA, apiLiveConnB}}
	sequences := map[string]uint64{"eastbox": 1, "westbox": 1}
	receiptSequence := map[string]uint64{}
	strategySequence := int64(0)
	lastStrategy := ""
	postSnapshot := func(host string) {
		sequences[host]++
		snapshot := liveSnapshot(sequences[host], active[host]...)
		for i := range snapshot.Connections {
			snapshot.Connections[i].Viewer = "fixture-" + snapshot.Connections[i].ConnectionID[:1]
			if snapshot.Connections[i].ConnectionID == apiLiveConnB {
				snapshot.Connections[i].Route = "relay"
			}
		}
		status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/"+host+"/live-connections",
			signedLiveSnapshot(t, host, host+" secret", snapshot), testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("fixture snapshot: %d %s", status, body)
		}
	}
	for host := range active {
		postSnapshot(host)
	}
	return func(tick uint64) {
		if !exists("pause-hosts") && tick%5 == 0 {
			for host := range active {
				registerLiveHost(t, s.URL, host, host+" secret")
				postSnapshot(host)
			}
		}
		if exists("poll-commands") && !exists("pause-hosts") {
			for host := range active {
				status, body := request(t, http.MethodGet, signedLiveCommandsURL(t, s.URL, host, host+" secret", apiLiveRuntime, time.Now()), "", testutil.ServerKey)
				if status != http.StatusOK {
					t.Fatalf("fixture poll: %d %s", status, body)
				}
				var response liveCommandsReply
				if err := json.Unmarshal(body, &response); err != nil {
					t.Fatal(err)
				}
				digest, err := liveconn.CommandsDigest(response.Commands)
				if err != nil {
					t.Fatal(err)
				}
				pub, err := identity.ParsePublicKey(s.Identity.PublicKeyText())
				if err != nil || !identity.Verify(pub, identity.ServerLiveCommandsResponseMessage(host, apiLiveRuntime, response.Timestamp, digest), response.Signature) {
					t.Fatal("fixture command signature invalid")
				}
				for _, command := range response.Commands {
					applied := exists("ack-applied")
					if !applied && !exists("ack-failed") {
						continue
					}
					if applied {
						remaining := []string{}
						for _, id := range active[host] {
							if id != command.ConnectionID {
								remaining = append(remaining, id)
							}
						}
						active[host] = remaining
						postSnapshot(host)
					}
					receiptSequence[host]++
					receipt := liveconn.Receipt{ReceiptSequence: receiptSequence[host], CommandID: command.CommandID, Status: liveconn.CommandFailed, Detail: "fixture: simulated refusal"}
					if applied {
						receipt.Status = liveconn.CommandApplied
						receipt.ConfirmedAbsent = true
						receipt.Detail = "fixture: simulated connection absent"
					}
					status, body = request(t, http.MethodPost, s.URL+"/v1/hosts/"+host+"/live-connections/"+apiLiveRuntime+"/receipts", signedLiveReceipt(t, host, host+" secret", apiLiveRuntime, receipt), testutil.ServerKey)
					if status != http.StatusOK {
						t.Fatalf("fixture receipt: %d %s", status, body)
					}
				}
			}
		}
		mode := ""
		for _, entry := range []struct{ file, status string }{{"strategy-applied", strategy.Applied}, {"strategy-pending", strategy.PendingRestart}, {"strategy-failed", strategy.Failed}} {
			if exists(entry.file) {
				mode = entry.status
				break
			}
		}
		if mode == "" {
			return
		}
		snapshot, err := s.Accounts.ResolveClientStrategy("eastbox", strategy.Host)
		if err != nil {
			t.Fatal(err)
		}
		if snapshot.Digest+mode == lastStrategy {
			return
		}
		strategySequence++
		receipt := strategy.Receipt{Revision: snapshot.Revision, Digest: snapshot.Digest, Sequence: strategySequence, Status: mode, Results: map[string]strategy.Result{}}
		for key := range snapshot.Settings {
			receipt.Results[key] = strategy.Result{Status: mode, Detail: "fixture: simulated application"}
		}
		encoded, err := strategy.CanonicalReceipt(receipt)
		if err != nil {
			t.Fatal(err)
		}
		priv, _ := hostKey("eastbox secret")
		timestamp := time.Now().Unix()
		body := mustJSON(t, map[string]any{"receipt": receipt, "timestamp": timestamp, "signature": identity.Sign(priv, identity.HostClientStrategyReceiptMessage("eastbox", encoded, timestamp))})
		status, response := request(t, http.MethodPost, s.URL+"/v1/hosts/eastbox/client-strategy/receipt", body, testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("fixture strategy receipt: %d %s", status, response)
		}
		lastStrategy = snapshot.Digest + mode
	}
}
