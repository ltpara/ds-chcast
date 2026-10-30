// Package recordings stores viewer recordings that accounts upload to the
// server. Metadata lives in the account database; the MP4 bytes live in one
// directory per account, so deleting an account or expiring a recording can
// remove both representations without a second index.
package recordings

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

var (
	ErrEmpty    = errors.New("recording is empty")
	ErrTooLarge = errors.New("recording exceeds the upload limit")
	ErrQuota    = errors.New("account recording quota is exhausted")
)

const (
	maxNameLength = 128
	idBytes       = 16
	// tempPrefix marks partially written uploads. They never carry a row, so
	// a crash simply leaves a file the sweep can remove.
	tempPrefix = ".upload-"
	// orphanAge keeps a freshly created account directory from being swept
	// between its creation and the metadata insert.
	orphanAge = 24 * time.Hour
)

type Config struct {
	// Dir is the root of the archive. One subdirectory per account id is
	// created on demand.
	Dir string
	// Quota caps the bytes one account may store; zero means unlimited.
	Quota int64
	// MaxUpload caps a single file; zero means unlimited.
	MaxUpload int64
	Accounts  *account.Store
	Logger    *slog.Logger
}

type Archive struct {
	dir      string
	quota    int64
	max      int64
	accounts *account.Store
	logger   *slog.Logger
}

func New(cfg Config) (*Archive, error) {
	if cfg.Accounts == nil {
		return nil, errors.New("recordings need the account store")
	}
	dir := strings.TrimSpace(cfg.Dir)
	if dir == "" {
		return nil, errors.New("recordings directory is empty")
	}
	if cfg.Quota < 0 || cfg.MaxUpload < 0 {
		return nil, errors.New("recording limits must not be negative")
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, fmt.Errorf("recordings directory %s: %w", dir, err)
	}
	if err := os.MkdirAll(abs, 0o700); err != nil {
		return nil, fmt.Errorf("recordings directory %s: %w", abs, err)
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}
	return &Archive{dir: abs, quota: cfg.Quota, max: cfg.MaxUpload, accounts: cfg.Accounts, logger: logger}, nil
}

func (a *Archive) Dir() string              { return a.dir }
func (a *Archive) Quota() int64             { return a.quota }
func (a *Archive) MaxUpload() int64         { return a.max }
func (a *Archive) Accounts() *account.Store { return a.accounts }

// Save streams one upload into the archive and records its metadata. The
// body is bounded while copying, so a client that lies about its length still
// cannot fill the disk past MaxUpload.
func (a *Archive) Save(userID, name, peerID string, startedAtMS, durationMS int64, body io.Reader, sizeHint int64) (account.Recording, error) {
	if a.max > 0 && sizeHint > a.max {
		return account.Recording{}, ErrTooLarge
	}
	if a.quota > 0 && sizeHint > 0 {
		used, _, err := a.accounts.RecordingUsage(userID)
		if err != nil {
			return account.Recording{}, err
		}
		if used+sizeHint > a.quota {
			return account.Recording{}, ErrQuota
		}
	}
	dir := filepath.Join(a.dir, userID)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return account.Recording{}, fmt.Errorf("recording directory %s: %w", dir, err)
	}
	file, err := os.CreateTemp(dir, tempPrefix+"*")
	if err != nil {
		return account.Recording{}, fmt.Errorf("recording temp file: %w", err)
	}
	tempPath := file.Name()
	keep := false
	defer func() {
		if !keep {
			file.Close()
			os.Remove(tempPath)
		}
	}()
	hasher := sha256.New()
	var source io.Reader = body
	if a.max > 0 {
		source = io.LimitReader(body, a.max+1)
	}
	written, err := io.Copy(io.MultiWriter(file, hasher), source)
	if err != nil {
		return account.Recording{}, fmt.Errorf("recording upload: %w", err)
	}
	if written == 0 {
		return account.Recording{}, ErrEmpty
	}
	if a.max > 0 && written > a.max {
		return account.Recording{}, ErrTooLarge
	}
	if err := file.Close(); err != nil {
		return account.Recording{}, fmt.Errorf("recording upload: %w", err)
	}
	if a.quota > 0 {
		used, _, err := a.accounts.RecordingUsage(userID)
		if err != nil {
			return account.Recording{}, err
		}
		if used+written > a.quota {
			return account.Recording{}, ErrQuota
		}
	}
	id, err := newID()
	if err != nil {
		return account.Recording{}, err
	}
	finalPath := a.path(userID, id)
	if err := os.Rename(tempPath, finalPath); err != nil {
		return account.Recording{}, fmt.Errorf("recording store: %w", err)
	}
	keep = true
	rec, err := a.accounts.AddRecording(account.Recording{
		ID:          id,
		UserID:      userID,
		Name:        cleanName(name),
		PeerID:      strings.TrimSpace(peerID),
		Size:        written,
		SHA256:      hex.EncodeToString(hasher.Sum(nil)),
		DurationMS:  max64(durationMS, 0),
		StartedAtMS: max64(startedAtMS, 0),
	})
	if err != nil {
		os.Remove(finalPath)
		return account.Recording{}, err
	}
	return rec, nil
}

// Open returns the file and metadata for one account-owned recording. A row
// whose file is gone reports os.ErrNotExist so the API answers 404 instead of
// serving an empty body.
func (a *Archive) Open(userID, id string) (*os.File, account.Recording, error) {
	rec, err := a.accounts.Recording(userID, id)
	if err != nil {
		return nil, account.Recording{}, err
	}
	file, err := os.Open(a.path(rec.UserID, rec.ID))
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, rec, os.ErrNotExist
		}
		return nil, rec, fmt.Errorf("recording open: %w", err)
	}
	return file, rec, nil
}

