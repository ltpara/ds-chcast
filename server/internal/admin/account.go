package admin

import (
	"crypto/rand"
	"encoding/base64"
	"fmt"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

const accountUsage = `user list | user add <username> [password]
user passwd <username> [password] | user del <username>
user role <username> admin|user
user ban <username> | user unban <username>
user email <username> <email|-> | user recover <username>
user phone <username> <phone|-> | user sms <username>
user 2fa <username> off | user unlock <username>
ab <username> [count]
`

// accountText manages viewer accounts. A missing password generates one and
// prints it once, because the loopback console is the only place an operator
// can read it without putting a secret on the command line.
func (c *Console) accountText(fields []string) string {
	if c.cfg.Accounts == nil {
		return "accounts require persistent state\n"
	}
	if len(fields) == 0 || fields[0] == "list" {
		return c.userListText()
	}
	command := fields[0]
	if len(fields) < 2 {
		return accountUsage
	}
	username := fields[1]
	switch command {
	case "add", "create":
		password := strings.Join(fields[2:], " ")
		generated := false
		if password == "" {
			var err error
			password, err = randomPassword()
			if err != nil {
				return fmt.Sprintf("could not generate a password: %v\n", err)
			}
			generated = true
		}
		user, err := c.cfg.Accounts.CreateUser(username, password)
		if err != nil {
			return fmt.Sprintf("could not create user: %v\n", err)
		}
		var b strings.Builder
		fmt.Fprintf(&b, "%s created id=%s role=%s\n", user.Username, user.ID, user.Role)
		if generated {
			fmt.Fprintf(&b, "password: %s\n", password)
		}
		return b.String()
	case "passwd", "password", "set-password":
		password := strings.Join(fields[2:], " ")
		generated := false
		if password == "" {
			var err error
			password, err = randomPassword()
			if err != nil {
				return fmt.Sprintf("could not generate a password: %v\n", err)
			}
			generated = true
		}
		if err := c.cfg.Accounts.SetPassword(username, password); err != nil {
			return fmt.Sprintf("could not update password: %v\n", err)
		}
		if generated {
			return fmt.Sprintf("%s password updated\npassword: %s\n", username, password)
		}
		return fmt.Sprintf("%s password updated\n", username)
	case "del", "delete", "remove":
		if err := c.cfg.Accounts.DeleteUser(username); err != nil {
			return fmt.Sprintf("could not delete user: %v\n", err)
		}
		return fmt.Sprintf("%s deleted\n", username)
	case "role":
		if len(fields) != 3 {
			return accountUsage
		}
		user, err := c.cfg.Accounts.SetRole(username, fields[2])
		if err != nil {
			return fmt.Sprintf("could not set role: %v\n", err)
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: username,
			Action:   account.AuditRoleChange,
			Detail:   "role=" + user.Role + " from admin console",
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			return fmt.Sprintf("%s role=%s; audit not recorded: %v\n", username, user.Role, err)
		}
		return fmt.Sprintf("%s role=%s\n", username, user.Role)
	case "ban", "unban", "block", "unblock":
		banned := command == "ban" || command == "block"
		user, err := c.cfg.Accounts.SetBanned(username, banned)
		if err != nil {
			return fmt.Sprintf("could not change ban state: %v\n", err)
		}
		action, verb := account.AuditAccountUnban, "unbanned"
		if banned {
			action, verb = account.AuditAccountBan, "banned"
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: username,
			Action:   action,
			Detail:   verb + " from admin console",
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			return fmt.Sprintf("%s %s; audit not recorded: %v\n", user.Username, verb, err)
		}
		return fmt.Sprintf("%s %s\n", user.Username, verb)
	case "email", "set-email":
		if len(fields) < 3 {
			return accountUsage
		}
		email := fields[2]
		if email == "-" {
			email = ""
		}
		user, err := c.cfg.Accounts.SetEmail(username, email)
		if err != nil {
			return fmt.Sprintf("could not set recovery email: %v\n", err)
		}
		detail := "recovery email cleared from admin console"
		if user.Email != "" {
			detail = "recovery email set from admin console"
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: user.Username,
			Action:   account.AuditAccountEmail,
			Detail:   detail,
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			return fmt.Sprintf("%s email updated; audit not recorded: %v\n", user.Username, err)
		}
		if user.Email == "" {
			return fmt.Sprintf("%s recovery email cleared\n", user.Username)
		}
		return fmt.Sprintf("%s recovery email=%s\n", user.Username, user.Email)
	case "recover", "reset-email", "email-reset":
		ttl := c.cfg.RecoveryTTL
		if ttl <= 0 {
			ttl = 30 * time.Minute
		}
		token, user, expires, err := c.cfg.Accounts.CreatePasswordReset(username, ttl)
		if err != nil {
			return fmt.Sprintf("could not create password reset: %v\n", err)
		}
		link := c.recoveryLink(token)
		if c.cfg.Mailer != nil {
			if err := c.cfg.Mailer.SendPasswordReset(user.Email, user.Username, token, link, ttl); err != nil {
				if auditErr := c.cfg.Accounts.AppendAudit(account.AuditEvent{
					Username: user.Username,
					Action:   account.AuditAccountRecover,
					Detail:   "reset email failed from admin console",
					ClientIP: "admin-console",
					Success:  false,
				}); auditErr != nil {
					c.cfg.Logger.Warn("reset email audit", "user", user.Username, "error", auditErr)
				}
				return fmt.Sprintf("could not send reset email to %s: %v\ntoken: %s\nexpires: %s\n",
					user.Email, err, token, expires.Format(time.RFC3339))
			}
			if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
				Username: user.Username,
				Action:   account.AuditAccountRecover,
				Detail:   "reset email sent from admin console",
				ClientIP: "admin-console",
				Success:  true,
			}); err != nil {
				c.cfg.Logger.Warn("reset email audit", "user", user.Username, "error", err)
			}
			return fmt.Sprintf("password reset email sent to %s, expires %s\n",
				user.Email, expires.Format(time.RFC3339))
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: user.Username,
			Action:   account.AuditAccountRecover,
			Detail:   "reset token issued for manual delivery from admin console",
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			c.cfg.Logger.Warn("reset token audit", "user", user.Username, "error", err)
		}
		var b strings.Builder
		fmt.Fprintf(&b, "no SMTP configured; deliver this token out of band\n")
		fmt.Fprintf(&b, "token: %s\nexpires: %s\n", token, expires.Format(time.RFC3339))
		if link != "" {
			fmt.Fprintf(&b, "link: %s\n", link)
		}
		return b.String()
	case "phone", "set-phone":
		if len(fields) < 3 {
			return accountUsage
		}
		phone := strings.Join(fields[2:], " ")
		if phone == "-" {
			phone = ""
		}
		user, err := c.cfg.Accounts.SetPhone(username, phone)
		if err != nil {
			return fmt.Sprintf("could not set recovery phone: %v\n", err)
		}
		detail := "recovery phone cleared from admin console"
		if user.Phone != "" {
			detail = "recovery phone set from admin console"
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: user.Username,
			Action:   account.AuditAccountPhone,
			Detail:   detail,
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			return fmt.Sprintf("%s phone updated; audit not recorded: %v\n", user.Username, err)
		}
		if user.Phone == "" {
			return fmt.Sprintf("%s recovery phone cleared\n", user.Username)
		}
		return fmt.Sprintf("%s recovery phone=%s\n", user.Username, user.Phone)
	case "sms", "send-sms":
		if c.cfg.SMS == nil {
			return "SMS recovery is not configured; set --sms-url first\n"
		}
		ttl := c.phoneTTL()
		code, user, expires, err := c.cfg.Accounts.CreatePhoneReset(username, ttl)
		if err != nil {
			return fmt.Sprintf("could not create SMS reset: %v\n", err)
		}
		if err := c.cfg.SMS.SendPasswordReset(user.Phone, user.Username, code, ttl); err != nil {
			if auditErr := c.cfg.Accounts.AppendAudit(account.AuditEvent{
				Username: user.Username,
				Action:   account.AuditAccountRecover,
				Detail:   "reset SMS failed from admin console",
				ClientIP: "admin-console",
				Success:  false,
			}); auditErr != nil {
				c.cfg.Logger.Warn("reset SMS audit", "user", user.Username, "error", auditErr)
			}
			return fmt.Sprintf("could not send reset SMS to %s: %v\n", user.Phone, err)
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: user.Username,
			Action:   account.AuditAccountRecover,
			Detail:   "reset SMS sent from admin console",
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			c.cfg.Logger.Warn("reset SMS audit", "user", user.Username, "error", err)
		}
		return fmt.Sprintf("password reset SMS sent to %s, expires %s\n",
			user.Phone, expires.Format(time.RFC3339))
	case "2fa", "totp", "two-factor":
		if len(fields) < 3 || (fields[2] != "off" && fields[2] != "disable" && fields[2] != "reset") {
			return accountUsage
		}
		if err := c.cfg.Accounts.ResetTwoFactor(username); err != nil {
			return fmt.Sprintf("could not reset two-factor: %v\n", err)
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: username,
			Action:   account.AuditTwoFactorReset,
			Detail:   "reset from admin console",
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			return fmt.Sprintf("%s two-factor disabled; audit not recorded: %v\n", username, err)
		}
		return fmt.Sprintf("%s two-factor disabled\n", username)
	case "unlock", "unlock-login":
		cleared, err := c.cfg.Accounts.ClearLoginLocks(username)
		if err != nil {
			return fmt.Sprintf("could not clear login lockout: %v\n", err)
		}
		if err := c.cfg.Accounts.AppendAudit(account.AuditEvent{
			Username: username,
			Action:   account.AuditLoginUnlock,
			Detail:   fmt.Sprintf("cleared %d lockout keys from admin console", cleared),
			ClientIP: "admin-console",
			Success:  true,
		}); err != nil {
			return fmt.Sprintf("%s login lockout cleared (%d keys); audit not recorded: %v\n",
				username, cleared, err)
		}
		return fmt.Sprintf("%s login lockout cleared (%d keys)\n", username, cleared)
	default:
		return accountUsage
	}
}

