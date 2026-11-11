package api

import (
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// reportTimeBounds parses the optional since/until pair shared by the audit,
// connection and file-transfer reports. Values are unix milliseconds or
// RFC 3339 and both edges are inclusive.
func reportTimeBounds(w http.ResponseWriter, r *http.Request) (account.TimeBounds, bool) {
	var bounds account.TimeBounds
	for _, edge := range []struct {
		name string
		dst  *int64
	}{{"since", &bounds.Since}, {"until", &bounds.Until}} {
		raw := strings.TrimSpace(r.URL.Query().Get(edge.name))
		if raw == "" {
			continue
		}
		if ms, err := strconv.ParseInt(raw, 10, 64); err == nil {
			if ms < 0 {
				writeError(w, http.StatusBadRequest, edge.name+" must not be negative")
				return account.TimeBounds{}, false
			}
			*edge.dst = ms
			continue
		}
		stamp, err := time.Parse(time.RFC3339, raw)
		if err != nil {
			writeError(w, http.StatusBadRequest, edge.name+" must be unix milliseconds or RFC3339")
			return account.TimeBounds{}, false
		}
		*edge.dst = stamp.UnixMilli()
	}
	if bounds.Since > 0 && bounds.Until > 0 && bounds.Since > bounds.Until {
		writeError(w, http.StatusBadRequest, "since must not be after until")
		return account.TimeBounds{}, false
	}
	return bounds, true
}
