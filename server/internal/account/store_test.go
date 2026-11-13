package account

import (
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"
)

func openTemp(t *testing.T, name string) *Store {
	t.Helper()
	store, err := Open(filepath.Join(t.TempDir(), name))
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	return store
}

func TestConcurrentOpenSharesFreshDatabase(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	errs := make([]error, 2)
	var wg sync.WaitGroup
	for i := range errs {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			store, err := Open(path)
			if err != nil {
				errs[i] = err
				return
			}
			defer store.Close()
			if _, err := store.CreateUser("alice", "correct horse"); err != nil &&
				!errors.Is(err, ErrUserExists) {
				errs[i] = err
			}
		}(i)
	}
	wg.Wait()
	for i, err := range errs {
		if err != nil {
			t.Fatalf("instance %d: %v", i+1, err)
		}
	}
	store, err := Open(path)
	if err != nil {
		t.Fatalf("reopen shared database: %v", err)
	}
	defer store.Close()
	if user, err := store.FindUser("alice"); err != nil || user.Username != "alice" {
		t.Fatalf("user after concurrent opens = %+v, %v", user, err)
	}
}

func TestUserLifecycle(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	if user.ID == "" || user.Username != "alice" || user.CreatedAt.IsZero() {
		t.Fatalf("unexpected user: %+v", user)
	}
	if found, err := store.FindUser("ALICE"); err != nil || found.ID != user.ID {
		t.Fatalf("find user = %+v, %v", found, err)
	}
	if _, err := store.FindUser("nobody"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("missing user error = %v", err)
	}
	if _, err := store.CreateUser("ALICE", "another password"); !errors.Is(err, ErrUserExists) {
		t.Fatalf("duplicate username error = %v", err)
	}
	if _, err := store.CreateUser("bad name", "correct horse"); !errors.Is(err, ErrInvalidUsername) {
		t.Fatalf("invalid username error = %v", err)
	}
	if _, err := store.CreateUser("bob", "short"); !errors.Is(err, ErrWeakPassword) {
		t.Fatalf("weak password error = %v", err)
	}

	if _, _, _, err := store.Login("alice", "wrong password"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong password error = %v", err)
	}
	if _, _, _, err := store.Login("nobody", "correct horse"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("unknown user error = %v", err)
	}

	logged, token, expires, err := store.Login("alice", "correct horse")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	if logged.ID != user.ID || token == "" || !expires.After(time.Now()) {
		t.Fatalf("unexpected login result: %+v token=%q expires=%v", logged, token, expires)
	}
	current, err := store.Authenticate(token)
	if err != nil || current.ID != user.ID {
		t.Fatalf("authenticate: %+v %v", current, err)
	}
	if err := store.Logout(token); err != nil {
		t.Fatalf("logout: %v", err)
	}
	if _, err := store.Authenticate(token); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("logged-out token error = %v", err)
	}

	_, second, _, err := store.Login("alice", "correct horse")
	if err != nil {
		t.Fatalf("second login: %v", err)
	}
	if err := store.SetPassword("alice", "new correct horse"); err != nil {
		t.Fatalf("set password: %v", err)
	}
	if _, err := store.Authenticate(second); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("token survived password change: %v", err)
	}
	if _, _, _, err := store.Login("alice", "correct horse"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("old password error = %v", err)
	}
	if _, _, _, err := store.Login("alice", "new correct horse"); err != nil {
		t.Fatalf("new password login: %v", err)
	}

	users, err := store.ListUsers()
	if err != nil || len(users) != 1 || users[0].Username != "alice" {
		t.Fatalf("list users = %+v, %v", users, err)
	}
	if err := store.DeleteUser("alice"); err != nil {
		t.Fatalf("delete user: %v", err)
	}
	if err := store.DeleteUser("alice"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("second delete error = %v", err)
	}
	if _, _, _, err := store.Login("alice", "new correct horse"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("deleted user login error = %v", err)
	}
}

func TestTokenExpiry(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	_, token, accessExpires, refresh, refreshExpires, err :=
		store.LoginWithRefresh("alice", "correct horse", "", "", "", "")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	if !accessExpires.Equal(now.Add(AccessTokenTTL)) {
		t.Fatalf("access expiry = %v, want %v", accessExpires, now.Add(AccessTokenTTL))
	}
	if !refreshExpires.Equal(now.Add(TokenTTL)) {
		t.Fatalf("refresh expiry = %v, want %v", refreshExpires, now.Add(TokenTTL))
	}
	now = now.Add(29 * time.Minute)
	if _, err := store.Authenticate(token); err != nil {
		t.Fatalf("access token should still be valid: %v", err)
	}
	now = now.Add(2 * time.Minute)
	if _, err := store.Authenticate(token); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("expired access token error = %v", err)
	}
	_, renewed, renewedExpiry, refresh, renewedRefreshExpiry, err := store.RefreshSession(refresh)
	if err != nil {
		t.Fatalf("refresh after access expiry: %v", err)
	}
	if !renewedExpiry.Equal(now.Add(AccessTokenTTL)) || !renewedRefreshExpiry.Equal(now.Add(TokenTTL)) {
		t.Fatalf("rotated expiries = %v/%v", renewedExpiry, renewedRefreshExpiry)
	}
	if _, err := store.Authenticate(renewed); err != nil {
		t.Fatalf("rotated access token: %v", err)
	}
	now = now.Add(TokenTTL)
	if _, _, _, _, _, err := store.RefreshSession(refresh); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("expired refresh token error = %v", err)
	}
	if _, err := store.Authenticate(renewed); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("access token outlived the session: %v", err)
	}
}

func TestRefreshRotationDetectsReplay(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	_, first, _, refresh, _, err := store.LoginWithRefresh("alice", "correct horse", "", "", "", "")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	now = now.Add(time.Minute)
	_, second, _, rotated, _, err := store.RefreshSession(refresh)
	if err != nil {
		t.Fatalf("rotate: %v", err)
	}
	if second == first || rotated == refresh {
		t.Fatalf("rotation reused a token")
	}
	if _, err := store.Authenticate(first); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("old access token survived rotation: %v", err)
	}
	if _, err := store.Authenticate(second); err != nil {
		t.Fatalf("new access token: %v", err)
	}
	now = now.Add(RefreshReuseGrace + time.Second)
	if _, _, _, _, _, err := store.RefreshSession(refresh); !errors.Is(err, ErrRefreshReused) {
		t.Fatalf("replayed refresh error = %v, want ErrRefreshReused", err)
	}
	if _, err := store.Authenticate(second); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("replay did not revoke the access token: %v", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(rotated); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("replay did not revoke the rotated refresh token: %v", err)
	}
	if _, _, _, _, _, err := store.RefreshSession("  "); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("empty refresh error = %v", err)
	}
}

// TestRefreshReuseGrace covers the lost-response retry the grace window
// exists for, plus the two cases that stay replays: a second use of the same
// retired token and a retired token whose replacement was already spent.
func TestRefreshReuseGrace(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })

	_, _, _, refresh, _, err := store.LoginWithRefresh("alice", "correct horse", "", "", "", "")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	now = now.Add(time.Minute)
	_, _, _, lost, _, err := store.RefreshSession(refresh)
	if err != nil {
		t.Fatalf("rotate: %v", err)
	}
	// The rotation response never reached the viewer; it retries inside the
	// window while the undelivered replacement is still unused.
	now = now.Add(30 * time.Second)
	_, recoveredAccess, _, recovered, _, err := store.RefreshSession(refresh)
	if err != nil {
		t.Fatalf("lost-response retry: %v", err)
	}
	if recovered == refresh || recovered == lost {
		t.Fatalf("lost-response retry reused a token")
	}
	if _, err := store.Authenticate(recoveredAccess); err != nil {
		t.Fatalf("recovered access token: %v", err)
	}
	// The same retired token cannot be spent twice: the recovery moved the
	// session past it.
	now = now.Add(time.Second)
	if _, _, _, _, _, err := store.RefreshSession(refresh); !errors.Is(err, ErrRefreshReused) {
		t.Fatalf("second lost-response retry = %v, want ErrRefreshReused", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(recovered); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("replay did not revoke the recovered token: %v", err)
	}

	// Two clients alternating the two newest tokens must not keep each other
	// alive: the first retry is tolerated, the next replay ends the session.
	_, _, _, chain, _, err := store.LoginWithRefresh("alice", "correct horse", "", "", "", "")
	if err != nil {
		t.Fatalf("third login: %v", err)
	}
	now = now.Add(time.Second)
	_, _, _, replacement, _, err := store.RefreshSession(chain)
	if err != nil {
		t.Fatalf("rotate chain: %v", err)
	}
	now = now.Add(time.Second)
	_, attackerAccess, _, attackerToken, _, err := store.RefreshSession(chain)
	if err != nil {
		t.Fatalf("first in-window retry: %v", err)
	}
	now = now.Add(time.Second)
	if _, _, _, _, _, err := store.RefreshSession(replacement); !errors.Is(err, ErrRefreshReused) {
		t.Fatalf("second in-window replay = %v, want ErrRefreshReused", err)
	}
	if _, err := store.Authenticate(attackerAccess); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("session survived the second replay: %v", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(attackerToken); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("token from the retry after revocation = %v", err)
	}

	// A retired token whose replacement was already used is a replay even
	// inside the window.
	_, _, _, fresh, _, err := store.LoginWithRefresh("alice", "correct horse", "", "", "", "")
	if err != nil {
		t.Fatalf("second login: %v", err)
	}
	now = now.Add(time.Second)
	_, _, _, once, _, err := store.RefreshSession(fresh)
	if err != nil {
		t.Fatalf("rotate fresh: %v", err)
	}
	now = now.Add(time.Second)
	_, _, _, twice, _, err := store.RefreshSession(once)
	if err != nil {
		t.Fatalf("rotate once: %v", err)
	}
	now = now.Add(time.Second)
	if _, _, _, _, _, err := store.RefreshSession(fresh); !errors.Is(err, ErrRefreshReused) {
		t.Fatalf("spent replacement replay = %v, want ErrRefreshReused", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(twice); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("spent-replacement replay kept the session: %v", err)
	}
}

func TestRefreshTokensDieWithTheirSession(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })

	_, accessA, _, refreshA, _, err :=
		store.LoginWithRefresh("alice", "correct horse", "device-a", "Office PC", "", "")
	if err != nil {
		t.Fatalf("login a: %v", err)
	}
	if _, err := store.Authenticate(accessA); err != nil {
		t.Fatalf("access a: %v", err)
	}
	_, accessB, _, refreshB, _, err :=
		store.LoginWithRefresh("alice", "correct horse", "device-b", "Laptop", "", "")
	if err != nil {
		t.Fatalf("login b: %v", err)
	}
	sessions, err := store.Sessions(user.ID, accessB)
	if err != nil || len(sessions) != 2 {
		t.Fatalf("sessions = %+v, %v", sessions, err)
	}
	for _, session := range sessions {
		if session.DeviceID == "device-a" {
			if revoked, err := store.RevokeSession(user.ID, session.ID); err != nil || !revoked {
				t.Fatalf("revoke a = %v, %v", revoked, err)
			}
		}
	}
	if _, _, _, _, _, err := store.RefreshSession(refreshA); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("revoked session refreshed: %v", err)
	}
	if _, nextAccess, _, next, _, err := store.RefreshSession(refreshB); err != nil {
		t.Fatalf("other session refresh: %v", err)
	} else {
		accessB = nextAccess
		refreshB = next
	}
	if err := store.Logout(accessB); err != nil {
		t.Fatalf("logout: %v", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(refreshB); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("logged-out session refreshed: %v", err)
	}

	_, accessC, _, refreshC, _, err :=
		store.LoginWithRefresh("alice", "correct horse", "device-c", "Tablet", "", "")
	if err != nil {
		t.Fatalf("login c: %v", err)
	}
	_, accessD, _, refreshD, _, err :=
		store.LoginWithRefresh("alice", "correct horse", "device-d", "Phone", "", "")
	if err != nil {
		t.Fatalf("login d: %v", err)
	}
	_ = accessC
	if err := store.ChangePassword(user.ID, accessD, "correct horse", "new correct horse"); err != nil {
		t.Fatalf("change password: %v", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(refreshC); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("password change left another refresh token: %v", err)
	}
	if _, _, _, next, _, err := store.RefreshSession(refreshD); err != nil {
		t.Fatalf("current session refresh: %v", err)
	} else {
		refreshD = next
	}
	if err := store.SetPassword("alice", "another password"); err != nil {
		t.Fatalf("set password: %v", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(refreshD); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("set password left a refresh token: %v", err)
	}
}

func TestAddressBookIsPerUserAndPersists(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	name := " 工作室 "
	peer, err := store.UpdatePeer(alice.ID, "studio", &name,
		&[]string{"office", "office", "", "prod"})
	if err != nil {
		t.Fatalf("update peer: %v", err)
	}
	if peer.Name != "工作室" || len(peer.Tags) != 2 || peer.Tags[0] != "office" || peer.Tags[1] != "prod" {
		t.Fatalf("unexpected peer: %+v", peer)
	}

	tags := []string{"lab", "office"}
	peer, err = store.UpdatePeer(alice.ID, "studio", nil, &tags)
	if err != nil {
		t.Fatalf("partial update: %v", err)
	}
	if peer.Name != "工作室" || len(peer.Tags) != 2 || peer.Tags[0] != "lab" || peer.Tags[1] != "office" {
		t.Fatalf("partial update lost name: %+v", peer)
	}

	empty := ""
	if peer, err = store.UpdatePeer(alice.ID, "studio", &empty, nil); err != nil {
		t.Fatalf("clear name: %v", err)
	}
	if peer.Name != "" || len(peer.Tags) != 2 {
		t.Fatalf("clear name result: %+v", peer)
	}

	labName := "Lab"
	if _, err := store.UpdatePeer(alice.ID, "lab", &labName, nil); err != nil {
		t.Fatalf("add lab: %v", err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 2 ||
		peers[0].ID != "lab" || peers[1].ID != "studio" {
		t.Fatalf("alice peers = %+v, %v", peers, err)
	}
	if peers, err := store.Peers(bob.ID); err != nil || len(peers) != 0 {
		t.Fatalf("bob peers = %+v, %v", peers, err)
	}
	if _, err := store.UpdatePeer(alice.ID, "bad id", &name, nil); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatalf("invalid peer error = %v", err)
	}
	if removed, err := store.DeletePeer(alice.ID, "missing"); err != nil || removed {
		t.Fatalf("delete missing = %v, %v", removed, err)
	}
	if removed, err := store.DeletePeer(alice.ID, "studio"); err != nil || !removed {
		t.Fatalf("delete studio = %v, %v", removed, err)
	}

	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}
	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen account store: %v", err)
	}
	defer reopened.Close()
	if peers, err := reopened.Peers(alice.ID); err != nil || len(peers) != 1 || peers[0].ID != "lab" {
		t.Fatalf("persisted peers = %+v, %v", peers, err)
	}
	if _, err := reopened.SetRole("bob", RoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if err := reopened.DeleteUser("alice"); err != nil {
		t.Fatalf("delete alice: %v", err)
	}
	if peers, err := reopened.Peers(alice.ID); err != nil || len(peers) != 0 {
		t.Fatalf("cascade peers = %+v, %v", peers, err)
	}
}

// TestAddressBookNotes covers the free-text note: normalization, partial
// updates, optimistic concurrency, tombstones and the shared-write path.
func TestAddressBookNotes(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	name := "工作区"
	note := "  机柜 A3\t管理员在 4 楼  "
	peer, err := store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{Name: &name, Note: &note}, 0)
	if err != nil {
		t.Fatalf("add peer with note: %v", err)
	}
	// Control characters are dropped and the value is trimmed, so a note can
	// never smuggle a column separator or a newline into a plain-text field.
	if peer.Note != "机柜 A3管理员在 4 楼" {
		t.Fatalf("normalized note = %q", peer.Note)
	}
	firstRevision := peer.Revision

	renamed := "Devbox"
	peer, err = store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{Name: &renamed}, firstRevision)
	if err != nil || peer.Name != "Devbox" || peer.Note != "机柜 A3管理员在 4 楼" {
		t.Fatalf("rename kept note = %+v, %v", peer, err)
	}
	if peer.Revision <= firstRevision {
		t.Fatalf("note edit did not advance revision: %d", peer.Revision)
	}
	currentRevision := peer.Revision

	stale := "覆盖"
	if _, err := store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{Note: &stale}, firstRevision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale note write = %v", err)
	}
	peers, err := store.Peers(alice.ID)
	if err != nil || len(peers) != 1 || peers[0].Note != "机柜 A3管理员在 4 楼" {
		t.Fatalf("peers after conflict = %+v, %v", peers, err)
	}

	long := strings.Repeat("备", maxNote+10)
	peer, err = store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{Note: &long}, currentRevision)
	if err != nil || len([]rune(peer.Note)) != maxNote {
		t.Fatalf("capped note runes = %d, %v", len([]rune(peer.Note)), err)
	}
	currentRevision = peer.Revision

	// A group move must carry the note along with the entry.
	moved, err := store.SetPeerGroupAt(alice.ID, "devbox", "", currentRevision)
	if err != nil || moved.Note != peer.Note {
		t.Fatalf("group move note = %q, %v", moved.Note, err)
	}
	cleared := ""
	if peer, err = store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{Note: &cleared}, moved.Revision); err != nil || peer.Note != "" {
		t.Fatalf("clear note = %+v, %v", peer, err)
	}

	sharedNote := "同事的提醒"
	peer, err = store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{Note: &sharedNote}, peer.Revision)
	if err != nil {
		t.Fatalf("restore note: %v", err)
	}
	if removed, err := store.DeletePeer(alice.ID, "devbox"); err != nil || !removed {
		t.Fatalf("delete peer = %v, %v", removed, err)
	}
	snapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	var tombstone *Peer
	for i := range snapshot.Changes {
		if snapshot.Changes[i].ID == "devbox" {
			tombstone = &snapshot.Changes[i]
		}
	}
	if tombstone == nil || !tombstone.Deleted || tombstone.Note != sharedNote {
		t.Fatalf("tombstone = %+v", tombstone)
	}
	revived, err := store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{Name: &name}, 0)
	if err != nil || revived.Note != sharedNote {
		t.Fatalf("revived peer = %+v, %v", revived, err)
	}

	if _, err := store.ShareBook(alice.ID, "bob", "write"); err != nil {
		t.Fatalf("share book: %v", err)
	}
	shared := "共享备注"
	if _, err := store.UpdateSharedPeerEditAt(bob.ID, "alice", "devbox", PeerEdit{Note: &shared}, revived.Revision); err != nil {
		t.Fatalf("shared note write: %v", err)
	}
	books, err := store.SharedBooks(bob.ID)
	if err != nil || len(books) != 1 || len(books[0].Peers) != 1 || books[0].Peers[0].Note != "共享备注" {
		t.Fatalf("shared note = %+v, %v", books, err)
	}
	added, err := store.AddSharedPeerNoteScoped(bob.ID, "alice", "newbox", "", "新增", nil, "新增备注", 0)
	if err != nil || added.Note != "新增备注" {
		t.Fatalf("shared add with note = %+v, %v", added, err)
	}
}

