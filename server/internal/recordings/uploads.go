package recordings

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

var (
	ErrUnknownUpload     = errors.New("unknown recording upload")
	ErrUploadOffset      = errors.New("recording upload offset is stale")
	ErrUploadIncomplete  = errors.New("recording upload is incomplete")
	ErrUploadSize        = errors.New("recording upload size is invalid")
	ErrUploadDigest      = errors.New("recording upload chunk digest is invalid")
	ErrUploadLimit       = errors.New("too many active recording uploads")
	ErrUploadKeyInvalid  = errors.New("recording upload idempotency key is invalid")
	ErrUploadKeyConflict = errors.New("recording upload idempotency key conflicts")
	uploadIDPattern      = regexp.MustCompile(`^[0-9a-f]{32}$`)
)

const (
	uploadStateSuffix = ".json"
	uploadPartSuffix  = ".part"
	maxActiveUploads  = 8
	uploadLockStripes = 64
	// A session that has received no bytes for this long is abandoned. Without
	// it a crashed uploader holds a quota reservation and one of the eight
	// active slots until the daily orphan sweep.
	uploadIdleTTL = time.Hour
)

// Upload is the resumable state exposed to a recording client. Offset is the
// number of durable bytes the server has accepted for this upload.
type Upload struct {
	ID             string `json:"id"`
	Offset         int64  `json:"offset"`
	Size           int64  `json:"size"`
	Name           string `json:"name"`
	PeerID         string `json:"peer_id"`
	StartedAtMS    int64  `json:"started_at_ms"`
	DurationMS     int64  `json:"duration_ms"`
	CreatedAtMS    int64  `json:"created_at_ms"`
	IdempotencyKey string `json:"idempotency_key,omitempty"`
}

type UploadOffsetError struct {
	Expected int64
	Actual   int64
}

func (e *UploadOffsetError) Error() string {
	return fmt.Sprintf("recording upload offset %d is stale; server has %d", e.Expected, e.Actual)
}

func (e *UploadOffsetError) Unwrap() error { return ErrUploadOffset }

type uploadState struct {
	ID             string `json:"id"`
	Name           string `json:"name"`
	PeerID         string `json:"peer_id"`
	StartedAtMS    int64  `json:"started_at_ms"`
	DurationMS     int64  `json:"duration_ms"`
	Size           int64  `json:"size"`
	CreatedAtMS    int64  `json:"created_at_ms"`
	IdempotencyKey string `json:"idempotency_key,omitempty"`
}

// BeginUpload creates a durable, account-owned upload session. The declared
// size is checked before any bytes arrive, then checked again transactionally
// against the account quota when CompleteUpload publishes the recording.
func (a *Archive) BeginUpload(userID, name, peerID string, startedAtMS, durationMS, size int64) (Upload, error) {
	upload, _, err := a.BeginUploadWithKey(userID, name, peerID,
		startedAtMS, durationMS, size, "")
	return upload, err
}

// ValidateBeginUpload checks request fields that are independent of the
// account's current quota. Host routes use it before recording the immutable
// device-job owner binding, so malformed jobs cannot reserve an idempotency
// key.
func (a *Archive) ValidateBeginUpload(size int64, idempotencyKey string) error {
	if size <= 0 {
		return ErrEmpty
	}
	if a.max > 0 && size > a.max {
		return ErrTooLarge
	}
	idempotencyKey = strings.TrimSpace(idempotencyKey)
	if idempotencyKey != "" && !uploadIDPattern.MatchString(idempotencyKey) {
		return ErrUploadKeyInvalid
	}
	return nil
}