func (c *Console) userListText() string {
	users, err := c.cfg.Accounts.ListUsers()
	if err != nil {
		return fmt.Sprintf("could not list users: %v\n", err)
	}
	if len(users) == 0 {
		return "no users\n"
	}
	var b strings.Builder
	for _, user := range users {
		fmt.Fprintf(&b, "%s role=%s id=%s created=%s", user.Username, user.Role, user.ID,
			user.CreatedAt.Format(time.RFC3339))
		if user.Email != "" {
			fmt.Fprintf(&b, " email=%s", user.Email)
		}
		if user.Phone != "" {
			fmt.Fprintf(&b, " phone=%s", user.Phone)
		}
		if user.Banned {
			b.WriteString(" banned=true")
		}
		b.WriteString("\n")
	}
	return b.String()
}

func (c *Console) addressBookText(args []string) string {
	if c.cfg.Accounts == nil {
		return "accounts require persistent state\n"
	}
	if len(args) == 0 {
		return "usage: ab <username> [count] | ab prune <days>\n"
	}
	if args[0] == "prune" || args[0] == "trim" {
		if len(args) < 2 {
			return "usage: ab prune <days>\n"
		}
		days, err := strconv.Atoi(args[1])
		if err != nil || days < 1 {
			return "usage: ab prune <days>\n"
		}
		cutoff := time.Now().Add(-time.Duration(days) * 24 * time.Hour)
		report, err := c.cfg.Accounts.PruneTombstones(cutoff)
		if err != nil {
			return fmt.Sprintf("could not prune address book tombstones: %v\n", err)
		}
		return fmt.Sprintf("pruned address book tombstones older than %d days: peers=%d groups=%d\n",
			days, report.Peers, report.Groups)
	}
	user, err := c.cfg.Accounts.FindUser(args[0])
	if err != nil {
		return fmt.Sprintf("unknown user %s\n", args[0])
	}
	count := 20
	if len(args) > 1 {
		count = parseCount(args[1:], count)
	}
	peers, err := c.cfg.Accounts.Peers(user.ID)
	if err != nil {
		return fmt.Sprintf("could not read address book: %v\n", err)
	}
	if len(peers) == 0 {
		return fmt.Sprintf("%s has no address book peers\n", user.Username)
	}
	if count > len(peers) {
		count = len(peers)
	}
	var b strings.Builder
	for _, peer := range peers[:count] {
		name := peer.Name
		if name == "" {
			name = "-"
		}
		tags := strings.Join(peer.Tags, ",")
		if tags == "" {
			tags = "-"
		}
		fmt.Fprintf(&b, "%s name=%s tags=%s updated=%s\n", peer.ID, name, tags,
			peer.UpdatedAt.Format(time.RFC3339))
	}
	return b.String()
}

