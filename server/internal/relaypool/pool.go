// Package relaypool talks to standalone chast-relay nodes, the hbbr
// equivalent. hbbs provisions one lease per host registration, asks the node
// for health so online state can include its shared carrier, and releases the
// lease when the host leaves or moves to another node.
package relaypool

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

type Config struct {
	// Nodes are http:// or https:// base URLs, without a path.
	Nodes []string
	// Key is the control key shared with every node (--relay-node-key).
	Key string
	// Interval is the health probe period. Zero defaults to 10s.
	Interval time.Duration
	// Timeout bounds one control request. Zero defaults to 2s.
	Timeout time.Duration
	Logger  *slog.Logger
	// TLS configures HTTPS control requests: a private CA bundle, a client
	// certificate for mTLS, or both. Nil keeps the default transport.
	TLS *tls.Config
}

type Lease struct {
	Node         string `json:"-"`
	Port         int    `json:"port"`
	Relay        string `json:"relay"`
	Carrier      string `json:"carrier,omitempty"`
	CarrierV6    string `json:"carrier_v6,omitempty"`
	ExpiresInSec int64  `json:"expires_in_sec,omitempty"`
}

type NodeStatus struct {
	URL     string `json:"url"`
	Healthy bool   `json:"healthy"`
	Checked bool   `json:"checked"`
	Leases  int    `json:"leases"`
	Linked  int    `json:"linked"`
	RTTMS   int64  `json:"rtt_ms,omitempty"`
	Error   string `json:"error,omitempty"`
}

type Status struct {
	Nodes   []NodeStatus `json:"nodes"`
	Healthy int          `json:"healthy"`
}

type Pool struct {
	cfg    Config
	client *http.Client
	log    *slog.Logger

	mu     sync.Mutex
	nodes  []*nodeState
	cursor int
	closed bool

	startOnce sync.Once
	closeOnce sync.Once
	done      chan struct{}
	wg        sync.WaitGroup
}

type nodeState struct {
	url     string
	healthy bool
	checked bool
	leases  int
	linked  map[string]struct{}
	rtt     time.Duration
	lastErr string
}

type healthResponse struct {
	OK     bool     `json:"ok"`
	Node   string   `json:"node,omitempty"`
	Leases int      `json:"leases"`
	Linked []string `json:"linked,omitempty"`
}

type leaseRequest struct {
	ID           string `json:"id"`
	PublicKey    string `json:"public_key"`
	CarrierToken string `json:"carrier_token"`
	TTLSeconds   int64  `json:"ttl_sec"`
}

func New(cfg Config) (*Pool, error) {
	if len(cfg.Nodes) == 0 {
		return nil, errors.New("relay pool needs at least one node URL")
	}
	if strings.TrimSpace(cfg.Key) == "" {
		return nil, errors.New("relay pool needs a control key")
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if cfg.Interval <= 0 {
		cfg.Interval = 10 * time.Second
	}
	if cfg.Timeout <= 0 {
		cfg.Timeout = 2 * time.Second
	}
	seen := make(map[string]bool)
	nodes := make([]*nodeState, 0, len(cfg.Nodes))
	for _, raw := range cfg.Nodes {
		normalized, err := normalizeNode(raw)
		if err != nil {
			return nil, err
		}
		if seen[normalized] {
			continue
		}
		seen[normalized] = true
		nodes = append(nodes, &nodeState{url: normalized, linked: make(map[string]struct{})})
	}
	if len(nodes) == 0 {
		return nil, errors.New("relay pool needs at least one node URL")
	}
	client := &http.Client{Timeout: cfg.Timeout}
	if cfg.TLS != nil {
		client.Transport = &http.Transport{TLSClientConfig: cfg.TLS.Clone()}
	}
	return &Pool{
		cfg:    cfg,
		client: client,
		log:    cfg.Logger,
		nodes:  nodes,
		done:   make(chan struct{}),
	}, nil
}

// normalizeNode accepts only base http/https URLs: a path would make lease
// endpoints ambiguous and userinfo would hide credentials from operators.
func normalizeNode(raw string) (string, error) {
	raw = strings.TrimSpace(raw)
	u, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("relay node %q: %w", raw, err)
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return "", fmt.Errorf("relay node %q must be an http:// or https:// URL", raw)
	}
	if u.Host == "" {
		return "", fmt.Errorf("relay node %q has no host", raw)
	}
	if u.User != nil {
		return "", fmt.Errorf("relay node %q must not embed credentials", raw)
	}
	if u.RawQuery != "" || u.Fragment != "" {
		return "", fmt.Errorf("relay node %q must not carry a query or fragment", raw)
	}
	if path := strings.TrimRight(u.Path, "/"); path != "" {
		return "", fmt.Errorf("relay node %q must not carry a path", raw)
	}
	u.Scheme = strings.ToLower(u.Scheme)
	u.Host = strings.ToLower(u.Host)
	u.Path = ""
	return strings.TrimRight(u.String(), "/"), nil
}