// BeginUploadWithKey resumes an active upload or returns its completed
// recording when the same account retries a durable client job.
func (a *Archive) BeginUploadWithKey(userID, name, peerID string,
	startedAtMS, durationMS, size int64, idempotencyKey string) (Upload, *account.Recording, error) {
	if err := a.ValidateBeginUpload(size, idempotencyKey); err != nil {
		return Upload{}, nil, err
	}
	userID = strings.TrimSpace(userID)
	if userID == "" {
		return Upload{}, nil, errors.New("recording upload owner is empty")
	}
	idempotencyKey = strings.TrimSpace(idempotencyKey)

	a.uploadBeginMu.Lock()
	defer a.uploadBeginMu.Unlock()
	// Reclaim abandoned sessions first so a client that retried after a crash
	// is not refused by its own leftovers.
	a.reclaimIdleUploads(userID)
	if idempotencyKey != "" {
		if recording, err := a.accounts.RecordingByUploadKey(userID, idempotencyKey); err == nil {
			if !recordingMatchesRequest(recording, name, peerID, startedAtMS, durationMS, size) {
				return Upload{}, nil, ErrUploadKeyConflict
			}
			return Upload{}, &recording, nil
		} else if !errors.Is(err, account.ErrUnknownRecording) {
			return Upload{}, nil, err
		}
		if upload, found, err := a.findActiveUploadByKey(userID, idempotencyKey); err != nil {
			return Upload{}, nil, err
		} else if found {
			if !uploadMatchesRequest(upload, name, peerID, startedAtMS, durationMS, size) {
				return Upload{}, nil, ErrUploadKeyConflict
			}
			return upload, nil, nil
		}
	}
	used, _, err := a.accounts.RecordingUsage(userID)
	if err != nil {
		return Upload{}, nil, err
	}
	reserved, active, err := a.activeUploadBytes(userID)
	if err != nil {
		return Upload{}, nil, err
	}
	if active >= maxActiveUploads {
		return Upload{}, nil, ErrUploadLimit
	}
	if a.quota > 0 && (size > a.quota-used-reserved || used+reserved > a.quota-size) {
		return Upload{}, nil, ErrQuota
	}

	dir := filepath.Join(a.dir, userID)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return Upload{}, nil, fmt.Errorf("recording directory %s: %w", dir, err)
	}
	id, err := newID()
	if err != nil {
		return Upload{}, nil, err
	}
	now := time.Now().UnixMilli()
	state := uploadState{
		ID:             id,
		Name:           cleanName(name),
		PeerID:         strings.TrimSpace(peerID),
		StartedAtMS:    max64(startedAtMS, 0),
		DurationMS:     max64(durationMS, 0),
		Size:           size,
		CreatedAtMS:    now,
		IdempotencyKey: idempotencyKey,
	}
	partPath := a.uploadPartPath(userID, id)
	file, err := os.OpenFile(partPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return Upload{}, nil, fmt.Errorf("recording upload create: %w", err)
	}
	if err := file.Close(); err != nil {
		os.Remove(partPath)
		return Upload{}, nil, fmt.Errorf("recording upload create: %w", err)
	}
	if err := a.writeUploadState(userID, state); err != nil {
		os.Remove(partPath)
		return Upload{}, nil, err
	}
	return uploadView(state, 0), nil, nil
}

func (a *Archive) findActiveUploadByKey(userID, key string) (Upload, bool, error) {
	entries, err := os.ReadDir(filepath.Join(a.dir, userID))
	if errors.Is(err, os.ErrNotExist) {
		return Upload{}, false, nil
	}
	if err != nil {
		return Upload{}, false, fmt.Errorf("recording upload directory: %w", err)
	}
	const prefix = tempPrefix + "session-"
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(name, prefix) || !strings.HasSuffix(name, uploadStateSuffix) {
			continue
		}
		id := strings.TrimSuffix(strings.TrimPrefix(name, prefix), uploadStateSuffix)
		if !uploadIDPattern.MatchString(id) {
			continue
		}
		state, err := a.readUploadState(userID, id)
		if err != nil {
			if errors.Is(err, ErrUnknownUpload) {
				continue
			}
			return Upload{}, false, err
		}
		if state.IdempotencyKey != key {
			continue
		}
		upload, err := a.UploadStatus(userID, id)
		if errors.Is(err, ErrUnknownUpload) {
			continue
		}
		if err != nil {
			return Upload{}, false, err
		}
		return upload, true, nil
	}
	return Upload{}, false, nil
}

