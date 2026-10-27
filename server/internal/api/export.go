package api

import (
	"encoding/csv"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// exportBatch is the page size used while walking a history table for export.
// Retention caps every table at 10000 rows, so a whole export fits in memory
// and can fail cleanly before the response starts.
const exportBatch = 500

// exportRequest reports whether the caller asked for a CSV download. An empty
// format or "json" keeps the usual paged JSON response.
func exportRequest(w http.ResponseWriter, r *http.Request) (bool, bool) {
	switch strings.ToLower(strings.TrimSpace(r.URL.Query().Get("format"))) {
	case "", "json":
		return false, true
	case "csv":
		return true, true
	default:
		writeError(w, http.StatusBadRequest, "format must be csv or json")
		return false, false
	}
}

func (s *server) writeCSV(w http.ResponseWriter, prefix string, header []string, rows [][]string) {
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition",
		fmt.Sprintf(`attachment; filename="%s-%s.csv"`,
			prefix, time.Now().UTC().Format("20060102-150405")))
	w.WriteHeader(http.StatusOK)
	cw := csv.NewWriter(w)
	if err := cw.Write(header); err != nil {
		s.cfg.Logger.Error("audit export header", "error", err)
		return
	}
	if err := cw.WriteAll(rows); err != nil {
		s.cfg.Logger.Error("audit export rows", "error", err)
	}
}

func (s *server) auditCSV(w http.ResponseWriter, r *http.Request) {
	action := r.URL.Query().Get("action")
	rows := make([][]string, 0, 64)
	var before int64
	for {
		events, err := s.cfg.Accounts.AuditEventsForBefore(action, before, exportBatch)
		if err != nil {
			s.cfg.Logger.Error("audit export query", "error", err)
			writeError(w, http.StatusInternalServerError, "could not read the audit log")
			return
		}
		for _, event := range events {
			rows = append(rows, []string{
				strconv.FormatInt(event.ID, 10),
				event.CreatedAt.UTC().Format(time.RFC3339Nano),
				event.Username,
				event.Action,
				event.Detail,
				event.ClientIP,
				event.Location,
				strconv.FormatBool(event.Success),
			})
		}
		if len(events) < exportBatch {
			break
		}
		before = events[len(events)-1].ID
	}
	s.writeCSV(w, "chast-audit", []string{
		"id", "created_at", "username", "action", "detail",
		"client_ip", "location", "success",
	}, rows)
}

func (s *server) connectionsCSV(w http.ResponseWriter, r *http.Request) {
	username := r.URL.Query().Get("username")
	host := strings.TrimSpace(r.URL.Query().Get("host"))
	rows := make([][]string, 0, 64)
	var before int64
	for {
		var (
			events []account.ConnectionEvent
			err    error
		)
		if host != "" {
			events, err = s.cfg.Accounts.ConnectionsForHostBefore(host, before, exportBatch)
		} else {
			events, err = s.cfg.Accounts.ConnectionsForBefore(username, before, exportBatch)
		}
		if err != nil {
			s.cfg.Logger.Error("connection export query", "error", err)
			writeError(w, http.StatusInternalServerError, "could not read the connection log")
			return
		}
		for _, event := range events {
			rows = append(rows, []string{
				strconv.FormatInt(event.ID, 10),
				event.StartedAt.UTC().Format(time.RFC3339Nano),
				event.EndedAt.UTC().Format(time.RFC3339Nano),
				strconv.FormatInt(event.Duration().Milliseconds(), 10),
				event.Username,
				event.PeerID,
				event.HostID,
				event.AuditRef,
				event.Source,
				event.Direction,
				event.ClientIP,
				strconv.FormatBool(event.Success),
				event.Detail,
			})
		}
		if len(events) < exportBatch {
			break
		}
		before = events[len(events)-1].ID
	}
	s.writeCSV(w, "chast-connections", []string{
		"id", "started_at", "ended_at", "duration_ms", "username", "peer_id",
		"host_id", "audit_ref", "source", "direction", "client_ip", "success", "detail",
	}, rows)
}

func (s *server) fileTransfersCSV(w http.ResponseWriter, r *http.Request) {
	username := r.URL.Query().Get("username")
	host := strings.TrimSpace(r.URL.Query().Get("host"))
	rows := make([][]string, 0, 64)
	var before int64
	for {
		var (
			events []account.FileEvent
			err    error
		)
		if host != "" {
			events, err = s.cfg.Accounts.FileTransfersForHostBefore(host, before, exportBatch)
		} else {
			events, err = s.cfg.Accounts.FileTransfersForBefore(username, before, exportBatch)
		}
		if err != nil {
			s.cfg.Logger.Error("file transfer export query", "error", err)
			writeError(w, http.StatusInternalServerError, "could not read the file transfer log")
			return
		}
		for _, event := range events {
			rows = append(rows, []string{
				strconv.FormatInt(event.ID, 10),
				event.OccurredAt.UTC().Format(time.RFC3339Nano),
				event.Username,
				event.PeerID,
				event.HostID,
				event.AuditRef,
				event.Source,
				event.Direction,
				event.Name,
				strconv.FormatInt(event.Size, 10),
				strconv.Itoa(event.Files),
				event.ClientIP,
				strconv.FormatBool(event.Success),
				event.Detail,
			})
		}
		if len(events) < exportBatch {
			break
		}
		before = events[len(events)-1].ID
	}
	s.writeCSV(w, "chast-file-transfers", []string{
		"id", "occurred_at", "username", "peer_id", "host_id", "audit_ref",
		"source", "direction", "name", "size", "files", "client_ip", "success", "detail",
	}, rows)
}
