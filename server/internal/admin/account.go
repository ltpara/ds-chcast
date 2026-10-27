package admin

import (
	"crypto/rand"
	"encoding/base64"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

const accountUsage = `user list | user add <username> [password]
user passwd <username> [password] | user del <username>
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
		fmt.Fprintf(&b, "%s created id=%s\n", user.Username, user.ID)
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
		fmt.Fprintf(&b, "%s id=%s created=%s\n", user.Username, user.ID,
			user.CreatedAt.Format(time.RFC3339))
	}
	return b.String()
}

func (c *Console) addressBookText(args []string) string {
	if c.cfg.Accounts == nil {
		return "accounts require persistent state\n"
	}
	if len(args) == 0 {
		return "usage: ab <username> [count]\n"
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
