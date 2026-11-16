package api_test

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/url"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestHostClientStrategySignedDeliveryAndReceipts(t *testing.T) {
	s := testutil.Start(t)
	for _, peer := range []string{"devbox", "other"} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", registerBody(t, peer, peer+"-secret", "AB12", time.Now().Unix()), testutil.ServerKey); status != http.StatusOK {
			t.Fatalf("register: %d %s", status, body)
		}
	}
	policy, err := s.Accounts.CreatePolicyObject("config", "open", 0, nil, nil, "root")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreatePolicyAssignment(policy.ID, "all", "", 0, "root"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.SetClientStrategy(policy.ID, 0, strategy.Settings{"host.keep_awake": {Mode: strategy.Locked, Value: json.RawMessage("false")}}, "root"); err != nil {
		t.Fatal(err)
	}
	priv, _ := hostKey("devbox-secret")
	timestamp := time.Now().Unix()
	query := func(peer string, ts int64) string {
		return "?" + url.Values{"timestamp": {strconv.FormatInt(ts, 10)}, "signature": {identity.Sign(priv, identity.HostClientStrategyFetchMessage(peer, ts))}}.Encode()
	}
	endpoint := s.URL + "/v1/hosts/devbox/client-strategy"
	for _, tc := range []struct {
		path, key string
		code      int
	}{
		{endpoint, testutil.ServerKey, http.StatusUnauthorized},
		{endpoint + query("devbox", timestamp), "wrong", http.StatusUnauthorized},
		{endpoint + query("other", timestamp), testutil.ServerKey, http.StatusUnauthorized},
		{endpoint + query("devbox", timestamp-3600), testutil.ServerKey, http.StatusUnauthorized},
		{s.URL + "/v1/hosts/other/client-strategy" + query("other", timestamp), testutil.ServerKey, http.StatusUnauthorized},
	} {
		if status, body := request(t, http.MethodGet, tc.path, "", tc.key); status != tc.code {
			t.Fatalf("fetch auth = %d %s", status, body)
		}
	}
	status, body := request(t, http.MethodGet, endpoint+query("devbox", timestamp), "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("fetch: %d %s", status, body)
	}
	var snapshot strategy.Snapshot
	if err := json.Unmarshal(body, &snapshot); err != nil {
		t.Fatal(err)
	}
	if err := snapshot.Validate("devbox", strategy.Host); err != nil {
		t.Fatal(err)
	}
	receipt := strategy.Receipt{Revision: snapshot.Revision, Digest: snapshot.Digest, Sequence: 1, Status: strategy.PendingRestart, Results: map[string]strategy.Result{"host.keep_awake": {Status: strategy.PendingRestart}}}
	postBody := func(receipt strategy.Receipt, signedPeer string) string {
		encoded, err := strategy.CanonicalReceipt(receipt)
		if err != nil {
			t.Fatal(err)
		}
		return mustJSON(t, map[string]any{"receipt": receipt, "timestamp": timestamp, "signature": identity.Sign(priv, identity.HostClientStrategyReceiptMessage(signedPeer, encoded, timestamp))})
	}
	if status, body := request(t, http.MethodPost, endpoint+"/receipt", postBody(receipt, "other"), testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("receipt wrong owner: %d %s", status, body)
	}
	valid := postBody(receipt, "devbox")
	var tampered map[string]any
	if err := json.Unmarshal([]byte(valid), &tampered); err != nil {
		t.Fatal(err)
	}
	tampered["receipt"].(map[string]any)["sequence"] = 9
	if status, body := request(t, http.MethodPost, endpoint+"/receipt", mustJSON(t, tampered), testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("tampered: %d %s", status, body)
	}
	for i := 0; i < 2; i++ {
		if status, body := request(t, http.MethodPost, endpoint+"/receipt", valid, testutil.ServerKey); status != http.StatusOK {
			t.Fatalf("receipt: %d %s", status, body)
		}
	}
	events, err := s.Accounts.AuditEventsFor(account.AuditClientStrategyReceipt, 10)
	if err != nil || len(events) != 1 {
		t.Fatalf("receipt audit should be idempotent: %d %v", len(events), err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-strategies/devbox", "", testutil.ServerKey)
	var preview struct {
		Receipt *account.ClientStrategyReceipt `json:"receipt"`
	}
	if err := json.Unmarshal(body, &preview); status != http.StatusOK || err != nil || preview.Receipt == nil || preview.Receipt.Status != strategy.PendingRestart {
		t.Fatalf("preview: %d %s %v", status, body, err)
	}
	if _, err := s.Accounts.SetClientStrategy(policy.ID, 1, strategy.Settings{}, "root"); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, endpoint+"/receipt", valid, testutil.ServerKey); status != http.StatusConflict {
		t.Fatalf("stale receipt: %d %s", status, body)
	}
	if _, _, _, err := s.Registry.SetDisabled("devbox", true); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodGet, endpoint+query("devbox", timestamp), "", testutil.ServerKey); status != http.StatusForbidden {
		t.Fatalf("disabled host: %d %s", status, body)
	}
}

