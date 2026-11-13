package api_test

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type groupTransferReply struct {
	Format  string `json:"format"`
	Version int    `json:"version"`
	Groups  []struct {
		ID       string `json:"id"`
		Name     string `json:"name"`
		ParentID string `json:"parent_id"`
		Position int64  `json:"position"`
	} `json:"groups"`
}

type groupImportReply struct {
	Revision  int64        `json:"revision"`
	Created   []groupReply `json:"created"`
	Updated   []groupReply `json:"updated"`
	Skipped   []string     `json:"skipped"`
	Unchanged []groupReply `json:"unchanged"`
}

func TestAddressBookGroupExportImportAPI(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := s.Accounts.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	aliceLogin := login(t, s.URL, "alice", "correct horse")
	bobLogin := login(t, s.URL, "bob", "correct horse")
	if _, err := s.Accounts.UpdateGroupAt(alice.ID, "root", stringPointer("Root"), nil, int64Pointer(1), 0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.UpdateGroupAt(alice.ID, "child", stringPointer("Child"), stringPointer("root"), int64Pointer(2), 0); err != nil {
		t.Fatal(err)
	}

	if status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook/groups/export", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("export without token: %d %s", status, body)
	}
	status, body := request(t, http.MethodGet, s.URL+"/v1/addressbook/groups/export", "", aliceLogin.Token)
	if status != http.StatusOK {
		t.Fatalf("export: %d %s", status, body)
	}
	var exported groupTransferReply
	if err := json.Unmarshal(body, &exported); err != nil {
		t.Fatalf("export reply: %v", err)
	}
	if exported.Format != account.AddressBookGroupDocumentFormat || exported.Version != account.AddressBookGroupDocumentVersion {
		t.Fatalf("export envelope = %+v", exported)
	}
	if len(exported.Groups) != 2 || exported.Groups[0].ID != "root" || exported.Groups[1].ID != "child" ||
		exported.Groups[1].ParentID != "root" {
		t.Fatalf("export groups = %+v", exported.Groups)
	}
	if strings.Contains(string(body), "revision") || strings.Contains(string(body), "updated_at_ms") {
		t.Fatalf("export leaked local metadata: %s", body)
	}

	importBody, err := json.Marshal(map[string]any{
		"format":   exported.Format,
		"version":  exported.Version,
		"conflict": account.AddressBookGroupImportConflictReject,
		"groups":   exported.Groups,
	})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", string(importBody), ""); status != http.StatusUnauthorized {
		t.Fatalf("import without token: %d %s", status, body)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", string(importBody), bobLogin.Token)
	if status != http.StatusOK {
		t.Fatalf("import: %d %s", status, body)
	}
	var imported groupImportReply
	if err := json.Unmarshal(body, &imported); err != nil {
		t.Fatalf("import reply: %v", err)
	}
	if imported.Revision <= 0 || len(imported.Created) != 2 || len(imported.Updated) != 0 ||
		len(imported.Skipped) != 0 || len(imported.Unchanged) != 0 {
		t.Fatalf("import result = %+v", imported)
	}
	if imported.Created[0].Revision != imported.Revision || imported.Created[1].Revision != imported.Revision {
		t.Fatalf("created revisions = %+v", imported.Created)
	}
	bobBook, err := s.Accounts.Snapshot(bob.ID, 0)
	if err != nil || len(bobBook.Groups) != 2 {
		t.Fatalf("bob book = %+v, %v", bobBook, err)
	}
	aliceBook, err := s.Accounts.Snapshot(alice.ID, 0)
	if err != nil || len(aliceBook.Groups) != 2 {
		t.Fatalf("alice book = %+v, %v", aliceBook, err)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", string(importBody), bobLogin.Token)
	if status != http.StatusConflict {
		t.Fatalf("reject existing ids: %d %s", status, body)
	}
	events, err := s.Accounts.AuditEventsFor(account.AuditAddressBookGroupImport, 20)
	if err != nil || len(events) != 1 || events[0].Username != "bob" {
		t.Fatalf("import audit = %+v, %v", events, err)
	}
}

func TestAddressBookGroupImportUpdateSkipAndStrictJSON(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	reply := login(t, s.URL, "alice", "correct horse")
	if _, err := s.Accounts.UpdateGroupAt(alice.ID, "same", stringPointer("Same"), nil, nil, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.UpdateGroupAt(alice.ID, "changed", stringPointer("Old"), nil, int64Pointer(1), 0); err != nil {
		t.Fatal(err)
	}

	updateBody := `{
		"format":"chast-addressbook-groups","version":1,"conflict":"update",
		"groups":[
			{"id":"new","name":"New","parent_id":"same","position":2},
			{"id":"same","name":"Same","parent_id":"","position":0},
			{"id":"changed","name":"Changed","parent_id":"same","position":3}
		]
	}`
	status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", updateBody, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("update import: %d %s", status, body)
	}
	var result groupImportReply
	if err := json.Unmarshal(body, &result); err != nil {
		t.Fatalf("update reply: %v", err)
	}
	if len(result.Created) != 1 || len(result.Updated) != 1 || len(result.Unchanged) != 1 || result.Revision <= 0 {
		t.Fatalf("update result = %+v", result)
	}
	if result.Unchanged[0].ID != "same" || result.Updated[0].ID != "changed" || result.Created[0].ID != "new" {
		t.Fatalf("update classification = %+v", result)
	}

	skipBody := `{
		"format":"chast-addressbook-groups","version":1,"conflict":"skip",
		"groups":[{"id":"changed","name":"Ignored","parent_id":"","position":9}]
	}`
	status, body = request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", skipBody, reply.Token)
	if status != http.StatusOK {
		t.Fatalf("skip import: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &result); err != nil {
		t.Fatalf("skip reply: %v", err)
	}
	if len(result.Skipped) != 1 || result.Skipped[0] != "changed" || result.Revision != 0 {
		t.Fatalf("skip result = %+v", result)
	}

	before, err := s.Accounts.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatal(err)
	}
	invalidBodies := []string{
		`{"format":"other","version":1,"groups":[]}`,
		`{"format":"chast-addressbook-groups","version":2,"groups":[]}`,
		`{"format":"chast-addressbook-groups","version":1}`,
		`{"format":"chast-addressbook-groups","version":1,"groups":[],"extra":true}`,
		`{"format":"chast-addressbook-groups","version":1,"groups":[{"id":"x","name":"X","extra":true}]}`,
		`{"format":"chast-addressbook-groups","version":1,"groups":[]} {}`,
	}
	for i, body := range invalidBodies {
		status, response := request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", body, reply.Token)
		if status != http.StatusBadRequest {
			t.Fatalf("invalid body %d: %d %s", i, status, response)
		}
	}
	oversized := &strings.Builder{}
	oversized.WriteString(`{"format":"chast-addressbook-groups","version":1,"groups":[],"padding":"`)
	oversized.WriteString(strings.Repeat("x", 1<<20))
	oversized.WriteString(`"}`)
	if status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", oversized.String(), reply.Token); status != http.StatusBadRequest {
		t.Fatalf("oversized import: %d %s", status, body)
	}
	tooMany := &strings.Builder{}
	tooMany.WriteString(`{"format":"chast-addressbook-groups","version":1,"groups":[`)
	for i := 0; i <= account.MaxAddressBookGroupImportGroups; i++ {
		if i > 0 {
			tooMany.WriteByte(',')
		}
		fmt.Fprintf(tooMany, `{"id":"g%03d","name":"G%03d","parent_id":"","position":0}`, i, i)
	}
	tooMany.WriteString(`]}`)
	if status, body := request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", tooMany.String(), reply.Token); status != http.StatusBadRequest {
		t.Fatalf("too many groups: %d %s", status, body)
	}
	after, err := s.Accounts.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot after invalid imports: %v", err)
	}
	if after.Revision != before.Revision || len(after.Groups) != len(before.Groups) {
		t.Fatalf("invalid imports wrote data: before=%+v after=%+v", before, after)
	}
}

