package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"unicode/utf8"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const (
	AddressBookGroupDocumentFormat  = "chast-addressbook-groups"
	AddressBookGroupDocumentVersion = 1

	// MaxAddressBookGroupImportGroups bounds one portable group package.
	MaxAddressBookGroupImportGroups = 256

	AddressBookGroupImportConflictReject = "reject"
	AddressBookGroupImportConflictSkip   = "skip"
	AddressBookGroupImportConflictUpdate = "update"

	maxAddressBookGroupImportName = 64
)

var (
	ErrInvalidAddressBookGroupImportConflict = errors.New("address book group import conflict must be reject, skip or update")
	ErrAddressBookGroupImportDuplicateID     = errors.New("address book group import contains duplicate ids")
	ErrAddressBookGroupImportTooMany         = fmt.Errorf("address book group import must contain at most %d groups", MaxAddressBookGroupImportGroups)
	ErrAddressBookGroupImportName            = errors.New("imported address book group name must be 1 to 64 characters")
	ErrAddressBookGroupImportConflict        = errors.New("address book group import id already exists")
)

// AddressBookGroupExport is the portable, connection-free shape of one live
// address-book group. Revisions, timestamps and tombstones stay local.
type AddressBookGroupExport struct {
	ID       string
	Name     string
	ParentID string
	Position int64
}

// AddressBookGroupImport is one group from a portable document. The importer
// accepts only structure; devices and their membership are never inferred or
// transferred.
type AddressBookGroupImport struct {
	ID       string
	Name     string
	ParentID string
	Position int64
}

// AddressBookGroupImportResult classifies every input. Created and Updated
// entries share one address-book revision, while Unchanged entries retain the
// stored revision and timestamp.
type AddressBookGroupImportResult struct {
	Revision  int64
	Created   []Group
	Updated   []Group
	Skipped   []string
	Unchanged []Group
}

type addressBookGroupImportAction struct {
	input AddressBookGroupImport
	kind  byte
}

// ExportAddressBookGroups returns only live groups in the same stable order as
// the signed-in user's address book.
func (s *Store) ExportAddressBookGroups(userID string) ([]AddressBookGroupExport, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var exists int
	err := s.db.QueryRow(`SELECT 1 FROM users WHERE id = ?`, userID).Scan(&exists)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrUnknownUser
	}
	if err != nil {
		return nil, fmt.Errorf("account store %s: read address book owner: %w", s.path, err)
	}
	groups, err := s.groupsLocked(userID)
	if err != nil {
		return nil, err
	}
	out := make([]AddressBookGroupExport, 0, len(groups))
	for _, group := range groups {
		out = append(out, AddressBookGroupExport{
			ID:       group.ID,
			Name:     group.Name,
			ParentID: group.ParentID,
			Position: group.Position,
		})
	}
	return out, nil
}