// Start probes every node once and then keeps the health snapshot fresh.
func (p *Pool) Start() {
	p.startOnce.Do(func() {
		p.wg.Add(1)
		go func() {
			defer p.wg.Done()
			p.probeAll()
			ticker := time.NewTicker(p.cfg.Interval)
			defer ticker.Stop()
			for {
				select {
				case <-p.done:
					return
				case <-ticker.C:
					p.probeAll()
				}
			}
		}()
	})
}

func (p *Pool) probeAll() {
	p.mu.Lock()
	nodes := make([]*nodeState, len(p.nodes))
	copy(nodes, p.nodes)
	closed := p.closed
	p.mu.Unlock()
	if closed {
		return
	}
	var wg sync.WaitGroup
	for _, node := range nodes {
		wg.Add(1)
		go func(node *nodeState) {
			defer wg.Done()
			p.probe(node)
		}(node)
	}
	wg.Wait()
}

func (p *Pool) probe(node *nodeState) {
	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), p.cfg.Timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, node.url+"/healthz", nil)
	if err != nil {
		p.markFailed(node, err)
		return
	}
	resp, err := p.client.Do(req)
	if err != nil {
		p.markFailed(node, err)
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		p.markFailed(node, fmt.Errorf("health returned HTTP %d", resp.StatusCode))
		return
	}
	var health healthResponse
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&health); err != nil {
		p.markFailed(node, fmt.Errorf("decode health: %w", err))
		return
	}
	if !health.OK {
		p.markFailed(node, errors.New("health reported not ready"))
		return
	}
	linked := make(map[string]struct{}, len(health.Linked))
	for _, id := range health.Linked {
		linked[id] = struct{}{}
	}
	p.mu.Lock()
	node.healthy = true
	node.checked = true
	node.leases = health.Leases
	node.linked = linked
	node.rtt = time.Since(start)
	node.lastErr = ""
	p.mu.Unlock()
}

func (p *Pool) markFailed(node *nodeState, err error) {
	p.mu.Lock()
	node.healthy = false
	node.checked = true
	node.leases = 0
	node.linked = make(map[string]struct{})
	node.lastErr = err.Error()
	p.mu.Unlock()
	p.log.Debug("relay node probe failed", "node", node.url, "error", err)
}

// Provision asks a node for a lease. The host's previous node is tried first
// so a renewal keeps its port and does not disturb a live viewer; otherwise
// every usable node is offered once in round-robin order. A failed attempt
// marks the node unhealthy until the next probe.
func (p *Pool) Provision(id, publicKey, carrierToken string, ttl time.Duration, preferred string) (Lease, error) {
	p.mu.Lock()
	closed := p.closed
	order := p.candidateOrderLocked(preferred)
	p.mu.Unlock()
	if closed {
		return Lease{}, errors.New("relay pool is closed")
	}
	if len(order) == 0 {
		return Lease{}, errors.New("no healthy relay node is available")
	}
	ttlSeconds := int64(ttl / time.Second)
	if ttlSeconds < 1 {
		ttlSeconds = 1
	}
	var lastErr error
	for _, node := range order {
		lease, err := p.requestLease(node.url, leaseRequest{
			ID:           id,
			PublicKey:    publicKey,
			CarrierToken: carrierToken,
			TTLSeconds:   ttlSeconds,
		})
		if err == nil {
			lease.Node = node.url
			p.mu.Lock()
			node.healthy = true
			node.checked = true
			node.lastErr = ""
			p.mu.Unlock()
			return lease, nil
		}
		lastErr = err
		p.markFailed(node, err)
		p.log.Warn("relay node lease failed", "node", node.url, "id", id, "error", err)
	}
	return Lease{}, fmt.Errorf("all relay nodes failed: %w", lastErr)
}

func (p *Pool) candidateOrderLocked(preferred string) []*nodeState {
	order := make([]*nodeState, 0, len(p.nodes))
	seen := make(map[*nodeState]bool, len(p.nodes))
	if preferred != "" {
		for _, node := range p.nodes {
			if node.url == preferred && p.usableLocked(node) {
				order = append(order, node)
				seen[node] = true
			}
		}
	}
	if len(p.nodes) == 0 {
		return order
	}
	start := p.cursor % len(p.nodes)
	p.cursor++
	for i := 0; i < len(p.nodes); i++ {
		node := p.nodes[(start+i)%len(p.nodes)]
		if !seen[node] && p.usableLocked(node) {
			order = append(order, node)
			seen[node] = true
		}
	}
	return order
}

// usableLocked lets an unprobed node have one chance, so a cold hbbs can use
// a healthy node before the first health tick; a node that failed a probe or a
// lease is skipped until the next tick.
func (p *Pool) usableLocked(node *nodeState) bool {
	return !p.closed && (node.healthy || !node.checked)
}

