package agent

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"runtime"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

// Version is reported to the server as peer metadata and can be overridden at
// build time with -ldflags "-X .../internal/agent.Version=<version>".
var Version = "dev"

// softwareInfo is the server's SoftwareUpdate equivalent.
type softwareInfo struct {
	Version string `json:"version"`
	URL     string `json:"url"`
}

// updateAvailable mirrors RustDesk's SoftwareUpdate check: the server only
// answers when its version differs from the client's, so ordering is not used.
func (s *softwareInfo) updateAvailable() bool {
	return s != nil && s.URL != "" && s.Version != "" && s.Version != Version
}

type apiClient struct {
	base      string
	serverKey string
	http      *http.Client
}

func newAPIClient(base, serverKey string) *apiClient {
	return &apiClient{
		base:      strings.TrimRight(base, "/"),
		serverKey: serverKey,
		http:      &http.Client{Timeout: 5 * time.Second},
	}
}

type registerResult struct {
	ID              string        `json:"id"`
	Relay           string        `json:"relay"`
	Relays          []string      `json:"relays"`
	Punch           string        `json:"punch"`
	PunchV6         string        `json:"punch_v6"`
	Carrier         string        `json:"carrier"`
	CarrierToken    string        `json:"carrier_token"`
	ExpiresInSec    int64         `json:"expires_in_sec"`
	PublicKey       string        `json:"public_key"`
	ServerPublicKey string        `json:"server_public_key"`
	Binding         string        `json:"binding"`
	MigratedFrom    string        `json:"migrated_from"`
	Software        *softwareInfo `json:"software"`
}

type lookupResult struct {
	ID              string        `json:"id"`
	Online          bool          `json:"online"`
	Candidate       string        `json:"candidate"`
	CandidateV6     string        `json:"candidate_v6"`
	Candidates      []string      `json:"candidates"`
	Relay           string        `json:"relay"`
	Relays          []string      `json:"relays"`
	Punch           string        `json:"punch"`
	PunchV6         string        `json:"punch_v6"`
	Carrier         string        `json:"carrier"`
	CarrierToken    string        `json:"carrier_token"`
	Fingerprint     string        `json:"fingerprint"`
	PublicKey       string        `json:"public_key"`
	ServerPublicKey string        `json:"server_public_key"`
	Binding         string        `json:"binding"`
	LocalAddrs      []string      `json:"local_addrs"`
	LocalAddrsV6    []string      `json:"local_addrs_v6"`
	NATType         string        `json:"nat_type"`
	UpnpPort        int           `json:"upnp_port"`
	LastSeenMS      int64         `json:"last_seen_ms"`
	ExpiresInMS     int64         `json:"expires_in_ms"`
	RelayOnly       bool          `json:"relay_only"`
	Transport       string        `json:"transport"`
	Software        *softwareInfo `json:"software"`
}

type apiError struct {
	Error string `json:"error"`
}

func (c *apiClient) register(ctx context.Context, id, oldID string, privateKey ed25519.PrivateKey, fingerprint string) (registerResult, error) {
	timestamp := time.Now().Unix()
	hostname, _ := os.Hostname()
	body := map[string]any{
		"id":          id,
		"public_key":  identity.PublicKeyText(privateKey.Public().(ed25519.PublicKey)),
		"fingerprint": fingerprint,
		"hostname":    hostname,
		"platform":    runtime.GOOS + "/" + runtime.GOARCH,
		"version":     Version,
		"timestamp":   timestamp,
		"signature":   identity.Sign(privateKey, identity.RegisterMessage(id, fingerprint, timestamp)),
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

func (c *apiClient) do(ctx context.Context, method, path string, body []byte, out any) error {
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(ctx, method, c.base+path, reader)
	if err != nil {
		return err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if c.serverKey != "" {
		req.Header.Set("Authorization", "Bearer "+c.serverKey)
	}
	resp, err := c.http.Do(req)
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
		if json.Unmarshal(data, &e) == nil && e.Error != "" {
			return fmt.Errorf("%s %s: %s", method, path, e.Error)
		}
		return fmt.Errorf("%s %s: HTTP %d", method, path, resp.StatusCode)
	}
	if out == nil {
		return nil
	}
	if err := json.Unmarshal(data, out); err != nil {
		return fmt.Errorf("%s %s: decode reply: %w", method, path, err)
	}
	return nil
}
