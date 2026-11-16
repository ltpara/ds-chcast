package api

import (
	"errors"
	"net/http"
	"strconv"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

func (s *server) clientStrategyCatalog(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"version": 1, "settings": strategy.Catalog()})
}

func (s *server) getClientStrategy(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	config, err := s.cfg.Accounts.ClientStrategy(r.PathValue("id"))
	if err != nil {
		s.writeClientStrategyError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, config)
}

func (s *server) setClientStrategy(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	var req struct {
		IfRevision *int64            `json:"if_revision"`
		Settings   strategy.Settings `json:"settings"`
	}
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.IfRevision == nil || req.Settings == nil {
		writeError(w, http.StatusBadRequest, "if_revision and settings are required")
		return
	}
	config, err := s.cfg.Accounts.SetClientStrategy(r.PathValue("id"), *req.IfRevision, req.Settings, principal.Actor)
	if err != nil {
		s.writeClientStrategyError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditClientStrategyUpdate,
		"saved client strategy for policy "+config.PolicyID+" at revision "+strconv.FormatInt(config.Revision, 10), true)
	writeJSON(w, http.StatusOK, config)
}

// This endpoint is a management preview, not an application receipt. A host
// registration/lookup consumer will use a separate authenticated delivery path.
func (s *server) previewClientStrategy(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	peerID := r.PathValue("peer")
	if _, ok := s.deviceActor(w, r, consoleRead, peerID); !ok {
		return
	}
	record, exists := s.cfg.Registry.Identity(peerID)
	if !exists {
		writeError(w, http.StatusNotFound, "unknown host")
		return
	}
	target := r.URL.Query().Get("target")
	if target == "" {
		target = strategy.Host
	}
	snapshot, err := s.cfg.Accounts.ResolveClientStrategy(peerID, target)
	if err != nil {
		s.writeClientStrategyError(w, err)
		return
	}
	receipt, err := s.cfg.Accounts.CurrentClientStrategyReceipt(snapshot, record.PublicKey)
	if err != nil {
		s.writeClientStrategyError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, struct {
		account.ClientStrategySnapshot
		Receipt *account.ClientStrategyReceipt `json:"receipt"`
	}{snapshot, receipt})
}

// attachLookupViewerStrategy delivers viewer-owned settings only to a live
// account token. Deployment and link keys deliberately receive no strategy:
// they have no stable account identity for receipt sequencing or isolation.
// Failure to materialise optional preferences does not revoke the access
// ticket; the viewer falls back to local settings and reports no success.
func (s *server) attachLookupViewerStrategy(r *http.Request, subject lookupSubject, peerID string, resp *lookupResponse) {
	clientKey := r.URL.Query().Get("client_public_key")
	if s.cfg.Accounts == nil || subject.UserID == "" || clientKey == "" {
		return
	}
	snapshot, err := s.cfg.Accounts.ResolveClientStrategy(peerID, strategy.Viewer)
	if err != nil {
		s.cfg.Logger.Warn("resolve viewer strategy", "peer", peerID, "error", err)
		return
	}
	delivery, err := s.cfg.Accounts.CreateViewerStrategyDelivery(subject.UserID, peerID, clientKey, snapshot)
	if err != nil {
		s.cfg.Logger.Warn("create viewer strategy delivery", "peer", peerID, "error", err)
		return
	}
	resp.ViewerStrategy = &delivery
}

func (s *server) verifyHostClientStrategy(w http.ResponseWriter, r *http.Request, timestamp int64, signature string, message []byte) string {
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential required")
		return ""
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return ""
	}
	peer := r.PathValue("peer")
	if !registry.ValidID(peer) {
		writeError(w, http.StatusBadRequest, "invalid host id")
		return ""
	}
	record, ok := s.cfg.Registry.Identity(peer)
	if !ok {
		writeError(w, http.StatusNotFound, "unknown host")
		return ""
	}
	if record.Disabled {
		writeError(w, http.StatusForbidden, "host identity is disabled")
		return ""
	}
	now := time.Now()
	signedAt := time.Unix(timestamp, 0)
	if timestamp == 0 || now.Sub(signedAt) > maxSkew || signedAt.Sub(now) > maxSkew {
		writeError(w, http.StatusUnauthorized, "strategy timestamp is outside the allowed window")
		return ""
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil || !identity.Verify(pub, message, signature) {
		writeError(w, http.StatusUnauthorized, "strategy signature was not accepted")
		return ""
	}
	return record.PublicKey
}

