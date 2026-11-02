package updater

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const (
	stateSuffix   = ".chast-update.json"
	healthSuffix  = ".chast-update.ok"
	maxStateBytes = 16 << 10
)

var (
	// ErrNoPendingUpdate is returned by Confirm when the target has no update
	// waiting for a health confirmation.
	ErrNoPendingUpdate = errors.New("no pending update")
	// ErrUpdateExited reports that the new image terminated before writing the
	// health marker.
	ErrUpdateExited = errors.New("the new image exited before confirming its health")
	// ErrUpdateUnhealthy reports that the health marker did not arrive within
	// the recorded deadline.
	ErrUpdateUnhealthy = errors.New("the new image did not confirm its health in time")
)

// PendingUpdate records an installed release whose first start has not been
// confirmed yet. The list paths are validated before a rollback touches them.
type PendingUpdate struct {
	Version      string `json:"version"`
	Target       string `json:"target"`
	Backup       string `json:"backup"`
	Nonce        string `json:"nonce"`
	InstalledAt  int64  `json:"installed_at"`
	DeadlineUnix int64  `json:"deadline_unix"`
}

// StatePath is the sidecar that records a pending update for target.
func StatePath(target string) string {
	return target + stateSuffix
}

// HealthPath is the marker the new image writes once it starts successfully.
func HealthPath(target string) string {
	return target + healthSuffix
}

// Begin records a pending update before the new image is started. backup is
// the image Install moved aside and is what Rollback restores.
func Begin(target, backup, version string, timeout time.Duration) (*PendingUpdate, error) {
	if strings.TrimSpace(target) == "" || strings.TrimSpace(backup) == "" {
		return nil, errors.New("a pending update needs a target and a backup")
	}
	if timeout <= 0 {
		timeout = 2 * time.Minute
	}
	nonce, err := randomNonce()
	if err != nil {
		return nil, err
	}
	now := time.Now()
	pending := &PendingUpdate{
		Version:      version,
		Target:       target,
		Backup:       backup,
		Nonce:        nonce,
		InstalledAt:  now.Unix(),
		DeadlineUnix: now.Add(timeout).Unix(),
	}
	if err := pending.Validate(); err != nil {
		return nil, err
	}
	if err := writeJSON(StatePath(target), pending); err != nil {
		return nil, fmt.Errorf("record the pending update: %w", err)
	}
	return pending, nil
}

// ReadPending loads the state recorded for target.
func ReadPending(target string) (*PendingUpdate, error) {
	raw, err := readCapped(StatePath(target))
	if err != nil {
		return nil, err
	}
	var pending PendingUpdate
	if err := json.Unmarshal(raw, &pending); err != nil {
		return nil, fmt.Errorf("pending update %s: %w", StatePath(target), err)
	}
	if !samePath(pending.Target, target) {
		return nil, fmt.Errorf("pending update %s names target %s", StatePath(target), pending.Target)
	}
	if err := pending.Validate(); err != nil {
		return nil, err
	}
	return &pending, nil
}

// Validate keeps a rollback inside the target directory and only accepts the
// backup naming pattern Install creates.
func (p *PendingUpdate) Validate() error {
	if strings.TrimSpace(p.Target) == "" || strings.TrimSpace(p.Backup) == "" {
		return errors.New("pending update needs a target and a backup")
	}
	if p.Nonce == "" {
		return errors.New("pending update carries no nonce")
	}
	if !samePath(filepath.Dir(p.Target), filepath.Dir(p.Backup)) {
		return errors.New("the backup must sit in the target directory")
	}
	if !strings.HasPrefix(filepath.Base(p.Backup), filepath.Base(p.Target)+".old-") {
		return fmt.Errorf("backup %s is not an image moved aside for %s", p.Backup, p.Target)
	}
	return nil
}

// Confirm writes the health marker for target. It returns false when there is
// no pending update, which lets the viewer call it on every start.
func Confirm(target string) (bool, error) {
	pending, err := ReadPending(target)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	marker := struct {
		Nonce       string `json:"nonce"`
		ConfirmedAt int64  `json:"confirmed_at"`
	}{Nonce: pending.Nonce, ConfirmedAt: time.Now().Unix()}
	if err := writeJSON(HealthPath(target), &marker); err != nil {
		return false, fmt.Errorf("write the health marker: %w", err)
	}
	return true, nil
}

// Healthy reports whether the marker for this update arrived.
func (p *PendingUpdate) Healthy() bool {
	raw, err := readCapped(HealthPath(p.Target))
	if err != nil {
		return false
	}
	var marker struct {
		Nonce string `json:"nonce"`
	}
	if err := json.Unmarshal(raw, &marker); err != nil {
		return false
	}
	return marker.Nonce != "" && marker.Nonce == p.Nonce
}

// AwaitHealth waits for the marker, an early exit of the new image, or the
// recorded deadline. exited is closed by the caller when the process ends.
func (p *PendingUpdate) AwaitHealth(exited <-chan struct{}, poll time.Duration) error {
	if poll <= 0 {
		poll = 250 * time.Millisecond
	}
	deadline := time.Unix(p.DeadlineUnix, 0)
	for {
		if p.Healthy() {
			return nil
		}
		select {
		case <-exited:
			if p.Healthy() {
				return nil
			}
			return ErrUpdateExited
		default:
		}
		if !time.Now().Before(deadline) {
			return ErrUpdateUnhealthy
		}
		select {
		case <-exited:
			if p.Healthy() {
				return nil
			}
			return ErrUpdateExited
		case <-time.After(poll):
		}
	}
}

// Rollback puts the previous image back and removes a partially written update.
func (p *PendingUpdate) Rollback() error {
	if err := p.Validate(); err != nil {
		return err
	}
	info, err := os.Stat(p.Backup)
	if err != nil {
		return fmt.Errorf("the previous image is unavailable: %w", err)
	}
	if !info.Mode().IsRegular() {
		return errors.New("the previous image is not a regular file")
	}
	return Restore(p.Target, p.Backup)
}

// Cancel removes the state and marker after a successful or abandoned update.
func (p *PendingUpdate) Cancel() {
	os.Remove(StatePath(p.Target))
	os.Remove(HealthPath(p.Target))
}

func randomNonce() (string, error) {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return hex.EncodeToString(raw), nil
}

func readCapped(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	raw, err := io.ReadAll(io.LimitReader(file, maxStateBytes+1))
	if err != nil {
		return nil, err
	}
	if len(raw) > maxStateBytes {
		return nil, fmt.Errorf("%s exceeds %d bytes", path, maxStateBytes)
	}
	return raw, nil
}

func writeJSON(path string, value any) error {
	raw, err := json.Marshal(value)
	if err != nil {
		return err
	}
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, ".chast-update-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if _, err := tmp.Write(raw); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}
