package account

import (
	"database/sql"
	"errors"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/dbmetric"
	"github.com/ltpara/ds-chcast/server/internal/pgtest"
	"github.com/ltpara/ds-chcast/server/internal/schemamig"
)

func TestAdminRoleConcurrentEdits(t *testing.T) {
	path := filepath.Join(t.TempDir(), "roles.db")
	testAdminRoleConcurrentEdits(t, func() (*Store, error) { return Open(path) })
}

func TestPostgresAdminRoleConcurrentEdits(t *testing.T) {
	dsn := pgtest.Schema(t)
	testAdminRoleConcurrentEdits(t, func() (*Store, error) { return OpenPostgres(dsn) })
}

func testAdminRoleConcurrentEdits(t *testing.T, open func() (*Store, error)) {
	t.Helper()
	first, err := open()
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := open()
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	// Both Stores use the same millisecond, including recreation: timestamps
	// alone cannot prevent stale writes or the delete/recreate (ABA) case.
	now := time.Date(2026, 11, 17, 12, 0, 0, 0, time.UTC)
	first.SetClock(func() time.Time { return now })
	second.SetClock(func() time.Time { return now })
	root, err := first.CreateUser("root", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	role, err := first.CreateAdminRole("shared", "Original", "original", []string{PermissionDevicesRead})
	if err != nil {
		t.Fatal(err)
	}
	binding, err := first.BindRole(root.Username, role.ID, RoleScopeGlobal, "", "")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := first.UpdateAdminRole(role.ID, "Missing", "", nil, ""); !errors.Is(err, ErrAdminRoleRevisionRequired) {
		t.Fatal(err)
	}
	if err := first.DeleteAdminRole(role.ID, ""); !errors.Is(err, ErrAdminRoleRevisionRequired) {
		t.Fatal(err)
	}
	start := make(chan struct{})
	results := make(chan error, 2)
	for i, store := range []*Store{first, second} {
		go func(i int, s *Store) {
			<-start
			_, err := s.UpdateAdminRole(role.ID, []string{"First", "Second"}[i], "winner", []string{PermissionAuditRead}, role.Revision)
			results <- err
		}(i, store)
	}
	close(start)
	successes, conflicts := 0, 0
	for i := 0; i < 2; i++ {
		err := <-results
		if err == nil {
			successes++
		} else if errors.Is(err, ErrAdminRoleRevisionConflict) {
			conflicts++
		} else {
			t.Fatal(err)
		}
	}
	if successes != 1 || conflicts != 1 {
		t.Fatalf("success=%d conflict=%d", successes, conflicts)
	}
	winner, err := second.AdminRole(role.ID)
	if err != nil {
		t.Fatal(err)
	}
	if winner.Revision == role.Revision || winner.UpdatedAt != role.UpdatedAt {
		t.Fatalf("revision did not change independently of clock: %+v", winner)
	}
	if err := first.DeleteAdminRole(role.ID, role.Revision); !errors.Is(err, ErrAdminRoleRevisionConflict) {
		t.Fatal(err)
	}
	bindings, err := second.RoleBindings(root.ID)
	if err != nil || len(bindings) != 1 || bindings[0].ID != binding.ID {
		t.Fatalf("stale delete damaged bindings: %+v %v", bindings, err)
	}
	if err := first.DeleteAdminRole(role.ID, winner.Revision); err != nil {
		t.Fatal(err)
	}
	bindings, err = second.RoleBindings(root.ID)
	if err != nil || len(bindings) != 0 {
		t.Fatalf("cascade: %+v %v", bindings, err)
	}
	recreated, err := first.CreateAdminRole(role.ID, role.Name, role.Description, role.Permissions)
	if err != nil {
		t.Fatal(err)
	}
	if recreated.Revision == role.Revision || recreated.Revision == winner.Revision {
		t.Fatal("recreation reused version")
	}
	if _, err := second.UpdateAdminRole(role.ID, "Stale", "", nil, winner.Revision); !errors.Is(err, ErrAdminRoleRevisionConflict) {
		t.Fatal(err)
	}
	if err := second.DeleteAdminRole(role.ID, winner.Revision); !errors.Is(err, ErrAdminRoleRevisionConflict) {
		t.Fatal(err)
	}
	// Rollback of the last-manager guard must also roll back the revision.
	full, err := first.CreateAdminRole("only-manager", "Manager", "", AllPermissions())
	if err != nil {
		t.Fatal(err)
	}
	if _, err := first.BindRole(root.Username, full.ID, RoleScopeGlobal, "", ""); err != nil {
		t.Fatal(err)
	}
	if _, err := first.SetRole(root.Username, RoleUser); err != nil {
		t.Fatal(err)
	}
	if _, err := first.UpdateAdminRole(full.ID, "Lost", "", nil, full.Revision); !errors.Is(err, ErrLastAdmin) {
		t.Fatal(err)
	}
	after, err := second.AdminRole(full.ID)
	if err != nil || !reflect.DeepEqual(after, full) {
		t.Fatalf("rollback changed role: %+v %v", after, err)
	}
	if err := second.DeleteAdminRole(full.ID, full.Revision); !errors.Is(err, ErrLastAdmin) {
		t.Fatal(err)
	}
}

func TestMigration26AdminRoleRevisions(t *testing.T) {
	path := filepath.Join(t.TempDir(), "v25.db")
	db, err := dbmetric.OpenSQLite("revision-fixture", path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := schemamig.Run(db, "schema_migrations", accountMigrations[:25]); err != nil {
		t.Fatal(err)
	}
	testAdminRoleRevisionMigration(t, db, func() (*Store, error) { return Open(path) }, func() error { _, err := schemamig.Run(db, "schema_migrations", accountMigrations[:25]); return err })
}

func TestPostgresMigration26AdminRoleRevisions(t *testing.T) {
	dsn := pgtest.Schema(t)
	db, err := dbmetric.OpenPostgres("revision-fixture", dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := schemamig.RunPostgres(db, "schema_migrations", accountPostgresMigrations[:25]); err != nil {
		t.Fatal(err)
	}
	testAdminRoleRevisionMigration(t, db, func() (*Store, error) { return OpenPostgres(dsn) }, func() error {
		_, err := schemamig.RunPostgres(db, "schema_migrations", accountPostgresMigrations[:25])
		return err
	})
}

func testAdminRoleRevisionMigration(t *testing.T, db *sql.DB, open func() (*Store, error), oldBinary func() error) {
	t.Helper()
	for _, q := range []string{
		`INSERT INTO users(id,username,role,password_hash,created_at) VALUES ('root','root','admin','',1234)`,
		`INSERT INTO admin_roles(id,name,description,permissions,builtin,created_at,updated_at) VALUES ('custom','Custom','preserved','devices.read',0,1234,5678)`,
		`INSERT INTO role_bindings(id,role_id,user_id,scope_kind,scope_value,object_kind,created_at) VALUES ('binding','custom','root','object','eastbox','device',1234)`,
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	var before AdminRole
	for i := 0; i < 2; i++ {
		store, err := open()
		if err != nil {
			t.Fatal(err)
		}
		roles, err := store.ListAdminRoles()
		if err != nil {
			t.Fatal(err)
		}
		seen := map[string]bool{}
		for _, r := range roles {
			if len(r.Revision) != 32 || seen[r.Revision] {
				t.Fatalf("invalid revision %+v", r)
			}
			seen[r.Revision] = true
		}
		got, err := store.AdminRole("custom")
		if err != nil {
			t.Fatal(err)
		}
		if i == 0 {
			before = got
		} else if !reflect.DeepEqual(before, got) {
			t.Fatal("reopen changed revision")
		}
		if got.Name != "Custom" || got.Description != "preserved" || got.CreatedAt.UnixMilli() != 1234 || got.UpdatedAt.UnixMilli() != 5678 || !reflect.DeepEqual(got.Permissions, []string{PermissionDevicesRead}) {
			t.Fatalf("migration changed role: %+v", got)
		}
		bindings, err := store.RoleBindings("root")
		if err != nil || len(bindings) != 1 || bindings[0].ID != "binding" {
			t.Fatalf("binding migration: %+v %v", bindings, err)
		}
		store.Close()
	}
	var tooNew *schemamig.TooNewError
	if err := oldBinary(); !errors.As(err, &tooNew) || tooNew.Found != accountSchemaVersion || tooNew.Supported != 25 {
		t.Fatalf("old binary accepted upgraded DB: %v", err)
	}
}