func recordingMatchesRequest(rec account.Recording, name, peerID string,
	startedAtMS, durationMS, size int64) bool {
	return rec.Name == cleanName(name) && rec.PeerID == strings.TrimSpace(peerID) &&
		rec.StartedAtMS == max64(startedAtMS, 0) &&
		rec.DurationMS == max64(durationMS, 0) && rec.Size == size
}

func uploadMatchesRequest(upload Upload, name, peerID string,
	startedAtMS, durationMS, size int64) bool {
	return upload.Name == cleanName(name) && upload.PeerID == strings.TrimSpace(peerID) &&
		upload.StartedAtMS == max64(startedAtMS, 0) &&
		upload.DurationMS == max64(durationMS, 0) && upload.Size == size
}

func recordingMatchesUpload(rec account.Recording, upload Upload, digest string) bool {
	return rec.Name == upload.Name && rec.PeerID == upload.PeerID &&
		rec.StartedAtMS == upload.StartedAtMS &&
		rec.DurationMS == upload.DurationMS && rec.Size == upload.Size &&
		rec.SHA256 == digest && rec.UploadKey == upload.IdempotencyKey
}

// UploadStatus reports the offset from the part file itself. A lost response
// after a successful chunk therefore resumes at the durable file size without
// relying on a separately committed counter.
func (a *Archive) UploadStatus(userID, id string) (Upload, error) {
	if !uploadIDPattern.MatchString(id) {
		return Upload{}, ErrUnknownUpload
	}
	state, err := a.readUploadState(userID, id)
	if err != nil {
		return Upload{}, err
	}
	// An expired session is gone even if its files survive a failed reclaim:
	// resuming it would resurrect a reservation the account already lost.
	if a.uploadIdle(state, userID, id, time.Now()) {
		return Upload{}, ErrUnknownUpload
	}
	info, err := os.Stat(a.uploadPartPath(userID, id))
	if errors.Is(err, os.ErrNotExist) {
		return uploadView(state, 0), nil
	}
	if err != nil {
		return Upload{}, fmt.Errorf("recording upload status: %w", err)
	}
	if info.Size() > state.Size {
		return Upload{}, fmt.Errorf("recording upload %s exceeds its declared size", id)
	}
	return uploadView(state, info.Size()), nil
}

// AppendUpload writes exactly size bytes at offset. Partial or overlong chunk
// writes are rolled back to the previous offset so a retry can safely reuse
// the same request.
func (a *Archive) AppendUpload(userID, id string, offset, size int64, body io.Reader) (Upload, error) {
	return a.AppendUploadWithDigest(userID, id, offset, size, body, "")
}

