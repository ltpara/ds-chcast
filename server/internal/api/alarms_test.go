package api_test

import (
	"encoding/json"
	"net/http"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type alarmListReply struct {
	Events []struct {
		ID            int64  `json:"id"`
		CreatedAtMS   int64  `json:"created_at_ms"`
		Username      string `json:"username"`
		HostID        string `json:"host_id"`
		Category      string `json:"category"`
		Severity      string `json:"severity"`
		Code          string `json:"code"`
		Summary       string `json:"summary"`
		SourceAction  string `json:"source_action"`
		SourceAuditID int64  `json:"source_audit_id"`
	} `json:"events"`
	NextBeforeID int64 `json:"next_before_id"`
}

func TestAlarmReportClassifiesSecurityEventsAndDeduplicatesHostRetries(t *testing.T) {
	s := testutil.Start(t)
	at := time.Date(2026, 11, 24, 9, 30, 0, 0, time.UTC)
	denial := account.AuditEvent{
		CreatedAt: at, Username: "-", Action: account.AuditHostDenied,
		Detail: "device access was refused", HostID: "host-lab", ClientIP: "192.0.2.4",
		EventKey: "stable-host-denial-1",
	}
	if err := s.Accounts.AppendAudit(denial); err != nil {
		t.Fatalf("append host denial: %v", err)
	}
	denial.Detail = "retry with changed transport timestamp"
	denial.CreatedAt = at.Add(time.Second)
	if err := s.Accounts.AppendAudit(denial); err != nil {
		t.Fatalf("append retried host denial: %v", err)
	}
	if err := s.Accounts.AppendAudit(account.AuditEvent{
		CreatedAt: at.Add(2 * time.Second), Username: "operator", Action: account.AuditEmergencyAccess,
		Detail: "granted emergency access to host-lab", Success: true,
	}); err != nil {
		t.Fatalf("append emergency access: %v", err)
	}

	status, body := request(t, http.MethodGet,
		s.URL+"/v1/alarms?category=security&severity=warning&host=HOST-LAB&limit=1", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("list filtered alarms: %d %s", status, body)
	}
	var first alarmListReply
	if err := json.Unmarshal(body, &first); err != nil {
		t.Fatalf("decode alarm page: %v", err)
	}
	if len(first.Events) != 1 || first.Events[0].Code != "host_access_denied" ||
		first.Events[0].SourceAction != account.AuditHostDenied || first.Events[0].HostID != "host-lab" ||
		first.Events[0].Severity != "warning" || first.Events[0].CreatedAtMS != at.UnixMilli() {
		t.Fatalf("filtered alarm = %+v", first.Events)
	}
	if first.NextBeforeID != first.Events[0].ID {
		t.Fatalf("full-page cursor = %d, want %d", first.NextBeforeID, first.Events[0].ID)
	}
	status, body = request(t, http.MethodGet,
		s.URL+"/v1/alarms?category=security&severity=warning&host=host-lab&limit=1&before_id="+
			strconv.FormatInt(first.NextBeforeID, 10), "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("read alarm after cursor: %d %s", status, body)
	}
	var afterCursor alarmListReply
	if err := json.Unmarshal(body, &afterCursor); err != nil || len(afterCursor.Events) != 0 {
		t.Fatalf("alarm after last cursor = %+v, decode error %v", afterCursor.Events, err)
	}

	status, body = request(t, http.MethodGet,
		s.URL+"/v1/alarms?category=access&severity=notice", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("list access alarms: %d %s", status, body)
	}
	var access alarmListReply
	if err := json.Unmarshal(body, &access); err != nil || len(access.Events) != 1 ||
		access.Events[0].Code != "emergency_access_granted" || access.Events[0].Username != "operator" {
		t.Fatalf("access alarm = %+v, decode error %v", access.Events, err)
	}

	rows := exportCSV(t, s.URL+"/v1/alarms?format=csv&code=host_access_denied")
	if len(rows) != 2 || rows[1][4] != "host_access_denied" || rows[1][10] != account.AuditHostDenied {
		t.Fatalf("alarm CSV = %v", rows)
	}
}

func TestAlarmReportRequiresAuditAccessAndHonorsGroupScope(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("root", "admin password"); err != nil {
		t.Fatalf("create initial administrator: %v", err)
	}
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create group: %v", err)
	}
	if _, err := s.Accounts.AddGroupMember("lab", alice.Username, account.RoleReadonly); err != nil {
		t.Fatalf("add alice to group: %v", err)
	}
	if err := s.Accounts.AppendAudit(account.AuditEvent{
		Username: "alice", Action: account.AuditEmergencyAccess, Detail: "alice scope", Success: true,
	}); err != nil {
		t.Fatalf("append alice alarm source: %v", err)
	}
	if err := s.Accounts.AppendAudit(account.AuditEvent{
		Username: "bob", Action: account.AuditEmergencyAccess, Detail: "bob scope", Success: true,
	}); err != nil {
		t.Fatalf("append bob alarm source: %v", err)
	}
	aliceSession := login(t, s.URL, "alice", "correct horse")
	if status, body := request(t, http.MethodGet, s.URL+"/v1/alarms", "", aliceSession.Token); status != http.StatusForbidden {
		t.Fatalf("unscoped alarm access: %d %s", status, body)
	}
	status, body := request(t, http.MethodGet, s.URL+"/v1/alarms?group=lab", "", aliceSession.Token)
	if status != http.StatusOK {
		t.Fatalf("group alarm access: %d %s", status, body)
	}
	var scoped alarmListReply
	if err := json.Unmarshal(body, &scoped); err != nil {
		t.Fatalf("decode scoped alarms: %v", err)
	}
	if len(scoped.Events) != 1 || scoped.Events[0].Username != "alice" {
		t.Fatalf("group-scoped alarms = %+v", scoped.Events)
	}
}
