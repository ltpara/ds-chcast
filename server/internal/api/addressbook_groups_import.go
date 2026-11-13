package api

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

const maxAddressBookGroupImportBody = 1 << 20

type addressBookGroupTransferGroup struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	ParentID string `json:"parent_id"`
	Position int64  `json:"position"`
}

type addressBookGroupExportResponse struct {
	Format  string                          `json:"format"`
	Version int                             `json:"version"`
	Groups  []addressBookGroupTransferGroup `json:"groups"`
}

type addressBookGroupImportRequest struct {
	Format   string                           `json:"format"`
	Version  int                              `json:"version"`
	Conflict string                           `json:"conflict,omitempty"`
	Groups   *[]addressBookGroupTransferGroup `json:"groups"`
}

type addressBookGroupImportResponse struct {
	Revision  int64              `json:"revision"`
	Created   []addressBookGroup `json:"created"`
	Updated   []addressBookGroup `json:"updated"`
	Skipped   []string           `json:"skipped"`
	Unchanged []addressBookGroup `json:"unchanged"`
}

func (s *server) exportAddressBookGroups(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	groups, err := s.cfg.Accounts.ExportAddressBookGroups(user.ID)
	if err != nil {
		s.cfg.Logger.Error("address book group export", "error", err)
		writeError(w, http.StatusInternalServerError, "could not export the address book groups")
		return
	}
	out := addressBookGroupExportResponse{
		Format:  account.AddressBookGroupDocumentFormat,
		Version: account.AddressBookGroupDocumentVersion,
		Groups:  make([]addressBookGroupTransferGroup, 0, len(groups)),
	}
	for _, group := range groups {
		out.Groups = append(out.Groups, addressBookGroupTransferGroup{
			ID:       group.ID,
			Name:     group.Name,
			ParentID: group.ParentID,
			Position: group.Position,
		})
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *server) importAddressBookGroups(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req addressBookGroupImportRequest
	if err := decodeAddressBookGroupImportBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Format != account.AddressBookGroupDocumentFormat {
		writeError(w, http.StatusBadRequest, "format must be "+account.AddressBookGroupDocumentFormat)
		return
	}
	if req.Version != account.AddressBookGroupDocumentVersion {
		writeError(w, http.StatusBadRequest, "version must be "+strconv.Itoa(account.AddressBookGroupDocumentVersion))
		return
	}
	if req.Groups == nil {
		writeError(w, http.StatusBadRequest, "groups is required")
		return
	}
	groups := make([]account.AddressBookGroupImport, 0, len(*req.Groups))
	for _, group := range *req.Groups {
		groups = append(groups, account.AddressBookGroupImport{
			ID:       group.ID,
			Name:     group.Name,
			ParentID: group.ParentID,
			Position: group.Position,
		})
	}
	conflict := strings.TrimSpace(req.Conflict)
	if conflict == "" {
		conflict = account.AddressBookGroupImportConflictReject
	}
	result, err := s.cfg.Accounts.ImportAddressBookGroups(user.ID, groups, conflict)
	if err != nil {
		s.writeAddressBookGroupImportError(w, err)
		return
	}
	s.recordAudit(r, user.Username, account.AuditAddressBookGroupImport,
		"conflict="+conflict+": "+strconv.Itoa(len(result.Created))+" created, "+
			strconv.Itoa(len(result.Updated))+" updated, "+
			strconv.Itoa(len(result.Skipped))+" skipped, "+
			strconv.Itoa(len(result.Unchanged))+" unchanged", true)
	out := addressBookGroupImportResponse{
		Revision:  result.Revision,
		Created:   make([]addressBookGroup, 0, len(result.Created)),
		Updated:   make([]addressBookGroup, 0, len(result.Updated)),
		Skipped:   append([]string(nil), result.Skipped...),
		Unchanged: make([]addressBookGroup, 0, len(result.Unchanged)),
	}
	if out.Skipped == nil {
		out.Skipped = []string{}
	}
	for _, group := range result.Created {
		out.Created = append(out.Created, viewAddressBookGroup(group))
	}
	for _, group := range result.Updated {
		out.Updated = append(out.Updated, viewAddressBookGroup(group))
	}
	for _, group := range result.Unchanged {
		out.Unchanged = append(out.Unchanged, viewAddressBookGroup(group))
	}
	writeJSON(w, http.StatusOK, out)
}

func decodeAddressBookGroupImportBody(w http.ResponseWriter, r *http.Request, dst any) error {
	defer r.Body.Close()
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxAddressBookGroupImportBody))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}
	var trailing any
	if err := dec.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("invalid JSON: trailing data")
		}
		return fmt.Errorf("invalid JSON: %w", err)
	}
	return nil
}

func (s *server) writeAddressBookGroupImportError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrInvalidAddressBookGroupImportConflict),
		errors.Is(err, account.ErrAddressBookGroupImportDuplicateID),
		errors.Is(err, account.ErrAddressBookGroupImportTooMany),
		errors.Is(err, account.ErrAddressBookGroupImportName):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrInvalidGroupID):
		writeError(w, http.StatusBadRequest, "group id is invalid")
	case errors.Is(err, account.ErrInvalidGroupName):
		writeError(w, http.StatusBadRequest, account.ErrInvalidGroupName.Error())
	case errors.Is(err, account.ErrUnknownGroup):
		writeError(w, http.StatusBadRequest, "parent group is unknown")
	case errors.Is(err, account.ErrGroupCycle):
		writeError(w, http.StatusBadRequest, "group nesting contains a cycle")
	case errors.Is(err, account.ErrGroupDepth):
		writeError(w, http.StatusBadRequest, "group nesting is too deep")
	case errors.Is(err, account.ErrInvalidPosition):
		writeError(w, http.StatusBadRequest, "position must not be negative")
	case errors.Is(err, account.ErrAddressBookGroupImportConflict):
		writeError(w, http.StatusConflict, err.Error())
	default:
		s.cfg.Logger.Error("address book group import", "error", err)
		writeError(w, http.StatusInternalServerError, "could not import the address book groups")
	}
}
