package api

import (
	"net/http"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// lookupSubject is the single identity a lookup request presented. Shared
// deployment credentials carry no account subject and therefore cannot satisfy
// a restricted device policy. Management and link tokens are both shared; they
// stay distinguishable so a later emergency-access path can require an explicit
// request instead of inheriting the console key's reach.
type lookupSubject struct {
	UserID     string
	Username   string
	Management bool
	Shared     bool
}

// resolveLookupSubject classifies the Authorization header instead of letting
// one credential type silently win. A token that is both a configured shared
// secret and a live account token is rejected as ambiguous.
func (s *server) resolveLookupSubject(r *http.Request) (lookupSubject, bool) {
	token, hasToken := bearerToken(r)
	management := s.serverKeyAuthorized(r)
	link := s.cfg.LinkKey != "" && bearerMatches(s.cfg.LinkKey, r)
	var (
		user     account.User
		userOK   bool
		accounts = s.cfg.Accounts
	)
	if hasToken && accounts != nil {
		if found, err := accounts.Authenticate(token); err == nil {
			user, userOK = found, true
		}
	}
	if userOK && (management || link) {
		return lookupSubject{}, false
	}
	switch {
	case management:
		return lookupSubject{Management: true, Shared: true}, true
	case link:
		return lookupSubject{Shared: true}, true
	case userOK:
		return lookupSubject{UserID: user.ID, Username: user.Username}, true
	case s.cfg.ServerKey == "" && s.cfg.LinkKey == "" && !hasToken:
		// A deployment with no viewer credential keeps the historical open
		// lookup, but a restricted device still sees no account subject.
		return lookupSubject{Shared: true}, true
	}
	return lookupSubject{}, false
}

// deviceAdmission applies the policy and then the owner assignment. The owner
// is an authenticated account, so a banned owner never reaches this call: the
// session would already be refused.
func (s *server) deviceAdmission(peerID, userID, username string) (account.DeviceAdmission, error) {
	admission, err := s.cfg.Accounts.CheckDeviceAdmission(peerID, userID)
	if err != nil {
		return admission, err
	}
	if !admission.Allowed && username != "" {
		if record, ok := s.cfg.Registry.Identity(peerID); ok &&
			record.Owner != "" && record.Owner == username {
			admission.Allowed = true
			admission.Reason = "device owner"
		}
	}
	return admission, nil
}

// lookupAdmission answers whether the subject may learn this device's
// addresses. Managed open devices keep admitting any caller; restricted
// devices need an allowed account. The host still performs local
// authentication; this is server-side admission, not the final access ticket.
func (s *server) lookupAdmission(hostID string, subject lookupSubject) (bool, string, error) {
	// A disabled device is not merely offline: even its durable record (name,
	// fingerprint, binding) must not be handed to a viewer.
	if record, ok := s.cfg.Registry.Identity(hostID); ok && record.Disabled {
		return false, "device is disabled", nil
	}
	if s.cfg.Accounts == nil {
		return true, "", nil
	}
	admission, err := s.deviceAdmission(hostID, subject.UserID, subject.Username)
	if err != nil {
		return false, "", err
	}
	if !admission.Managed || admission.Allowed {
		return true, "", nil
	}
	if subject.Shared {
		return false, "device policy requires an account with access", nil
	}
	return false, admission.Reason, nil
}
