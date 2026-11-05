package recordings

import (
	"errors"
	"io"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

type barrierReader struct {
	ready   *sync.WaitGroup
	release <-chan struct{}
	done    bool
}

func (r *barrierReader) Read(p []byte) (int, error) {
	if r.done {
		return 0, io.EOF
	}
	r.done = true
	r.ready.Done()
	<-r.release
	return copy(p, []byte("12345678")), io.EOF
}

func TestConcurrentUploadsRespectQuota(t *testing.T) {
	archive, store, user := openArchive(t, 8, 8)
	const workers = 32
	var ready sync.WaitGroup
	ready.Add(workers)
	release := make(chan struct{})
	results := make(chan error, workers)
	for i := 0; i < workers; i++ {
		go func() {
			_, err := archive.Save(user.ID, "recording.mp4", "", 0, 0,
				&barrierReader{ready: &ready, release: release}, 8)
			results <- err
		}()
	}
	ready.Wait()
	close(release)
	saved := 0
	for i := 0; i < workers; i++ {
		if err := <-results; err == nil {
			saved++
		} else if !errors.Is(err, ErrQuota) {
			t.Errorf("save: %v", err)
		}
	}
	used, count, err := store.RecordingUsage(user.ID)
	if err != nil || saved != 1 || used != 8 || count != 1 {
		t.Fatalf("saved=%d bytes=%d files=%d err=%v", saved, used, count, err)
	}
	files, err := os.ReadDir(filepath.Join(archive.Dir(), user.ID))
	if err != nil || len(files) != 1 {
		t.Fatalf("rejected uploads left files: %d, %v", len(files), err)
	}
}