// AppendUploadWithDigest verifies an optional caller-signed chunk digest
// before making the appended bytes durable. A mismatch truncates the part file
// back to its previous offset so the client can safely retry.
func (a *Archive) AppendUploadWithDigest(userID, id string, offset, size int64,
	body io.Reader, expectedDigest string) (Upload, error) {
	if !uploadIDPattern.MatchString(id) {
		return Upload{}, ErrUnknownUpload
	}
	if expectedDigest != "" {
		decoded, err := hex.DecodeString(expectedDigest)
		if err != nil || len(decoded) != sha256.Size || expectedDigest != strings.ToLower(expectedDigest) {
			return Upload{}, ErrUploadDigest
		}
	}
	lock := a.uploadLock(id)
	lock.Lock()
	defer lock.Unlock()

	current, err := a.UploadStatus(userID, id)
	if err != nil {
		return Upload{}, err
	}
	if offset != current.Offset {
		return Upload{}, &UploadOffsetError{Expected: offset, Actual: current.Offset}
	}
	if size <= 0 || offset > current.Size || size > current.Size-offset {
		return Upload{}, ErrUploadSize
	}
	partPath := a.uploadPartPath(userID, id)
	// O_CREATE restores a part file that vanished under a session whose state
	// file survived. Status reports offset 0 in that case, so recreating the
	// file lets the client resume from the start instead of failing every
	// append until it deletes the session.
	file, err := os.OpenFile(partPath, os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		return Upload{}, fmt.Errorf("recording upload append: %w", err)
	}
	if _, err := file.Seek(offset, io.SeekStart); err != nil {
		_ = file.Close()
		return Upload{}, fmt.Errorf("recording upload append: %w", err)
	}
	var writer io.Writer = file
	hasher := sha256.New()
	if expectedDigest != "" {
		writer = io.MultiWriter(file, hasher)
	}
	written, copyErr := io.Copy(writer, io.LimitReader(body, size+1))
	rollback := func() error {
		return file.Truncate(offset)
	}
	if copyErr != nil {
		rollbackErr := rollback()
		_ = file.Close()
		if rollbackErr != nil {
			return Upload{}, fmt.Errorf("recording upload rollback: %w", rollbackErr)
		}
		return Upload{}, fmt.Errorf("recording upload append: %w", copyErr)
	}
	if written != size {
		rollbackErr := rollback()
		_ = file.Close()
		if rollbackErr != nil {
			return Upload{}, fmt.Errorf("recording upload rollback: %w", rollbackErr)
		}
		if written > size {
			return Upload{}, ErrUploadSize
		}
		return Upload{}, io.ErrUnexpectedEOF
	}
	if expectedDigest != "" && hex.EncodeToString(hasher.Sum(nil)) != expectedDigest {
		rollbackErr := rollback()
		_ = file.Close()
		if rollbackErr != nil {
			return Upload{}, fmt.Errorf("recording upload rollback: %w", rollbackErr)
		}
		return Upload{}, ErrUploadDigest
	}
	if err := file.Sync(); err != nil {
		rollbackErr := rollback()
		_ = file.Close()
		if rollbackErr != nil {
			return Upload{}, fmt.Errorf("recording upload rollback: %w", rollbackErr)
		}
		return Upload{}, fmt.Errorf("recording upload sync: %w", err)
	}
	if err := file.Close(); err != nil {
		if rollbackErr := truncateByPath(partPath, offset); rollbackErr != nil {
			return Upload{}, fmt.Errorf("recording upload rollback: %w", rollbackErr)
		}
		return Upload{}, fmt.Errorf("recording upload close: %w", err)
	}
	current.Offset = offset + size
	return current, nil
}

// CompleteUpload verifies the declared length and computes the final digest
// from the durable part file before publishing one recording row.
func (a *Archive) CompleteUpload(userID, id string) (account.Recording, error) {
	if !uploadIDPattern.MatchString(id) {
		return account.Recording{}, ErrUnknownUpload
	}
	lock := a.uploadLock(id)
	lock.Lock()
	defer lock.Unlock()

	upload, err := a.UploadStatus(userID, id)
	if err != nil {
		return account.Recording{}, err
	}
	if upload.Offset != upload.Size {
		return account.Recording{}, ErrUploadIncomplete
	}
	partPath := a.uploadPartPath(userID, id)
	file, err := os.Open(partPath)
	if err != nil {
		return account.Recording{}, fmt.Errorf("recording upload complete: %w", err)
	}
	hasher := sha256.New()
	written, copyErr := io.Copy(hasher, file)
	closeErr := file.Close()
	if copyErr != nil {
		return account.Recording{}, fmt.Errorf("recording upload digest: %w", copyErr)
	}
	if closeErr != nil {
		return account.Recording{}, fmt.Errorf("recording upload digest: %w", closeErr)
	}
	if written != upload.Size {
		return account.Recording{}, ErrUploadIncomplete
	}
	digest := hex.EncodeToString(hasher.Sum(nil))
	if upload.IdempotencyKey != "" {
		existing, lookupErr := a.accounts.RecordingByUploadKey(userID, upload.IdempotencyKey)
		if lookupErr == nil {
			if !recordingMatchesUpload(existing, upload, digest) {
				return account.Recording{}, ErrUploadKeyConflict
			}
			a.removeUploadArtifacts(userID, id)
			return existing, nil
		}
		if !errors.Is(lookupErr, account.ErrUnknownRecording) {
			return account.Recording{}, lookupErr
		}
	}

	recID, err := newID()
	if err != nil {
		return account.Recording{}, err
	}
	finalPath := a.path(userID, recID)
	if err := os.Rename(partPath, finalPath); err != nil {
		return account.Recording{}, fmt.Errorf("recording store: %w", err)
	}
	rec, err := a.accounts.AddRecordingWithQuota(account.Recording{
		ID:          recID,
		UserID:      userID,
		Name:        upload.Name,
		PeerID:      upload.PeerID,
		Size:        upload.Size,
		SHA256:      digest,
		DurationMS:  upload.DurationMS,
		StartedAtMS: upload.StartedAtMS,
		CreatedAtMS: upload.CreatedAtMS,
		UploadKey:   upload.IdempotencyKey,
	}, a.quota)
	if err != nil {
		if upload.IdempotencyKey != "" {
			existing, lookupErr := a.accounts.RecordingByUploadKey(userID, upload.IdempotencyKey)
			if lookupErr == nil {
				if !recordingMatchesUpload(existing, upload, digest) {
					return account.Recording{}, ErrUploadKeyConflict
				}
				if removeErr := os.Remove(finalPath); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
					a.logger.Error("recording duplicate cleanup", "id", id, "error", removeErr)
				}
				a.removeUploadArtifacts(userID, id)
				return existing, nil
			}
		}
		if renameErr := os.Rename(finalPath, partPath); renameErr != nil {
			a.logger.Error("recording upload restore", "id", id, "error", renameErr)
		}
		return account.Recording{}, err
	}
	a.removeUploadArtifacts(userID, id)
	return rec, nil
}

