package agent

import (
	"bufio"
	"context"
	"crypto/ed25519"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

const (
	auditSpoolInterval = 2 * time.Second
	auditSpoolBatch    = 200
)

// auditSpoolEvent is one JSONL record written by chast-host. The host does not
// put its rendezvous id or key in the file: the agent supplies both when it
// signs and forwards the event, so a spool copied to another host cannot be
// replayed against the original id.
type auditSpoolEvent struct {
	Kind         string `json:"kind"`
	AuditRef     string `json:"audit_ref"`
	StartedAtMS  int64  `json:"started_at_ms,omitempty"`
	EndedAtMS    int64  `json:"ended_at_ms,omitempty"`
	Direction    string `json:"direction,omitempty"`
	Name         string `json:"name,omitempty"`
	Size         int64  `json:"size,omitempty"`
	OccurredAtMS int64  `json:"occurred_at_ms,omitempty"`
	Success      *bool  `json:"success,omitempty"`
	Detail       string `json:"detail,omitempty"`
}

// auditSpool tails the host spool in order. Lines are consumed only after the
// server accepted them, so a disconnect or a restart does not lose events; the
// whole file is removed once it has been forwarded.
type auditSpool struct {
	path       string
	hostID     string
	priv       ed25519.PrivateKey
	api        *apiClient
	log        *slog.Logger
	offset     int64
	lastFailed string
}

func (h *HostAgent) auditSpoolLoop(path string) {
	sp := &auditSpool{
		path:   path,
		hostID: h.cfg.ID,
		priv:   h.priv,
		api:    h.api,
		log:    h.log.With("component", "audit-spool"),
	}
	ticker := time.NewTicker(auditSpoolInterval)
	defer ticker.Stop()
	for {
		sp.drain(h.ctx)
		select {
		case <-h.ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (sp *auditSpool) drain(ctx context.Context) {
	sp.readLines(ctx)
	// A partial trailing line stays for the next tick; the file is only
	// removed when every byte in it was forwarded. Windows refuses to delete
	// a file with an open handle, so readLines has already closed it.
	if sp.offset == 0 {
		return
	}
	if current, err := os.Stat(sp.path); err == nil && current.Size() == sp.offset {
		if err := os.Remove(sp.path); err != nil {
			sp.log.Warn("audit spool remove", "error", err)
		} else {
			sp.offset = 0
		}
	}
}

func (sp *auditSpool) readLines(ctx context.Context) {
	file, err := os.Open(sp.path)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			sp.report(err)
		}
		sp.offset = 0
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		sp.report(err)
		return
	}
	if info.Size() < sp.offset {
		// The host rotated or recreated the file; start over.
		sp.offset = 0
	}
	if info.Size() == sp.offset {
		return
	}
	if _, err := file.Seek(sp.offset, io.SeekStart); err != nil {
		sp.report(err)
		return
	}
	reader := bufio.NewReader(file)
	for lines := 0; lines < auditSpoolBatch; lines++ {
		line, readErr := reader.ReadString('\n')
		complete := strings.HasSuffix(line, "\n")
		if complete {
			if err := sp.forward(ctx, line); err != nil {
				sp.report(err)
				return
			}
			sp.offset += int64(len(line))
			sp.lastFailed = ""
		}
		if readErr != nil || !complete {
			break
		}
	}
}

func (sp *auditSpool) forward(ctx context.Context, line string) error {
	line = strings.TrimSpace(line)
	if line == "" {
		return nil
	}
	var event auditSpoolEvent
	if err := json.Unmarshal([]byte(line), &event); err != nil {
		// A malformed line would otherwise block every later event.
		sp.log.Warn("audit spool line is not JSON; dropping", "line", line)
		return nil
	}
	if event.Kind != "session" && event.Kind != "file" {
		sp.log.Warn("audit spool line has an unknown kind; dropping", "kind", event.Kind)
		return nil
	}
	return sp.api.audit(ctx, sp.hostID, sp.priv, event)
}

func (sp *auditSpool) report(err error) {
	text := err.Error()
	if text == sp.lastFailed {
		sp.log.Debug("audit spool retry", "error", err)
		return
	}
	sp.lastFailed = text
	sp.log.Warn("audit spool forward", "error", err)
}

func (c *apiClient) audit(ctx context.Context, hostID string, priv ed25519.PrivateKey, event auditSpoolEvent) error {
	success := true
	if event.Success != nil {
		success = *event.Success
	}
	timestamp := time.Now().Unix()
	body := map[string]any{
		"kind":      event.Kind,
		"audit_ref": event.AuditRef,
		"success":   success,
		"detail":    event.Detail,
		"timestamp": timestamp,
	}
	var msg []byte
	if event.Kind == "session" {
		body["started_at_ms"] = event.StartedAtMS
		body["ended_at_ms"] = event.EndedAtMS
		msg = identity.HostAuditSessionMessage(hostID, event.AuditRef,
			event.StartedAtMS, event.EndedAtMS, success, event.Detail)
	} else {
		body["direction"] = event.Direction
		body["name"] = event.Name
		body["size"] = event.Size
		body["occurred_at_ms"] = event.OccurredAtMS
		msg = identity.HostAuditFileMessage(hostID, event.AuditRef, event.Direction,
			event.Name, event.Size, event.OccurredAtMS, success, event.Detail)
	}
	body["signature"] = identity.Sign(priv, msg)
	data, err := json.Marshal(body)
	if err != nil {
		return err
	}
	return c.do(ctx, http.MethodPost, "/v1/hosts/"+url.PathEscape(hostID)+"/audit", data, nil)
}