func (c *Console) auditText(args []string) string {
	if c.cfg.Accounts == nil {
		return "accounts require persistent state\n"
	}
	if len(args) > 0 && (args[0] == "prune" || args[0] == "trim") {
		if len(args) < 2 {
			return "usage: audit prune <days>\n"
		}
		days, err := strconv.Atoi(args[1])
		if err != nil || days < 1 {
			return "usage: audit prune <days>\n"
		}
		cutoff := time.Now().Add(-time.Duration(days) * 24 * time.Hour)
		report, err := c.cfg.Accounts.PruneBefore(cutoff)
		if err != nil {
			return fmt.Sprintf("could not prune history: %v\n", err)
		}
		return fmt.Sprintf("pruned older than %d days: audit=%d connections=%d transfers=%d\n",
			days, report.AuditEvents, report.Connections, report.FileTransfers)
	}
	count := 20
	if len(args) > 0 {
		count = parseCount(args, count)
	}
	events, err := c.cfg.Accounts.AuditEvents(count)
	if err != nil {
		return fmt.Sprintf("could not read audit log: %v\n", err)
	}
	if len(events) == 0 {
		return "no audit events\n"
	}
	var b strings.Builder
	for _, event := range events {
		status := "failed"
		if event.Success {
			status = "ok"
		}
		detail := event.Detail
		if detail == "" {
			detail = "-"
		}
		location := event.Location
		if location == "" {
			location = "-"
		}
		fmt.Fprintf(&b, "%s user=%s action=%s %s ip=%s location=%s detail=%s\n",
			event.CreatedAt.Format(time.RFC3339), event.Username, event.Action,
			status, event.ClientIP, location, detail)
	}
	return b.String()
}