func TestAddressBookRevisionsAndTombstones(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	name := "Studio"
	peer, err := store.UpdatePeer(alice.ID, "studio", &name, nil)
	if err != nil {
		t.Fatalf("add peer: %v", err)
	}
	if peer.Revision != 1 {
		t.Fatalf("first revision = %d, want 1", peer.Revision)
	}
	snapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	if snapshot.Revision != 1 || len(snapshot.Peers) != 1 || len(snapshot.Changes) != 1 {
		t.Fatalf("first snapshot = %+v", snapshot)
	}

	tags := []string{"office"}
	peer, err = store.UpdatePeer(alice.ID, "studio", nil, &tags)
	if err != nil {
		t.Fatalf("second update: %v", err)
	}
	if peer.Revision != 2 {
		t.Fatalf("second revision = %d, want 2", peer.Revision)
	}
	snapshot, err = store.Snapshot(alice.ID, 1)
	if err != nil {
		t.Fatalf("incremental snapshot: %v", err)
	}
	if snapshot.Revision != 2 || len(snapshot.Changes) != 1 ||
		snapshot.Changes[0].ID != "studio" || snapshot.Changes[0].Revision != 2 {
		t.Fatalf("incremental changes = %+v", snapshot.Changes)
	}

	if removed, err := store.DeletePeer(alice.ID, "studio"); err != nil || !removed {
		t.Fatalf("delete studio = %v, %v", removed, err)
	}
	if removed, err := store.DeletePeer(alice.ID, "studio"); err != nil || removed {
		t.Fatalf("second delete = %v, %v", removed, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 0 {
		t.Fatalf("peers after delete = %+v, %v", peers, err)
	}
	snapshot, err = store.Snapshot(alice.ID, 2)
	if err != nil {
		t.Fatalf("snapshot after delete: %v", err)
	}
	if snapshot.Revision != 3 || len(snapshot.Peers) != 0 || len(snapshot.Changes) != 1 {
		t.Fatalf("snapshot after delete = %+v", snapshot)
	}
	tombstone := snapshot.Changes[0]
	if tombstone.ID != "studio" || !tombstone.Deleted || tombstone.Revision != 3 ||
		tombstone.Name != "Studio" || len(tombstone.Tags) != 1 || tombstone.Tags[0] != "office" {
		t.Fatalf("tombstone = %+v", tombstone)
	}
	// A full history request also surfaces the tombstone; only the current row
	// state is stored per peer, so intermediate edits are folded into it.
	snapshot, err = store.Snapshot(alice.ID, 0)
	if err != nil || len(snapshot.Changes) != 1 || !snapshot.Changes[0].Deleted ||
		snapshot.Changes[0].Revision != 3 {
		t.Fatalf("full history = %+v, %v", snapshot, err)
	}
	// Out-of-range cursors fall back to full history instead of missing deletes.
	for _, since := range []int64{-5, 99} {
		snapshot, err = store.Snapshot(alice.ID, since)
		if err != nil || snapshot.Revision != 3 || len(snapshot.Changes) != 1 ||
			!snapshot.Changes[0].Deleted {
			t.Fatalf("snapshot since %d = %+v, %v", since, snapshot, err)
		}
	}

	// Editing a tombstoned peer revives it with a fresh revision.
	revived, err := store.UpdatePeer(alice.ID, "studio", nil, &tags)
	if err != nil || revived.Revision != 4 || revived.Deleted {
		t.Fatalf("revive peer = %+v, %v", revived, err)
	}
	snapshot, err = store.Snapshot(alice.ID, 3)
	if err != nil || len(snapshot.Peers) != 1 || len(snapshot.Changes) != 1 || snapshot.Changes[0].Deleted {
		t.Fatalf("snapshot after revive = %+v, %v", snapshot, err)
	}

	// Revisions are per user; bob's first write does not advance alice.
	bobName := "Bob box"
	if peer, err := store.UpdatePeer(bob.ID, "bobbox", &bobName, nil); err != nil || peer.Revision != 1 {
		t.Fatalf("bob first peer = %+v, %v", peer, err)
	}
	bobSnapshot, err := store.Snapshot(bob.ID, 0)
	if err != nil || bobSnapshot.Revision != 1 || len(bobSnapshot.Peers) != 1 {
		t.Fatalf("bob snapshot = %+v, %v", bobSnapshot, err)
	}
	if _, err := store.Snapshot("missing-user", 0); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("unknown user snapshot error = %v", err)
	}
}

func TestAddressBookTrashRestoreAndPurge(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	group := "work"
	if _, err := store.UpsertGroup(alice.ID, group, "Work"); err != nil {
		t.Fatalf("create group: %v", err)
	}
	name := "工作室"
	tags := []string{"lab"}
	note := "机柜 A3"
	peer, err := store.UpdatePeerEditAt(alice.ID, "devbox", PeerEdit{
		Name: &name, Tags: &tags, Note: &note, GroupID: &group,
	}, 0)
	if err != nil {
		t.Fatalf("seed peer: %v", err)
	}

	if removed, err := store.DeletePeer(alice.ID, "devbox"); err != nil || !removed {
		t.Fatalf("delete peer = %v, %v", removed, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 0 {
		t.Fatalf("live peers after delete = %+v, %v", peers, err)
	}
	trash, err := store.Trash(alice.ID)
	if err != nil || len(trash) != 1 {
		t.Fatalf("trash = %+v, %v", trash, err)
	}
	entry := trash[0]
	if entry.ID != "devbox" || !entry.Deleted || entry.Name != name ||
		entry.Note != note || len(entry.Tags) != 1 || entry.Tags[0] != "lab" ||
		entry.GroupID != group || entry.Revision == peer.Revision {
		t.Fatalf("trash entry = %+v", entry)
	}

	restored, err := store.RestorePeer(alice.ID, "devbox")
	if err != nil || restored.Deleted || restored.Name != name || restored.Note != note ||
		len(restored.Tags) != 1 || restored.Tags[0] != "lab" ||
		restored.GroupID != group || restored.Revision == entry.Revision {
		t.Fatalf("restored = %+v, %v", restored, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 1 || peers[0].ID != "devbox" {
		t.Fatalf("peers after restore = %+v, %v", peers, err)
	}
	if trash, err := store.Trash(alice.ID); err != nil || len(trash) != 0 {
		t.Fatalf("trash after restore = %+v, %v", trash, err)
	}
	if _, err := store.RestorePeer(alice.ID, "devbox"); !errors.Is(err, ErrUnknownPeer) {
		t.Fatalf("restore live peer = %v", err)
	}
	if _, err := store.RestorePeer(alice.ID, "nope"); !errors.Is(err, ErrUnknownPeer) {
		t.Fatalf("restore missing peer = %v", err)
	}
	if _, err := store.RestorePeer(alice.ID, "bad id"); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatalf("restore invalid id = %v", err)
	}
	// Purging a live entry is refused so the trash API cannot bypass delete.
	if removed, err := store.PurgePeer(alice.ID, "devbox"); err != nil || removed {
		t.Fatalf("purge live peer = %v, %v", removed, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 1 {
		t.Fatalf("peers after refused purge = %+v, %v", peers, err)
	}

	// A group deleted while an entry sat in the trash is dropped on restore.
	if _, err := store.DeletePeer(alice.ID, "devbox"); err != nil {
		t.Fatalf("delete before group removal: %v", err)
	}
	if _, err := store.DeleteGroup(alice.ID, group); err != nil {
		t.Fatalf("delete group: %v", err)
	}
	ungrouped, err := store.RestorePeer(alice.ID, "devbox")
	if err != nil || ungrouped.GroupID != "" || ungrouped.Name != name {
		t.Fatalf("restore with dead group = %+v, %v", ungrouped, err)
	}

	// Purge removes one entry for good; EmptyTrash drops the rest but never
	// touches live entries.
	if removed, err := store.DeletePeer(alice.ID, "devbox"); err != nil || !removed {
		t.Fatalf("delete for purge = %v, %v", removed, err)
	}
	second := "lab"
	if _, err := store.UpdatePeer(alice.ID, "labbox", &second, nil); err != nil {
		t.Fatalf("second peer: %v", err)
	}
	if removed, err := store.DeletePeer(alice.ID, "labbox"); err != nil || !removed {
		t.Fatalf("delete second peer = %v, %v", removed, err)
	}
	if trash, err := store.Trash(alice.ID); err != nil || len(trash) != 2 {
		t.Fatalf("trash before purge = %+v, %v", trash, err)
	}
	if removed, err := store.PurgePeer(alice.ID, "labbox"); err != nil || !removed {
		t.Fatalf("purge peer = %v, %v", removed, err)
	}
	if removed, err := store.PurgePeer(alice.ID, "labbox"); err != nil || removed {
		t.Fatalf("second purge = %v, %v", removed, err)
	}
	kept := "kept"
	keptPeer, err := store.UpdatePeer(alice.ID, "keepbox", &kept, nil)
	if err != nil {
		t.Fatalf("kept peer: %v", err)
	}
	if dropped, err := store.EmptyTrash(alice.ID); err != nil || dropped != 1 {
		t.Fatalf("empty trash = %d, %v", dropped, err)
	}
	if trash, err := store.Trash(alice.ID); err != nil || len(trash) != 0 {
		t.Fatalf("trash after empty = %+v, %v", trash, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 1 ||
		peers[0].ID != keptPeer.ID {
		t.Fatalf("live peers after empty = %+v, %v", peers, err)
	}
	if _, err := store.RestorePeer(alice.ID, "devbox"); !errors.Is(err, ErrUnknownPeer) {
		t.Fatalf("restore purged peer = %v", err)
	}
	// Each user has an independent trash.
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if trash, err := store.Trash(bob.ID); err != nil || len(trash) != 0 {
		t.Fatalf("bob trash = %+v, %v", trash, err)
	}
}

func TestAddressBookGroups(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	if _, err := store.UpsertGroup(alice.ID, "bad id", "工作室"); !errors.Is(err, ErrInvalidGroupID) {
		t.Fatalf("invalid group id error = %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "studio", "   "); !errors.Is(err, ErrInvalidGroupName) {
		t.Fatalf("empty group name error = %v", err)
	}

	group, err := store.UpsertGroup(alice.ID, "studio", " 工作室 ")
	if err != nil || group.ID != "studio" || group.Name != "工作室" ||
		group.Revision != 1 || group.Deleted || group.UpdatedAt.IsZero() {
		t.Fatalf("create group = %+v, %v", group, err)
	}
	if group, err = store.UpsertGroup(alice.ID, "lab", "Lab"); err != nil || group.Revision != 2 {
		t.Fatalf("second group = %+v, %v", group, err)
	}
	renamed, err := store.UpsertGroup(alice.ID, "studio", "Studio")
	if err != nil || renamed.Name != "Studio" || renamed.Revision != 3 {
		t.Fatalf("rename group = %+v, %v", renamed, err)
	}
	if groups, err := store.groupsLocked(alice.ID); err != nil || len(groups) != 2 ||
		groups[0].ID != "lab" || groups[1].ID != "studio" || groups[1].Name != "Studio" {
		t.Fatalf("listed groups = %+v, %v", groups, err)
	}

	studio, lab, missing := "studio", "lab", "missing"
	name := "开发机"
	peer, err := store.UpdatePeerWithGroup(alice.ID, "devbox", &name, nil, &studio)
	if err != nil || peer.GroupID != "studio" || peer.Revision != 4 {
		t.Fatalf("peer with group = %+v, %v", peer, err)
	}
	if _, err := store.UpdatePeerWithGroup(alice.ID, "devbox", nil, nil, &missing); !errors.Is(err, ErrUnknownGroup) {
		t.Fatalf("unknown group on peer write = %v", err)
	}
	labName := "Lab box"
	if peer, err := store.UpdatePeerWithGroup(alice.ID, "labbox", &labName, nil, &lab); err != nil ||
		peer.GroupID != "lab" {
		t.Fatalf("second grouped peer = %+v, %v", peer, err)
	}

	moved, err := store.SetPeerGroup(alice.ID, "labbox", "studio")
	if err != nil || moved.GroupID != "studio" || moved.Name != "Lab box" ||
		moved.Revision != 6 {
		t.Fatalf("move peer = %+v, %v", moved, err)
	}
	again, err := store.SetPeerGroup(alice.ID, "labbox", "studio")
	if err != nil || again.Revision != moved.Revision || again.GroupID != "studio" {
		t.Fatalf("no-op move = %+v, %v", again, err)
	}
	if _, err := store.SetPeerGroup(alice.ID, "missing", "studio"); !errors.Is(err, ErrUnknownPeer) {
		t.Fatalf("move unknown peer = %v", err)
	}
	if _, err := store.SetPeerGroup(alice.ID, "devbox", "bad id"); !errors.Is(err, ErrInvalidGroupID) {
		t.Fatalf("move to invalid group = %v", err)
	}
	cleared, err := store.SetPeerGroup(alice.ID, "devbox", "")
	if err != nil || cleared.GroupID != "" || cleared.Revision != 7 {
		t.Fatalf("clear peer group = %+v, %v", cleared, err)
	}
	if _, err := store.DeleteGroup(alice.ID, "bad id"); !errors.Is(err, ErrInvalidGroupID) {
		t.Fatalf("delete invalid group = %v", err)
	}

	snapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil || snapshot.Revision != 7 || len(snapshot.Groups) != 2 ||
		len(snapshot.GroupChanges) != 2 || snapshot.GroupChanges[1].Revision != 3 {
		t.Fatalf("snapshot with groups = %+v, %v", snapshot, err)
	}
	if bobGroups, err := store.groupsLocked(bob.ID); err != nil || len(bobGroups) != 0 {
		t.Fatalf("groups leaked across accounts = %+v, %v", bobGroups, err)
	}

	removed, err := store.DeleteGroup(alice.ID, "studio")
	if err != nil || !removed {
		t.Fatalf("delete group = %v, %v", removed, err)
	}
	if removed, err := store.DeleteGroup(alice.ID, "studio"); err != nil || removed {
		t.Fatalf("second delete group = %v, %v", removed, err)
	}
	peers, err := store.Peers(alice.ID)
	if err != nil || len(peers) != 2 || peers[0].ID != "devbox" || peers[1].ID != "labbox" ||
		peers[0].GroupID != "" || peers[1].GroupID != "" {
		t.Fatalf("peers after group delete = %+v, %v", peers, err)
	}
	if peers[0].Revision != cleared.Revision {
		t.Fatalf("ungrouped peer revision changed = %d, want %d", peers[0].Revision, cleared.Revision)
	}
	if peers[1].Revision != 9 {
		t.Fatalf("moved peer revision = %d, want 9", peers[1].Revision)
	}
	snapshot, err = store.Snapshot(alice.ID, cleared.Revision)
	if err != nil || snapshot.Revision != 9 || len(snapshot.Changes) != 1 ||
		snapshot.Changes[0].ID != "labbox" || snapshot.Changes[0].GroupID != "" {
		t.Fatalf("incremental group delete = %+v, %v", snapshot, err)
	}
	if len(snapshot.GroupChanges) != 1 || snapshot.GroupChanges[0].ID != "studio" ||
		!snapshot.GroupChanges[0].Deleted || snapshot.GroupChanges[0].Revision != 8 ||
		snapshot.GroupChanges[0].Name != "" {
		t.Fatalf("group tombstone = %+v", snapshot.GroupChanges)
	}
	if len(snapshot.Groups) != 1 || snapshot.Groups[0].ID != "lab" {
		t.Fatalf("live groups after delete = %+v", snapshot.Groups)
	}

	revived, err := store.UpsertGroup(alice.ID, "studio", "Studio")
	if err != nil || revived.Revision != 10 || revived.Deleted {
		t.Fatalf("revive group = %+v, %v", revived, err)
	}
	snapshot, err = store.Snapshot(alice.ID, 9)
	if err != nil || len(snapshot.GroupChanges) != 1 || snapshot.GroupChanges[0].Deleted {
		t.Fatalf("revived group change = %+v, %v", snapshot, err)
	}
}

func TestAddressBookBulkGroupMove(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "studio", "Studio"); err != nil {
		t.Fatalf("create studio: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "lab", "Lab"); err != nil {
		t.Fatalf("create lab: %v", err)
	}

	studio, lab := "studio", "lab"
	peers := make(map[string]Peer)
	for _, item := range []struct {
		id    string
		name  string
		group string
	}{
		{"peer-a", "A", studio},
		{"peer-b", "B", studio},
		{"peer-c", "C", lab},
	} {
		name, group := item.name, item.group
		peer, err := store.UpdatePeerWithGroup(alice.ID, item.id, &name, nil, &group)
		if err != nil {
			t.Fatalf("create %s: %v", item.id, err)
		}
		peers[item.id] = peer
	}

	batch, err := store.MovePeersToGroup(alice.ID, lab, []PeerGroupMove{
		{ID: "peer-a", IfRevision: peers["peer-a"].Revision},
		{ID: "peer-b", IfRevision: peers["peer-b"].Revision},
		{ID: "peer-c", IfRevision: peers["peer-c"].Revision},
	})
	if err != nil {
		t.Fatalf("bulk move: %v", err)
	}
	if batch.Revision != 6 || len(batch.Moved) != 2 || len(batch.Unchanged) != 1 ||
		batch.Unchanged[0] != "peer-c" {
		t.Fatalf("bulk move result = %+v", batch)
	}
	for _, peer := range batch.Moved {
		if peer.GroupID != lab || peer.Revision != batch.Revision {
			t.Fatalf("moved peer = %+v, batch revision %d", peer, batch.Revision)
		}
	}

	noop, err := store.MovePeersToGroup(alice.ID, lab, []PeerGroupMove{
		{ID: "peer-a", IfRevision: batch.Revision},
		{ID: "peer-b", IfRevision: batch.Revision},
		{ID: "peer-c", IfRevision: peers["peer-c"].Revision},
	})
	if err != nil || noop.Revision != batch.Revision || len(noop.Moved) != 0 || len(noop.Unchanged) != 3 {
		t.Fatalf("no-op bulk move = %+v, %v", noop, err)
	}

	name := "D"
	if _, err := store.UpdatePeerWithGroup(alice.ID, "peer-d", &name, nil, &lab); err != nil {
		t.Fatalf("create peer-d: %v", err)
	}
	if _, err := store.MovePeersToGroup(alice.ID, studio, []PeerGroupMove{
		{ID: "peer-a", IfRevision: batch.Revision},
		{ID: "peer-b", IfRevision: peers["peer-b"].Revision},
	}); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale bulk move error = %v", err)
	}
	if peer, err := store.SetPeerGroup(alice.ID, "peer-a", lab); err != nil || peer.Revision != batch.Revision {
		t.Fatalf("stale batch changed peer-a = %+v, %v", peer, err)
	}
	if _, err := store.MovePeersToGroup(alice.ID, studio, []PeerGroupMove{
		{ID: "peer-a", IfRevision: batch.Revision},
		{ID: "missing", IfRevision: 0},
	}); !errors.Is(err, ErrUnknownPeer) {
		t.Fatalf("unknown peer batch error = %v", err)
	}
	if _, err := store.MovePeersToGroup(alice.ID, "missing", []PeerGroupMove{{ID: "peer-a"}}); !errors.Is(err, ErrUnknownGroup) {
		t.Fatalf("unknown group batch error = %v", err)
	}
	if _, err := store.MovePeersToGroup(alice.ID, lab, []PeerGroupMove{{ID: "peer-a"}, {ID: "peer-a"}}); !errors.Is(err, ErrInvalidPeerBatch) {
		t.Fatalf("duplicate peer batch error = %v", err)
	}

	oversized := make([]PeerGroupMove, MaxAddressBookMoveBatch+1)
	for i := range oversized {
		oversized[i].ID = fmt.Sprintf("peer-%03d", i)
	}
	if _, err := store.MovePeersToGroup(alice.ID, lab, oversized); !errors.Is(err, ErrInvalidPeerBatch) {
		t.Fatalf("oversized batch error = %v", err)
	}
}

func TestAddressBookGroupOptimisticConcurrency(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	group, err := store.UpsertGroup(alice.ID, "studio", "Studio")
	if err != nil || group.Revision != 1 {
		t.Fatalf("create group = %+v, %v", group, err)
	}
	renamed, err := store.UpsertGroupAt(alice.ID, "studio", "Lab", group.Revision)
	if err != nil || renamed.Revision != 2 || renamed.Name != "Lab" {
		t.Fatalf("matching group rename = %+v, %v", renamed, err)
	}
	if _, err := store.UpsertGroupAt(alice.ID, "studio", "Stale", group.Revision); !errors.Is(err, ErrGroupConflict) {
		t.Fatalf("stale group rename error = %v", err)
	}
	if _, err := store.UpsertGroupAt(alice.ID, "missing", "New", group.Revision); !errors.Is(err, ErrGroupConflict) {
		t.Fatalf("unknown group with token error = %v", err)
	}
	if _, err := store.DeleteGroupAt(alice.ID, "studio", 99); !errors.Is(err, ErrGroupConflict) {
		t.Fatalf("stale group delete error = %v", err)
	}
	if removed, err := store.DeleteGroupAt(alice.ID, "studio", renamed.Revision); err != nil || !removed {
		t.Fatalf("matching group delete = %v, %v", removed, err)
	}
	if _, err := store.UpsertGroupAt(alice.ID, "studio", "Revive", renamed.Revision); !errors.Is(err, ErrGroupConflict) {
		t.Fatalf("tombstoned group with token error = %v", err)
	}
	revived, err := store.UpsertGroup(alice.ID, "studio", "Revive")
	if err != nil || revived.Revision != 4 || revived.Deleted {
		t.Fatalf("legacy group revive = %+v, %v", revived, err)
	}
}

func TestAddressBookGroupNesting(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	root, err := store.UpsertGroup(alice.ID, "root", "Root")
	if err != nil || root.ParentID != "" || root.Position != 0 {
		t.Fatalf("create root = %+v, %v", root, err)
	}
	devName, devParent, devPosition := "Dev", "root", int64(5)
	dev, err := store.UpdateGroupAt(alice.ID, "dev", &devName, &devParent, &devPosition, 0)
	if err != nil || dev.ParentID != "root" || dev.Position != 5 || dev.Name != "Dev" {
		t.Fatalf("create child = %+v, %v", dev, err)
	}
	rackName, rackParent, rackPosition := "Rack", "dev", int64(2)
	rack, err := store.UpdateGroupAt(alice.ID, "rack", &rackName, &rackParent, &rackPosition, 0)
	if err != nil || rack.ParentID != "dev" || rack.Position != 2 {
		t.Fatalf("create grandchild = %+v, %v", rack, err)
	}
	alphaName, alphaPosition := "Alpha", int64(9)
	if _, err := store.UpdateGroupAt(alice.ID, "alpha", &alphaName, nil, &alphaPosition, 0); err != nil {
		t.Fatalf("create alpha: %v", err)
	}
	betaName, betaPosition := "Beta", int64(1)
	if _, err := store.UpdateGroupAt(alice.ID, "beta", &betaName, nil, &betaPosition, 0); err != nil {
		t.Fatalf("create beta: %v", err)
	}
	groups, err := store.groupsLocked(alice.ID)
	if err != nil {
		t.Fatalf("list groups: %v", err)
	}
	wantOrder := []string{"root", "beta", "rack", "dev", "alpha"}
	if len(groups) != len(wantOrder) {
		t.Fatalf("groups = %+v", groups)
	}
	for i, id := range wantOrder {
		if groups[i].ID != id {
			t.Fatalf("group order = %+v, want %v", groups, wantOrder)
		}
	}

	renamed, err := store.UpdateGroupAt(alice.ID, "dev", &rackName, nil, nil, 0)
	if err != nil || renamed.Name != "Rack" || renamed.ParentID != "root" || renamed.Position != 5 {
		t.Fatalf("rename keeps placement = %+v, %v", renamed, err)
	}

	selfParent := "dev"
	if _, err := store.UpdateGroupAt(alice.ID, "dev", nil, &selfParent, nil, 0); !errors.Is(err, ErrGroupCycle) {
		t.Fatalf("self parent error = %v", err)
	}
	cycleParent := "rack"
	if _, err := store.UpdateGroupAt(alice.ID, "root", nil, &cycleParent, nil, 0); !errors.Is(err, ErrGroupCycle) {
		t.Fatalf("descendant parent error = %v", err)
	}
	ghost := "ghost"
	if _, err := store.UpdateGroupAt(alice.ID, "dev", nil, &ghost, nil, 0); !errors.Is(err, ErrUnknownGroup) {
		t.Fatalf("unknown parent error = %v", err)
	}
	badParent := "bad id"
	if _, err := store.UpdateGroupAt(alice.ID, "dev", nil, &badParent, nil, 0); !errors.Is(err, ErrInvalidGroupID) {
		t.Fatalf("invalid parent id error = %v", err)
	}
	negative := int64(-1)
	if _, err := store.UpdateGroupAt(alice.ID, "dev", nil, nil, &negative, 0); !errors.Is(err, ErrInvalidPosition) {
		t.Fatalf("negative group position error = %v", err)
	}
	if _, err := store.UpdateGroupAt(alice.ID, "nameless", nil, nil, nil, 0); !errors.Is(err, ErrInvalidGroupName) {
		t.Fatalf("nameless create error = %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	foreignName := "Foreign"
	if _, err := store.UpdateGroupAt(bob.ID, "foreign", &foreignName, &devParent, nil, 0); !errors.Is(err, ErrUnknownGroup) {
		t.Fatalf("foreign parent error = %v", err)
	}

	parentID := "root"
	for depth := 2; depth <= maxGroupDepth; depth++ {
		id := fmt.Sprintf("deep%d", depth)
		childName := fmt.Sprintf("Deep %d", depth)
		if _, err := store.UpdateGroupAt(alice.ID, id, &childName, &parentID, nil, 0); err != nil {
			t.Fatalf("create depth %d: %v", depth, err)
		}
		parentID = id
	}
	tooDeep := "Deep 9"
	if _, err := store.UpdateGroupAt(alice.ID, "deep9", &tooDeep, &parentID, nil, 0); !errors.Is(err, ErrGroupDepth) {
		t.Fatalf("depth cap error = %v", err)
	}

	if removed, err := store.DeleteGroup(alice.ID, "dev"); err != nil || !removed {
		t.Fatalf("delete middle group = %v, %v", removed, err)
	}
	snapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot after delete: %v", err)
	}
	promoted := false
	for _, group := range snapshot.Groups {
		if group.ID == "rack" && group.ParentID == "root" {
			promoted = true
		}
		if group.ID == "dev" {
			t.Fatalf("deleted group still live: %+v", group)
		}
	}
	if !promoted {
		t.Fatalf("child group was not promoted: %+v", snapshot.Groups)
	}
	changeSeen := false
	for _, change := range snapshot.GroupChanges {
		if change.ID == "rack" && !change.Deleted && change.ParentID == "root" {
			changeSeen = true
		}
	}
	if !changeSeen {
		t.Fatalf("promotion missing from group changes: %+v", snapshot.GroupChanges)
	}

	peerName, peerGroup := "Box", "root"
	highPosition := int64(10)
	if _, err := store.UpdatePeerAt(alice.ID, "peer-b", &peerName, nil, &peerGroup, &highPosition, 0); err != nil {
		t.Fatalf("create peer-b: %v", err)
	}
	lowPosition := int64(3)
	if _, err := store.UpdatePeerAt(alice.ID, "peer-a", &peerName, nil, &peerGroup, &lowPosition, 0); err != nil {
		t.Fatalf("create peer-a: %v", err)
	}
	if _, err := store.UpdatePeerAt(alice.ID, "peer-c", &peerName, nil, &peerGroup, nil, 0); err != nil {
		t.Fatalf("create peer-c: %v", err)
	}
	if _, err := store.UpdatePeerAt(alice.ID, "peer-d", &peerName, nil, &peerGroup, &negative, 0); !errors.Is(err, ErrInvalidPosition) {
		t.Fatalf("negative peer position error = %v", err)
	}
	peers, err := store.Peers(alice.ID)
	if err != nil {
		t.Fatalf("list peers: %v", err)
	}
	wantPeers := []string{"peer-c", "peer-a", "peer-b"}
	if len(peers) != len(wantPeers) {
		t.Fatalf("peers = %+v", peers)
	}
	for i, id := range wantPeers {
		if peers[i].ID != id {
			t.Fatalf("peer order = %+v, want %v", peers, wantPeers)
		}
	}
}

func TestSharedBookGroupSubtree(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "correct horse")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "team", "Team"); err != nil {
		t.Fatalf("create team: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "ops", "Ops"); err != nil {
		t.Fatalf("create ops: %v", err)
	}
	devName, devParent := "Dev", "team"
	if _, err := store.UpdateGroupAt(alice.ID, "dev", &devName, &devParent, nil, 0); err != nil {
		t.Fatalf("create dev: %v", err)
	}
	box := "Box"
	for peerID, groupID := range map[string]string{
		"team-peer": "team",
		"dev-peer":  "dev",
		"ops-peer":  "ops",
	} {
		group := groupID
		if _, err := store.UpdatePeerWithGroup(alice.ID, peerID, &box, nil, &group); err != nil {
			t.Fatalf("seed %s: %v", peerID, err)
		}
	}
	if _, err := store.ShareBookScoped(alice.ID, "bob", PermissionWrite, "team"); err != nil {
		t.Fatalf("share team: %v", err)
	}
	books, err := store.SharedBooks(bob.ID)
	if err != nil || len(books) != 1 {
		t.Fatalf("bob books = %+v, %v", books, err)
	}
	visiblePeers := map[string]bool{}
	for _, peer := range books[0].Peers {
		visiblePeers[peer.ID] = true
	}
	if !visiblePeers["team-peer"] || !visiblePeers["dev-peer"] || visiblePeers["ops-peer"] {
		t.Fatalf("bob visible peers = %+v", books[0].Peers)
	}
	visibleGroups := map[string]bool{}
	for _, group := range books[0].Groups {
		visibleGroups[group.ID] = true
	}
	if !visibleGroups["team"] || !visibleGroups["dev"] || visibleGroups["ops"] {
		t.Fatalf("bob visible groups = %+v", books[0].Groups)
	}
	if _, err := store.AddSharedPeerScoped(bob.ID, "alice", "stalebox", "dev", "Stale", nil, books[0].Revision+1); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale shared add = %v", err)
	}
	if _, err := store.AddSharedPeerScoped(bob.ID, "alice", "newbox", "dev", "Shared", nil, books[0].Revision); err != nil {
		t.Fatalf("write into granted subtree: %v", err)
	}
	if _, err := store.AddSharedPeerScoped(bob.ID, "alice", "escape", "ops", "Escape", nil, 0); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("write outside granted subtree = %v", err)
	}

	if _, err := store.ShareBookScoped(alice.ID, "carol", PermissionRead, "dev"); err != nil {
		t.Fatalf("share dev: %v", err)
	}
	carolBooks, err := store.SharedBooks(carol.ID)
	if err != nil || len(carolBooks) != 1 {
		t.Fatalf("carol books = %+v, %v", carolBooks, err)
	}
	carolPeers := map[string]bool{}
	for _, peer := range carolBooks[0].Peers {
		carolPeers[peer.ID] = true
	}
	if !carolPeers["dev-peer"] || !carolPeers["newbox"] ||
		carolPeers["team-peer"] || carolPeers["ops-peer"] {
		t.Fatalf("carol visible peers = %+v", carolBooks[0].Peers)
	}
	carolGroups := map[string]bool{}
	for _, group := range carolBooks[0].Groups {
		carolGroups[group.ID] = true
	}
	if !carolGroups["dev"] || !carolGroups["team"] || carolGroups["ops"] {
		t.Fatalf("carol visible groups = %+v", carolBooks[0].Groups)
	}
}

func TestAddressBookOptimisticConcurrency(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	name := "开发机"
	peer, err := store.UpdatePeer(alice.ID, "devbox", &name, nil)
	if err != nil || peer.Revision != 1 {
		t.Fatalf("seed peer = %+v, %v", peer, err)
	}

	// A client that has the current revision wins and gets a new revision.
	newName := "Dev box"
	updated, err := store.UpdatePeerWithGroupAt(alice.ID, "devbox", &newName, nil, nil, peer.Revision)
	if err != nil || updated.Name != "Dev box" || updated.Revision != 2 {
		t.Fatalf("matching revision update = %+v, %v", updated, err)
	}

	// The same base revision is now stale, whether the edit touches name, tags
	// or only the group.
	if _, err := store.UpdatePeerWithGroupAt(alice.ID, "devbox", &newName, nil, nil, peer.Revision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale name update = %v", err)
	}
	tags := []string{"lab"}
	if _, err := store.UpdatePeerWithGroupAt(alice.ID, "devbox", nil, &tags, nil, peer.Revision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale tag update = %v", err)
	}
	if _, err := store.SetPeerGroupAt(alice.ID, "devbox", "", peer.Revision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale group move = %v", err)
	}

	// A conflict changes nothing.
	peers, err := store.Peers(alice.ID)
	if err != nil || len(peers) != 1 || peers[0].Name != "Dev box" ||
		len(peers[0].Tags) != 0 || peers[0].GroupID != "" || peers[0].Revision != 2 {
		t.Fatalf("peers after conflicts = %+v, %v", peers, err)
	}

	// Omitting the base revision keeps last-writer-wins for older clients.
	thirdName := "Devbox"
	if peer, err = store.UpdatePeerWithGroupAt(alice.ID, "devbox", &thirdName, nil, nil, 0); err != nil || peer.Revision != 3 {
		t.Fatalf("unconditional update = %+v, %v", peer, err)
	}

	// An unknown peer cannot satisfy a supplied base revision, even though the
	// unconditional path can create it.
	if _, err := store.UpdatePeerWithGroupAt(alice.ID, "newbox", &name, nil, nil, peer.Revision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("unknown peer with revision = %v", err)
	}
	if peer, err = store.UpdatePeerWithGroupAt(alice.ID, "newbox", &name, nil, nil, 0); err != nil || peer.Revision != 4 {
		t.Fatalf("unconditional create = %+v, %v", peer, err)
	}

	// Deleting a peer advances its tombstone revision; a stale edit conflicts
	// instead of silently reviving it.
	if removed, err := store.DeletePeer(alice.ID, "newbox"); err != nil || !removed {
		t.Fatalf("delete peer = %v, %v", removed, err)
	}
	if _, err := store.UpdatePeerWithGroupAt(alice.ID, "newbox", &name, nil, nil, peer.Revision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("edit after delete = %v", err)
	}
	if peer, err = store.UpdatePeerWithGroupAt(alice.ID, "newbox", &name, nil, nil, 0); err != nil || peer.Deleted || peer.Revision != 6 {
		t.Fatalf("unconditional revive = %+v, %v", peer, err)
	}
}

func TestSharedAddressBookPermissions(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "correct horse")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	name := "工作区"
	tags := []string{"lab"}
	if _, err := store.UpdatePeer(alice.ID, "devbox", &name, &tags); err != nil {
		t.Fatalf("seed alice peer: %v", err)
	}
	if _, err := store.ShareBook(alice.ID, "nobody", ""); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("share with unknown user = %v", err)
	}
	if _, err := store.ShareBook(alice.ID, "ALICE", ""); !errors.Is(err, ErrSelfShare) {
		t.Fatalf("share with self = %v", err)
	}
	if _, err := store.ShareBook(alice.ID, "bob", "owner"); !errors.Is(err, ErrInvalidPermission) {
		t.Fatalf("invalid permission = %v", err)
	}
	first, err := store.ShareBook(alice.ID, "Bob", "")
	if err != nil || first.Username != "bob" || first.Permission != PermissionRead ||
		first.CreatedAt.IsZero() {
		t.Fatalf("share with bob = %+v, %v", first, err)
	}
	upgraded, err := store.ShareBook(alice.ID, "bob", "WRITE")
	if err != nil || upgraded.Username != "bob" || upgraded.Permission != PermissionWrite {
		t.Fatalf("upgrade share = %+v, %v", upgraded, err)
	}
	outgoing, incoming, err := store.Grants(alice.ID)
	if err != nil || len(outgoing) != 1 || len(incoming) != 0 ||
		outgoing[0].Username != "bob" || outgoing[0].Permission != PermissionWrite {
		t.Fatalf("alice grants = %+v/%+v, %v", outgoing, incoming, err)
	}
	if outgoing, incoming, err = store.Grants(bob.ID); err != nil ||
		len(outgoing) != 0 || len(incoming) != 1 ||
		incoming[0].Username != "alice" || incoming[0].Permission != PermissionWrite {
		t.Fatalf("bob grants = %+v/%+v, %v", outgoing, incoming, err)
	}
	books, err := store.SharedBooks(bob.ID)
	if err != nil || len(books) != 1 || books[0].Owner != "alice" ||
		books[0].Permission != PermissionWrite || books[0].Revision == 0 ||
		len(books[0].Peers) != 1 {
		t.Fatalf("bob shared books = %+v, %v", books, err)
	}
	if peer := books[0].Peers[0]; peer.ID != "devbox" || peer.Name != "工作区" ||
		len(peer.Tags) != 1 || peer.Tags[0] != "lab" {
		t.Fatalf("shared peer = %+v", peer)
	}
	if peers, err := store.Peers(bob.ID); err != nil || len(peers) != 0 {
		t.Fatalf("shared book leaked into bob's own book: %+v, %v", peers, err)
	}
	if books, err = store.SharedBooks(carol.ID); err != nil || len(books) != 0 {
		t.Fatalf("carol shared books = %+v, %v", books, err)
	}

	if _, err := store.UpdateSharedPeer(carol.ID, "alice", "devbox", nil, nil); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("write without a grant = %v", err)
	}
	added, err := store.AddSharedPeer(bob.ID, "alice", "addedbox", "新增设备", []string{"shared", "lab"})
	if err != nil || added.ID != "addedbox" || added.Name != "新增设备" ||
		len(added.Tags) != 2 || added.Revision == 0 {
		t.Fatalf("shared add = %+v, %v", added, err)
	}
	if peers, err := store.Peers(bob.ID); err != nil || len(peers) != 0 {
		t.Fatalf("shared add leaked into bob's own book: %+v, %v", peers, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 2 {
		t.Fatalf("owner book after shared add = %+v, %v", peers, err)
	}
	edited := "共享改名"
	peer, err := store.UpdateSharedPeer(bob.ID, "alice", "devbox", &edited, nil)
	if err != nil || peer.Name != "共享改名" || len(peer.Tags) != 1 || peer.Tags[0] != "lab" {
		t.Fatalf("shared write = %+v, %v", peer, err)
	}
	if peers, err := store.Peers(bob.ID); err != nil || len(peers) != 0 {
		t.Fatalf("shared write leaked into bob's own book: %+v, %v", peers, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 2 {
		t.Fatalf("owner book after shared write = %+v, %v", peers, err)
	} else if peers[0].ID == "devbox" {
		if peers[0].Name != "共享改名" || peers[0].Tags[0] != "lab" {
			t.Fatalf("owner book after shared write = %+v", peers)
		}
	} else if peers[1].ID != "devbox" || peers[1].Name != "共享改名" {
		t.Fatalf("owner book after shared write = %+v", peers)
	}

	if _, err := store.ShareBook(alice.ID, "bob", PermissionRead); err != nil {
		t.Fatalf("downgrade share: %v", err)
	}
	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "devbox", &edited, nil); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("read-only write = %v", err)
	}
	if _, err := store.AddSharedPeer(bob.ID, "alice", "addedbox", "只读新增", nil); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("read-only add = %v", err)
	}
	if _, err := store.DeleteSharedPeer(bob.ID, "alice", "devbox"); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("read-only delete = %v", err)
	}
	if _, err := store.SharedBookGrants(bob.ID, "alice"); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("read-only grant list = %v", err)
	}
	if _, err := store.ShareBookAs(bob.ID, "alice", "carol", ""); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("read-only grant = %v", err)
	}
	if err := store.UnshareBookAs(bob.ID, "alice", "carol"); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("read-only revoke = %v", err)
	}

	if _, err := store.ShareBook(alice.ID, "bob", PermissionControl); err != nil {
		t.Fatalf("grant control: %v", err)
	}
	granted, err := store.ShareBookAs(bob.ID, "alice", "carol", PermissionWrite)
	if err != nil || granted.Username != "carol" || granted.Permission != PermissionWrite {
		t.Fatalf("control grant = %+v, %v", granted, err)
	}
	if books, err = store.SharedBooks(carol.ID); err != nil || len(books) != 1 ||
		books[0].Permission != PermissionWrite {
		t.Fatalf("carol shared books = %+v, %v", books, err)
	}
	if _, err := store.SharedBookGrants(carol.ID, "alice"); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("write-only grant list = %v", err)
	}
	grants, err := store.SharedBookGrants(bob.ID, "alice")
	if err != nil || len(grants) != 2 || grants[0].Username != "bob" ||
		grants[0].Permission != PermissionControl || grants[1].Username != "carol" {
		t.Fatalf("shared book grants = %+v, %v", grants, err)
	}
	if err := store.UnshareBookAs(bob.ID, "alice", "carol"); err != nil {
		t.Fatalf("control revoke: %v", err)
	}
	if books, err = store.SharedBooks(carol.ID); err != nil || len(books) != 0 {
		t.Fatalf("carol books after revoke = %+v, %v", books, err)
	}
	if err := store.UnshareBookAs(bob.ID, "alice", "carol"); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("revoke absent grant = %v", err)
	}

	if _, err := store.UpdateSharedPeerAt(bob.ID, "alice", "devbox", &edited, nil, peer.Revision-1); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale shared write = %v", err)
	}
	if _, err := store.DeleteSharedPeerAt(bob.ID, "alice", "devbox", peer.Revision-1); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale shared delete = %v", err)
	}
	if live, err := store.Peers(alice.ID); err != nil || len(live) != 2 {
		t.Fatalf("conflicting shared writes changed the book: %+v, %v", live, err)
	}
	if _, err := store.UpdateSharedPeerAt(bob.ID, "alice", "missing", &edited, nil, peer.Revision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("shared write to a missing peer with a token = %v", err)
	}

	snapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot before shared delete: %v", err)
	}
	removed, err := store.DeleteSharedPeerAt(bob.ID, "alice", "devbox", peer.Revision)
	if err != nil || !removed {
		t.Fatalf("shared delete = %t, %v", removed, err)
	}
	if _, err := store.DeleteSharedPeerAt(bob.ID, "alice", "devbox", peer.Revision); !errors.Is(err, ErrPeerConflict) {
		t.Fatalf("stale shared delete after tombstone = %v", err)
	}
	if removed, err = store.DeleteSharedPeer(bob.ID, "alice", "devbox"); err != nil || removed {
		t.Fatalf("repeated shared delete = %t, %v", removed, err)
	}
	delta, err := store.Snapshot(alice.ID, snapshot.Revision)
	if err != nil || len(delta.Changes) != 1 || delta.Changes[0].ID != "devbox" ||
		!delta.Changes[0].Deleted {
		t.Fatalf("shared delete tombstone = %+v, %v", delta, err)
	}
	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "bad id", nil, nil); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatalf("shared write invalid id = %v", err)
	}
	if _, err := store.AddSharedPeer(bob.ID, "alice", "bad id", "坏 ID", nil); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatalf("shared add invalid id = %v", err)
	}
	revived, err := store.AddSharedPeer(bob.ID, "alice", "devbox", "复活", []string{"revived"})
	if err != nil || revived.Name != "复活" || len(revived.Tags) != 1 ||
		revived.Tags[0] != "revived" {
		t.Fatalf("shared add revival = %+v, %v", revived, err)
	}
	if peers, err := store.Peers(alice.ID); err != nil || len(peers) != 2 {
		t.Fatalf("owner book after revival = %+v, %v", peers, err)
	}
	if err := store.UnshareBookAs(bob.ID, "alice", "bob"); err != nil {
		t.Fatalf("control self-revoke: %v", err)
	}
	if books, err = store.SharedBooks(bob.ID); err != nil || len(books) != 0 {
		t.Fatalf("books after self-revoke = %+v, %v", books, err)
	}
	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "other", nil, nil); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("write after self-revoke = %v", err)
	}
	if err := store.UnshareBook(alice.ID, "bob"); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("owner revoke absent grant = %v", err)
	}
	if _, err := store.ShareBook(alice.ID, "bob", PermissionRead); err != nil {
		t.Fatalf("re-share: %v", err)
	}
	if _, err := store.SetRole("bob", RoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if err := store.DeleteUser("alice"); err != nil {
		t.Fatalf("delete owner: %v", err)
	}
	if _, incoming, err = store.Grants(bob.ID); err != nil || len(incoming) != 0 {
		t.Fatalf("grants after owner deletion = %+v, %v", incoming, err)
	}
}