func TestAddressBookGroupImportRejectIsAtomic(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	reply := login(t, s.URL, "alice", "correct horse")
	if _, err := s.Accounts.UpdateGroupAt(alice.ID, "existing", stringPointer("Existing"), nil, nil, 0); err != nil {
		t.Fatal(err)
	}
	before, err := s.Accounts.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatal(err)
	}

	body := `{
		"format":"chast-addressbook-groups","version":1,"conflict":"reject",
		"groups":[
			{"id":"would-create","name":"Would Create","parent_id":"","position":1},
			{"id":"existing","name":"Conflict","parent_id":"","position":2}
		]
	}`
	status, response := request(t, http.MethodPost, s.URL+"/v1/addressbook/groups/import", body, reply.Token)
	if status != http.StatusConflict {
		t.Fatalf("reject import: %d %s", status, response)
	}
	after, err := s.Accounts.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatal(err)
	}
	if after.Revision != before.Revision || len(after.Groups) != len(before.Groups) {
		t.Fatalf("rejected import wrote data: before=%+v after=%+v", before, after)
	}
	for _, group := range after.Groups {
		if group.ID == "would-create" || group.Name == "Conflict" {
			t.Fatalf("rejected import wrote group: %+v", group)
		}
	}
}

func stringPointer(value string) *string { return &value }
func int64Pointer(value int64) *int64    { return &value }
