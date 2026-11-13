package account

import (
	"errors"
	"fmt"
	"strings"
	"testing"
)

func TestAddressBookGroupImportCreateUpdateUnchanged(t *testing.T) {
	store := openGroupStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "root", "Root"); err != nil {
		t.Fatalf("create root: %v", err)
	}
	same, err := store.UpdateGroupAt(alice.ID, "same", stringPtr("Same"), nil, nil, 0)
	if err != nil {
		t.Fatalf("create same: %v", err)
	}
	changed, err := store.UpdateGroupAt(alice.ID, "changed", stringPtr("Old"), nil, int64Ptr(1), 0)
	if err != nil {
		t.Fatalf("create changed: %v", err)
	}

	result, err := store.ImportAddressBookGroups(alice.ID, []AddressBookGroupImport{
		{ID: "child", Name: "Child", ParentID: "root", Position: 2},
		{ID: "same", Name: "Same", Position: 0},
		{ID: "changed", Name: "Changed", ParentID: "root", Position: 3},
	}, AddressBookGroupImportConflictUpdate)
	if err != nil {
		t.Fatalf("import groups: %v", err)
	}
	if len(result.Created) != 1 || len(result.Updated) != 1 || len(result.Unchanged) != 1 || len(result.Skipped) != 0 {
		t.Fatalf("result = %+v", result)
	}
	if result.Revision <= 0 || result.Created[0].Revision != result.Revision || result.Updated[0].Revision != result.Revision {
		t.Fatalf("batch revision = %+v", result)
	}
	if !result.Created[0].UpdatedAt.Equal(result.Updated[0].UpdatedAt) {
		t.Fatalf("batch timestamps differ: %+v", result)
	}
	if result.Unchanged[0].Revision != same.Revision || result.Unchanged[0].UpdatedAt.UnixMilli() != same.UpdatedAt.UnixMilli() {
		t.Fatalf("unchanged group was rewritten: got %+v want %+v", result.Unchanged[0], same)
	}
	if result.Updated[0].Name != "Changed" || result.Updated[0].ParentID != "root" || result.Updated[0].Position != 3 {
		t.Fatalf("updated group = %+v", result.Updated[0])
	}
	if result.Updated[0].Revision <= changed.Revision {
		t.Fatalf("updated revision did not advance: %+v", result.Updated[0])
	}

	snapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	if snapshot.Revision != result.Revision {
		t.Fatalf("snapshot revision = %d want %d", snapshot.Revision, result.Revision)
	}
	byID := make(map[string]Group, len(snapshot.Groups))
	for _, group := range snapshot.Groups {
		byID[group.ID] = group
	}
	if byID["child"].ParentID != "root" || byID["child"].Revision != result.Revision {
		t.Fatalf("created group = %+v", byID["child"])
	}
	if byID["changed"].Name != "Changed" || byID["changed"].ParentID != "root" {
		t.Fatalf("changed group = %+v", byID["changed"])
	}
}

func TestAddressBookGroupImportSkipAndReject(t *testing.T) {
	store := openGroupStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	existing, err := store.UpdateGroupAt(alice.ID, "existing", stringPtr("Existing"), nil, int64Ptr(4), 0)
	if err != nil {
		t.Fatalf("create existing: %v", err)
	}
	before, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot before: %v", err)
	}

	if _, err := store.ImportAddressBookGroups(alice.ID, []AddressBookGroupImport{
		{ID: "existing", Name: "Replacement"},
	}, AddressBookGroupImportConflictReject); !errors.Is(err, ErrAddressBookGroupImportConflict) {
		t.Fatalf("reject conflict = %v", err)
	}
	afterReject, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot after reject: %v", err)
	}
	if afterReject.Revision != before.Revision || len(afterReject.Groups) != 1 || afterReject.Groups[0] != before.Groups[0] {
		t.Fatalf("rejected import changed the book: before=%+v after=%+v", before, afterReject)
	}

	result, err := store.ImportAddressBookGroups(alice.ID, []AddressBookGroupImport{
		{ID: "existing", Name: "Ignored", Position: 9},
	}, AddressBookGroupImportConflictSkip)
	if err != nil {
		t.Fatalf("skip import: %v", err)
	}
	if len(result.Skipped) != 1 || result.Skipped[0] != "existing" || result.Revision != 0 {
		t.Fatalf("skip result = %+v", result)
	}
	afterSkip, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot after skip: %v", err)
	}
	if len(afterSkip.Groups) != 1 || afterSkip.Groups[0].ID != existing.ID ||
		afterSkip.Groups[0].Name != existing.Name || afterSkip.Groups[0].Position != existing.Position ||
		afterSkip.Groups[0].Revision != existing.Revision ||
		afterSkip.Groups[0].UpdatedAt.UnixMilli() != existing.UpdatedAt.UnixMilli() ||
		afterSkip.Revision != before.Revision {
		t.Fatalf("skip changed the book: %+v", afterSkip)
	}
}