func TestAddressBookTargetGrants(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "correct horse")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	name := "工作区"
	tags := []string{"lab"}
	if _, err := store.UpdatePeer(alice.ID, "devbox", &name, &tags); err != nil {
		t.Fatalf("seed alice peer: %v", err)
	}
	if _, err := store.CreateGroup("team", "Team", "", 0, 0); err != nil {
		t.Fatalf("create account group: %v", err)
	}
	if _, err := store.AddGroupMember("team", "alice", RoleAdmin); err != nil {
		t.Fatalf("add alice to group: %v", err)
	}
	if _, err := store.AddGroupMember("team", "bob", RoleReadonly); err != nil {
		t.Fatalf("add bob to group: %v", err)
	}

	if _, err := store.ShareBookTarget(alice.ID, "user", "bob", "", ""); !errors.Is(err, ErrInvalidShareTarget) {
		t.Fatalf("user kind in target table = %v", err)
	}
	if _, err := store.ShareBookTarget(alice.ID, ShareTargetGroup, "nope", "", ""); !errors.Is(err, ErrUnknownAccountGroup) {
		t.Fatalf("unknown target group = %v", err)
	}
	if _, err := store.ShareBookTarget(carol.ID, ShareTargetGroup, "team", "", ""); !errors.Is(err, ErrShareTargetMember) {
		t.Fatalf("share to a group carol does not belong to = %v", err)
	}
	if _, err := store.ShareBookTarget(alice.ID, ShareTargetGroup, "team", "owner", ""); !errors.Is(err, ErrInvalidPermission) {
		t.Fatalf("invalid target permission = %v", err)
	}
	if _, err := store.ShareBookTarget(alice.ID, ShareTargetGroup, "team", "", "bad id"); !errors.Is(err, ErrInvalidGroupID) {
		t.Fatalf("invalid target scope = %v", err)
	}
	grant, err := store.ShareBookTarget(alice.ID, " GROUP ", "team", "WRITE", "")
	if err != nil || grant.TargetKind != ShareTargetGroup || grant.TargetID != "team" ||
		grant.TargetName != "Team" || grant.Permission != PermissionWrite {
		t.Fatalf("share to group = %+v, %v", grant, err)
	}

	books, err := store.SharedBooks(bob.ID)
	if err != nil || len(books) != 1 || books[0].Owner != "alice" ||
		books[0].Permission != PermissionWrite || len(books[0].Peers) != 1 {
		t.Fatalf("member shared book = %+v, %v", books, err)
	}
	if books, err = store.SharedBooks(carol.ID); err != nil || len(books) != 0 {
		t.Fatalf("non-member shared book = %+v, %v", books, err)
	}
	if books, err = store.SharedBooks(alice.ID); err != nil || len(books) != 0 {
		t.Fatalf("owner shared own book = %+v, %v", books, err)
	}
	edited := "组授权改名"
	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "devbox", &edited, nil); err != nil {
		t.Fatalf("member shared write: %v", err)
	}

	outgoing, incoming, err := store.Grants(alice.ID)
	if err != nil || len(outgoing) != 1 || len(incoming) != 0 ||
		outgoing[0].TargetKind != ShareTargetGroup || outgoing[0].TargetName != "Team" ||
		outgoing[0].Username != "" || outgoing[0].Permission != PermissionWrite {
		t.Fatalf("alice target grants = %+v/%+v, %v", outgoing, incoming, err)
	}
	if outgoing, incoming, err = store.Grants(bob.ID); err != nil ||
		len(outgoing) != 0 || len(incoming) != 1 ||
		incoming[0].Username != "alice" || incoming[0].TargetKind != ShareTargetGroup ||
		incoming[0].TargetName != "Team" {
		t.Fatalf("bob target grants = %+v/%+v, %v", outgoing, incoming, err)
	}

	// Membership drives access: removing bob revokes the book immediately.
	if err := store.RemoveGroupMember("team", "bob"); err != nil {
		t.Fatalf("remove bob from group: %v", err)
	}
	if books, err = store.SharedBooks(bob.ID); err != nil || len(books) != 0 {
		t.Fatalf("shared book after removal = %+v, %v", books, err)
	}
	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "devbox", nil, nil); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("write after group removal = %v", err)
	}

	// Everyone reaches every account except the owner.
	if _, err := store.ShareBookTarget(alice.ID, ShareTargetEveryone, "", PermissionRead, ""); err != nil {
		t.Fatalf("share with everyone: %v", err)
	}
	for _, user := range []User{bob, carol} {
		if books, err = store.SharedBooks(user.ID); err != nil || len(books) != 1 ||
			books[0].Owner != "alice" || books[0].Permission != PermissionRead {
			t.Fatalf("everyone shared book for %s = %+v, %v", user.Username, books, err)
		}
	}
	if books, err = store.SharedBooks(alice.ID); err != nil || len(books) != 0 {
		t.Fatalf("owner shared own book through everyone = %+v, %v", books, err)
	}

	// A scoped everyone grant only exposes that group.
	if err := store.UnshareBookTarget(alice.ID, ShareTargetEveryone, ""); err != nil {
		t.Fatalf("revoke everyone grant: %v", err)
	}
	if err := store.UnshareBookTarget(alice.ID, ShareTargetEveryone, ""); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("revoke absent everyone grant = %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "lab", "Lab"); err != nil {
		t.Fatalf("create address book group: %v", err)
	}
	if _, err := store.SetPeerGroup(alice.ID, "devbox", "lab"); err != nil {
		t.Fatalf("group peer: %v", err)
	}
	if _, err := store.ShareBookTarget(alice.ID, ShareTargetEveryone, "", PermissionRead, "lab"); err != nil {
		t.Fatalf("scoped everyone grant: %v", err)
	}
	if books, err = store.SharedBooks(carol.ID); err != nil || len(books) != 1 ||
		len(books[0].Peers) != 1 || len(books[0].Groups) != 1 ||
		books[0].Peers[0].GroupID != "lab" {
		t.Fatalf("scoped everyone book = %+v, %v", books, err)
	}
	if err := store.UnshareBookTargetScope(alice.ID, ShareTargetEveryone, "", ""); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("revoke wrong scope = %v", err)
	}

	// Deleting the scoped address-book group revokes its target grants.
	if removed, err := store.DeleteGroup(alice.ID, "lab"); err != nil || !removed {
		t.Fatalf("delete address book group = %v, %v", removed, err)
	}
	if books, err = store.SharedBooks(carol.ID); err != nil || len(books) != 0 {
		t.Fatalf("shared book after group delete = %+v, %v", books, err)
	}

	// Re-sharing to the account group and then deleting that group also
	// clears the grant from the owner's outgoing list.
	if _, err := store.ShareBookTarget(alice.ID, ShareTargetGroup, "team", PermissionRead, ""); err != nil {
		t.Fatalf("re-share to group: %v", err)
	}
	if err := store.DeleteAccountGroup("team"); err != nil {
		t.Fatalf("delete account group: %v", err)
	}
	if outgoing, incoming, err = store.Grants(alice.ID); err != nil ||
		len(outgoing) != 0 || len(incoming) != 0 {
		t.Fatalf("grants after group delete = %+v/%+v, %v", outgoing, incoming, err)
	}
}

