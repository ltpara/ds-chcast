package account

import (
	"errors"
	"fmt"
	"testing"
)

func TestImportPolicyObjectsCreateUpdateUnchanged(t *testing.T) {
	store := openGroupStore(t)
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create lab: %v", err)
	}
	same := mustCreatePolicy(t, store, "same", DevicePolicyRestricted, 3, []string{"alice"}, []string{"lab"})
	changed := mustCreatePolicy(t, store, "changed", DevicePolicyOpen, 1, nil, nil)

	result, err := store.ImportPolicyObjects([]PolicyObjectImport{
		{Name: "new", Mode: DevicePolicyOpen, PermissionCeiling: 4},
		{
			Name:              "same",
			Mode:              DevicePolicyRestricted,
			PermissionCeiling: 3,
			AllowedUsers:      []string{"alice"},
			AllowedGroups:     []string{"lab"},
		},
		{
			Name:              "changed",
			Mode:              DevicePolicyRestricted,
			PermissionCeiling: 7,
			AllowedUsers:      []string{"alice"},
		},
	}, PolicyImportConflictUpdate, "importer")
	if err != nil {
		t.Fatalf("import policies: %v", err)
	}
	if len(result.Created) != 1 || len(result.Updated) != 1 || len(result.Unchanged) != 1 {
		t.Fatalf("import result = %+v", result)
	}
	if result.Created[0].Name != "new" || result.Created[0].PolicyVersion != 1 {
		t.Fatalf("created = %+v", result.Created[0])
	}
	if result.Unchanged[0].Name != "same" || result.Unchanged[0].PolicyVersion != same.PolicyVersion ||
		!result.Unchanged[0].UpdatedAt.Equal(same.UpdatedAt) {
		t.Fatalf("unchanged = %+v, original = %+v", result.Unchanged[0], same)
	}
	if result.Updated[0].Name != "changed" || result.Updated[0].PolicyVersion != changed.PolicyVersion+1 ||
		result.Updated[0].Mode != DevicePolicyRestricted || result.Updated[0].PermissionCeiling != 7 {
		t.Fatalf("updated = %+v", result.Updated[0])
	}

	policies, err := store.PolicyObjects()
	if err != nil || len(policies) != 3 {
		t.Fatalf("policies after import = %+v, %v", policies, err)
	}
}

func TestImportPolicyObjectsRejectConflictIsAtomic(t *testing.T) {
	store := openGroupStore(t)
	existing := mustCreatePolicy(t, store, "kept", DevicePolicyOpen, 1, nil, nil)
	_, err := store.ImportPolicyObjects([]PolicyObjectImport{
		{Name: "new", Mode: DevicePolicyOpen},
		{Name: "kept", Mode: DevicePolicyRestricted, PermissionCeiling: 9},
	}, PolicyImportConflictReject, "importer")
	if !errors.Is(err, ErrPolicyObjectExists) {
		t.Fatalf("reject conflict = %v", err)
	}
	policies, err := store.PolicyObjects()
	if err != nil || len(policies) != 1 || policies[0].Name != "kept" ||
		policies[0].PolicyVersion != existing.PolicyVersion || policies[0].Mode != existing.Mode {
		t.Fatalf("policies after rejected import = %+v, %v", policies, err)
	}
}

func TestImportPolicyObjectsValidatesWholePackage(t *testing.T) {
	t.Run("conflict mode", func(t *testing.T) {
		store := openGroupStore(t)
		_, err := store.ImportPolicyObjects([]PolicyObjectImport{{Name: "new"}}, "merge", "importer")
		if !errors.Is(err, ErrInvalidPolicyImportConflict) {
			t.Fatalf("invalid conflict = %v", err)
		}
	})

	t.Run("duplicate name", func(t *testing.T) {
		store := openGroupStore(t)
		_, err := store.ImportPolicyObjects([]PolicyObjectImport{
			{Name: "new"},
			{Name: "new"},
		}, PolicyImportConflictUpdate, "importer")
		if !errors.Is(err, ErrPolicyImportDuplicateName) {
			t.Fatalf("duplicate names = %v", err)
		}
		policies, listErr := store.PolicyObjects()
		if listErr != nil || len(policies) != 0 {
			t.Fatalf("policies after duplicate = %+v, %v", policies, listErr)
		}
	})

	t.Run("unknown user", func(t *testing.T) {
		store := openGroupStore(t)
		_, err := store.ImportPolicyObjects([]PolicyObjectImport{
			{Name: "valid"},
			{Name: "invalid", AllowedUsers: []string{"ghost"}},
		}, PolicyImportConflictUpdate, "importer")
		if !errors.Is(err, ErrDevicePolicyUser) {
			t.Fatalf("unknown user = %v", err)
		}
		policies, listErr := store.PolicyObjects()
		if listErr != nil || len(policies) != 0 {
			t.Fatalf("policies after invalid user = %+v, %v", policies, listErr)
		}
	})

	t.Run("unknown group", func(t *testing.T) {
		store := openGroupStore(t)
		_, err := store.ImportPolicyObjects([]PolicyObjectImport{
			{Name: "invalid", AllowedGroups: []string{"missing"}},
		}, PolicyImportConflictUpdate, "importer")
		if !errors.Is(err, ErrDevicePolicyGroup) {
			t.Fatalf("unknown group = %v", err)
		}
	})

	t.Run("too many", func(t *testing.T) {
		store := openGroupStore(t)
		items := make([]PolicyObjectImport, maxPolicyObjects+1)
		for i := range items {
			items[i].Name = fmt.Sprintf("policy-%03d", i)
		}
		_, err := store.ImportPolicyObjects(items, PolicyImportConflictUpdate, "importer")
		if !errors.Is(err, ErrPolicyImportTooMany) {
			t.Fatalf("too many = %v", err)
		}
	})
}

func TestImportPolicyObjectsQuota(t *testing.T) {
	store := openGroupStore(t)
	items := make([]PolicyObjectImport, maxPolicyObjects)
	for i := range items {
		items[i].Name = fmt.Sprintf("policy-%03d", i)
	}
	if _, err := store.ImportPolicyObjects(items, PolicyImportConflictUpdate, "importer"); err != nil {
		t.Fatalf("fill policy quota: %v", err)
	}
	_, err := store.ImportPolicyObjects([]PolicyObjectImport{{Name: "overflow"}},
		PolicyImportConflictUpdate, "importer")
	if !errors.Is(err, ErrPolicyObjectQuota) {
		t.Fatalf("quota = %v", err)
	}
}
