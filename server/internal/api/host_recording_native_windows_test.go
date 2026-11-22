//go:build windows

package api_test

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestWindowsNativeHostRecordingArchiveAcceptance(t *testing.T) {
	if os.Getenv("CHAST_NATIVE_ARCHIVE_ACCEPTANCE") != "1" {
		t.Skip("set CHAST_NATIVE_ARCHIVE_ACCEPTANCE=1 to run isolated real-host recording acceptance")
	}
	bin, err := filepath.Abs(os.Getenv("CHAST_NATIVE_BIN_DIR"))
	if err != nil || os.Getenv("CHAST_NATIVE_BIN_DIR") == "" {
		t.Fatal("CHAST_NATIVE_BIN_DIR must name the Release binary directory")
	}
	for _, name := range []string{"chast-host.exe", "chast-tunnel.exe", "chast-live-viewer-probe.exe"} {
		if info, err := os.Stat(filepath.Join(bin, name)); err != nil || info.IsDir() {
			t.Fatalf("required native binary %s is missing: %v", name, err)
		}
	}

	// The Release host carries a Low Integrity label. Create its inputs under
	// the inherited low-integrity test root, never under the high-integrity temp.
	_, source, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("locate repository for the isolated native test directory")
	}
	repo := filepath.Clean(filepath.Join(filepath.Dir(source), "..", "..", ".."))
	testRoot := filepath.Join(repo, "build", "test-temp")
	if err := os.MkdirAll(testRoot, 0700); err != nil {
		t.Fatalf("create isolated native test root: %v", err)
	}
	dir, err := os.MkdirTemp(testRoot, "host-recording-native-")
	if err != nil {
		t.Fatal(err)
	}
	passed := false
	t.Cleanup(func() {
		if passed {
			_ = os.RemoveAll(dir)
		} else {
			t.Logf("preserved isolated host recording artifacts: %s", dir)
		}
	})

	archiveDir := filepath.Join(dir, "server-archive")
	s := testutil.StartWith(t, testutil.Options{RecordingsDir: archiveDir})
	owner, err := s.Accounts.CreateUser("native-host-archive", "isolated acceptance password")
	if err != nil {
		t.Fatalf("create archive owner: %v", err)
	}

	hostSocket, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	hostPort := hostSocket.LocalAddr().(*net.UDPAddr).Port
	tunnelSocket, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		hostSocket.Close()
		t.Fatal(err)
	}
	tunnelAddress := tunnelSocket.LocalAddr().String()
	if err := hostSocket.Close(); err != nil {
		tunnelSocket.Close()
		t.Fatal(err)
	}
	if err := tunnelSocket.Close(); err != nil {
		t.Fatal(err)
	}

	const hostID = "native-recording-host"
	const deviceKey = "native-recording-device-key"
	const accessToken = "native-recording-viewer-token"
	recordDir := filepath.Join(dir, "source-recordings")
	if err := os.MkdirAll(recordDir, 0700); err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(dir, "host.ini")
	config := fmt.Sprintf("bind=127.0.0.1\nport=%d\ncarrier=false\nlan_discovery=false\nmax_viewers=1\nkeep_awake=false\nlock_after_session_end=false\nauto_disconnect_minutes=0\ninput=false\nclipboard=false\naudio=false\nfile_transfer=false\nport_forward=false\nterminal=false\nallow_remote_config=false\nrestart=false\nblock_input=false\nprivacy_mode=false\nmicrophone=false\ncamera=false\nrecord_incoming=true\nrecord_directory=%s\nupload_recordings=true\nautostart=false\nauth_mode=password\ntoken_auth=true\ntoken_mode=reusable\ntoken=%s\ntunnel=true\nserver_url=%s\nserver_key=%s\ntunnel_id=%s\ntunnel_key=%s\ntunnel_listen=%s\ncodec=vp9\nbitrate_kbps=256\nframerate=30\n",
		hostPort, recordDir, accessToken, s.URL, testutil.ServerKey, hostID, deviceKey, tunnelAddress)
	if err := os.WriteFile(configPath, []byte(config), 0600); err != nil {
		t.Fatal(err)
	}

	// The acceptance-only switch is honored only by the test-enabled host build
	// and only with --portable plus the isolated loopback configuration above.
	env := make([]string, 0, len(os.Environ())+3)
	for _, item := range os.Environ() {
		key, _, _ := strings.Cut(item, "=")
		if strings.EqualFold(key, "CHAST_NATIVE_TESTING") ||
			strings.EqualFold(key, "CHAST_NATIVE_ARCHIVE_FIXTURE") ||
			strings.EqualFold(key, "CHAST_NATIVE_ARCHIVE_TRACE") ||
			strings.EqualFold(key, "CHAST_NATIVE_ARCHIVE_ROOT") {
			continue
		}
		env = append(env, item)
	}
	env = append(env, "CHAST_NATIVE_TESTING=1", "CHAST_NATIVE_ARCHIVE_FIXTURE=1",
		"CHAST_NATIVE_ARCHIVE_TRACE="+filepath.Join(dir, "host-server-trace.log"),
		"CHAST_NATIVE_ARCHIVE_ROOT="+dir)
	host := startNativeProcessWithEnv(t, dir, "native-host", filepath.Join(bin, "chast-host.exe"),
		"", env, false, "--portable", "--tray", "--config", configPath)

	var fingerprint string
	registered := false
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		record, found := s.Registry.Identity(hostID)
		if found && record.Fingerprint != "" {
			fingerprint = record.Fingerprint
			registered = true
			break
		}
		if _, err := os.Stat(filepath.Join(dir, "native-host.log")); err == nil {
			log, _ := os.ReadFile(filepath.Join(dir, "native-host.log"))
			if strings.Contains(string(log), "no existing user certificate; no identity was created") {
				t.Skip("no Chast identity certificate already exists; acceptance did not create one")
			}
		}
		select {
		case <-host.done:
			log, _ := os.ReadFile(filepath.Join(dir, "native-host.log"))
			t.Fatalf("real chast-host exited before registration: %v\n%s", host.err, log)
		default:
		}
		time.Sleep(100 * time.Millisecond)
	}
	if !registered {
		log, _ := os.ReadFile(filepath.Join(dir, "native-host.log"))
		t.Fatalf("real chast-host did not register through its tunnel child\n%s", log)
	}
	if _, found, err := s.Registry.SetOwner(hostID, owner.Username); err != nil || !found {
		t.Fatalf("bind host recording owner: found=%v err=%v", found, err)
	}

	probeDir := filepath.Join(dir, "viewer-probe")
	if err := os.Mkdir(probeDir, 0700); err != nil {
		t.Fatal(err)
	}
	probe := startNativeProcess(t, dir, "native-viewer-probe", filepath.Join(bin, "chast-live-viewer-probe.exe"),
		accessToken+"\n", strconv.Itoa(hostPort), fingerprint, probeDir, "--archive-desktop")
	connectedPath := filepath.Join(probeDir, "connected")
	nativeWait(t, "pinned desktop probe receives five synthetic VP9 frames", 30*time.Second, func() bool {
		select {
		case <-probe.done:
			probeLog, _ := os.ReadFile(filepath.Join(dir, "native-viewer-probe.log"))
			hostLog, _ := os.ReadFile(filepath.Join(dir, "native-host.log"))
			serverTrace, _ := os.ReadFile(filepath.Join(dir, "host-server-trace.log"))
			t.Fatalf("viewer probe exited before receiving video: %v\nprobe:\n%s\nhost:\n%s\nserver trace:\n%s", probe.err, probeLog, hostLog, serverTrace)
		default:
		}
		return nativeFile(connectedPath)
	})
	if err := os.WriteFile(filepath.Join(probeDir, "stop"), []byte("finish session\n"), 0600); err != nil {
		t.Fatal(err)
	}
	select {
	case <-probe.done:
		if probe.err != nil {
			t.Fatalf("finish desktop probe session: %v", probe.err)
		}
	case <-time.After(15 * time.Second):
		t.Fatal("desktop probe did not finish after the stop signal")
	}

	var archivedPath string
	nativeWait(t, "sealed host segment is uploaded and indexed for its registered owner", 45*time.Second, func() bool {
		records, err := s.Accounts.Recordings(owner.ID)
		if err != nil || len(records) != 1 {
			return false
		}
		record := records[0]
		if record.PeerID != hostID || record.Name == "" {
			return false
		}
		archivedPath = filepath.Join(archiveDir, owner.ID, record.ID+".mp4")
		info, err := os.Stat(archivedPath)
		return err == nil && info.Size() == record.Size && info.Size() > 0
	})
	records, err := s.Accounts.Recordings(owner.ID)
	if err != nil || len(records) != 1 {
		t.Fatalf("expected one host archive record, count=%d err=%v", len(records), err)
	}
	var sourcePath string
	err = filepath.WalkDir(recordDir, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if !entry.IsDir() && strings.EqualFold(filepath.Ext(entry.Name()), ".mp4") {
			if sourcePath != "" {
				return fmt.Errorf("expected exactly one sealed source MP4, found %s and %s", sourcePath, path)
			}
			sourcePath = path
		}
		return nil
	})
	if err != nil {
		t.Fatalf("scan sealed host recordings: %v", err)
	}
	if sourcePath == "" {
		t.Fatal("host did not retain its sealed source MP4")
	}
	sourceBytes, err := os.ReadFile(sourcePath)
	if err != nil {
		t.Fatalf("read retained host MP4: %v", err)
	}
	archiveBytes, err := os.ReadFile(archivedPath)
	if err != nil {
		t.Fatalf("read server archive MP4: %v", err)
	}
	sourceHash := sha256.Sum256(sourceBytes)
	archiveHash := sha256.Sum256(archiveBytes)
	if len(sourceBytes) < 16 || string(sourceBytes[4:8]) != "ftyp" ||
		hex.EncodeToString(sourceHash[:]) != records[0].SHA256 || sourceHash != archiveHash ||
		int64(len(sourceBytes)) != records[0].Size {
		t.Fatalf("source/archive mismatch: source=%d sha256=%s archive=%d metadata=%+v",
			len(sourceBytes), hex.EncodeToString(sourceHash[:]), len(archiveBytes), records[0])
	}
	select {
	case <-host.done:
		t.Fatal("host exited before acceptance cleanup")
	default:
	}
	t.Logf("host MP4 retained and archived: %d bytes sha256=%s", len(sourceBytes), hex.EncodeToString(sourceHash[:]))
	passed = true
}