func TestSharedGrantsSurviveReopen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	name := "工作区"
	if _, err := store.UpdatePeer(alice.ID, "devbox", &name, nil); err != nil {
		t.Fatalf("seed alice peer: %v", err)
	}
	if _, err := store.ShareBook(alice.ID, "bob", PermissionWrite); err != nil {
		t.Fatalf("share with bob: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close account store: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen account store: %v", err)
	}
	defer reopened.Close()
	books, err := reopened.SharedBooks(bob.ID)
	if err != nil || len(books) != 1 || books[0].Owner != "alice" ||
		books[0].Permission != PermissionWrite ||
		len(books[0].Peers) != 1 || books[0].Peers[0].Name != "工作区" {
		t.Fatalf("shared books after reopen = %+v, %v", books, err)
	}
	if err := reopened.UnshareBook(alice.ID, "bob"); err != nil {
		t.Fatalf("revoke after reopen: %v", err)
	}
	if books, err = reopened.SharedBooks(bob.ID); err != nil || len(books) != 0 {
		t.Fatalf("books after reopen revoke = %+v, %v", books, err)
	}
}

func TestSharedAddressBookGroupScopes(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "correct horse")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "lab", "Lab"); err != nil {
		t.Fatalf("create lab group: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "work", "Work"); err != nil {
		t.Fatalf("create work group: %v", err)
	}
	lab, work := "lab", "work"
	name := "开发机"
	if _, err := store.UpdatePeerWithGroup(alice.ID, "devbox", &name, nil, &lab); err != nil {
		t.Fatalf("seed lab peer: %v", err)
	}
	workName := "办公机"
	if _, err := store.UpdatePeerWithGroup(alice.ID, "office", &workName, nil, &work); err != nil {
		t.Fatalf("seed work peer: %v", err)
	}
	loose := "散装"
	if _, err := store.UpdatePeer(alice.ID, "loose", &loose, nil); err != nil {
		t.Fatalf("seed loose peer: %v", err)
	}

	if _, err := store.ShareBookScoped(alice.ID, "bob", "", "bad id"); !errors.Is(err, ErrInvalidGroupID) {
		t.Fatalf("invalid group id = %v", err)
	}
	if _, err := store.ShareBookScoped(alice.ID, "bob", "", "ghost"); !errors.Is(err, ErrUnknownGroup) {
		t.Fatalf("unknown group = %v", err)
	}
	if _, err := store.ShareBookScoped(alice.ID, "ALICE", "", "lab"); !errors.Is(err, ErrSelfShare) {
		t.Fatalf("self share = %v", err)
	}
	grant, err := store.ShareBookScoped(alice.ID, "bob", PermissionWrite, "lab")
	if err != nil || grant.GroupID != "lab" || grant.Permission != PermissionWrite || grant.Username != "bob" {
		t.Fatalf("scoped share = %+v, %v", grant, err)
	}

	books, err := store.SharedBooks(bob.ID)
	if err != nil || len(books) != 1 || books[0].Owner != "alice" ||
		books[0].Permission != PermissionWrite || len(books[0].Peers) != 1 ||
		books[0].Peers[0].ID != "devbox" || len(books[0].Groups) != 1 ||
		books[0].Groups[0].ID != "lab" || len(books[0].Scopes) != 1 ||
		books[0].Scopes[0].GroupID != "lab" {
		t.Fatalf("scoped shared book = %+v, %v", books, err)
	}

	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "office", &name, nil); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("write outside the group = %v", err)
	}
	if _, err := store.DeleteSharedPeer(bob.ID, "alice", "office"); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("delete outside the group = %v", err)
	}
	if _, err := store.AddSharedPeer(bob.ID, "alice", "newbox", "越权新增", nil); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("ungrouped add through a group grant = %v", err)
	}
	if _, err := store.AddSharedPeerScoped(bob.ID, "alice", "newbox", "work", "越权新增", nil, 0); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("add into another group = %v", err)
	}
	if _, err := store.AddSharedPeerScoped(bob.ID, "alice", "newbox", "ghost", "不存在的组", nil, 0); !errors.Is(err, ErrUnknownGroup) {
		t.Fatalf("add into an unknown group = %v", err)
	}
	added, err := store.AddSharedPeerScoped(bob.ID, "alice", "newbox", "lab", "新增设备", []string{"lab"}, 0)
	if err != nil || added.GroupID != "lab" || added.ID != "newbox" {
		t.Fatalf("add into the granted group = %+v, %v", added, err)
	}
	edited := "共享改名"
	if peer, err := store.UpdateSharedPeer(bob.ID, "alice", "devbox", &edited, nil); err != nil || peer.Name != "共享改名" {
		t.Fatalf("write inside the group = %+v, %v", peer, err)
	}

	// Whole-book read plus group write: everything becomes visible, but only
	// the lab group stays writable.
	if _, err := store.ShareBook(alice.ID, "bob", PermissionRead); err != nil {
		t.Fatalf("whole-book share = %v", err)
	}
	books, err = store.SharedBooks(bob.ID)
	if err != nil || len(books) != 1 || books[0].Permission != PermissionWrite ||
		len(books[0].Peers) != 4 || len(books[0].Groups) != 2 || len(books[0].Scopes) != 2 {
		t.Fatalf("combined scopes = %+v, %v", books, err)
	}
	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "office", &name, nil); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("whole-book read write = %v", err)
	}
	if _, err := store.UpdateSharedPeer(bob.ID, "alice", "devbox", &edited, nil); err != nil {
		t.Fatalf("group write under whole-book read = %v", err)
	}

	// Control of the whole book is still required to manage grants, no matter
	// how many group scopes an actor holds.
	if _, err := store.ShareBookScoped(alice.ID, "bob", PermissionControl, "lab"); err != nil {
		t.Fatalf("lab control: %v", err)
	}
	if _, err := store.ShareBookScopedAs(bob.ID, "alice", "carol", "", "lab"); !errors.Is(err, ErrPermissionDenied) {
		t.Fatalf("lab controller manages grants = %v", err)
	}
	if _, err := store.ShareBook(alice.ID, "bob", PermissionControl); err != nil {
		t.Fatalf("whole-book control: %v", err)
	}
	granted, err := store.ShareBookScopedAs(bob.ID, "alice", "carol", PermissionRead, "work")
	if err != nil || granted.GroupID != "work" || granted.Username != "carol" {
		t.Fatalf("controller scoped grant = %+v, %v", granted, err)
	}
	if books, err = store.SharedBooks(carol.ID); err != nil || len(books) != 1 ||
		len(books[0].Peers) != 1 || books[0].Peers[0].ID != "office" {
		t.Fatalf("carol scoped book = %+v, %v", books, err)
	}

	if err := store.UnshareBookScope(alice.ID, "bob", "lab"); err != nil {
		t.Fatalf("revoke lab scope: %v", err)
	}
	if err := store.UnshareBookScope(alice.ID, "bob", "lab"); !errors.Is(err, ErrUnknownShare) {
		t.Fatalf("revoke lab scope twice = %v", err)
	}
	if err := store.UnshareBookScope(alice.ID, "bob", ""); err != nil {
		t.Fatalf("revoke whole-book scope: %v", err)
	}
	if books, err = store.SharedBooks(bob.ID); err != nil || len(books) != 0 {
		t.Fatalf("book after scoped revokes = %+v, %v", books, err)
	}

	// The legacy pair-wide revoke drops every remaining scope at once.
	if _, err := store.ShareBook(alice.ID, "bob", PermissionRead); err != nil {
		t.Fatalf("re-share whole book: %v", err)
	}
	if _, err := store.ShareBookScoped(alice.ID, "bob", PermissionWrite, "lab"); err != nil {
		t.Fatalf("re-share lab: %v", err)
	}
	if err := store.UnshareBook(alice.ID, "bob"); err != nil {
		t.Fatalf("revoke all scopes: %v", err)
	}
	if books, err = store.SharedBooks(bob.ID); err != nil || len(books) != 0 {
		t.Fatalf("book after pair revoke = %+v, %v", books, err)
	}
	if outgoing, _, err := store.Grants(alice.ID); err != nil || len(outgoing) != 1 ||
		outgoing[0].Username != "carol" || outgoing[0].GroupID != "work" {
		t.Fatalf("grants after revokes = %+v, %v", outgoing, err)
	}

	// Deleting a group revokes its scoped grants instead of leaving dead
	// scopes behind.
	if _, err := store.ShareBookScoped(alice.ID, "carol", PermissionRead, "lab"); err != nil {
		t.Fatalf("share lab with carol: %v", err)
	}
	if removed, err := store.DeleteGroup(alice.ID, "lab"); err != nil || !removed {
		t.Fatalf("delete lab group = %v, %v", removed, err)
	}
	if outgoing, _, err := store.Grants(alice.ID); err != nil || len(outgoing) != 1 ||
		outgoing[0].GroupID != "work" {
		t.Fatalf("grants after group delete = %+v, %v", outgoing, err)
	}
}