func (a *Archive) removeUploadArtifacts(userID, id string) {
	for _, path := range []string{a.uploadPartPath(userID, id), a.uploadStatePath(userID, id)} {
		if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			a.logger.Error("recording upload state remove", "id", id, "error", err)
		}
	}
}

// RemoveUpload discards an unfinished session. It never touches a completed
// recording because that upload's state file has already been removed.
func (a *Archive) RemoveUpload(userID, id string) error {
	if !uploadIDPattern.MatchString(id) {
		return ErrUnknownUpload
	}
	lock := a.uploadLock(id)
	lock.Lock()
	defer lock.Unlock()
	found := false
	for _, path := range []string{a.uploadPartPath(userID, id), a.uploadStatePath(userID, id)} {
		err := os.Remove(path)
		if err == nil {
			found = true
		} else if !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("recording upload remove: %w", err)
		}
	}
	if !found {
		return ErrUnknownUpload
	}
	return nil
}

func (a *Archive) activeUploadBytes(userID string) (int64, int, error) {
	entries, err := os.ReadDir(filepath.Join(a.dir, userID))
	if errors.Is(err, os.ErrNotExist) {
		return 0, 0, nil
	}
	if err != nil {
		return 0, 0, fmt.Errorf("recording upload directory: %w", err)
	}
	var total int64
	var count int
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(name, tempPrefix) || !strings.HasSuffix(name, uploadStateSuffix) {
			continue
		}
		data, err := os.ReadFile(filepath.Join(a.dir, userID, name))
		if err != nil {
			return 0, 0, fmt.Errorf("recording upload state: %w", err)
		}
		var state uploadState
		if err := json.Unmarshal(data, &state); err != nil || state.Size <= 0 {
			continue
		}
		total += state.Size
		count++
	}
	return total, count, nil
}

// uploadIdle reports whether a session has seen no bytes for uploadIdleTTL.
// The part file's mtime is the last accepted chunk; a missing part file falls
// back to the creation time.
func (a *Archive) uploadIdle(state uploadState, userID, id string, now time.Time) bool {
	if info, err := os.Stat(a.uploadPartPath(userID, id)); err == nil {
		return now.Sub(info.ModTime()) >= uploadIdleTTL
	}
	return now.UnixMilli()-state.CreatedAtMS >= uploadIdleTTL.Milliseconds()
}