func TestAddressBookGroupImportRoundTripExport(t *testing.T) {
	store := openGroupStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := store.UpdateGroupAt(alice.ID, "root", stringPtr("Root"), nil, int64Ptr(1), 0); err != nil {
		t.Fatalf("create root: %v", err)
	}
	if _, err := store.UpdateGroupAt(alice.ID, "child", stringPtr("Child"), stringPtr("root"), int64Ptr(2), 0); err != nil {
		t.Fatalf("create child: %v", err)
	}
	if _, err := store.UpdateGroupAt(alice.ID, "deleted", stringPtr("Deleted"), nil, nil, 0); err != nil {
		t.Fatalf("create deleted: %v", err)
	}
	if _, err := store.DeleteGroup(alice.ID, "deleted"); err != nil {
		t.Fatalf("delete group: %v", err)
	}
	if _, err := store.UpsertGroup(bob.ID, "bob-only", "Bob only"); err != nil {
		t.Fatalf("create bob group: %v", err)
	}

	exported, err := store.ExportAddressBookGroups(alice.ID)
	if err != nil {
		t.Fatalf("export: %v", err)
	}
	if len(exported) != 2 || exported[0].ID != "root" || exported[1].ID != "child" || exported[1].ParentID != "root" {
		t.Fatalf("exported groups = %+v", exported)
	}
	result, err := store.ImportAddressBookGroups(bob.ID, []AddressBookGroupImport{
		{ID: exported[0].ID, Name: exported[0].Name, ParentID: exported[0].ParentID, Position: exported[0].Position},
		{ID: exported[1].ID, Name: exported[1].Name, ParentID: exported[1].ParentID, Position: exported[1].Position},
	}, AddressBookGroupImportConflictReject)
	if err != nil {
		t.Fatalf("re-import export: %v", err)
	}
	if len(result.Created) != 2 {
		t.Fatalf("round-trip result = %+v", result)
	}
	bobGroups, err := store.ExportAddressBookGroups(bob.ID)
	if err != nil {
		t.Fatalf("export bob: %v", err)
	}
	if len(bobGroups) != 3 || bobGroups[2].ID != "child" || bobGroups[2].ParentID != "root" {
		t.Fatalf("bob groups = %+v", bobGroups)
	}
	aliceSnapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("alice snapshot: %v", err)
	}
	if len(aliceSnapshot.Groups) != 2 {
		t.Fatalf("bob import changed alice: %+v", aliceSnapshot.Groups)
	}
}