func TestGrantScopesMigrationSplitsLegacyPairKey(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open legacy database: %v", err)
	}
	for _, schema := range []string{
		`CREATE TABLE users (
			id            TEXT PRIMARY KEY,
			username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
			password_hash TEXT NOT NULL,
			created_at    INTEGER NOT NULL
		)`,
		`CREATE TABLE address_book_grants (
			owner_user_id   TEXT NOT NULL,
			grantee_user_id TEXT NOT NULL,
			permission      TEXT NOT NULL DEFAULT 'read',
			created_at      INTEGER NOT NULL,
			PRIMARY KEY (owner_user_id, grantee_user_id)
		)`,
		`INSERT INTO users (id, username, password_hash, created_at) VALUES
			('a', 'alice', 'x', 1), ('b', 'bob', 'x', 1)`,
		`INSERT INTO address_book_grants (owner_user_id, grantee_user_id, permission, created_at)
			VALUES ('a', 'b', 'read', 10)`,
	} {
		if _, err := db.Exec(schema); err != nil {
			t.Fatalf("seed legacy schema: %v", err)
		}
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close legacy database: %v", err)
	}

	store, err := Open(path)
	if err != nil {
		t.Fatalf("migrate account store: %v", err)
	}
	defer store.Close()
	if _, err := store.db.Exec(
		`INSERT INTO address_book_grants (owner_user_id, grantee_user_id, group_id, permission, created_at)
		 VALUES ('a', 'b', 'lab', 'write', 20)`,
	); err != nil {
		t.Fatalf("insert scoped grant after migration: %v", err)
	}
	outgoing, _, err := store.Grants("a")
	if err != nil || len(outgoing) != 2 ||
		outgoing[0].Username != "bob" || outgoing[0].GroupID != "" ||
		outgoing[1].Username != "bob" || outgoing[1].GroupID != "lab" ||
		outgoing[1].Permission != PermissionWrite {
		t.Fatalf("grants after migration = %+v, %v", outgoing, err)
	}
}

