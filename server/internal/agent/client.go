package agent

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/wake"
)

// Version is reported to the server as peer metadata and can be overridden at
// build time with -ldflags "-X .../internal/agent.Version=<version>".
var Version = "dev"

// softwareInfo is the server's SoftwareUpdate equivalent.
type softwareInfo struct {
	Version string `json:"version"`
	URL     string `json:"url"`
	// SHA256, Size and Signature are present when the server hosts the file
	// itself and signs the notice with its identity. The host agent needs all
	// three to verify a staged release before running it.
	SHA256    string `json:"sha256"`
	Size      int64  `json:"size"`
	Signature string `json:"signature"`
	// PublisherKeyID and PublisherSignature carry the publisher's detached
	// signature over the same release. They are absent on older servers.
	PublisherKeyID     string `json:"publisher_key_id"`
	PublisherSignature string `json:"publisher_sig"`
}

// updateAvailable mirrors RustDesk's SoftwareUpdate check: the server only
// answers when its version differs from the client's, so ordering is not used.
func (s *softwareInfo) updateAvailable() bool {
	return s != nil && s.URL != "" && s.Version != "" && s.Version != Version
}

type apiClient struct {
	mu        sync.Mutex
	base      string
	serverKey string
	http      *http.Client
	// poll serves the host event stream: a long poll must outlive the 5 s
	// control-plane timeout, so it relies on the caller's context instead.
	poll *http.Client
}

const hostEventPollWait = 25 * time.Second

func newAPIClient(base, serverKey string, tlsConfig *tls.Config, proxy *netproxy.Proxy) *apiClient {
	client := &http.Client{Timeout: 5 * time.Second}
	if tlsConfig != nil || proxy != nil {
		client.Transport = proxy.HTTPTransport(tlsConfig)
	}
	return &apiClient{
		base:      strings.TrimRight(base, "/"),
		serverKey: serverKey,
		http:      client,
		poll:      &http.Client{Transport: client.Transport},
	}
}

// hostAPIKey picks the registration credential, falling back to the
// management key when the deployment does not split them.
func hostAPIKey(cfg HostConfig) string {
	if cfg.HostKey != "" {
		return cfg.HostKey
	}
	return cfg.ServerKey
}

// viewerAPIKey picks the lookup credential, falling back to the management key
// when the deployment does not split them.
func viewerAPIKey(cfg ViewerConfig) string {
	if cfg.LinkKey != "" {
		return cfg.LinkKey
	}
	return cfg.ServerKey
}

// setBase points the client at another rendezvous server after a failover.
// In-flight requests keep the base they already read.
func (c *apiClient) setBase(base string) {
	base = strings.TrimRight(base, "/")
	c.mu.Lock()
	c.base = base
	c.mu.Unlock()
}

func (c *apiClient) endpoint() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.base
}

type registerResult struct {
	ID                string        `json:"id"`
	Relay             string        `json:"relay"`
	Relays            []string      `json:"relays"`
	Punch             string        `json:"punch"`
	PunchV6           string        `json:"punch_v6"`
	TCPPunch          string        `json:"tcp_punch"`
	Carrier           string        `json:"carrier"`
	CarrierV6         string        `json:"carrier_v6"`
	CarrierToken      string        `json:"carrier_token"`
	ExpiresInSec      int64         `json:"expires_in_sec"`
	PublicKey         string        `json:"public_key"`
	ServerPublicKey   string        `json:"server_public_key"`
	Binding           string        `json:"binding"`
	MigratedFrom      string        `json:"migrated_from"`
	Software          *softwareInfo `json:"software"`
	ConfigSerial      int64         `json:"config_serial"`
	RendezvousServers []string      `json:"rendezvous_servers"`
	// DevicePolicy is the managed-device statement. Nil keeps the unmanaged
	// local-authentication behavior.
	DevicePolicy *DevicePolicy `json:"device_policy"`
	// Wake carries cross-subnet Wake-on-LAN jobs queued since the previous
	// registration. Only agents started with the relay enabled act on them.
	Wake []wake.Job `json:"wake"`
}

type lookupResult struct {
	ID                string        `json:"id"`
	Online            bool          `json:"online"`
	Candidate         string        `json:"candidate"`
	CandidateV6       string        `json:"candidate_v6"`
	Candidates        []string      `json:"candidates"`
	Relay             string        `json:"relay"`
	Relays            []string      `json:"relays"`
	Punch             string        `json:"punch"`
	PunchV6           string        `json:"punch_v6"`
	TCPPunch          string        `json:"tcp_punch"`
	Carrier           string        `json:"carrier"`
	CarrierV6         string        `json:"carrier_v6"`
	CarrierToken      string        `json:"carrier_token"`
	Fingerprint       string        `json:"fingerprint"`
	PublicKey         string        `json:"public_key"`
	ServerPublicKey   string        `json:"server_public_key"`
	Binding           string        `json:"binding"`
	LocalAddrs        []string      `json:"local_addrs"`
	LocalAddrsV6      []string      `json:"local_addrs_v6"`
	NATType           string        `json:"nat_type"`
	UpnpPort          int           `json:"upnp_port"`
	LastSeenMS        int64         `json:"last_seen_ms"`
	ExpiresInMS       int64         `json:"expires_in_ms"`
	RelayOnly         bool          `json:"relay_only"`
	Transport         string        `json:"transport"`
	Software          *softwareInfo `json:"software"`
	ConfigSerial      int64         `json:"config_serial"`
	RendezvousServers []string      `json:"rendezvous_servers"`
}

