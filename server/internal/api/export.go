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

// exportBatch is the page size for the audit table. The connection and file
// listings cap a page at 200 rows, so their exports walk in that size; using
// the audit batch there would stop after the first page.
const (
	exportBatch      = 500
	exportEventBatch = 200
)

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

// csvExport streams one history table into a CSV attachment. The status line
// is written only after the first page query succeeds, so a broken store still
// produces a clean 500; once the header is out, a later failure can only be
// logged and the download is cut short.
type csvExport struct {
	s       *server
	w       http.ResponseWriter
	prefix  string
	label   string
	header  []string
	cw      *csv.Writer
	started bool
}

func (s *server) newCSVExport(w http.ResponseWriter, prefix, label string, header []string) *csvExport {
	return &csvExport{s: s, w: w, prefix: prefix, label: label, header: header}
}

func (e *csvExport) begin() bool {
	e.w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	e.w.Header().Set("Content-Disposition",
		fmt.Sprintf(`attachment; filename="%s-%s.csv"`,
			e.prefix, time.Now().UTC().Format("20060102-150405")))
	e.w.WriteHeader(http.StatusOK)
	e.cw = csv.NewWriter(e.w)
	if err := e.cw.Write(e.header); err != nil {
		e.s.cfg.Logger.Error(e.label+" export header", "error", err)
		return false
	}
	e.started = true
	e.cw.Flush()
	if err := e.cw.Error(); err != nil {
		e.s.cfg.Logger.Error(e.label+" export header", "error", err)
		return false
	}
	e.flush()
	return true
}

func (e *csvExport) write(rows [][]string) bool {
	if !e.started && !e.begin() {
		return false
	}
	for _, row := range rows {
		safe := make([]string, len(row))
		for i, cell := range row {
			safe[i] = csvSafe(cell)
		}
		if err := e.cw.Write(safe); err != nil {
			e.s.cfg.Logger.Error(e.label+" export write", "error", err)
			return false
		}
	}
	e.cw.Flush()
	if err := e.cw.Error(); err != nil {
		e.s.cfg.Logger.Error(e.label+" export flush", "error", err)
		return false
	}
	e.flush()
	return true
}

// csvSafe defuses spreadsheet formula injection: a leading apostrophe makes
// Excel and LibreOffice treat the cell as literal text. Usernames, peer ids,
// file names and detail fields all reach this from remote clients. The lone
// "-" placeholder for absent values is left alone because it cannot carry a
// formula.
func csvSafe(cell string) string {
	if cell == "" || cell == "-" {
		return cell
	}
	switch cell[0] {
	case '=', '+', '-', '@', '\t', '\r':
		return "'" + cell
	}
	return cell
}

func (e *csvExport) flush() {
	if f, ok := e.w.(http.Flusher); ok {
		f.Flush()
	}
}

// fail reports a query error before the stream started and only records it
// afterwards, because the response status is already on the wire.
func (e *csvExport) fail(err error, message string) {
	e.s.cfg.Logger.Error(e.label+" export query", "error", err)
	if !e.started {
		writeError(e.w, http.StatusInternalServerError, message)
	}
}

// done emits a header-only file when no row matched, so an empty export is
// still a valid CSV document.
func (e *csvExport) done() {
	if !e.started {
		e.begin()
	}
}

func (s *server) auditCSV(w http.ResponseWriter, r *http.Request) {
	action := r.URL.Query().Get("action")
	user := strings.TrimSpace(r.URL.Query().Get("user"))
	group := strings.TrimSpace(r.URL.Query().Get("group"))
	bounds, ok := reportTimeBounds(w, r)
	if !ok {
		return
	}
	export := s.newCSVExport(w, "chast-audit", "audit", []string{
		"id", "created_at", "username", "action", "detail",
		"client_ip", "location", "success", "host_id",
	})
	var before int64
	for {
		var (
			events []account.AuditEvent
			err    error
		)
		if group != "" {
			events, err = s.cfg.Accounts.AuditEventsForGroupBeforeRange(group, action, user, bounds, before, exportBatch)
		} else {
			events, err = s.cfg.Accounts.AuditEventsForBeforeRange(action, user, bounds, before, exportBatch)
		}
		if err != nil {
			export.fail(err, "could not read the audit log")
			return
		}
		rows := make([][]string, 0, len(events))
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
				event.HostID,
			})
		}
		if !export.write(rows) {
			return
		}
		if len(events) < exportBatch {
			break
		}
		before = events[len(events)-1].ID
	}
	export.done()
}

func (s *server) connectionsCSV(w http.ResponseWriter, r *http.Request) {
	username := r.URL.Query().Get("username")
	host := strings.TrimSpace(r.URL.Query().Get("host"))
	group := strings.TrimSpace(r.URL.Query().Get("group"))
	bounds, ok := reportTimeBounds(w, r)
	if !ok {
		return
	}
	export := s.newCSVExport(w, "chast-connections", "connection", []string{
		"id", "started_at", "ended_at", "duration_ms", "username", "peer_id",
		"host_id", "audit_ref", "source", "direction", "client_ip", "location",
		"route", "success", "detail",
	})
	var before int64
	for {
		var (
			events []account.ConnectionEvent
			err    error
		)
		if group != "" {
			events, err = s.cfg.Accounts.ConnectionsForGroupBeforeRange(
				group, username, host, bounds, before, exportEventBatch)
		} else if host != "" {
			events, err = s.cfg.Accounts.ConnectionsForHostBeforeRange(host, bounds, before, exportEventBatch)
		} else {
			events, err = s.cfg.Accounts.ConnectionsForBeforeRange(username, bounds, before, exportEventBatch)
		}
		if err != nil {
			export.fail(err, "could not read the connection log")
			return
		}
		rows := make([][]string, 0, len(events))
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
				event.Location,
				event.Route,
				strconv.FormatBool(event.Success),
				event.Detail,
			})
		}
		if !export.write(rows) {
			return
		}
		if len(events) < exportEventBatch {
			break
		}
		before = events[len(events)-1].ID
	}
	export.done()
}

func (s *server) fileTransfersCSV(w http.ResponseWriter, r *http.Request) {
	username := r.URL.Query().Get("username")
	host := strings.TrimSpace(r.URL.Query().Get("host"))
	group := strings.TrimSpace(r.URL.Query().Get("group"))
	bounds, ok := reportTimeBounds(w, r)
	if !ok {
		return
	}
	export := s.newCSVExport(w, "chast-file-transfers", "file transfer", []string{
		"id", "occurred_at", "username", "peer_id", "host_id", "audit_ref",
		"source", "direction", "name", "size", "files", "client_ip", "location",
		"route", "success", "detail",
	})
	var before int64
	for {
		var (
			events []account.FileEvent
			err    error
		)
		if group != "" {
			events, err = s.cfg.Accounts.FileTransfersForGroupBeforeRange(
				group, username, host, bounds, before, exportEventBatch)
		} else if host != "" {
			events, err = s.cfg.Accounts.FileTransfersForHostBeforeRange(host, bounds, before, exportEventBatch)
		} else {
			events, err = s.cfg.Accounts.FileTransfersForBeforeRange(username, bounds, before, exportEventBatch)
		}
		if err != nil {
			export.fail(err, "could not read the file transfer log")
			return
		}
		rows := make([][]string, 0, len(events))
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
				event.Location,
				event.Route,
				strconv.FormatBool(event.Success),
				event.Detail,
			})
		}
		if !export.write(rows) {
			return
		}
		if len(events) < exportEventBatch {
			break
		}
		before = events[len(events)-1].ID
	}
	export.done()
}
