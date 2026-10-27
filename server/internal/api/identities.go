package api

import (
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// identityListEntry is a durable peer record plus the live lease fields an
// operator needs. Credentials such as the carrier token stay out of the list.
type identityListEntry struct {
	registry.Identity
	Online     bool   `json:"online"`
	Relay      string `json:"relay,omitempty"`
	Candidate  string `json:"candidate,omitempty"`
	LastSeenMS int64  `json:"last_seen_ms,omitempty"`
}

type identityListResponse struct {
	Identities []identityListEntry `json:"identities"`
	// Total and Online count the filtered set before limit/offset, so a paged
	// console can show the full population.
	Total  int `json:"total"`
	Online int `json:"online"`
}

// listIdentities powers the web admin console's peer list: every durable
// identity, including offline peers, with a substring search and an optional
// online filter. RustDesk's hbbs console lists the same peer-table view.
func (s *server) listIdentities(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	query := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("q")))
	onlineOnly := false
	switch strings.ToLower(strings.TrimSpace(r.URL.Query().Get("online"))) {
	case "", "0", "false":
	case "1", "true":
		onlineOnly = true
	default:
		writeError(w, http.StatusBadRequest, "online must be 0 or 1")
		return
	}
	limit, ok := intQuery(w, r, "limit", 200, 1, 1000)
	if !ok {
		return
	}
	offset, ok := intQuery(w, r, "offset", 0, 0, 1_000_000)
	if !ok {
		return
	}
	now := time.Now()
	out := identityListResponse{Identities: make([]identityListEntry, 0)}
	for _, record := range s.cfg.Registry.Identities() {
		host, live := s.cfg.Registry.Lookup(record.ID)
		online := live && s.cfg.Registry.Online(host)
		if onlineOnly && !online {
			continue
		}
		if query != "" && !identityMatches(record, query) {
			continue
		}
		out.Total++
		if online {
			out.Online++
		}
		if out.Total <= offset {
			continue
		}
		if len(out.Identities) >= limit {
			continue
		}
		entry := identityListEntry{Identity: record, Online: online}
		lastSeen := record.LastSeen
		if online {
			entry.Candidate = host.Candidate
			if host.RelayPort != 0 {
				entry.Relay = net.JoinHostPort(s.relayHostFor(host), strconv.Itoa(host.RelayPort))
			}
			lastSeen = host.LastSeen
		}
		if !lastSeen.IsZero() {
			entry.LastSeenMS = now.Sub(lastSeen).Milliseconds()
		}
		out.Identities = append(out.Identities, entry)
	}
	writeJSON(w, http.StatusOK, out)
}

func identityMatches(record registry.Identity, query string) bool {
	fields := []string{record.ID, record.Fingerprint, record.Hostname, record.Platform, record.Version, record.Alias}
	fields = append(fields, record.Tags...)
	for _, field := range fields {
		if strings.Contains(strings.ToLower(field), query) {
			return true
		}
	}
	return false
}

// intQuery parses a bounded integer query parameter with a default.
func intQuery(w http.ResponseWriter, r *http.Request, name string, def, min, max int) (int, bool) {
	raw := strings.TrimSpace(r.URL.Query().Get(name))
	if raw == "" {
		return def, true
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value < min || value > max {
		writeError(w, http.StatusBadRequest, name+" is out of range")
		return 0, false
	}
	return value, true
}