func (c *Console) connectionsText(args []string) string {
	if c.cfg.Accounts == nil {
		return "accounts require persistent state\n"
	}
	count := 20
	if len(args) > 0 {
		n, err := strconv.Atoi(args[0])
		if err != nil || n <= 0 {
			return "usage: connections [count]\n"
		}
		if n > 200 {
			n = 200
		}
		count = n
	}
	events, err := c.cfg.Accounts.ConnectionsFor("", count)
	if err != nil {
		return fmt.Sprintf("could not read connection log: %v\n", err)
	}
	if len(events) == 0 {
		return "no connection records\n"
	}
	var b strings.Builder
	for _, event := range events {
		status := "failed"
		if event.Success {
			status = "ok"
		}
		detail := event.Detail
		if detail == "" {
			detail = "-"
		}
		fmt.Fprintf(&b, "%s user=%s peer=%s %s ip=%s duration=%s detail=%s\n",
			event.StartedAt.Format(time.RFC3339), event.Username, event.PeerID,
			status, event.ClientIP, event.Duration().Round(time.Second), detail)
	}
	return b.String()
}

func (c *Console) transfersText(args []string) string {
	if c.cfg.Accounts == nil {
		return "accounts require persistent state\n"
	}
	count := 20
	if len(args) > 0 {
		n, err := strconv.Atoi(args[0])
		if err != nil || n <= 0 {
			return "usage: transfers [count]\n"
		}
		if n > 200 {
			n = 200
		}
		count = n
	}
	events, err := c.cfg.Accounts.FileTransfersFor("", count)
	if err != nil {
		return fmt.Sprintf("could not read file transfer log: %v\n", err)
	}
	if len(events) == 0 {
		return "no file transfer records\n"
	}
	var b strings.Builder
	for _, event := range events {
		status := "failed"
		if event.Success {
			status = "ok"
		}
		name := event.Name
		if name == "" {
			name = "-"
		}
		detail := event.Detail
		if detail == "" {
			detail = "-"
		}
		fmt.Fprintf(&b, "%s user=%s peer=%s %s name=%s size=%d files=%d ip=%s %s detail=%s\n",
			event.OccurredAt.Format(time.RFC3339), event.Username, event.PeerID,
			event.Direction, name, event.Size, event.Files, event.ClientIP,
			status, detail)
	}
	return b.String()
}

func randomPassword() (string, error) {
	raw := make([]byte, 18)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(raw), nil
}

func (c *Console) phoneTTL() time.Duration {
	if c.cfg.SMSTTL > 0 {
		return c.cfg.SMSTTL
	}
	return 10 * time.Minute
}

// recoveryLink mirrors the API's reset link so a console-issued token can be
// pasted into the same page.
func (c *Console) recoveryLink(token string) string {
	base := strings.TrimSpace(c.cfg.RecoveryURL)
	if base == "" {
		return ""
	}
	target, err := url.Parse(base)
	if err != nil {
		return ""
	}
	query := target.Query()
	query.Set("token", token)
	target.RawQuery = query.Encode()
	return target.String()
}
