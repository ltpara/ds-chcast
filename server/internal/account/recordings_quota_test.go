package account

import (
	"errors"
	"fmt"
	"path/filepath"
	"testing"
)

func TestRecordingQuotaAcrossStores(t *testing.T) {
	path := filepath.Join(t.TempDir(), "accounts.db")
	first, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	user, err := first.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	second, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	stores := []*Store{first, second}
	start := make(chan struct{})
	results := make(chan error, 32)
	for i := 0; i < cap(results); i++ {
		go func(i int) {
			<-start
			_, err := stores[i%2].AddRecordingWithQuota(Recording{
				ID: fmt.Sprintf("%032x", i+1), UserID: user.ID, Name: "test.mp4", Size: 8,
			}, 8)
			results <- err
		}(i)
	}
	close(start)
	saved := 0
	for i := 0; i < cap(results); i++ {
		if err := <-results; err == nil {
			saved++
		} else if !errors.Is(err, ErrRecordingQuota) {
			t.Errorf("insert: %v", err)
		}
	}
	used, count, err := first.RecordingUsage(user.ID)
	if err != nil || saved != 1 || used != 8 || count != 1 {
		t.Fatalf("saved=%d usage=%d/%d err=%v", saved, used, count, err)
	}
	other, err := second.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := second.AddRecordingWithQuota(Recording{ID: fmt.Sprintf("%032x", 100), UserID: other.ID, Size: 8}, 8); err != nil {
		t.Fatalf("another account should have its own quota: %v", err)
	}
	if _, err := first.AddRecording(Recording{ID: fmt.Sprintf("%032x", 101), UserID: user.ID, Size: 8}); err != nil {
		t.Fatalf("unlimited insertion: %v", err)
	}
}

func TestHostRecordingUploadOwnerBindingPersistsAcrossStores(t *testing.T) {
	path := filepath.Join(t.TempDir(), "accounts.db")
	first, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	alice, err := first.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	bob, err := first.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	second, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()

	const hostID = "devbox"
	const key = "0123456789abcdef0123456789abcdef"
	if err := first.BindHostRecordingUploadOwner(hostID, key, alice.ID); err != nil {
		t.Fatalf("initial binding: %v", err)
	}
	if err := second.BindHostRecordingUploadOwner(hostID, key, alice.ID); err != nil {
		t.Fatalf("same-owner retry: %v", err)
	}
	if err := second.BindHostRecordingUploadOwner(hostID, key, bob.ID); !errors.Is(err, ErrHostRecordingOwnerConflict) {
		t.Fatalf("owner reassignment = %v", err)
	}
	if err := second.BindHostRecordingUploadOwner("other-host", key, bob.ID); err != nil {
		t.Fatalf("another host may use the same key: %v", err)
	}

	const concurrentKey = "abcdefabcdefabcdefabcdefabcdefab"
	start := make(chan struct{})
	results := make(chan error, 2)
	go func() {
		<-start
		results <- first.BindHostRecordingUploadOwner(hostID, concurrentKey, alice.ID)
	}()
	go func() {
		<-start
		results <- second.BindHostRecordingUploadOwner(hostID, concurrentKey, bob.ID)
	}()
	close(start)
	accepted, conflicts := 0, 0
	for range 2 {
		err := <-results
		switch {
		case err == nil:
			accepted++
		case errors.Is(err, ErrHostRecordingOwnerConflict):
			conflicts++
		default:
			t.Fatalf("concurrent owner binding: %v", err)
		}
	}
	if accepted != 1 || conflicts != 1 {
		t.Fatalf("concurrent binding results: accepted=%d conflicts=%d", accepted, conflicts)
	}
}