// ImportAddressBookGroups validates the complete package and the resulting
// tree before the first write. conflict=reject aborts on any live id,
// conflict=skip leaves live ids untouched, and conflict=update replaces their
// name, parent and position when they differ. Every actual write shares one
// revision and timestamp.
func (s *Store) ImportAddressBookGroups(
	userID string,
	groups []AddressBookGroupImport,
	conflict string,
) (AddressBookGroupImportResult, error) {
	conflict = strings.TrimSpace(conflict)
	if conflict == "" {
		conflict = AddressBookGroupImportConflictReject
	}
	if conflict != AddressBookGroupImportConflictReject &&
		conflict != AddressBookGroupImportConflictSkip &&
		conflict != AddressBookGroupImportConflictUpdate {
		return AddressBookGroupImportResult{}, ErrInvalidAddressBookGroupImportConflict
	}
	if len(groups) > MaxAddressBookGroupImportGroups {
		return AddressBookGroupImportResult{}, ErrAddressBookGroupImportTooMany
	}

	prepared := make([]AddressBookGroupImport, 0, len(groups))
	seen := make(map[string]struct{}, len(groups))
	for _, input := range groups {
		if !registry.ValidID(input.ID) {
			return AddressBookGroupImportResult{}, ErrInvalidGroupID
		}
		if _, duplicate := seen[input.ID]; duplicate {
			return AddressBookGroupImportResult{}, fmt.Errorf("%w: %s", ErrAddressBookGroupImportDuplicateID, input.ID)
		}
		seen[input.ID] = struct{}{}
		name := normalizeText(input.Name, maxName)
		if name == "" || utf8.RuneCountInString(name) > maxAddressBookGroupImportName {
			return AddressBookGroupImportResult{}, fmt.Errorf("%w: %s", ErrAddressBookGroupImportName, input.ID)
		}
		parentID := strings.TrimSpace(input.ParentID)
		if parentID != "" && !registry.ValidID(parentID) {
			return AddressBookGroupImportResult{}, ErrInvalidGroupID
		}
		if parentID == input.ID {
			return AddressBookGroupImportResult{}, ErrGroupCycle
		}
		if input.Position < 0 {
			return AddressBookGroupImportResult{}, ErrInvalidPosition
		}
		prepared = append(prepared, AddressBookGroupImport{
			ID:       input.ID,
			Name:     name,
			ParentID: parentID,
			Position: input.Position,
		})
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return AddressBookGroupImportResult{}, fmt.Errorf("account store %s: begin address book group import: %w", s.path, err)
	}
	defer tx.Rollback()
	var owner int
	if err := tx.QueryRow(`SELECT 1 FROM users WHERE id = ?`, userID).Scan(&owner); errors.Is(err, sql.ErrNoRows) {
		return AddressBookGroupImportResult{}, ErrUnknownUser
	} else if err != nil {
		return AddressBookGroupImportResult{}, fmt.Errorf("account store %s: read address book owner: %w", s.path, err)
	}

	live, err := liveAddressBookGroupsLocked(tx, userID)
	if err != nil {
		return AddressBookGroupImportResult{}, fmt.Errorf("account store %s: read imported address book groups: %w", s.path, err)
	}
	for _, input := range prepared {
		if input.ParentID == "" || live[input.ParentID].ID != "" {
			continue
		}
		if _, inPackage := seen[input.ParentID]; !inPackage {
			return AddressBookGroupImportResult{}, fmt.Errorf("%w: %s", ErrUnknownGroup, input.ParentID)
		}
	}

	actions := make([]addressBookGroupImportAction, 0, len(prepared))
	final := make(map[string]AddressBookGroupImport, len(live)+len(prepared))
	for id, group := range live {
		final[id] = AddressBookGroupImport{
			ID:       group.ID,
			Name:     group.Name,
			ParentID: group.ParentID,
			Position: group.Position,
		}
	}
	for _, input := range prepared {
		existing, exists := live[input.ID]
		if exists {
			switch conflict {
			case AddressBookGroupImportConflictReject:
				return AddressBookGroupImportResult{}, fmt.Errorf("%w: %s", ErrAddressBookGroupImportConflict, input.ID)
			case AddressBookGroupImportConflictSkip:
				actions = append(actions, addressBookGroupImportAction{input: input, kind: 's'})
				continue
			}
		}
		final[input.ID] = input
		kind := byte('c')
		if exists {
			if existing.Name == input.Name && existing.ParentID == input.ParentID && existing.Position == input.Position {
				kind = 'u'
			} else {
				kind = 'w'
			}
		}
		actions = append(actions, addressBookGroupImportAction{input: input, kind: kind})
	}

	if err := validateAddressBookGroupTree(final); err != nil {
		return AddressBookGroupImportResult{}, err
	}

	writeCount := 0
	for _, action := range actions {
		if action.kind == 'c' || action.kind == 'w' {
			writeCount++
		}
	}
	var revision int64
	var now = s.now().UTC()
	if writeCount > 0 {
		revision, err = nextRevision(tx, userID)
		if err != nil {
			return AddressBookGroupImportResult{}, fmt.Errorf("account store %s: advance imported address book revision: %w", s.path, err)
		}
		for _, action := range actions {
			if action.kind != 'c' && action.kind != 'w' {
				continue
			}
			input := action.input
			if _, err := tx.Exec(
				`INSERT INTO address_book_groups
					(user_id, group_id, name, parent_id, position, updated_at, revision, deleted)
				 VALUES (?, ?, ?, ?, ?, ?, ?, false)
				 ON CONFLICT(user_id, group_id) DO UPDATE SET
					name = excluded.name, parent_id = excluded.parent_id,
					position = excluded.position, updated_at = excluded.updated_at,
					revision = excluded.revision, deleted = false`,
				userID, input.ID, input.Name, input.ParentID, input.Position,
				unixMillis(now), revision,
			); err != nil {
				return AddressBookGroupImportResult{}, fmt.Errorf("account store %s: import address book group: %w", s.path, err)
			}
		}
	}
	if err := tx.Commit(); err != nil {
		return AddressBookGroupImportResult{}, fmt.Errorf("account store %s: commit address book group import: %w", s.path, err)
	}

	result := AddressBookGroupImportResult{Revision: revision}
	for _, action := range actions {
		input := action.input
		switch action.kind {
		case 'c':
			result.Created = append(result.Created, Group{
				ID: input.ID, Name: input.Name, ParentID: input.ParentID,
				Position: input.Position, UpdatedAt: now, Revision: revision,
			})
		case 'w':
			result.Updated = append(result.Updated, Group{
				ID: input.ID, Name: input.Name, ParentID: input.ParentID,
				Position: input.Position, UpdatedAt: now, Revision: revision,
			})
		case 's':
			result.Skipped = append(result.Skipped, input.ID)
		case 'u':
			result.Unchanged = append(result.Unchanged, live[input.ID])
		}
	}
	return result, nil
}

func liveAddressBookGroupsLocked(tx *sql.Tx, userID string) (map[string]Group, error) {
	rows, err := tx.Query(
		`SELECT group_id, name, parent_id, position, updated_at, revision
		 FROM address_book_groups WHERE user_id = ? AND deleted = 0`,
		userID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := make(map[string]Group)
	for rows.Next() {
		var (
			group     Group
			updatedAt int64
		)
		if err := rows.Scan(&group.ID, &group.Name, &group.ParentID, &group.Position, &updatedAt, &group.Revision); err != nil {
			return nil, err
		}
		group.UpdatedAt = timeFromMillis(updatedAt)
		out[group.ID] = group
	}
	return out, rows.Err()
}

// validateAddressBookGroupTree checks every path in the final merged tree.
// Repeating the bounded walk for each node also catches cycles that pass
// through groups outside the imported package.
func validateAddressBookGroupTree(groups map[string]AddressBookGroupImport) error {
	for id := range groups {
		seen := make(map[string]struct{}, maxGroupDepth+1)
		current := id
		for current != "" {
			if _, cycle := seen[current]; cycle {
				return ErrGroupCycle
			}
			seen[current] = struct{}{}
			if len(seen) > maxGroupDepth {
				return ErrGroupDepth
			}
			group, ok := groups[current]
			if !ok {
				return ErrUnknownGroup
			}
			current = group.ParentID
		}
	}
	return nil
}