type apiError struct {
	Error string `json:"error"`
}

type apiStatusError struct {
	method, path, detail string
	status               int
}

func (e *apiStatusError) Error() string {
	return fmt.Sprintf("%s %s: %s", e.method, e.path, e.detail)
}

func (c *apiClient) register(ctx context.Context, id, oldID string, privateKey ed25519.PrivateKey, fingerprint string, wakeRelay bool, wakeTargets []string) (registerResult, error) {
	timestamp := time.Now().Unix()
	hostname, _ := os.Hostname()
	hostname = strings.TrimSpace(hostname)
	platform := runtime.GOOS + "/" + runtime.GOARCH
	body := map[string]any{
		"id":          id,
		"public_key":  identity.PublicKeyText(privateKey.Public().(ed25519.PublicKey)),
		"fingerprint": fingerprint,
		"hostname":    hostname,
		"platform":    platform,
		"version":     Version,
		"timestamp":   timestamp,
		"signature":   identity.Sign(privateKey, identity.RegisterMessage(id, fingerprint, timestamp)),
		"metadata_signature": identity.Sign(privateKey,
			identity.HostMetadataMessage(id, hostname, platform, Version)),
		"wake_relay":   wakeRelay,
		"wake_targets": wakeTargets,
	}
	if oldID != "" {
		body["old_id"] = oldID
		body["migrate_signature"] = identity.Sign(privateKey, identity.MigrateMessage(oldID, id, timestamp))
	}
	data, err := json.Marshal(body)
	if err != nil {
		return registerResult{}, err
	}
	var out registerResult
	if err := c.do(ctx, http.MethodPost, "/v1/hosts", data, &out); err != nil {
		return registerResult{}, err
	}
	return out, nil
}

func (c *apiClient) lookup(ctx context.Context, id string) (lookupResult, error) {
	var out lookupResult
	if err := c.do(ctx, http.MethodGet, "/v1/hosts/"+id, nil, &out); err != nil {
		return lookupResult{}, err
	}
	return out, nil
}

func (c *apiClient) unregister(ctx context.Context, id string) error {
	return c.do(ctx, http.MethodDelete, "/v1/hosts/"+id, nil, nil)
}

// reportDevicePolicyReceipt posts the host-key-signed application result. The
// request goes to the same server that delivered the policy; a failed POST is
// retried by a later registration rather than blocking the read loop.
func (c *apiClient) reportDevicePolicyReceipt(ctx context.Context, peerID string, req policyReceiptRequest) error {
	data, err := json.Marshal(req)
	if err != nil {
		return err
	}
	return c.do(ctx, http.MethodPost,
		"/v1/device-policies/"+url.PathEscape(peerID)+"/receipt", data, nil)
}

// hostEvent is one statement on the host control stream. Policy events carry
// the new managed statement; revoke events name an issuer key that must no
// longer admit sessions.
type hostEvent struct {
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

type hostEventsResult struct {
	Events []hostEvent `json:"events"`
	Cursor uint64      `json:"cursor"`
}

// hostEvents long-polls the server for policy and key-revocation statements.
// The cursor and timestamp are signed with the device key, so a shared host
// credential cannot read another machine's stream.
func (c *apiClient) hostEvents(ctx context.Context, id string, after uint64, priv ed25519.PrivateKey) (hostEventsResult, error) {
	timestamp := time.Now().Unix()
	query := url.Values{}
	query.Set("after", strconv.FormatUint(after, 10))
	query.Set("wait_ms", strconv.FormatInt(hostEventPollWait.Milliseconds(), 10))
	query.Set("timestamp", strconv.FormatInt(timestamp, 10))
	query.Set("signature", identity.Sign(priv, identity.HostEventsSubscribeMessage(id, after, timestamp)))
	var out hostEventsResult
	path := "/v1/hosts/" + url.PathEscape(id) + "/events?" + query.Encode()
	if err := c.doWith(ctx, c.poll, http.MethodGet, path, nil, &out); err != nil {
		return hostEventsResult{}, err
	}
	return out, nil
}

func (c *apiClient) do(ctx context.Context, method, path string, body []byte, out any) error {
	return c.doWith(ctx, c.http, method, path, body, out)
}

func (c *apiClient) doWith(ctx context.Context, client *http.Client, method, path string, body []byte, out any) error {
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	base := c.endpoint()
	req, err := http.NewRequestWithContext(ctx, method, base+path, reader)
	if err != nil {
		return err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if c.serverKey != "" {
		req.Header.Set("Authorization", "Bearer "+c.serverKey)
	}
	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("%s %s: %w", method, path, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return fmt.Errorf("%s %s: read reply: %w", method, path, err)
	}
	if resp.StatusCode >= 300 {
		var e apiError
		detail := fmt.Sprintf("HTTP %d", resp.StatusCode)
		if json.Unmarshal(data, &e) == nil && e.Error != "" {
			detail = e.Error
		}
		return &apiStatusError{method: method, path: path, detail: detail, status: resp.StatusCode}
	}
	if out == nil {
		return nil
	}
	if err := json.Unmarshal(data, out); err != nil {
		return fmt.Errorf("%s %s: decode reply: %w", method, path, err)
	}
	return nil
}
