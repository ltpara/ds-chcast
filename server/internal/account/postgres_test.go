package account

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/authlimit"
)

// TestPostgresStore exercises the PostgreSQL backend end to end. It is gated
// on CHAST_TEST_POSTGRES_DSN so the default SQLite test run stays self-contained.
func TestPostgresStore(t *testing.T) {
	dsn := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	username := "PgUser" + strconv.FormatInt(time.Now().UnixNano(), 36)
	const password = "correct horse battery"
	user, err := store.CreateUser(username, password)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := store.DeleteUser(username); err != nil {
			t.Errorf("delete user: %v", err)
		}
	}()

	// The username column keeps SQLite's NOCASE semantics through the ICU
	// collation, so a lowercase login must find the mixed-case account.
	logged, token, _, err := store.Login(strings.ToLower(username), password)
	if err != nil {
		t.Fatalf("case-insensitive login: %v", err)
	}
	if logged.ID != user.ID {
		t.Fatalf("logged in as %s, want %s", logged.ID, user.ID)
	}

	if _, _, _, err := store.Login(username, "wrong password"); err == nil {
		t.Fatal("wrong password was accepted")
	}

	name := "PostgreSQL peer"
	if _, err := store.UpdatePeer(user.ID, "123456789", &name, nil); err != nil {
		t.Fatal(err)
	}
	snapshot, err := store.Snapshot(user.ID, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(snapshot.Peers) != 1 || snapshot.Peers[0].Name != name {
		t.Fatalf("snapshot: %+v", snapshot)
	}

	start := time.Now().UTC().Add(-time.Minute)
	connection, err := store.RecordConnection(user.ID, username, "123456789", "viewer",
		"192.0.2.10", "Testland", RoutePunch, "abcdef0123456789",
		start, time.Now().UTC(), true, "test")
	if err != nil {
		t.Fatal(err)
	}
	if connection.ID == 0 {
		t.Fatal("connection event has no id")
	}
	if connection.AuditRef != "abcdef0123456789" {
		t.Fatalf("audit ref = %q", connection.AuditRef)
	}
	transfer, err := store.RecordFileTransfer(user.ID, username, "123456789", "upload",
		"report.txt", "192.0.2.10", "Testland", RouteRelay, "abcdef0123456789",
		42, 1, time.Now().UTC(), true, "test")
	if err != nil {
		t.Fatal(err)
	}
	if transfer.ID == 0 {
		t.Fatal("file event has no id")
	}
	connections, err := store.ConnectionsFor(username, 10)
	if err != nil || len(connections) != 1 {
		t.Fatalf("connections: %v %+v", err, connections)
	}
	transfers, err := store.FileTransfersFor(username, 10)
	if err != nil || len(transfers) != 1 {
		t.Fatalf("transfers: %v %+v", err, transfers)
	}
	archivedConnections, err := store.ConnectionsAfter(0, 50)
	if err != nil {
		t.Fatal(err)
	}
	archivedTransfers, err := store.FileTransfersAfter(0, 50)
	if err != nil {
		t.Fatal(err)
	}
	foundArchived := false
	for _, event := range archivedTransfers {
		if event.ID == transfer.ID {
			foundArchived = true
			break
		}
	}
	if !foundArchived || len(archivedConnections) == 0 {
		t.Fatalf("archive read: %d connections, %d transfers", len(archivedConnections), len(archivedTransfers))
	}
	if err := store.AdvanceArchiveCheckpoint("postgres-"+username, connection.ID); err != nil {
		t.Fatal(err)
	}

	if err := store.AppendAudit(AuditEvent{
		Username: username, Action: AuditLogin, ClientIP: "192.0.2.10", Success: true,
	}); err != nil {
		t.Fatal(err)
	}
	events, err := store.AuditEvents(50)
	if err != nil {
		t.Fatal(err)
	}
	audited := false
	for _, event := range events {
		if event.Username == username && event.Action == AuditLogin {
			audited = event.Success
			break
		}
	}
	if !audited {
		t.Fatalf("audit event for %s is missing: %+v", username, events)
	}
	denial := AuditEvent{Username: username, Action: AuditHostDenied, HostID: username,
		EventKey: "pg-denial-" + username, Detail: "refused", ClientIP: "192.0.2.10"}
	for i := 0; i < 3; i++ {
		if err := store.AppendAudit(denial); err != nil {
			t.Fatalf("denial retry: %v", err)
		}
	}
	denials, err := store.AuditEventsFor(AuditHostDenied, 50)
	if err != nil {
		t.Fatal(err)
	}
	matching := 0
	for _, event := range denials {
		if event.EventKey == denial.EventKey {
			matching++
			if event.HostID != denial.HostID {
				t.Fatalf("denial host identity: %+v", event)
			}
		}
	}
	if matching != 1 {
		t.Fatalf("denial retries stored %d rows, want 1", matching)
	}

	firstID := fmt.Sprintf("%032x", time.Now().UnixNano())
	secondID := fmt.Sprintf("%032x", time.Now().UnixNano()+1)
	for i, id := range []string{firstID, secondID} {
		if _, err := store.AddRecording(Recording{
			ID: id, UserID: user.ID, Name: "recording.mp4", Size: int64(100 * (i + 1)),
			SHA256: "digest", CreatedAtMS: time.Now().UTC().UnixMilli(),
		}); err != nil {
			t.Fatal(err)
		}
	}
	byID, err := store.RecordingByID(firstID)
	if err != nil {
		t.Fatal(err)
	}
	if byID.Username != username || byID.Size != 100 {
		t.Fatalf("recording by id: %+v", byID)
	}
	page, err := store.AllRecordings(username, "", 0, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(page) != 1 || page[0].ID != secondID || page[0].Seq == 0 {
		t.Fatalf("first recording page: %+v", page)
	}
	older, err := store.AllRecordings(username, "", page[0].Seq, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(older) != 1 || older[0].ID != firstID {
		t.Fatalf("older recording page: %+v", older)
	}
	bytes, files, err := store.RecordingTotals(username, "")
	if err != nil {
		t.Fatal(err)
	}
	if bytes != 300 || files != 2 {
		t.Fatalf("recording totals = %d bytes, %d files", bytes, files)
	}

	record, err := store.UpdateLoginLock(username+"|192.0.2.10",
		func(record authlimit.Record, found bool) authlimit.Record {
			if !found {
				record.WindowStart = time.Now().UTC()
			}
			record.Failures++
			record.LastSeen = time.Now().UTC()
			return record
		})
	if err != nil {
		t.Fatal(err)
	}
	if record.Failures != 1 {
		t.Fatalf("login lock failures = %d", record.Failures)
	}
	if _, err := store.ClearLoginLocks(username); err != nil {
		t.Fatal(err)
	}

	if err := store.Logout(token); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Authenticate(token); err == nil {
		t.Fatal("token was accepted after logout")
	}
}

// TestPostgresConcurrentLoginLocks runs two stores against one database. The
// pool is no longer capped at one connection, so the advisory lock, not the
// pool, must serialise the read-modify-write on the shared key.
func TestPostgresConcurrentLoginLocks(t *testing.T) {
	dsn := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	first, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()

	key := "concurrent-" + strconv.FormatInt(time.Now().UnixNano(), 36) + "|192.0.2.1"
	defer first.DeleteLoginLock(key)
	const workers, perWorker = 8, 10
	var wg sync.WaitGroup
	failures := make(chan error, workers)
	for i := 0; i < workers; i++ {
		store := first
		if i%2 == 1 {
			store = second
		}
		wg.Add(1)
		go func(store *Store) {
			defer wg.Done()
			for j := 0; j < perWorker; j++ {
				if _, err := store.UpdateLoginLock(key, func(record authlimit.Record, found bool) authlimit.Record {
					if !found {
						record.WindowStart = time.Now().UTC()
					}
					record.Failures++
					record.LastSeen = time.Now().UTC()
					return record
				}); err != nil {
					failures <- err
					return
				}
			}
		}(store)
	}
	wg.Wait()
	close(failures)
	for err := range failures {
		t.Fatal(err)
	}
	record, found, err := first.LoadLoginLock(key)
	if err != nil {
		t.Fatal(err)
	}
	if !found || record.Failures != workers*perWorker {
		t.Fatalf("failures = %d (found %v), want %d", record.Failures, found, workers*perWorker)
	}
}

// TestPostgresSchemaMigrationBackfillsSessionActivity exercises the v17 to v18
// application migration on a real PostgreSQL database. The SQLite migration
// has an equivalent raw-database test, but PostgreSQL needs its own ALTER and
// backfill syntax verified against a real server.
func TestPostgresSchemaMigrationBackfillsSessionActivity(t *testing.T) {
	dsn := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	username := "PgMigrate" + strconv.FormatInt(time.Now().UnixNano(), 36)
	const password = "correct horse battery"
	user, err := store.CreateUser(username, password)
	if err != nil {
		store.Close()
		t.Fatal(err)
	}
	_, token, _, err := store.Login(username, password)
	if err != nil {
		store.DeleteUser(username)
		store.Close()
		t.Fatal(err)
	}
	var createdAt int64
	if err := store.db.QueryRow(
		`SELECT created_at FROM user_tokens WHERE token_hash = ?`, tokenHash(token),
	).Scan(&createdAt); err != nil {
		store.DeleteUser(username)
		store.Close()
		t.Fatalf("read login creation time: %v", err)
	}
	for _, statement := range []string{
		`ALTER TABLE user_tokens DROP COLUMN IF EXISTS login_at,
			DROP COLUMN IF EXISTS last_seen_at`,
		`DELETE FROM schema_migrations WHERE version = 18`,
	} {
		if _, err := store.db.Exec(statement); err != nil {
			store.DeleteUser(username)
			store.Close()
			t.Fatalf("seed pre-v18 database: %v", err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	store, err = OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	defer store.DeleteUser(username)
	var loginAt, lastSeenAt int64
	if err := store.db.QueryRow(
		`SELECT login_at, last_seen_at FROM user_tokens WHERE token_hash = ?`, tokenHash(token),
	).Scan(&loginAt, &lastSeenAt); err != nil {
		t.Fatalf("read migrated activity: %v", err)
	}
	if loginAt != createdAt || lastSeenAt != createdAt {
		t.Fatalf("migrated activity = %d/%d, want %d/%d",
			loginAt, lastSeenAt, createdAt, createdAt)
	}
	sessions, _, err := store.SessionsPage(user.ID, token, 10, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(sessions) != 1 || !sessions[0].CreatedAt.Equal(timeFromMillis(createdAt)) ||
		!sessions[0].LastSeenAt.Equal(timeFromMillis(createdAt)) {
		t.Fatalf("migrated sessions = %+v", sessions)
	}
}

// TestPostgresDevicePolicyReceipt exercises both branches of receipt storage
// against PostgreSQL, where applied is a BOOLEAN rather than SQLite's INTEGER.
func TestPostgresDevicePolicyReceipt(t *testing.T) {
	dsn := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	peerID := "pgreceipt" + strconv.FormatInt(time.Now().UnixNano(), 36)
	username := "PgReceiptUser" + strconv.FormatInt(time.Now().UnixNano(), 36)
	user, err := store.CreateUser(username, "correct horse battery")
	if err != nil {
		t.Fatal(err)
	}
	defer store.DeleteUser(username)
	policy, err := store.SetDevicePolicy(peerID, DevicePolicyOpen, 15, nil, nil, "postgres-test")
	if err != nil {
		t.Fatal(err)
	}
	defer store.DeleteDevicePolicy(peerID)

	for _, applied := range []bool{true, false} {
		detail := "PostgreSQL receipt probe"
		receipt, err := store.RecordDevicePolicyReceipt(peerID, policy.PolicyVersion, applied, detail)
		if err != nil {
			t.Fatalf("record applied=%v: %v", applied, err)
		}
		if receipt.PolicyVersion != policy.PolicyVersion || receipt.Applied != applied ||
			receipt.Detail != detail || receipt.ReportedAt.IsZero() {
			t.Fatalf("recorded receipt for applied=%v: %+v", applied, receipt)
		}
		stored, managed, err := store.DevicePolicy(peerID)
		if err != nil {
			t.Fatalf("read applied=%v: %v", applied, err)
		}
		if !managed || stored.Receipt == nil || stored.Receipt.Applied != applied ||
			stored.Receipt.PolicyVersion != policy.PolicyVersion || stored.Receipt.Detail != detail {
			t.Fatalf("stored receipt for applied=%v: managed=%v policy=%+v", applied, managed, stored)
		}
	}

	if _, err := store.SetDevicePolicy(peerID, DevicePolicyRestricted, 15,
		[]string{username}, nil, "postgres-test"); err != nil {
		t.Fatal(err)
	}
	admission, err := store.CheckDeviceAdmission(peerID, user.ID)
	if err != nil {
		t.Fatalf("restricted admission: %v", err)
	}
	if !admission.Allowed || admission.Reason != "user allowed" {
		t.Fatalf("restricted admission = %+v", admission)
	}
	if _, err := store.SetBanned(username, true); err != nil {
		t.Fatal(err)
	}
	admission, err = store.CheckDeviceAdmission(peerID, user.ID)
	if err != nil {
		t.Fatalf("banned admission: %v", err)
	}
	if admission.Allowed || admission.Reason != "account disabled" {
		t.Fatalf("banned admission = %+v", admission)
	}
}

// TestPostgresAccountGroups runs the group CRUD, quota and scoped-history
// paths against PostgreSQL. The FOR UPDATE quota lock and the group event
// subqueries are the parts most likely to differ from SQLite.
func TestPostgresAccountGroups(t *testing.T) {
	dsn := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	suffix := strconv.FormatInt(time.Now().UnixNano(), 36)
	alice, err := store.CreateUser("PgGroupA"+suffix, "correct horse battery")
	if err != nil {
		t.Fatal(err)
	}
	bob, err := store.CreateUser("PgGroupB"+suffix, "correct horse battery")
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := store.DeleteUser(bob.Username); err != nil {
			t.Errorf("delete bob: %v", err)
		}
		if err := store.DeleteUser(alice.Username); err != nil {
			t.Errorf("delete alice: %v", err)
		}
	}()
	groupID := "pg" + suffix
	if _, err := store.CreateGroup(groupID, "PostgreSQL", "", 2, 1); err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := store.DeleteAccountGroup(groupID); err != nil {
			t.Errorf("delete group: %v", err)
		}
	}()
	if _, err := store.AddGroupMember(groupID, bob.Username, RoleReadonly); err != nil {
		t.Fatal(err)
	}
	if _, err := store.AddGroupMember(groupID, alice.Username, RoleAdmin); err != nil {
		t.Fatal(err)
	}
	if _, err := store.AddGroupMember(groupID, alice.Username, RoleAdmin); !errors.Is(err, ErrGroupMemberExists) {
		t.Fatalf("duplicate member = %v", err)
	}
	if _, err := store.AddGroupMember(groupID, bob.Username, RoleAdmin); !errors.Is(err, ErrGroupMemberExists) {
		t.Fatalf("duplicate bob = %v", err)
	}
	if _, err := store.AddGroupDevice(groupID, "pgdev"+suffix, alice.Username); err != nil {
		t.Fatal(err)
	}
	if _, err := store.AddGroupDevice(groupID, "pgdev2"+suffix, alice.Username); !errors.Is(err, ErrGroupDeviceQuota) {
		t.Fatalf("device quota = %v", err)
	}
	if err := store.RemoveGroupMember(groupID, alice.Username); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("last admin removal = %v", err)
	}

	now := time.Now().UTC()
	if _, err := store.RecordConnection(bob.ID, bob.Username, "peer-bob",
		"viewer", "127.0.0.1", "", "", "", now.Add(-time.Minute), now, true, ""); err != nil {
		t.Fatal(err)
	}
	if _, err := store.RecordConnection(alice.ID, alice.Username, "peer-alice",
		"viewer", "127.0.0.1", "", "", "", now.Add(-time.Minute), now, true, ""); err != nil {
		t.Fatal(err)
	}
	if err := store.AppendAudit(AuditEvent{Username: bob.Username, Action: "pg_group_probe"}); err != nil {
		t.Fatal(err)
	}
	if err := store.AppendAudit(AuditEvent{Username: alice.Username, Action: "pg_group_probe"}); err != nil {
		t.Fatal(err)
	}
	events, err := store.AuditEventsForGroupBefore(groupID, "pg_group_probe", 0, 50)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) != 2 {
		t.Fatalf("group audit = %+v", events)
	}
	connections, err := store.ConnectionsForGroupBefore(groupID, "", "", 0, 50)
	if err != nil {
		t.Fatal(err)
	}
	seen := make(map[string]bool)
	for _, event := range connections {
		seen[event.Username] = true
	}
	if len(seen) != 2 || !seen[alice.Username] || !seen[bob.Username] {
		t.Fatalf("group connections = %+v", connections)
	}
	access, err := store.GroupsForUser(bob.ID)
	if err != nil || len(access) != 1 || access[0].Role != RoleReadonly {
		t.Fatalf("bob groups = %+v, %v", access, err)
	}
}