func TestViewerClientStrategyAuthenticatedDeliveryAndReceipts(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: newTestIssuer(t)})
	for _, peer := range []string{"devbox", "other"} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", registerBody(t, peer, peer+"-secret", "AB12", time.Now().Unix()), testutil.ServerKey); status != http.StatusOK {
			t.Fatalf("register: %d %s", status, body)
		}
	}
	viewer, err := s.Accounts.CreateUser("viewer", "viewer-password")
	if err != nil {
		t.Fatal(err)
	}
	_, viewerToken, _, err := s.Accounts.Login(viewer.Username, "viewer-password")
	if err != nil {
		t.Fatal(err)
	}
	_, otherToken, _, err := func() (account.User, string, time.Time, error) {
		user, err := s.Accounts.CreateUser("other", "other-password")
		if err != nil {
			return account.User{}, "", time.Time{}, err
		}
		return s.Accounts.Login(user.Username, "other-password")
	}()
	if err != nil {
		t.Fatal(err)
	}
	policy, err := s.Accounts.CreatePolicyObject("viewer config", "open", 0, nil, nil, "root")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreatePolicyAssignment(policy.ID, "device", "devbox", 0, "root"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.SetClientStrategy(policy.ID, 0, strategy.Settings{
		"viewer.tunnel_mode":            {Mode: strategy.Locked, Value: json.RawMessage(`"relay"`)},
		"viewer.use_websocket":          {Mode: strategy.Default, Value: json.RawMessage(`true`)},
		"viewer.upload_recordings":      {Mode: strategy.Locked, Value: json.RawMessage(`false`)},
		"viewer.sync_initial_clipboard": {Mode: strategy.Locked, Value: json.RawMessage(`false`)},
		"viewer.view_fit":               {Mode: strategy.Locked, Value: json.RawMessage(`1`)},
	}, "root"); err != nil {
		t.Fatal(err)
	}

	clientKey := base64.RawURLEncoding.EncodeToString(bytes.Repeat([]byte{0x42}, 32))
	query := url.Values{"client_public_key": {clientKey}}.Encode()
	status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox?"+query, "", viewerToken)
	if status != http.StatusOK {
		t.Fatalf("viewer lookup: %d %s", status, body)
	}
	var response struct {
		ViewerStrategy *account.ViewerStrategyDelivery `json:"viewer_strategy"`
	}
	if err := json.Unmarshal(body, &response); err != nil {
		t.Fatal(err)
	}
	if response.ViewerStrategy == nil {
		t.Fatal("authenticated account lookup omitted viewer strategy")
	}
	delivery := response.ViewerStrategy
	if err := delivery.Snapshot.Validate("devbox", strategy.Viewer); err != nil {
		t.Fatal(err)
	}
	if delivery.Context == "" || delivery.ExpiresAtMS <= time.Now().UnixMilli() ||
		string(delivery.Snapshot.Settings["viewer.tunnel_mode"].Value) != `"relay"` {
		t.Fatalf("delivery = %+v", delivery)
	}

	for _, tc := range []struct {
		name, path, key string
	}{
		{"shared key", "/v1/hosts/devbox", testutil.ServerKey},
		{"without client identity", "/v1/hosts/devbox", viewerToken},
	} {
		status, body := request(t, http.MethodGet, s.URL+tc.path, "", tc.key)
		if status != http.StatusOK {
			t.Fatalf("%s lookup: %d %s", tc.name, status, body)
		}
		var got struct {
			ViewerStrategy *account.ViewerStrategyDelivery `json:"viewer_strategy"`
		}
		if err := json.Unmarshal(body, &got); err != nil {
			t.Fatal(err)
		}
		if got.ViewerStrategy != nil {
			t.Fatalf("%s received viewer strategy", tc.name)
		}
	}

	receipt := strategy.Receipt{
		Revision: delivery.Snapshot.Revision,
		Digest:   delivery.Snapshot.Digest,
		Sequence: 1,
		Status:   strategy.Applied,
		Results: map[string]strategy.Result{
			"viewer.sync_initial_clipboard": {Status: strategy.Applied},
			"viewer.tunnel_mode":            {Status: strategy.Applied},
			"viewer.upload_recordings":      {Status: strategy.Applied},
			"viewer.use_websocket":          {Status: strategy.Applied},
			"viewer.view_fit":               {Status: strategy.Applied},
		},
	}
	receiptBody := mustJSON(t, map[string]any{
		"context":           delivery.Context,
		"client_public_key": clientKey,
		"receipt":           receipt,
	})
	endpoint := s.URL + "/v1/account/device-strategies/devbox/receipt"
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/device-strategies/other/receipt", receiptBody, viewerToken); status != http.StatusConflict {
		t.Fatalf("cross-peer receipt: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, endpoint, receiptBody, otherToken); status != http.StatusConflict {
		t.Fatalf("cross-account receipt: %d %s", status, body)
	}
	for i := 0; i < 2; i++ {
		if status, body := request(t, http.MethodPost, endpoint, receiptBody, viewerToken); status != http.StatusOK {
			t.Fatalf("receipt: %d %s", status, body)
		}
	}
	events, err := s.Accounts.AuditEventsFor(account.AuditClientStrategyReceipt, 10)
	if err != nil || len(events) != 1 {
		t.Fatalf("viewer receipt audit should be idempotent: %d %v", len(events), err)
	}
	receipt.Sequence = 2
	receipt.Status = strategy.Failed
	receipt.Results["viewer.tunnel_mode"] = strategy.Result{Status: strategy.Failed, Detail: "relay unavailable"}
	if status, body := request(t, http.MethodPost, endpoint, mustJSON(t, map[string]any{
		"context":           delivery.Context,
		"client_public_key": clientKey,
		"receipt":           receipt,
	}), viewerToken); status != http.StatusOK {
		t.Fatalf("later receipt: %d %s", status, body)
	}
	if _, err := s.Accounts.SetClientStrategy(policy.ID, 1, strategy.Settings{}, "root"); err != nil {
		t.Fatal(err)
	}
	receipt.Sequence = 3
	if status, body := request(t, http.MethodPost, endpoint, mustJSON(t, map[string]any{
		"context":           delivery.Context,
		"client_public_key": clientKey,
		"receipt":           receipt,
	}), viewerToken); status != http.StatusConflict {
		t.Fatalf("stale viewer receipt: %d %s", status, body)
	}
}