func TestAddressBookGroupImportValidatesWholeTree(t *testing.T) {
	tests := []struct {
		name     string
		groups   []AddressBookGroupImport
		conflict string
		want     error
	}{
		{
			name:     "conflict mode",
			groups:   []AddressBookGroupImport{{ID: "new", Name: "New"}},
			conflict: "merge",
			want:     ErrInvalidAddressBookGroupImportConflict,
		},
		{
			name: "duplicate id",
			groups: []AddressBookGroupImport{
				{ID: "new", Name: "New"},
				{ID: "new", Name: "Again"},
			},
			want: ErrAddressBookGroupImportDuplicateID,
		},
		{
			name:   "empty name",
			groups: []AddressBookGroupImport{{ID: "new", Name: "  "}},
			want:   ErrAddressBookGroupImportName,
		},
		{
			name:   "long name",
			groups: []AddressBookGroupImport{{ID: "new", Name: strings.Repeat("x", 65)}},
			want:   ErrAddressBookGroupImportName,
		},
		{
			name:   "negative position",
			groups: []AddressBookGroupImport{{ID: "new", Name: "New", Position: -1}},
			want:   ErrInvalidPosition,
		},
		{
			name: "unknown parent",
			groups: []AddressBookGroupImport{
				{ID: "valid", Name: "Valid"},
				{ID: "new", Name: "New", ParentID: "missing"},
			},
			want: ErrUnknownGroup,
		},
		{
			name:   "self cycle",
			groups: []AddressBookGroupImport{{ID: "new", Name: "New", ParentID: "new"}},
			want:   ErrGroupCycle,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			store := openGroupStore(t)
			alice, err := store.CreateUser("alice", "correct horse")
			if err != nil {
				t.Fatalf("create alice: %v", err)
			}
			conflict := tt.conflict
			if conflict == "" {
				conflict = AddressBookGroupImportConflictUpdate
			}
			_, err = store.ImportAddressBookGroups(alice.ID, tt.groups, conflict)
			if !errors.Is(err, tt.want) {
				t.Fatalf("import error = %v, want %v", err, tt.want)
			}
			snapshot, snapErr := store.Snapshot(alice.ID, 0)
			if snapErr != nil {
				t.Fatalf("snapshot: %v", snapErr)
			}
			if len(snapshot.Groups) != 0 || snapshot.Revision != 0 {
				t.Fatalf("invalid import partially wrote: %+v", snapshot)
			}
		})
	}

	t.Run("cycle through existing groups", func(t *testing.T) {
		store := openGroupStore(t)
		alice, _ := store.CreateUser("alice", "correct horse")
		if _, err := store.UpdateGroupAt(alice.ID, "a", stringPtr("A"), nil, nil, 0); err != nil {
			t.Fatal(err)
		}
		if _, err := store.UpdateGroupAt(alice.ID, "b", stringPtr("B"), stringPtr("a"), nil, 0); err != nil {
			t.Fatal(err)
		}
		before, _ := store.Snapshot(alice.ID, 0)
		_, err := store.ImportAddressBookGroups(alice.ID, []AddressBookGroupImport{
			{ID: "a", Name: "A", ParentID: "b"},
		}, AddressBookGroupImportConflictUpdate)
		if !errors.Is(err, ErrGroupCycle) {
			t.Fatalf("existing-group cycle = %v", err)
		}
		after, _ := store.Snapshot(alice.ID, 0)
		if after.Revision != before.Revision || len(after.Groups) != 2 {
			t.Fatalf("cycle import partially wrote: before=%+v after=%+v", before, after)
		}
	})

	t.Run("too deep", func(t *testing.T) {
		store := openGroupStore(t)
		alice, _ := store.CreateUser("alice", "correct horse")
		groups := make([]AddressBookGroupImport, maxGroupDepth+1)
		for i := range groups {
			groups[i] = AddressBookGroupImport{ID: fmt.Sprintf("g%02d", i), Name: fmt.Sprintf("G%02d", i)}
			if i > 0 {
				groups[i].ParentID = groups[i-1].ID
			}
		}
		_, err := store.ImportAddressBookGroups(alice.ID, groups, AddressBookGroupImportConflictUpdate)
		if !errors.Is(err, ErrGroupDepth) {
			t.Fatalf("deep import = %v", err)
		}
	})

	t.Run("too many", func(t *testing.T) {
		store := openGroupStore(t)
		alice, _ := store.CreateUser("alice", "correct horse")
		groups := make([]AddressBookGroupImport, MaxAddressBookGroupImportGroups+1)
		for i := range groups {
			groups[i] = AddressBookGroupImport{ID: fmt.Sprintf("g%03d", i), Name: fmt.Sprintf("G%03d", i)}
		}
		_, err := store.ImportAddressBookGroups(alice.ID, groups, AddressBookGroupImportConflictUpdate)
		if !errors.Is(err, ErrAddressBookGroupImportTooMany) {
			t.Fatalf("oversized import = %v", err)
		}
	})
}

func stringPtr(value string) *string { return &value }
func int64Ptr(value int64) *int64    { return &value }