func TestAddressBookRevisionMigrationBackfillsLegacyRows(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open legacy database: %v", err)
	}
	for _, schema := range []string{
		`CREATE TABLE users (
			id            TEXT PRIMARY KEY,
			username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
			password_hash TEXT NOT NULL,
			created_at    INTEGER NOT NULL
		)`,
		`CREATE TABLE address_book (
			user_id    TEXT NOT NULL,
			peer_id    TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			tags       TEXT NOT NULL DEFAULT '',
			updated_at INTEGER NOT NULL,
			PRIMARY KEY (user_id, peer_id)
		)`,
		`INSERT INTO users (id, username, password_hash, created_at)
		 VALUES ('legacy-user', 'legacy', 'pbkdf2-sha256$1$c2FsdA$c2FsdA', 1)`,
		`INSERT INTO address_book (user_id, peer_id, name, tags, updated_at)
		 VALUES ('legacy-user', 'legacybox', '旧设备', '["lab"]', 1)`,
	} {
		if _, err := db.Exec(schema); err != nil {
			db.Close()
			t.Fatalf("legacy schema: %v", err)
		}
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close legacy database: %v", err)
	}

	store, err := Open(path)
	if err != nil {
		t.Fatalf("open migrated store: %v", err)
	}
	defer store.Close()
	snapshot, err := store.Snapshot("legacy-user", 0)
	if err != nil {
		t.Fatalf("snapshot migrated store: %v", err)
	}
	if snapshot.Revision != 1 || len(snapshot.Peers) != 1 || len(snapshot.Changes) != 1 {
		t.Fatalf("migrated snapshot = %+v", snapshot)
	}
	peer := snapshot.Peers[0]
	if peer.ID != "legacybox" || peer.Name != "旧设备" || peer.Revision != 1 ||
		len(peer.Tags) != 1 || peer.Tags[0] != "lab" || peer.GroupID != "" {
		t.Fatalf("migrated peer = %+v", peer)
	}
	if snapshot.Groups == nil || len(snapshot.Groups) != 0 || len(snapshot.GroupChanges) != 0 {
		t.Fatalf("migrated groups = %+v", snapshot)
	}
	name := "改名"
	updated, err := store.UpdatePeer("legacy-user", "legacybox", &name, nil)
	if err != nil || updated.Revision != 2 {
		t.Fatalf("update after migration = %+v, %v", updated, err)
	}
	if _, err := store.UpsertGroup("legacy-user", "legacygroup", "旧分组"); err != nil {
		t.Fatalf("group write after migration: %v", err)
	}
}

func TestSessionsTrackDevicesAndRevoke(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	_, firstToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-a", "  笔记本\x07  ")
	if err != nil {
		t.Fatalf("first login: %v", err)
	}
	now = now.Add(time.Minute)
	_, secondToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-b", "Office PC")
	if err != nil {
		t.Fatalf("second login: %v", err)
	}

	sessions, err := store.Sessions(user.ID, secondToken)
	if err != nil {
		t.Fatalf("sessions: %v", err)
	}
	if len(sessions) != 2 {
		t.Fatalf("sessions = %+v, want 2", sessions)
	}
	var first, second Session
	for _, session := range sessions {
		switch session.DeviceID {
		case "device-a":
			first = session
		case "device-b":
			second = session
		default:
			t.Fatalf("unexpected session %+v", session)
		}
	}
	if first.ID == "" || first.ID == second.ID || first.Current || !second.Current {
		t.Fatalf("session identity/current flags: first=%+v second=%+v", first, second)
	}
	if first.DeviceName != "笔记本" {
		t.Fatalf("device name = %q, want trimmed 笔记本", first.DeviceName)
	}
	if !first.CreatedAt.Equal(time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)) ||
		!first.ExpiresAt.After(now) {
		t.Fatalf("session times = %+v", first)
	}

	revoked, err := store.RevokeSession(user.ID, first.ID)
	if err != nil || !revoked {
		t.Fatalf("revoke first = %v, %v", revoked, err)
	}
	if _, err := store.Authenticate(firstToken); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("revoked token still authenticates: %v", err)
	}
	if _, err := store.Authenticate(secondToken); err != nil {
		t.Fatalf("other session was revoked: %v", err)
	}
	if revoked, err := store.RevokeSession(user.ID, first.ID); err != nil || revoked {
		t.Fatalf("second revoke = %v, %v; want false", revoked, err)
	}
	if revoked, err := store.RevokeSession("someone-else", second.ID); err != nil || revoked {
		t.Fatalf("revoke across users = %v, %v; want false", revoked, err)
	}
	sessions, err = store.Sessions(user.ID, secondToken)
	if err != nil || len(sessions) != 1 || sessions[0].DeviceID != "device-b" {
		t.Fatalf("sessions after revoke = %+v, %v", sessions, err)
	}
}

func TestSessionsPageTracksLoginOrderAndOnlineActivity(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	type login struct {
		token   string
		created time.Time
	}
	logins := make([]login, 0, 3)
	for i, device := range []string{"device-a", "device-b", "device-c"} {
		if i > 0 {
			now = now.Add(time.Minute)
		}
		_, token, _, err := store.LoginWithDevice("alice", "correct horse", device, device)
		if err != nil {
			t.Fatalf("login %s: %v", device, err)
		}
		logins = append(logins, login{token: token, created: now})
	}

	page, total, err := store.SessionsPage(user.ID, logins[2].token, 2, 0)
	if err != nil || total != 3 || len(page) != 2 {
		t.Fatalf("first page = %+v total=%d err=%v", page, total, err)
	}
	if page[0].DeviceID != "device-c" || !page[0].Current || !page[0].Online ||
		page[1].DeviceID != "device-b" || page[1].Current || !page[1].Online {
		t.Fatalf("first page order/state = %+v", page)
	}
	page, total, err = store.SessionsPage(user.ID, "", 1, 1)
	if err != nil || total != 3 || len(page) != 1 || page[0].DeviceID != "device-b" {
		t.Fatalf("stable second page = %+v total=%d err=%v", page, total, err)
	}

	now = now.Add(SessionOnlineWindow + time.Minute)
	if _, err := store.Authenticate(logins[0].token); err != nil {
		t.Fatalf("touch oldest session: %v", err)
	}
	if _, err := store.Authenticate(logins[1].token); err != nil {
		t.Fatalf("touch middle session: %v", err)
	}
	all, total, err := store.SessionsPage(user.ID, "", 0, 0)
	if err != nil || total != 3 || len(all) != 3 {
		t.Fatalf("all sessions = %+v total=%d err=%v", all, total, err)
	}
	byDevice := make(map[string]Session, len(all))
	for _, session := range all {
		byDevice[session.DeviceID] = session
	}
	if !byDevice["device-a"].Online || !byDevice["device-b"].Online || byDevice["device-c"].Online {
		t.Fatalf("online activity = %+v", byDevice)
	}
	if !byDevice["device-a"].CreatedAt.Equal(logins[0].created) ||
		!byDevice["device-b"].CreatedAt.Equal(logins[1].created) ||
		!byDevice["device-b"].LastSeenAt.Equal(now) {
		t.Fatalf("stable login/activity timestamps = %+v", byDevice)
	}
	if all[0].DeviceID != "device-c" || all[1].DeviceID != "device-b" || all[2].DeviceID != "device-a" {
		t.Fatalf("refresh changed login order = %+v", all)
	}
}

func TestRevokeOtherSessionsKeepsCurrent(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	_, firstToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-a", "Office PC")
	if err != nil {
		t.Fatalf("first login: %v", err)
	}
	_, secondToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-b", "Laptop")
	if err != nil {
		t.Fatalf("second login: %v", err)
	}
	_, bobToken, _, err := store.Login("bob", "correct horse")
	if err != nil {
		t.Fatalf("bob login: %v", err)
	}

	revoked, err := store.RevokeOtherSessions(alice.ID, firstToken)
	if err != nil || revoked != 1 {
		t.Fatalf("revoke others = %d, %v; want 1", revoked, err)
	}
	if _, err := store.Authenticate(firstToken); err != nil {
		t.Fatalf("current session was revoked: %v", err)
	}
	if _, err := store.Authenticate(secondToken); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("other session survived: %v", err)
	}
	if _, err := store.Authenticate(bobToken); err != nil {
		t.Fatalf("another user's session was revoked: %v", err)
	}
	if revoked, err := store.RevokeOtherSessions(alice.ID, firstToken); err != nil || revoked != 0 {
		t.Fatalf("second revoke = %d, %v; want 0", revoked, err)
	}
	if _, err := store.RevokeOtherSessions(alice.ID, ""); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("empty token = %v", err)
	}
	if _, err := store.Authenticate(firstToken); err != nil {
		t.Fatalf("empty token revoked the current session: %v", err)
	}
}