func (s *server) fetchHostClientStrategy(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query()
	timestamp, _ := strconv.ParseInt(query.Get("timestamp"), 10, 64)
	if s.verifyHostClientStrategy(w, r, timestamp, query.Get("signature"), identity.HostClientStrategyFetchMessage(r.PathValue("peer"), timestamp)) == "" {
		return
	}
	snapshot, err := s.cfg.Accounts.ResolveClientStrategy(r.PathValue("peer"), strategy.Host)
	if err != nil {
		s.writeClientStrategyError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, snapshot)
}

func (s *server) reportHostClientStrategyReceipt(w http.ResponseWriter, r *http.Request) {
	// Check the shared credential before decoding; the device signature below
	// separately proves which registered host owns this result.
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential required")
		return
	}
	var req struct {
		Receipt   strategy.Receipt `json:"receipt"`
		Timestamp int64            `json:"timestamp"`
		Signature string           `json:"signature"`
	}
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	encoded, err := strategy.CanonicalReceipt(req.Receipt)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid receipt")
		return
	}
	peer := r.PathValue("peer")
	reporterKey := s.verifyHostClientStrategy(w, r, req.Timestamp, req.Signature, identity.HostClientStrategyReceiptMessage(peer, encoded, req.Timestamp))
	if reporterKey == "" {
		return
	}
	receipt, err := s.cfg.Accounts.RecordHostStrategyReceipt(peer, reporterKey, req.Receipt)
	if err != nil {
		s.writeClientStrategyError(w, err)
		return
	}
	if receipt.Changed {
		s.recordAudit(r, "host", account.AuditClientStrategyReceipt,
			"client strategy "+peer+" version "+strconv.FormatInt(receipt.Revision, 10)+" sequence "+strconv.FormatInt(receipt.Sequence, 10)+" "+receipt.Status,
			receipt.Status == strategy.Applied)
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, receipt)
}

// reportViewerClientStrategyReceipt accepts only an authenticated account
// receipt for the exact opaque delivery returned by lookup. The account and
// device in the URL are checked again against the delivery context in the
// account store.
func (s *server) reportViewerClientStrategyReceipt(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req struct {
		Context         string           `json:"context"`
		ClientPublicKey string           `json:"client_public_key"`
		Receipt         strategy.Receipt `json:"receipt"`
	}
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	receipt, err := s.cfg.Accounts.RecordViewerStrategyReceipt(
		user.ID, r.PathValue("peer"), req.Context, req.ClientPublicKey, req.Receipt,
	)
	if err != nil {
		s.writeClientStrategyError(w, err)
		return
	}
	if receipt.Changed {
		s.recordAudit(r, user.Username, account.AuditClientStrategyReceipt,
			"viewer client strategy "+r.PathValue("peer")+" version "+
				strconv.FormatInt(receipt.Revision, 10)+" sequence "+
				strconv.FormatInt(receipt.Sequence, 10)+" "+receipt.Status,
			receipt.Status == strategy.Applied)
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, receipt)
}

func (s *server) writeClientStrategyError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownPolicyObject):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrStrategyConflict):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, strategy.ErrInvalid), errors.Is(err, account.ErrInvalidPeerID):
		writeError(w, http.StatusBadRequest, err.Error())
	default:
		s.cfg.Logger.Error("client strategy", "error", err)
		writeError(w, http.StatusInternalServerError, "could not process client strategy")
	}
}