func (p *Pool) requestLease(base string, body leaseRequest) (Lease, error) {
	payload, err := json.Marshal(body)
	if err != nil {
		return Lease{}, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), p.cfg.Timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, base+"/v1/leases", bytes.NewReader(payload))
	if err != nil {
		return Lease{}, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+p.cfg.Key)
	resp, err := p.client.Do(req)
	if err != nil {
		return Lease{}, fmt.Errorf("%s: %w", base, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return Lease{}, fmt.Errorf("%s: %s", base, responseError(resp))
	}
	var lease Lease
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&lease); err != nil {
		return Lease{}, fmt.Errorf("%s: decode lease: %w", base, err)
	}
	if lease.Port == 0 || lease.Relay == "" {
		return Lease{}, fmt.Errorf("%s: lease has no relay endpoint", base)
	}
	return lease, nil
}

// Release drops a lease on the node it was provisioned from. It is best
// effort: an unreachable node expires the lease on its own.
func (p *Pool) Release(nodeURL, id string) {
	p.mu.Lock()
	node := p.findLocked(nodeURL)
	closed := p.closed
	p.mu.Unlock()
	if closed || node == nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), p.cfg.Timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodDelete,
		node.url+"/v1/leases/"+url.PathEscape(id), nil)
	if err != nil {
		return
	}
	req.Header.Set("Authorization", "Bearer "+p.cfg.Key)
	resp, err := p.client.Do(req)
	if err != nil {
		p.log.Debug("relay node release failed", "node", node.url, "id", id, "error", err)
		return
	}
	resp.Body.Close()
	if resp.StatusCode >= http.StatusBadRequest && resp.StatusCode != http.StatusNotFound {
		p.log.Debug("relay node release refused", "node", node.url, "id", id, "status", resp.Status)
	}
}

// ReleaseAsync keeps host switching and unregistration off the request path.
func (p *Pool) ReleaseAsync(nodeURL, id string) {
	if nodeURL == "" {
		return
	}
	go p.Release(nodeURL, id)
}

// HostLinked reports whether the node's latest health snapshot listed the
// host's shared carrier link. The answer can lag by one health interval.
func (p *Pool) HostLinked(nodeURL, id string) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	node := p.findLocked(nodeURL)
	if node == nil || !node.healthy {
		return false
	}
	_, ok := node.linked[id]
	return ok
}

func (p *Pool) Status() Status {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := Status{Nodes: make([]NodeStatus, 0, len(p.nodes))}
	for _, node := range p.nodes {
		status := NodeStatus{
			URL:     node.url,
			Healthy: node.healthy,
			Checked: node.checked,
			Leases:  node.leases,
			Linked:  len(node.linked),
			RTTMS:   node.rtt.Milliseconds(),
			Error:   node.lastErr,
		}
		out.Nodes = append(out.Nodes, status)
		if node.healthy {
			out.Healthy++
		}
	}
	return out
}

// Preview names the next healthy node in round-robin order without consuming
// the rotation or provisioning a lease.
func (p *Pool) Preview() (NodeStatus, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	for i := 0; i < len(p.nodes); i++ {
		node := p.nodes[(p.cursor+i)%len(p.nodes)]
		if node.healthy {
			return NodeStatus{
				URL:     node.url,
				Healthy: true,
				Checked: node.checked,
				Leases:  node.leases,
				Linked:  len(node.linked),
				RTTMS:   node.rtt.Milliseconds(),
				Error:   node.lastErr,
			}, true
		}
	}
	return NodeStatus{}, false
}

func (p *Pool) Healthy() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	count := 0
	for _, node := range p.nodes {
		if node.healthy {
			count++
		}
	}
	return count
}

func (p *Pool) NodeURLs() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]string, 0, len(p.nodes))
	for _, node := range p.nodes {
		out = append(out, node.url)
	}
	return out
}

func (p *Pool) Close() {
	p.closeOnce.Do(func() {
		p.mu.Lock()
		p.closed = true
		p.mu.Unlock()
		close(p.done)
		p.wg.Wait()
	})
}

func (p *Pool) findLocked(nodeURL string) *nodeState {
	for _, node := range p.nodes {
		if node.url == nodeURL {
			return node
		}
	}
	return nil
}

func responseError(resp *http.Response) string {
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
	var payload struct {
		Error string `json:"error"`
	}
	if json.Unmarshal(body, &payload) == nil && payload.Error != "" {
		return fmt.Sprintf("HTTP %d: %s", resp.StatusCode, payload.Error)
	}
	text := strings.TrimSpace(string(body))
	if text == "" {
		text = http.StatusText(resp.StatusCode)
	}
	return fmt.Sprintf("HTTP %d: %s", resp.StatusCode, text)
}