func TestListUsersPage(t *testing.T) {
	store := openTemp(t, "state.db")
	seed := []struct {
		name  string
		role  string
		email string
		phone string
	}{
		{"alice", RoleAdmin, "zed@example.com", "13900000005"},
		{"bob", RoleUser, "bob@example.com", "13800000001"},
		{"bob_1", RoleUser, "lab@example.com", "13700000002"},
		{"carol", RoleReadonly, "", ""},
		{"dave", RoleOperator, "other@example.com", ""},
	}
	for i, user := range seed {
		created := time.Date(2026, 11, 1+i, 12, 0, 0, 0, time.UTC)
		store.SetClock(func() time.Time { return created })
		if _, err := store.CreateUserWithRole(user.name, "correct horse", user.email, user.phone, user.role); err != nil {
			t.Fatalf("create %s: %v", user.name, err)
		}
	}
	names := func(users []User) string {
		t.Helper()
		out := make([]string, 0, len(users))
		for _, user := range users {
			out = append(out, user.Username)
		}
		return strings.Join(out, ",")
	}

	users, total, err := store.ListUsersPage(UserListOptions{Limit: 2})
	if err != nil || total != 5 || len(users) != 2 ||
		users[0].Username != "alice" || users[1].Username != "bob" {
		t.Fatalf("first page = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Limit: 2, Offset: 4})
	if err != nil || total != 5 || len(users) != 1 || users[0].Username != "dave" {
		t.Fatalf("last page = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{})
	if err != nil || total != 5 || len(users) != 5 {
		t.Fatalf("unlimited = %d users total=%d err=%v", len(users), total, err)
	}

	users, total, err = store.ListUsersPage(UserListOptions{Query: "AR"})
	if err != nil || total != 1 || len(users) != 1 || users[0].Username != "carol" {
		t.Fatalf("case-insensitive query = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Query: "BOB@EXAMPLE"})
	if err != nil || total != 1 || len(users) != 1 || users[0].Username != "bob" {
		t.Fatalf("email through query = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Query: "138-0000"})
	if err != nil || total != 1 || len(users) != 1 || users[0].Username != "bob" {
		t.Fatalf("phone fragment through query = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Email: "example.com"})
	if err != nil || total != 4 || names(users) != "alice,bob,bob_1,dave" {
		t.Fatalf("email filter = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Phone: "138 0000"})
	if err != nil || total != 1 || len(users) != 1 || users[0].Username != "bob" {
		t.Fatalf("phone filter = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Role: RoleOperator})
	if err != nil || total != 1 || len(users) != 1 || users[0].Username != "dave" {
		t.Fatalf("role filter = %+v total=%d err=%v", users, total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Query: "bob", Role: RoleUser})
	if err != nil || total != 2 || len(users) != 2 {
		t.Fatalf("query+role = %+v total=%d err=%v", users, total, err)
	}
	// LIKE wildcards in the search text are literals, not patterns.
	users, total, err = store.ListUsersPage(UserListOptions{Query: "_"})
	if err != nil || total != 1 || len(users) != 1 || users[0].Username != "bob_1" {
		t.Fatalf("underscore query = %+v total=%d err=%v", users, total, err)
	}
	if _, total, err = store.ListUsersPage(UserListOptions{Query: "%"}); err != nil || total != 0 {
		t.Fatalf("percent query total=%d err=%v", total, err)
	}
	if _, _, err := store.ListUsersPage(UserListOptions{Role: "owner"}); !errors.Is(err, ErrInvalidRole) {
		t.Fatalf("invalid role = %v", err)
	}
	if _, _, err := store.ListUsersPage(UserListOptions{Sort: "banned"}); !errors.Is(err, ErrInvalidUserSort) {
		t.Fatalf("invalid sort = %v", err)
	}

	users, total, err = store.ListUsersPage(UserListOptions{Sort: UserSortEmail, Desc: true})
	if err != nil || total != 5 || names(users) != "alice,dave,bob_1,bob,carol" {
		t.Fatalf("email descending = %s total=%d err=%v", names(users), total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Sort: UserSortPhone})
	if err != nil || total != 5 || names(users) != "bob_1,bob,alice,carol,dave" {
		t.Fatalf("phone ascending = %s total=%d err=%v", names(users), total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Sort: UserSortCreatedAt, Desc: true})
	if err != nil || total != 5 || names(users) != "dave,carol,bob_1,bob,alice" {
		t.Fatalf("created descending = %s total=%d err=%v", names(users), total, err)
	}
	users, total, err = store.ListUsersPage(UserListOptions{Sort: UserSortRole, Limit: 2, Offset: 2})
	if err != nil || total != 5 || names(users) != "carol,bob" {
		t.Fatalf("stable role page = %s total=%d err=%v", names(users), total, err)
	}
}

func TestRevokeAllSessions(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	_, firstToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-a", "Office PC")
	if err != nil {
		t.Fatalf("first login: %v", err)
	}
	_, secondToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-b", "Laptop")
	if err != nil {
		t.Fatalf("second login: %v", err)
	}
	_, bobToken, _, err := store.Login("bob", "correct horse")
	if err != nil {
		t.Fatalf("bob login: %v", err)
	}

	revoked, err := store.RevokeAllSessions(alice.ID)
	if err != nil || revoked != 2 {
		t.Fatalf("revoke all = %d, %v; want 2", revoked, err)
	}
	for name, token := range map[string]string{"first": firstToken, "second": secondToken} {
		if _, err := store.Authenticate(token); !errors.Is(err, ErrInvalidToken) {
			t.Fatalf("%s session survived: %v", name, err)
		}
	}
	if _, err := store.Authenticate(bobToken); err != nil {
		t.Fatalf("another user's session was revoked: %v", err)
	}
	if revoked, err := store.RevokeAllSessions(alice.ID); err != nil || revoked != 0 {
		t.Fatalf("second revoke all = %d, %v; want 0", revoked, err)
	}
}

func TestRenameSession(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "correct horse"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	_, token, _, err := store.LoginWithDevice("alice", "correct horse", "device-a", "Office PC")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	sessions, err := store.Sessions(alice.ID, token)
	if err != nil || len(sessions) != 1 {
		t.Fatalf("sessions = %+v, %v", sessions, err)
	}
	id := sessions[0].ID

	renamed, err := store.RenameSession(alice.ID, id, "  书房 PC\x07  ")
	if err != nil || !renamed {
		t.Fatalf("rename = %v, %v", renamed, err)
	}
	sessions, err = store.Sessions(alice.ID, token)
	if err != nil || len(sessions) != 1 || sessions[0].DeviceName != "书房 PC" {
		t.Fatalf("sessions after rename = %+v, %v", sessions, err)
	}

	if renamed, err := store.RenameSession(alice.ID, id, " \x01 "); renamed || !errors.Is(err, ErrInvalidDeviceName) {
		t.Fatalf("empty rename = %v, %v; want ErrInvalidDeviceName", renamed, err)
	}
	sessions, err = store.Sessions(alice.ID, token)
	if err != nil || sessions[0].DeviceName != "书房 PC" {
		t.Fatalf("rejected rename changed the name: %+v, %v", sessions, err)
	}
	if renamed, err := store.RenameSession(alice.ID, "missing", "Laptop"); renamed || err != nil {
		t.Fatalf("unknown session rename = %v, %v; want false", renamed, err)
	}
	if renamed, err := store.RenameSession("someone-else", id, "Laptop"); renamed || err != nil {
		t.Fatalf("cross-user rename = %v, %v; want false", renamed, err)
	}

	now = now.Add(TokenTTL + time.Minute)
	if renamed, err := store.RenameSession(alice.ID, id, "Laptop"); renamed || err != nil {
		t.Fatalf("expired session rename = %v, %v; want false", renamed, err)
	}
}

func TestChangePasswordKeepsCurrentSessionAndRevokesOthers(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	_, firstToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-a", "Office PC")
	if err != nil {
		t.Fatalf("first login: %v", err)
	}
	_, secondToken, _, err := store.LoginWithDevice("alice", "correct horse", "device-b", "Laptop")
	if err != nil {
		t.Fatalf("second login: %v", err)
	}

	if err := store.ChangePassword(user.ID, firstToken, "wrong horse", "new correct horse"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong current password = %v", err)
	}
	if err := store.ChangePassword(user.ID, firstToken, "correct horse", "short"); !errors.Is(err, ErrWeakPassword) {
		t.Fatalf("weak new password = %v", err)
	}
	if _, err := store.Authenticate(secondToken); err != nil {
		t.Fatalf("failed change revoked another session: %v", err)
	}

	if err := store.ChangePassword(user.ID, firstToken, "correct horse", "new correct horse"); err != nil {
		t.Fatalf("change password: %v", err)
	}
	if _, err := store.Authenticate(firstToken); err != nil {
		t.Fatalf("current session was revoked: %v", err)
	}
	if _, err := store.Authenticate(secondToken); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("other session survived the change: %v", err)
	}
	if _, _, _, err := store.Login("alice", "correct horse"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("old password still logs in: %v", err)
	}
	sessions, err := store.Sessions(user.ID, firstToken)
	if err != nil || len(sessions) != 1 || !sessions[0].Current {
		t.Fatalf("sessions after password change = %+v, %v", sessions, err)
	}
	if _, _, _, err := store.Login("alice", "new correct horse"); err != nil {
		t.Fatalf("new password login: %v", err)
	}
	if err := store.ChangePassword("unknown-user", firstToken, "new correct horse", "another password"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("unknown user change = %v", err)
	}
}

func TestSessionMigrationBackfillsLegacyTokens(t *testing.T) {
	path := filepath.Join(t.TempDir(), "legacy.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	_, token, _, err := store.Login("alice", "correct horse")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open raw db: %v", err)
	}
	if _, err := db.Exec(`UPDATE user_tokens SET session_id = '', device_id = '', device_name = ''`); err != nil {
		db.Close()
		t.Fatalf("clear session metadata: %v", err)
	}
	// Dropping the recorded version makes the next Open treat the file as a
	// pre-migration database, which is the state this test simulates.
	if _, err := db.Exec(`DELETE FROM schema_migrations`); err != nil {
		db.Close()
		t.Fatalf("clear schema version: %v", err)
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close raw db: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	t.Cleanup(func() { reopened.Close() })
	sessions, err := reopened.Sessions(user.ID, token)
	if err != nil {
		t.Fatalf("sessions after migration: %v", err)
	}
	if len(sessions) != 1 || sessions[0].ID == "" || !sessions[0].Current {
		t.Fatalf("migrated sessions = %+v", sessions)
	}
	if _, err := reopened.Authenticate(token); err != nil {
		t.Fatalf("legacy token stopped authenticating: %v", err)
	}
	if revoked, err := reopened.RevokeSession(user.ID, sessions[0].ID); err != nil || !revoked {
		t.Fatalf("revoke migrated session = %v, %v", revoked, err)
	}
}

func TestPBKDF2SHA256Vector(t *testing.T) {
	got := pbkdf2SHA256([]byte("password"), []byte("salt"), 1, 32)
	const want = "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"
	if hex.EncodeToString(got) != want {
		t.Fatalf("pbkdf2 vector = %x, want %s", got, want)
	}
}

func TestTwoFactorLoginLifecycle(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })

	if enabled, err := store.TwoFactorEnabled(user.ID); err != nil || enabled {
		t.Fatalf("new account two-factor = %v, %v", enabled, err)
	}
	if _, err := store.StartTwoFactor("missing", "correct horse"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("start for unknown user = %v", err)
	}
	secret, err := store.StartTwoFactor(user.ID, "correct horse")
	if err != nil || secret == "" {
		t.Fatalf("start two-factor = %q, %v", secret, err)
	}
	// A pending secret is not enforced yet.
	if _, token, _, err := store.Login("alice", "correct horse"); err != nil {
		t.Fatalf("login while pending: %v", err)
	} else {
		_ = store.Logout(token)
	}

	code, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, "", "correct horse", "000000"); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("enable with wrong code = %v", err)
	}
	if err := store.EnableTwoFactor(user.ID, "", "correct horse", code); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}
	if enabled, err := store.TwoFactorEnabled(user.ID); err != nil || !enabled {
		t.Fatalf("enabled state = %v, %v", enabled, err)
	}
	if _, err := store.StartTwoFactor(user.ID, "correct horse"); !errors.Is(err, ErrTwoFactorEnabled) {
		t.Fatalf("restart while enabled = %v", err)
	}

	if _, _, _, err := store.Login("alice", "correct horse"); !errors.Is(err, ErrTwoFactorRequired) {
		t.Fatalf("login without code = %v", err)
	}
	if _, _, _, err := store.LoginWithDeviceCode("alice", "wrong horse", "", "", code); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong password with code = %v", err)
	}
	if _, _, _, err := store.LoginWithDeviceCode("alice", "correct horse", "", "", "000000"); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("login with wrong code = %v", err)
	}
	// The enrollment code belongs to the step already spent by EnableTwoFactor.
	if _, _, _, err := store.LoginWithDeviceCode("alice", "correct horse", "", "", code); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("replayed enrollment code = %v", err)
	}

	now = now.Add(30 * time.Second)
	nextCode, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	logged, token, _, err := store.LoginWithDeviceCode("alice", "correct horse", "device-a", "Viewer", nextCode)
	if err != nil || logged.ID != user.ID || token == "" {
		t.Fatalf("login with valid code = %+v %q %v", logged, token, err)
	}
	if _, _, _, err := store.LoginWithDeviceCode("alice", "correct horse", "", "", nextCode); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("replayed login code = %v", err)
	}

	if err := store.DisableTwoFactor(user.ID, "wrong horse", nextCode); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("disable with wrong password = %v", err)
	}
	if err := store.DisableTwoFactor(user.ID, "correct horse", "000000"); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("disable with wrong code = %v", err)
	}
	if err := store.DisableTwoFactor(user.ID, "correct horse", nextCode); err != nil {
		t.Fatalf("disable two-factor: %v", err)
	}
	if enabled, err := store.TwoFactorEnabled(user.ID); err != nil || enabled {
		t.Fatalf("disabled state = %v, %v", enabled, err)
	}
	if _, _, _, err := store.Login("alice", "correct horse"); err != nil {
		t.Fatalf("login after disable: %v", err)
	}
	if err := store.DisableTwoFactor(user.ID, "correct horse", nextCode); !errors.Is(err, ErrTwoFactorNotEnabled) {
		t.Fatalf("second disable = %v", err)
	}
	if enabled, err := store.TwoFactorEnabled(user.ID); err != nil || enabled {
		t.Fatalf("state after second disable = %v, %v", enabled, err)
	}

	// Re-enroll, then let an operator reset a lost authenticator.
	secret, err = store.StartTwoFactor(user.ID, "correct horse")
	if err != nil {
		t.Fatalf("restart two-factor: %v", err)
	}
	code, err = TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, "", "correct horse", code); err != nil {
		t.Fatalf("re-enable two-factor: %v", err)
	}
	if err := store.ResetTwoFactor("nobody"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("reset unknown user = %v", err)
	}
	if err := store.ResetTwoFactor("ALICE"); err != nil {
		t.Fatalf("reset two-factor: %v", err)
	}
	if enabled, err := store.TwoFactorEnabled(user.ID); err != nil || enabled {
		t.Fatalf("state after reset = %v, %v", enabled, err)
	}
	if _, _, _, err := store.Login("alice", "correct horse"); err != nil {
		t.Fatalf("login after reset: %v", err)
	}
}

func TestRecoveryCodesLifecycle(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	secret, err := store.StartTwoFactor(user.ID, "correct horse")
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	code, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, "", "correct horse", code); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}

	if count, err := store.RecoveryCodeCount(user.ID); err != nil || count != 0 {
		t.Fatalf("initial recovery codes = %d, %v", count, err)
	}
	if _, err := store.GenerateRecoveryCodes(user.ID, "wrong horse", code); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("generate with wrong password = %v", err)
	}
	if _, err := store.GenerateRecoveryCodes(user.ID, "correct horse", "000000"); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("generate with wrong code = %v", err)
	}
	codes, err := store.GenerateRecoveryCodes(user.ID, "correct horse", code)
	if err != nil || len(codes) != recoveryCodeCount {
		t.Fatalf("generate recovery codes = %d codes, %v", len(codes), err)
	}
	pattern := regexp.MustCompile(`^[A-Z2-7]{4}(-[A-Z2-7]{4}){3}$`)
	seen := make(map[string]struct{}, len(codes))
	for _, generated := range codes {
		if !pattern.MatchString(generated) {
			t.Fatalf("recovery code %q is not grouped base32", generated)
		}
		if _, dup := seen[generated]; dup {
			t.Fatalf("duplicate recovery code %q", generated)
		}
		seen[generated] = struct{}{}
	}
	if count, err := store.RecoveryCodeCount(user.ID); err != nil || count != recoveryCodeCount {
		t.Fatalf("recovery code count = %d, %v", count, err)
	}

	// A code read off paper may be typed in lower case with spaces.
	typed := strings.ToLower(strings.ReplaceAll(codes[0], "-", " "))
	if _, token, _, err := store.LoginWithDeviceRecovery("alice", "correct horse", "", "", "", typed); err != nil {
		t.Fatalf("login with recovery code: %v", err)
	} else {
		_ = store.Logout(token)
	}
	if count, err := store.RecoveryCodeCount(user.ID); err != nil || count != recoveryCodeCount-1 {
		t.Fatalf("count after recovery login = %d, %v", count, err)
	}
	if _, _, _, err := store.LoginWithDeviceRecovery("alice", "correct horse", "", "", "", codes[0]); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("replayed recovery code = %v", err)
	}
	if _, _, _, err := store.LoginWithDeviceRecovery("alice", "wrong horse", "", "", "", codes[1]); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("recovery code with wrong password = %v", err)
	}

	// The authenticator keeps working and does not spend backup codes.
	now = now.Add(30 * time.Second)
	nextCode, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := store.LoginWithDeviceRecovery("alice", "correct horse", "", "", nextCode, ""); err != nil {
		t.Fatalf("totp login after recovery use: %v", err)
	}
	if count, err := store.RecoveryCodeCount(user.ID); err != nil || count != recoveryCodeCount-1 {
		t.Fatalf("count after totp login = %d, %v", count, err)
	}

	// Issuing a fresh set invalidates every code from the previous set.
	replacement, err := store.GenerateRecoveryCodes(user.ID, "correct horse", nextCode)
	if err != nil || len(replacement) != recoveryCodeCount {
		t.Fatalf("regenerate recovery codes = %d codes, %v", len(replacement), err)
	}
	if count, err := store.RecoveryCodeCount(user.ID); err != nil || count != recoveryCodeCount {
		t.Fatalf("count after regenerate = %d, %v", count, err)
	}
	if _, _, _, err := store.LoginWithDeviceRecovery("alice", "correct horse", "", "", "", codes[1]); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("code from replaced set = %v", err)
	}

	if err := store.DisableTwoFactor(user.ID, "correct horse", nextCode); err != nil {
		t.Fatalf("disable two-factor: %v", err)
	}
	if count, err := store.RecoveryCodeCount(user.ID); err != nil || count != 0 {
		t.Fatalf("codes after disable = %d, %v", count, err)
	}
	if _, err := store.GenerateRecoveryCodes(user.ID, "correct horse", nextCode); !errors.Is(err, ErrTwoFactorNotEnabled) {
		t.Fatalf("generate while disabled = %v", err)
	}
}