// Remove deletes the metadata first so a failed unlink cannot leave a listed
// file without a caller; the stale file is logged and recovered by the next
// purge, which ignores missing entries.
func (a *Archive) Remove(userID, id string) (account.Recording, error) {
	rec, found, err := a.accounts.DeleteRecording(userID, id)
	if err != nil {
		return account.Recording{}, err
	}
	if !found {
		return account.Recording{}, account.ErrUnknownRecording
	}
	if err := os.Remove(a.path(rec.UserID, rec.ID)); err != nil && !errors.Is(err, os.ErrNotExist) {
		a.logger.Error("recording file remove", "id", rec.ID, "error", err)
	}
	return rec, nil
}

// PurgeExpired deletes recordings created before cutoff, row and file. File
// removal failures do not keep the row: the record is already past retention,
// and leaving it would make every later sweep retry the same entry.
func (a *Archive) PurgeExpired(cutoff time.Time) (int, error) {
	purged := 0
	for {
		batch, err := a.accounts.ExpiredRecordings(cutoff.UnixMilli(), 200)
		if err != nil {
			return purged, err
		}
		for _, rec := range batch {
			if err := os.Remove(a.path(rec.UserID, rec.ID)); err != nil && !errors.Is(err, os.ErrNotExist) {
				a.logger.Error("recording file remove", "id", rec.ID, "error", err)
			}
			if _, found, err := a.accounts.DeleteRecording(rec.UserID, rec.ID); err != nil {
				return purged, err
			} else if found {
				purged++
			}
		}
		if len(batch) < 200 {
			return purged, nil
		}
	}
}

// PurgeOrphans removes account directories whose rows disappeared through an
// account deletion, plus abandoned partial uploads. Only entries older than
// orphanAge are considered so an in-flight upload is never swept.
func (a *Archive) PurgeOrphans(now time.Time) (int, error) {
	owners, err := a.accounts.RecordingOwners()
	if err != nil {
		return 0, err
	}
	entries, err := os.ReadDir(a.dir)
	if err != nil {
		return 0, fmt.Errorf("recordings directory %s: %w", a.dir, err)
	}
	cutoff := now.Add(-orphanAge)
	removed := 0
	for _, entry := range entries {
		info, err := entry.Info()
		if err != nil {
			continue
		}
		if !info.IsDir() {
			if info.ModTime().Before(cutoff) {
				if err := os.Remove(filepath.Join(a.dir, entry.Name())); err == nil {
					removed++
				}
			}
			continue
		}
		if _, live := owners[entry.Name()]; live {
			pruned, err := a.purgeFiles(entry.Name(), cutoff)
			if err != nil {
				a.logger.Error("recording file sweep", "dir", entry.Name(), "error", err)
			}
			removed += pruned
			continue
		}
		if info.ModTime().After(cutoff) {
			continue
		}
		if err := os.RemoveAll(filepath.Join(a.dir, entry.Name())); err != nil {
			a.logger.Error("recording orphan remove", "dir", entry.Name(), "error", err)
			continue
		}
		removed++
	}
	return removed, nil
}

// purgeFiles removes abandoned partial uploads and MP4 files whose metadata
// row is gone from one live account directory.
func (a *Archive) purgeFiles(userID string, cutoff time.Time) (int, error) {
	ids, err := a.accounts.RecordingIDs(userID)
	if err != nil {
		return 0, err
	}
	entries, err := os.ReadDir(filepath.Join(a.dir, userID))
	if err != nil {
		return 0, err
	}
	removed := 0
	for _, entry := range entries {
		info, err := entry.Info()
		if err != nil || info.IsDir() || info.ModTime().After(cutoff) {
			continue
		}
		name := entry.Name()
		if strings.HasPrefix(name, tempPrefix) {
			if err := os.Remove(filepath.Join(a.dir, userID, name)); err == nil {
				removed++
			}
			continue
		}
		if !strings.HasSuffix(strings.ToLower(name), ".mp4") {
			continue
		}
		if _, live := ids[strings.TrimSuffix(name, ".mp4")]; live {
			continue
		}
		if err := os.Remove(filepath.Join(a.dir, userID, name)); err == nil {
			removed++
		}
	}
	return removed, nil
}

func (a *Archive) path(userID, id string) string {
	return filepath.Join(a.dir, userID, id+".mp4")
}

// cleanName turns a client-supplied display name into a safe attachment name
// without dropping non-ASCII characters the user typed.
func cleanName(name string) string {
	name = strings.TrimSpace(name)
	if cut := strings.LastIndexAny(name, `/\`); cut >= 0 {
		name = name[cut+1:]
	}
	var b strings.Builder
	for _, r := range name {
		if unicode.IsControl(r) {
			continue
		}
		b.WriteRune(r)
	}
	name = strings.TrimSpace(b.String())
	if name == "" {
		name = "recording.mp4"
	}
	if runes := []rune(name); len(runes) > maxNameLength {
		name = string(runes[:maxNameLength])
	}
	if !strings.HasSuffix(strings.ToLower(name), ".mp4") {
		name += ".mp4"
	}
	return name
}

func newID() (string, error) {
	var raw [idBytes]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", fmt.Errorf("recording id: %w", err)
	}
	return hex.EncodeToString(raw[:]), nil
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}
