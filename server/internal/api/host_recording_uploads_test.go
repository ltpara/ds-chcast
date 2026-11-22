package api_test

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func signedHostRecordingRequest(t *testing.T, base string, priv ed25519.PrivateKey, method, hostID,
	uploadID, path string, offset, size int64, metadata []string, body []byte, validSignature bool) (int, []byte) {
	t.Helper()
	timestamp := time.Now().Unix()
	message := identity.HostRecordingUploadMessage(hostID, method, uploadID,
		timestamp, offset, size, metadata...)
	signature := identity.Sign(priv, message)
	if !validSignature {
		signature = identity.Sign(identity.PrivateKeyFromSecret("different-host-key"), message)
	}
	req, err := http.NewRequest(method, base+path, bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+testutil.ServerKey)
	req.Header.Set("X-Host-Timestamp", strconv.FormatInt(timestamp, 10))
	req.Header.Set("X-Host-Signature", signature)
	if method == http.MethodPatch {
		req.Header.Set("X-Upload-Offset", strconv.FormatInt(offset, 10))
		req.Header.Set("X-Chunk-SHA256", metadata[0])
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, data
}

func TestHostRecordingUploadUsesRegisteredOwnerAndRejectsOwnerChanges(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RecordingsDir: t.TempDir()})
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	bob, err := s.Accounts.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	priv := identity.PrivateKeyFromSecret("recording-host")
	const hostID = "recording-host"
	if _, err := s.Registry.Register(registry.RegisterRequest{
		ID: hostID, PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)), TTL: time.Minute,
	}); err != nil {
		t.Fatalf("register host: %v", err)
	}
	const key = "1234567890abcdef1234567890abcdef"
	name := "host-segment.mp4"
	body := []byte("closed mp4 bytes")
	startedAt, duration := int64(1700000000000), int64(5000)
	metadata := []string{name, strconv.FormatInt(startedAt, 10), strconv.FormatInt(duration, 10), key}
	beginBody, err := json.Marshal(map[string]any{
		"name": name, "size": len(body), "started_at_ms": startedAt,
		"duration_ms": duration, "idempotency_key": key,
	})
	if err != nil {
		t.Fatal(err)
	}
	beginPath := "/v1/hosts/" + hostID + "/recordings/uploads"
	if status, data := signedHostRecordingRequest(t, s.URL, priv, http.MethodPost, hostID,
		"", beginPath, 0, int64(len(body)), metadata, beginBody, true); status != http.StatusConflict {
		t.Fatalf("ownerless host begin: %d %s", status, data)
	}
	if _, _, err := s.Registry.SetOwner(hostID, "alice"); err != nil {
		t.Fatalf("assign owner: %v", err)
	}
	if status, data := signedHostRecordingRequest(t, s.URL, priv, http.MethodPost, hostID,
		"", beginPath, 0, int64(len(body)), metadata, beginBody, false); status != http.StatusUnauthorized {
		t.Fatalf("wrong device signature: %d %s", status, data)
	}
	status, data := signedHostRecordingRequest(t, s.URL, priv, http.MethodPost, hostID,
		"", beginPath, 0, int64(len(body)), metadata, beginBody, true)
	if status != http.StatusCreated {
		t.Fatalf("begin: %d %s", status, data)
	}
	upload := decodeResumableUpload(t, data)
	if upload.PeerID != hostID || upload.IdempotencyKey != key {
		t.Fatalf("host upload metadata = %+v", upload)
	}
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodPost, hostID,
		"", beginPath, 0, int64(len(body)), metadata, beginBody, true)
	if status != http.StatusCreated || decodeResumableUpload(t, data).ID != upload.ID {
		t.Fatalf("idempotent active begin: %d %s", status, data)
	}

	chunkPath := beginPath + "/" + upload.ID
	chunkHash := sha256.Sum256(body)
	chunkDigest := hex.EncodeToString(chunkHash[:])
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodPatch, hostID,
		upload.ID, chunkPath, 0, int64(len(body)), []string{chunkDigest}, []byte("BROKEN mp4 bytes"), true)
	if status != http.StatusBadRequest {
		t.Fatalf("tampered chunk: %d %s", status, data)
	}
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodGet, hostID,
		upload.ID, chunkPath, 0, 0, nil, nil, true)
	if status != http.StatusOK || decodeResumableUpload(t, data).Offset != 0 {
		t.Fatalf("offset after rejected chunk: %d %s", status, data)
	}
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodPatch, hostID,
		upload.ID, chunkPath, 0, int64(len(body)), []string{chunkDigest}, body, true)
	if status != http.StatusAccepted {
		t.Fatalf("append: %d %s", status, data)
	}
	completePath := chunkPath + "/complete"
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodPost, hostID,
		upload.ID, completePath, 0, 0, nil, nil, true)
	if status != http.StatusCreated {
		t.Fatalf("complete: %d %s", status, data)
	}
	var completed struct {
		Recording struct {
			ID     string `json:"id"`
			PeerID string `json:"peer_id"`
		} `json:"recording"`
	}
	if err := json.Unmarshal(data, &completed); err != nil || completed.Recording.ID == "" ||
		completed.Recording.PeerID != hostID {
		t.Fatalf("owner-bound archive result: %+v err=%v data=%s", completed, err, data)
	}
	if _, count, err := s.Accounts.RecordingUsage(alice.ID); err != nil || count != 1 {
		t.Fatalf("archive count for registered owner: count=%d err=%v", count, err)
	}
	if _, count, err := s.Accounts.RecordingUsage(bob.ID); err != nil || count != 0 {
		t.Fatalf("archive count for other account: count=%d err=%v", count, err)
	}

	// A later owner change must not make an in-progress upload visible under
	// the new account. The same device signature is still valid, but ownership
	// is resolved afresh from the registry for every request.
	secondKey := "abcdefabcdefabcdefabcdefabcdefab"
	secondMetadata := []string{name, strconv.FormatInt(startedAt, 10), strconv.FormatInt(duration, 10), secondKey}
	secondBody, err := json.Marshal(map[string]any{
		"name": name, "size": len(body), "started_at_ms": startedAt,
		"duration_ms": duration, "idempotency_key": secondKey,
	})
	if err != nil {
		t.Fatal(err)
	}
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodPost, hostID,
		"", beginPath, 0, int64(len(body)), secondMetadata, secondBody, true)
	if status != http.StatusCreated {
		t.Fatalf("second begin: %d %s", status, data)
	}
	second := decodeResumableUpload(t, data)
	if _, _, err := s.Registry.SetOwner(hostID, "bob"); err != nil {
		t.Fatalf("change owner: %v", err)
	}
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodGet, hostID,
		second.ID, beginPath+"/"+second.ID, 0, 0, nil, nil, true)
	if status != http.StatusConflict {
		t.Fatalf("status after owner change: %d %s", status, data)
	}
	status, data = signedHostRecordingRequest(t, s.URL, priv, http.MethodPost, hostID,
		"", beginPath, 0, int64(len(body)), secondMetadata, secondBody, true)
	if status != http.StatusConflict {
		t.Fatalf("begin after owner change: %d %s", status, data)
	}
	if _, count, err := s.Accounts.RecordingUsage(bob.ID); err != nil || count != 0 {
		t.Fatalf("recordings transferred to new owner: count=%d err=%v", count, err)
	}
}