func TestTwoFactorSurvivesReopen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	secret, err := store.StartTwoFactor(user.ID, "correct horse")
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	code, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, "", "correct horse", code); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}
	codes, err := store.GenerateRecoveryCodes(user.ID, "correct horse", code)
	if err != nil || len(codes) != recoveryCodeCount {
		t.Fatalf("generate recovery codes = %d codes, %v", len(codes), err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	t.Cleanup(func() { reopened.Close() })
	reopened.SetClock(func() time.Time { return now })
	if enabled, err := reopened.TwoFactorEnabled(user.ID); err != nil || !enabled {
		t.Fatalf("two-factor after reopen = %v, %v", enabled, err)
	}
	if _, _, _, err := reopened.Login("alice", "correct horse"); !errors.Is(err, ErrTwoFactorRequired) {
		t.Fatalf("login after reopen without code = %v", err)
	}
	now = now.Add(30 * time.Second)
	nextCode, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if count, err := reopened.RecoveryCodeCount(user.ID); err != nil || count != recoveryCodeCount {
		t.Fatalf("recovery codes after reopen = %d, %v", count, err)
	}
	if _, _, _, err := reopened.LoginWithDeviceCode("alice", "correct horse", "", "", nextCode); err != nil {
		t.Fatalf("login after reopen with code: %v", err)
	}
	if _, _, _, err := reopened.LoginWithDeviceRecovery("alice", "correct horse", "", "", "", codes[0]); err != nil {
		t.Fatalf("recovery login after reopen: %v", err)
	}
}

func TestAuditLoginEventsPersistNewestFirst(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	at := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	for _, event := range []AuditEvent{
		{
			CreatedAt: at,
			Username:  "alice",
			Action:    AuditLogin,
			Detail:    "password",
			ClientIP:  "127.0.0.1",
			Success:   true,
		},
		{
			CreatedAt: at.Add(time.Minute),
			Username:  "bob",
			Action:    AuditLogin,
			Detail:    "invalid credentials",
			ClientIP:  "192.0.2.9",
		},
	} {
		if err := store.AppendAudit(event); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	if err := store.AppendAudit(AuditEvent{Username: "alice"}); err == nil {
		t.Fatal("audit without an action was accepted")
	}
	events, err := store.AuditEvents(10)
	if err != nil {
		t.Fatalf("list audit: %v", err)
	}
	if len(events) != 2 {
		t.Fatalf("audit events = %d, want 2", len(events))
	}
	if events[0].Username != "bob" || events[0].Success ||
		events[0].Detail != "invalid credentials" ||
		events[0].ClientIP != "192.0.2.9" ||
		!events[0].CreatedAt.Equal(at.Add(time.Minute)) {
		t.Fatalf("newest audit event = %+v", events[0])
	}
	if events[1].Username != "alice" || !events[1].Success ||
		events[1].Detail != "password" || events[1].Action != AuditLogin {
		t.Fatalf("oldest audit event = %+v", events[1])
	}
	limited, err := store.AuditEvents(1)
	if err != nil || len(limited) != 1 || limited[0].Username != "bob" {
		t.Fatalf("limited audit = %+v, %v", limited, err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	t.Cleanup(func() { reopened.Close() })
	persisted, err := reopened.AuditEvents(10)
	if err != nil {
		t.Fatalf("list audit after reopen: %v", err)
	}
	if len(persisted) != 2 || persisted[0].Username != "bob" || !persisted[1].Success {
		t.Fatalf("audit after reopen = %+v", persisted)
	}
}

func TestConnectionReportsMergeByAuditRef(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	at := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return at })
	const ref = "00112233aabbccdd"

	host, err := store.RecordHostConnection("audithost", ref, at, at.Add(time.Minute), true, "")
	if err != nil {
		t.Fatalf("host report: %v", err)
	}
	if host.Source != connectionHost || host.Direction != connectionHost ||
		host.HostID != "audithost" || host.Username != "-" || host.AuditRef != ref {
		t.Fatalf("host row = %+v", host)
	}

	report := func() (ConnectionEvent, error) {
		return store.RecordConnection(alice.ID, "alice", "viewerbox", connectionViewer,
			"127.0.0.1", "Testland", RoutePunch, ref, at, at.Add(2*time.Minute), true, "")
	}
	merged, err := report()
	if err != nil {
		t.Fatalf("viewer report: %v", err)
	}
	if merged.ID != host.ID || merged.Source != connectionBoth ||
		merged.Username != "alice" || merged.HostID != "audithost" ||
		merged.PeerID != "viewerbox" || merged.Direction != connectionViewer {
		t.Fatalf("merged row = %+v", merged)
	}
	if !merged.EndedAt.Equal(at.Add(2 * time.Minute)) {
		t.Fatalf("merged window = %v", merged)
	}
	if merged.Location != "Testland" || merged.Route != RoutePunch {
		t.Fatalf("merged context = %+v", merged)
	}

	// A retry from either side must not pretend the other one reported twice.
	if repeat, err := report(); err != nil || repeat.Source != connectionBoth {
		t.Fatalf("repeated viewer report = %+v, %v", repeat, err)
	}
	if repeat, err := store.RecordHostConnection("audithost", ref,
		at, at.Add(time.Minute), true, ""); err != nil || repeat.Source != connectionBoth {
		t.Fatalf("repeated host report = %+v, %v", repeat, err)
	}
	if rows, err := store.Connections(alice.ID, 10); err != nil || len(rows) != 1 {
		t.Fatalf("viewer rows = %+v, %v", rows, err)
	}

	// A host-only session keeps "host"; a second host report for the same ref
	// must not upgrade it to "both".
	const hostOnlyRef = "aabbccdd00112233"
	first, err := store.RecordHostConnection("audithost", hostOnlyRef,
		at, at.Add(time.Minute), true, "")
	if err != nil {
		t.Fatalf("second host session: %v", err)
	}
	again, err := store.RecordHostConnection("audithost", hostOnlyRef,
		at, at.Add(2*time.Minute), true, "")
	if err != nil {
		t.Fatalf("repeated host session: %v", err)
	}
	if first.Source != connectionHost || again.Source != connectionHost || again.ID != first.ID {
		t.Fatalf("host-only merge = %+v / %+v", first, again)
	}
	if rows, err := store.ConnectionsForHost("audithost", 10); err != nil || len(rows) != 2 {
		t.Fatalf("host rows = %+v, %v", rows, err)
	}
}

func TestAuditAndSessionRecordLocation(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	_, token, _, err := store.Login("alice", "correct horse")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	if err := store.SetSessionOrigin(token, "203.0.113.7", "Tokyo, JP"); err != nil {
		t.Fatalf("session origin: %v", err)
	}
	if err := store.AppendAudit(AuditEvent{
		Username: "alice", Action: AuditLogin, ClientIP: "203.0.113.7",
		Location: "Tokyo, JP", Success: true,
	}); err != nil {
		t.Fatalf("append audit: %v", err)
	}
	sessions, err := store.Sessions(user.ID, token)
	if err != nil || len(sessions) != 1 {
		t.Fatalf("sessions = %+v, %v", sessions, err)
	}
	if sessions[0].LoginIP != "203.0.113.7" || sessions[0].Location != "Tokyo, JP" {
		t.Fatalf("session origin = %+v", sessions[0])
	}
	events, err := store.AuditEvents(10)
	if err != nil || len(events) != 1 {
		t.Fatalf("audit events = %+v, %v", events, err)
	}
	if events[0].ClientIP != "203.0.113.7" || events[0].Location != "Tokyo, JP" {
		t.Fatalf("audit location = %+v", events[0])
	}
}

func TestAuditEventsForFiltersByAction(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	for _, event := range []AuditEvent{
		{Username: "alice", Action: AuditLogin, Success: true},
		{Username: "alice", Action: AuditPasswordChange, Success: true},
		{Username: "bob", Action: AuditLogin, Detail: "invalid credentials"},
	} {
		if err := store.AppendAudit(event); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	logins, err := store.AuditEventsFor(AuditLogin, 10)
	if err != nil {
		t.Fatalf("list login audit: %v", err)
	}
	if len(logins) != 2 || logins[0].Username != "bob" || logins[1].Username != "alice" {
		t.Fatalf("filtered logins = %+v", logins)
	}
	changes, err := store.AuditEventsFor(AuditPasswordChange, 10)
	if err != nil || len(changes) != 1 || changes[0].Action != AuditPasswordChange {
		t.Fatalf("filtered password changes = %+v, %v", changes, err)
	}
	none, err := store.AuditEventsFor(AuditLogout, 10)
	if err != nil || len(none) != 0 {
		t.Fatalf("filtered logout events = %+v, %v", none, err)
	}
}

func TestAuditEventsForBeforeRangeFiltersByUser(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	for _, username := range []string{"alice", "alice2", "bob"} {
		if _, err := store.CreateUser(username, "correct horse"); err != nil {
			t.Fatalf("create %s: %v", username, err)
		}
	}
	if _, err := store.CreateGroup("team", "Team", "", 0, 0); err != nil {
		t.Fatalf("create group: %v", err)
	}
	if _, err := store.AddGroupMember("team", "alice2", "readonly"); err != nil {
		t.Fatalf("add group member: %v", err)
	}
	for _, event := range []AuditEvent{
		{Username: "alice", Action: AuditLogin, Success: true},
		{Username: "alice", Action: AuditLogin, Detail: "invalid credentials"},
		{Username: "alice2", Action: AuditLogin, Success: true},
		{Username: "bob", Action: AuditPasswordChange, Success: true},
	} {
		if err := store.AppendAudit(event); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	user := func(events []AuditEvent) []string {
		names := make([]string, 0, len(events))
		for _, event := range events {
			names = append(names, event.Username+"/"+event.Action)
		}
		return names
	}
	alice, err := store.AuditEventsForBeforeRange("", "alice", TimeBounds{}, 0, 10)
	if err != nil {
		t.Fatalf("list alice audit: %v", err)
	}
	if got := user(alice); len(got) != 2 || got[0] != "alice/login" || got[1] != "alice/login" {
		t.Fatalf("alice events = %v", got)
	}
	if alice[0].Detail != "invalid credentials" {
		t.Fatalf("newest alice event = %+v", alice[0])
	}
	// Surrounding whitespace from a query string must not hide the account.
	trimmed, err := store.AuditEventsForBeforeRange("", "  alice  ", TimeBounds{}, 0, 10)
	if err != nil || len(trimmed) != 2 {
		t.Fatalf("trimmed alice events = %+v, %v", trimmed, err)
	}
	// Exact match: "alice" does not also return "alice2".
	other, err := store.AuditEventsForBeforeRange("", "bob", TimeBounds{}, 0, 10)
	if err != nil || len(other) != 1 || other[0].Action != AuditPasswordChange {
		t.Fatalf("bob events = %+v, %v", other, err)
	}
	combo, err := store.AuditEventsForBeforeRange(AuditLogin, "alice", TimeBounds{}, 0, 10)
	if err != nil || len(combo) != 2 {
		t.Fatalf("alice login events = %+v, %v", combo, err)
	}
	empty, err := store.AuditEventsForBeforeRange(AuditPasswordChange, "alice", TimeBounds{}, 0, 10)
	if err != nil || len(empty) != 0 {
		t.Fatalf("alice password changes = %+v, %v", empty, err)
	}
	// The cursor pages through one user's trail without leaking others.
	first, err := store.AuditEventsForBeforeRange("", "alice", TimeBounds{}, 0, 1)
	if err != nil || len(first) != 1 || first[0].Detail != "invalid credentials" {
		t.Fatalf("alice first page = %+v, %v", first, err)
	}
	second, err := store.AuditEventsForBeforeRange("", "alice", TimeBounds{}, first[0].ID, 1)
	if err != nil || len(second) != 1 || second[0].Detail != "" {
		t.Fatalf("alice second page = %+v, %v", second, err)
	}
	// The group trail composes with the same account filter.
	group, err := store.AuditEventsForGroupBeforeRange("team", "", "alice2", TimeBounds{}, 0, 10)
	if err != nil || len(group) != 1 || group[0].Username != "alice2" {
		t.Fatalf("group alice2 events = %+v, %v", group, err)
	}
	outsider, err := store.AuditEventsForGroupBeforeRange("team", "", "bob", TimeBounds{}, 0, 10)
	if err != nil || len(outsider) != 0 {
		t.Fatalf("group bob events = %+v, %v", outsider, err)
	}
}
