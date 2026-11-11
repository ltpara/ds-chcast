package api

import (
	"crypto/rand"
	"encoding/base64"
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const accessTicketLifetime = 60 * time.Second

var (
	errTicketNotConfigured = errors.New("access tickets are not configured")
	errTicketReplayFull    = errors.New("access ticket replay cache is full")
)

// accessTicketGrant is one signed ticket plus everything a caller needs to
// report it on the wire.
type accessTicketGrant struct {
	signed        []byte
	issuedAt      time.Time
	expiresAt     time.Time
	policyVersion uint32
	permissions   uint32
	subject       string
}

// mintAccessTicket signs the credential shared by the exchange endpoint and
// lookup. The caller has already resolved the account subject, device record
// and admission; this function only enforces the replay cache and the wire
// format. A nil issuer is a configuration error, not an admission decision.
func (s *server) mintAccessTicket(peerID, subject, fingerprint string, clientKey []byte,
	requested uint32, admission account.DeviceAdmission, now time.Time) (accessTicketGrant, error) {
	if s.accessIssuer == nil {
		return accessTicketGrant{}, errTicketNotConfigured
	}
	permissions := requested
	if admission.PermissionCeiling != 0 {
		permissions &= admission.PermissionCeiling
	}
	expires := now.Add(accessTicketLifetime)
	id, err := randomTicketID()
	if err != nil {
		return accessTicketGrant{}, err
	}
	ticket := acl.Ticket{
		Audience:                 "device:" + peerID,
		HostID:                   peerID,
		HostPublicKeyFingerprint: ticketFingerprint(fingerprint),
		Subject:                  subject,
		Permissions:              permissions,
		PolicyVersion:            admission.PolicyVersion,
		IssuedAt:                 now.Unix(),
		NotBefore:                now.Unix(),
		ExpiresAt:                expires.Unix(),
		ID:                       id,
	}
	copy(ticket.ClientPublicKey[:], clientKey)
	// The endpoint consumes the id before signing so a failed signature does
	// not leave a usable ticket; the grant below is the only copy handed out.
	if !s.ticketReplay.Consume(ticket.ID, ticket.ExpiresAt, now) {
		return accessTicketGrant{}, errTicketReplayFull
	}
	signed, err := s.accessIssuer.Sign(ticket, now)
	if err != nil {
		return accessTicketGrant{}, err
	}
	return accessTicketGrant{
		signed:        signed,
		issuedAt:      now,
		expiresAt:     expires,
		policyVersion: admission.PolicyVersion,
		permissions:   permissions,
		subject:       subject,
	}, nil
}

// ticketFingerprint renders the host certificate digest in the self-describing
// spelling the frozen ticket format uses. Hosts register the bare digest in
// either hex case, so only a 64-digit digest is rewritten.
func ticketFingerprint(fingerprint string) string {
	trimmed := strings.TrimSpace(fingerprint)
	if len(trimmed) != 64 {
		return fingerprint
	}
	digits := make([]byte, 0, 64)
	for i := 0; i < len(trimmed); i++ {
		c := trimmed[i]
		switch {
		case c >= '0' && c <= '9':
			digits = append(digits, c)
		case c >= 'a' && c <= 'f':
			digits = append(digits, c)
		case c >= 'A' && c <= 'F':
			digits = append(digits, c-'A'+'a')
		default:
			return fingerprint
		}
	}
	return "SHA256:" + string(digits)
}

type accessTicketRequest struct {
	PeerID          string `json:"peer_id"`
	ClientPublicKey string `json:"client_public_key"`
	Permissions     uint32 `json:"permissions"`
}

type accessTicketResponse struct {
	Ticket        string `json:"ticket"`
	ExpiresAtMS   int64  `json:"expires_at_ms"`
	IssuedAtMS    int64  `json:"issued_at_ms"`
	PolicyVersion uint32 `json:"policy_version"`
	Issuer        string `json:"issuer"`
	KeyID         string `json:"kid"`
}

// issueAccessTicket mints the short-lived credential a managed viewer presents
// to a host. Only an account token can bind a subject; shared deployment keys
// have no place in an authorization ticket.
func (s *server) issueAccessTicket(w http.ResponseWriter, r *http.Request) {
	if s.accessIssuer == nil {
		writeError(w, http.StatusServiceUnavailable, "access tickets are not configured")
		return
	}
	if s.accessIssuerRevoked() {
		writeError(w, http.StatusServiceUnavailable, "the access ticket issuer key has been revoked")
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req accessTicketRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	peerID := strings.TrimSpace(req.PeerID)
	if !registry.ValidID(peerID) {
		writeError(w, http.StatusBadRequest, "host id must be 1..64 characters of [A-Za-z0-9_.-]")
		return
	}
	clientKey, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(req.ClientPublicKey))
	if err != nil || len(clientKey) != 32 {
		writeError(w, http.StatusBadRequest, "client_public_key must be 32 base64url bytes")
		return
	}
	record, exists := s.cfg.Registry.Identity(peerID)
	if !exists {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	if record.Disabled {
		writeError(w, http.StatusForbidden, "device is disabled")
		return
	}
	if record.Fingerprint == "" {
		writeError(w, http.StatusConflict, "host fingerprint is not available")
		return
	}
	admission, err := s.deviceAdmission(peerID, user.ID, user.Username)
	if err != nil {
		s.cfg.Logger.Error("access ticket admission", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not check the device policy")
		return
	}
	if !admission.Allowed {
		writeError(w, http.StatusForbidden, admission.Reason)
		return
	}
	permissions := req.Permissions
	now := time.Now().UTC()
	grant, err := s.mintAccessTicket(peerID, user.ID, record.Fingerprint, clientKey,
		permissions, admission, now)
	switch {
	case errors.Is(err, errTicketNotConfigured):
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	case errors.Is(err, errTicketReplayFull):
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	case err != nil:
		s.cfg.Logger.Error("access ticket sign", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not sign the access ticket")
		return
	}
	s.recordAudit(r, user.Username, account.AuditAccessTicket,
		"issued ticket for "+peerID+" policy "+strconv.FormatUint(uint64(grant.policyVersion), 10)+
			" permissions "+strconv.FormatUint(uint64(grant.permissions), 10), true)
	writeJSON(w, http.StatusCreated, accessTicketResponse{
		Ticket:        base64.RawURLEncoding.EncodeToString(grant.signed),
		ExpiresAtMS:   grant.expiresAt.UnixMilli(),
		IssuedAtMS:    grant.issuedAt.UnixMilli(),
		PolicyVersion: grant.policyVersion,
		Issuer:        s.accessIssuer.Name(),
		KeyID:         s.accessIssuer.KeyID(),
	})
}

func randomTicketID() (string, error) {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(raw), nil
}

// lookupAccessTicket is the short-lived credential a viewer presents on the
// connection it is about to open.
type lookupAccessTicket struct {
	Ticket        string `json:"ticket"`
	IssuedAtMS    int64  `json:"issued_at_ms"`
	ExpiresAtMS   int64  `json:"expires_at_ms"`
	ExpiresInMS   int64  `json:"expires_in_ms"`
	PolicyVersion uint32 `json:"policy_version"`
	Permissions   uint32 `json:"permissions"`
	Issuer        string `json:"issuer"`
	KeyID         string `json:"kid"`
	Subject       string `json:"subject"`
}

func (s *server) accessTicketView(grant accessTicketGrant) *lookupAccessTicket {
	return &lookupAccessTicket{
		Ticket:        base64.RawURLEncoding.EncodeToString(grant.signed),
		IssuedAtMS:    grant.issuedAt.UnixMilli(),
		ExpiresAtMS:   grant.expiresAt.UnixMilli(),
		ExpiresInMS:   grant.expiresAt.Sub(grant.issuedAt).Milliseconds(),
		PolicyVersion: grant.policyVersion,
		Permissions:   grant.permissions,
		Issuer:        s.accessIssuer.Name(),
		KeyID:         s.accessIssuer.KeyID(),
		Subject:       grant.subject,
	}
}

// attachLookupTicket fills resp.AccessTicket when the lookup carries the
// ephemeral Ed25519 public key (`client_public_key`) of the connection the
// viewer is about to open. A shared deployment credential has no account
// subject and never receives a ticket; an unmanaged device needs none, so
// lookup answers without one and the viewer keeps the local-auth path.
func (s *server) attachLookupTicket(w http.ResponseWriter, r *http.Request,
	subject lookupSubject, peerID, fingerprint string, resp *lookupResponse) bool {
	query := r.URL.Query()
	rawKey := strings.TrimSpace(query.Get("client_public_key"))
	if rawKey == "" {
		return true
	}
	if subject.UserID == "" {
		writeError(w, http.StatusForbidden, "an access ticket requires an account token")
		return false
	}
	if s.accessIssuer == nil {
		writeError(w, http.StatusServiceUnavailable, errTicketNotConfigured.Error())
		return false
	}
	if s.accessIssuerRevoked() {
		writeError(w, http.StatusServiceUnavailable, "the access ticket issuer key has been revoked")
		return false
	}
	clientKey, err := base64.RawURLEncoding.DecodeString(rawKey)
	if err != nil || len(clientKey) != 32 {
		writeError(w, http.StatusBadRequest, "client_public_key must be 32 base64url bytes")
		return false
	}
	requested := ^uint32(0)
	if raw := strings.TrimSpace(query.Get("permissions")); raw != "" {
		value, err := strconv.ParseUint(raw, 10, 32)
		if err != nil {
			writeError(w, http.StatusBadRequest, "permissions must be an unsigned 32-bit integer")
			return false
		}
		requested = uint32(value)
	}
	admission, err := s.deviceAdmission(peerID, subject.UserID, subject.Username)
	if err != nil {
		s.cfg.Logger.Error("lookup access ticket admission", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not check the device policy")
		return false
	}
	if !admission.Allowed {
		writeError(w, http.StatusForbidden, admission.Reason)
		return false
	}
	if !admission.Managed {
		// An unmanaged host pins no issuer and still authenticates locally;
		// handing it a ticket would be dead weight on the wire.
		return true
	}
	if strings.TrimSpace(fingerprint) == "" {
		writeError(w, http.StatusConflict, "host fingerprint is not available")
		return false
	}
	grant, err := s.mintAccessTicket(peerID, subject.UserID, fingerprint, clientKey,
		requested, admission, time.Now().UTC())
	switch {
	case errors.Is(err, errTicketNotConfigured):
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return false
	case errors.Is(err, errTicketReplayFull):
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return false
	case err != nil:
		s.cfg.Logger.Error("lookup access ticket sign", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not sign the access ticket")
		return false
	}
	s.recordAudit(r, subject.Username, account.AuditAccessTicket,
		"issued lookup ticket for "+peerID+" policy "+
			strconv.FormatUint(uint64(grant.policyVersion), 10)+" permissions "+
			strconv.FormatUint(uint64(grant.permissions), 10), true)
	resp.AccessTicket = s.accessTicketView(grant)
	return true
}

// lookupTransportTicket is one transport admission credential. The payload is
// opaque to the viewer: the Go agent carries it to the transport node and
// proves possession of the bound key.
type lookupTransportTicket struct {
	Ticket        string `json:"ticket"`
	Audience      string `json:"audience"`
	IssuedAtMS    int64  `json:"issued_at_ms"`
	ExpiresAtMS   int64  `json:"expires_at_ms"`
	ExpiresInMS   int64  `json:"expires_in_ms"`
	PolicyVersion uint32 `json:"policy_version"`
	Issuer        string `json:"issuer"`
	KeyID         string `json:"kid"`
	Subject       string `json:"subject"`
}

// transportCredentials is the rendered credential for each transport purpose.
type transportCredentials struct {
	punch   *lookupTransportTicket
	relay   *lookupTransportTicket
	carrier *lookupTransportTicket
}

// mintTransportCredentials signs one short-lived credential per transport
// audience. The caller has already checked the issuer state and admission.
func (s *server) mintTransportCredentials(peerID, subject string,
	admission account.DeviceAdmission, clientKey []byte, now time.Time) (transportCredentials, error) {
	var out transportCredentials
	slots := [...]**lookupTransportTicket{&out.punch, &out.relay, &out.carrier}
	expires := now.Add(accessTicketLifetime)
	for i, audience := range []string{
		acl.TransportAudiencePunch,
		acl.TransportAudienceRelay,
		acl.TransportAudienceCarrier,
	} {
		id, err := randomTicketID()
		if err != nil {
			return transportCredentials{}, err
		}
		ticket := acl.TransportTicket{
			Audience:      audience,
			HostID:        peerID,
			Subject:       subject,
			PolicyVersion: admission.PolicyVersion,
			IssuedAt:      now.Unix(),
			NotBefore:     now.Unix(),
			ExpiresAt:     expires.Unix(),
			ID:            id,
		}
		copy(ticket.ClientPublicKey[:], clientKey)
		signed, err := s.accessIssuer.SignTransport(ticket, now)
		if err != nil {
			return transportCredentials{}, err
		}
		*slots[i] = &lookupTransportTicket{
			Ticket:        base64.RawURLEncoding.EncodeToString(signed),
			Audience:      audience,
			IssuedAtMS:    now.UnixMilli(),
			ExpiresAtMS:   expires.UnixMilli(),
			ExpiresInMS:   expires.Sub(now).Milliseconds(),
			PolicyVersion: admission.PolicyVersion,
			Issuer:        s.accessIssuer.Name(),
			KeyID:         s.accessIssuer.KeyID(),
			Subject:       subject,
		}
	}
	return out, nil
}

// attachLookupTransport fills the punch, relay and carrier credentials when
// the lookup carries the agent's ephemeral transport_public_key. Transport
// credentials are deliberately separate from access tickets: they bind a
// transport purpose, never application permissions, and the host still runs
// the full access-ticket handshake afterwards.
func (s *server) attachLookupTransport(w http.ResponseWriter, r *http.Request,
	subject lookupSubject, peerID string, resp *lookupResponse) bool {
	rawKey := strings.TrimSpace(r.URL.Query().Get("transport_public_key"))
	if rawKey == "" {
		return true
	}
	clientKey, err := base64.RawURLEncoding.DecodeString(rawKey)
	if err != nil || len(clientKey) != 32 {
		writeError(w, http.StatusBadRequest, "transport_public_key must be 32 base64url bytes")
		return false
	}
	if s.cfg.Accounts == nil {
		return true
	}
	admission, err := s.deviceAdmission(peerID, subject.UserID, subject.Username)
	if err != nil {
		s.cfg.Logger.Error("lookup transport admission", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not check the device policy")
		return false
	}
	if !admission.Allowed {
		writeError(w, http.StatusForbidden, admission.Reason)
		return false
	}
	if s.accessIssuer == nil {
		// Without an issuer a deployment keeps the legacy shared-key punch and
		// carrier token. A managed device must not silently fall back, because
		// its transport checks require a central credential.
		if admission.Managed {
			writeError(w, http.StatusServiceUnavailable, errTicketNotConfigured.Error())
			return false
		}
		return true
	}
	if s.accessIssuerRevoked() {
		writeError(w, http.StatusServiceUnavailable, "the access ticket issuer key has been revoked")
		return false
	}
	subjectText := subject.UserID
	switch {
	case subjectText != "":
	case subject.Management:
		subjectText = "management"
	default:
		subjectText = "shared"
	}
	now := time.Now().UTC()
	credentials, err := s.mintTransportCredentials(peerID, subjectText, admission, clientKey, now)
	if err != nil {
		s.cfg.Logger.Error("lookup transport ticket", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not mint the transport ticket")
		return false
	}
	resp.TransportPunch = credentials.punch
	resp.TransportRelay = credentials.relay
	resp.TransportCarrier = credentials.carrier
	return true
}