// reclaimIdleUploads removes abandoned sessions for one account. Callers hold
// uploadBeginMu, so the sweep cannot race a concurrent BeginUpload for the
// same account.
func (a *Archive) reclaimIdleUploads(userID string) {
	dir := filepath.Join(a.dir, userID)
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	now := time.Now()
	const statePrefix = tempPrefix + "session-"
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(name, statePrefix) || !strings.HasSuffix(name, uploadStateSuffix) {
			continue
		}
		id := strings.TrimSuffix(strings.TrimPrefix(name, statePrefix), uploadStateSuffix)
		if !uploadIDPattern.MatchString(id) {
			continue
		}
		lock := a.uploadLock(id)
		lock.Lock()
		state, err := a.readUploadState(userID, id)
		if err != nil || !a.uploadIdle(state, userID, id, now) {
			lock.Unlock()
			continue
		}
		// Remove the part first. If that fails, retain the state so the client can
		// still inspect or explicitly remove the resumable session.
		if err := os.Remove(a.uploadPartPath(userID, id)); err != nil &&
			!errors.Is(err, os.ErrNotExist) {
			a.logger.Error("recording upload reclaim", "id", id, "error", err)
			lock.Unlock()
			continue
		}
		if err := os.Remove(a.uploadStatePath(userID, id)); err != nil &&
			!errors.Is(err, os.ErrNotExist) {
			a.logger.Error("recording upload reclaim", "id", id, "error", err)
		}
		lock.Unlock()
	}
}

func (a *Archive) readUploadState(userID, id string) (uploadState, error) {
	data, err := os.ReadFile(a.uploadStatePath(userID, id))
	if errors.Is(err, os.ErrNotExist) {
		return uploadState{}, ErrUnknownUpload
	}
	if err != nil {
		return uploadState{}, fmt.Errorf("recording upload state: %w", err)
	}
	var state uploadState
	if err := json.Unmarshal(data, &state); err != nil {
		return uploadState{}, fmt.Errorf("recording upload state: %w", err)
	}
	if state.ID != id || state.Size <= 0 {
		return uploadState{}, ErrUnknownUpload
	}
	return state, nil
}

func (a *Archive) writeUploadState(userID string, state uploadState) error {
	data, err := json.Marshal(state)
	if err != nil {
		return fmt.Errorf("recording upload state: %w", err)
	}
	dir := filepath.Join(a.dir, userID)
	file, err := os.CreateTemp(dir, tempPrefix+"state-*")
	if err != nil {
		return fmt.Errorf("recording upload state: %w", err)
	}
	path := file.Name()
	keep := false
	defer func() {
		if !keep {
			_ = file.Close()
			_ = os.Remove(path)
		}
	}()
	if _, err := file.Write(data); err != nil {
		return fmt.Errorf("recording upload state: %w", err)
	}
	if err := file.Sync(); err != nil {
		return fmt.Errorf("recording upload state: %w", err)
	}
	if err := file.Close(); err != nil {
		return fmt.Errorf("recording upload state: %w", err)
	}
	if err := os.Rename(path, a.uploadStatePath(userID, state.ID)); err != nil {
		return fmt.Errorf("recording upload state: %w", err)
	}
	keep = true
	return nil
}

func (a *Archive) uploadStatePath(userID, id string) string {
	return filepath.Join(a.dir, userID, tempPrefix+"session-"+id+uploadStateSuffix)
}

func (a *Archive) uploadPartPath(userID, id string) string {
	return filepath.Join(a.dir, userID, tempPrefix+"session-"+id+uploadPartSuffix)
}

func (a *Archive) uploadLock(id string) *sync.Mutex {
	index := 0
	for i := 0; i < len(id); i++ {
		index = (index*31 + int(id[i])) % uploadLockStripes
	}
	return &a.uploadLocks[index]
}

func uploadView(state uploadState, offset int64) Upload {
	return Upload{
		ID:             state.ID,
		Offset:         offset,
		Size:           state.Size,
		Name:           state.Name,
		PeerID:         state.PeerID,
		StartedAtMS:    state.StartedAtMS,
		DurationMS:     state.DurationMS,
		CreatedAtMS:    state.CreatedAtMS,
		IdempotencyKey: state.IdempotencyKey,
	}
}

func truncateByPath(path string, offset int64) error {
	file, err := os.OpenFile(path, os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	if err := file.Truncate(offset); err != nil {
		_ = file.Close()
		return err
	}
	return file.Close()
}
