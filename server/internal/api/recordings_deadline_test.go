package api_test

import (
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/recordings"
)

type slowRecordingReader struct{ done bool }

func (r *slowRecordingReader) Read(p []byte) (int, error) {
	if r.done {
		return 0, io.EOF
	}
	r.done = true
	time.Sleep(1200 * time.Millisecond)
	return copy(p, []byte("mp4-data")), io.EOF
}

func TestRecordingUploadExtendsReadAndWriteDeadlines(t *testing.T) {
	store, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	_, token, _, err := store.Login("alice", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	archive, err := recordings.New(recordings.Config{Dir: t.TempDir(), Accounts: store, MaxUpload: 1024})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewUnstartedServer(api.New(api.Config{Accounts: store, Recordings: archive, ServerKey: "key"}))
	srv.Config.ReadTimeout = 500 * time.Millisecond
	srv.Config.WriteTimeout = 500 * time.Millisecond
	srv.Start()
	defer srv.Close()
	client := srv.Client()
	client.Timeout = 10 * time.Second
	req, err := http.NewRequest(http.MethodPost, srv.URL+"/v1/account/recordings?name=slow.mp4", &slowRecordingReader{})
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.ContentLength = 8
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil || resp.StatusCode != http.StatusCreated {
		t.Fatalf("response: %d %s %v", resp.StatusCode, body, err)
	}
	used, count, err := store.RecordingUsage(user.ID)
	if err != nil || used != 8 || count != 1 {
		t.Fatalf("usage: %d/%d %v", used, count, err)
	}
}
