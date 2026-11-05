package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/updater"
)

// runApplyUpdate stages a signed release, waits for the caller to exit, swaps
// the target binary and starts it again. It is the only place the viewer runs
// an update, so all verification stays in one small helper.
func runApplyUpdate(args []string) error {
	fs := flag.NewFlagSet("apply-update", flag.ExitOnError)
	version := fs.String("version", "", "release version named by the signed notice")
	url := fs.String("url", "", "absolute http(s) URL of the release")
	digest := fs.String("sha256", "", "lowercase SHA-256 of the release")
	size := fs.Int64("size", 0, "release size in bytes")
	signature := fs.String("signature", "", "server signature over version, url, sha256 and size")
	serverPubKey := fs.String("server-pubkey", "", "pinned server Ed25519 public key; required")
	publisherPubKey := fs.String("publisher-pubkey", "", "pinned release publisher Ed25519 public key; checks a second signature")
	publisherSig := fs.String("publisher-sig", "", "publisher signature over version, url, sha256 and size")
	requirePublisher := fs.Bool("require-publisher", false, "refuse the release unless --publisher-pubkey and a publisher signature are present")
	target := fs.String("target", "", "binary to replace, e.g. chast_viewer.exe")
	launch := fs.String("launch", "", "binary to start after the swap; defaults to --target")
	staging := fs.String("dir", "", "staging directory (default: updates beside this helper)")
	waitPID := fs.Int("wait-pid", 0, "wait for this process to exit before swapping")
	waitTimeout := fs.Duration("wait-timeout", 90*time.Second, "how long to wait for --wait-pid")
	healthTimeout := fs.Duration("health-timeout", 60*time.Second, "wait this long for the new process to write its health marker; 0 disables the startup rollback")
	healthFile := fs.String("health-file", "", "health marker path (default: a fresh file in the staging directory)")
	retireTimeout := fs.Duration("retire-timeout", 20*time.Second, "how long to retry removing the previous binary")
	logPath := fs.String("log", "", "append progress to this file; stderr is always written")
	fs.Parse(args)

	if *logPath != "" {
		if file, err := os.OpenFile(*logPath, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644); err == nil {
			log.SetOutput(file)
			defer file.Close()
		}
	}
	logf := func(format string, values ...any) {
		log.Printf("apply-update: "+format, values...)
	}

	notice := updater.Notice{
		Version:   *version,
		URL:       *url,
		SHA256:    *digest,
		Size:      *size,
		Signature: *signature,
	}
	if err := updater.Verify(notice, *serverPubKey); err != nil {
		return err
	}
	if *requirePublisher && (*publisherPubKey == "" || *publisherSig == "") {
		return errors.New("--require-publisher needs --publisher-pubkey and --publisher-sig")
	}
	if *publisherPubKey != "" {
		keyID, err := identity.PublisherKeyID(*publisherPubKey)
		if err != nil {
			return fmt.Errorf("--publisher-pubkey: %w", err)
		}
		notice.PublisherKeyID = keyID
		notice.PublisherSignature = *publisherSig
		if err := updater.VerifyPublisher(notice, *publisherPubKey); err != nil {
			return err
		}
	}
	if strings.TrimSpace(*target) == "" {
		return errors.New("--target is required")
	}
	self, err := os.Executable()
	if err != nil {
		return fmt.Errorf("locate the helper: %w", err)
	}
	if sameExecutable(self, *target) {
		return errors.New("refusing to replace the running helper")
	}
	if *waitPID > 0 {
		logf("waiting for process %d to exit", *waitPID)
		if err := updater.WaitForPID(*waitPID, *waitTimeout); err != nil {
			return err
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
	defer cancel()
	staged, err := updater.Fetch(ctx, nil, notice, *staging)
	if err != nil {
		return err
	}
	logf("staged %s", staged)
	if err := updater.VerifyFile(staged, notice.SHA256, notice.Size); err != nil {
		return fmt.Errorf("staged release changed after the download: %w", err)
	}
	backup, err := updater.Install(staged, *target)
	if err != nil {
		return err
	}
	launchPath := *launch
	if launchPath == "" {
		launchPath = *target
	}
	rollback := func(reason error) error {
		if backup == "" {
			return reason
		}
		if restoreErr := updater.Restore(*target, backup); restoreErr != nil {
			return fmt.Errorf("%v (restoring the previous binary also failed: %v)", reason, restoreErr)
		}
		if relaunchErr := updater.Launch(*target); relaunchErr != nil {
			return fmt.Errorf("%v (the previous binary was restored but starting it also failed: %v)", reason, relaunchErr)
		}
		logf("restored and restarted the previous binary at %s", *target)
		return reason
	}
	if *healthTimeout > 0 {
		marker := strings.TrimSpace(*healthFile)
		if marker == "" {
			stagingDir, err := filepath.Abs(*staging)
			if err != nil {
				stagingDir = *staging
			}
			marker = filepath.Join(stagingDir, fmt.Sprintf(".health-%d", time.Now().UnixNano()))
		} else if abs, err := filepath.Abs(marker); err == nil {
			marker = abs
		}
		pid, launchErr := updater.LaunchEnv(launchPath, []string{updater.HealthEnv + "=" + marker})
		if launchErr != nil {
			return rollback(fmt.Errorf("start %s: %w", launchPath, launchErr))
		}
		logf("started %s (pid %d), waiting for the health marker", launchPath, pid)
		healthy, alive := updater.WaitForHealth(marker, pid, *healthTimeout, 250*time.Millisecond)
		if healthy {
			os.Remove(marker)
			logf("the new release reported healthy")
		} else if !alive {
			return rollback(fmt.Errorf("the new release exited before reporting healthy"))
		} else {
			// The process is still running but slow to report. Leaving it alone
			// is safer than killing a live UI; keep the backup for a manual
			// rollback and let the next update replace it.
			logf("the new release is still running but did not report healthy within %s; kept %s", *healthTimeout, backup)
			return nil
		}
	} else if err := updater.Launch(launchPath); err != nil {
		return rollback(fmt.Errorf("start %s: %w", launchPath, err))
	} else {
		logf("started %s", launchPath)
	}
	updater.Retire(backup, *retireTimeout)
	logf("update to %s finished", notice.Version)
	return nil
}

func sameExecutable(self, target string) bool {
	absSelf, errA := filepath.Abs(self)
	absTarget, errB := filepath.Abs(target)
	if errA != nil || errB != nil {
		return filepath.Clean(self) == filepath.Clean(target)
	}
	return strings.EqualFold(filepath.Clean(absSelf), filepath.Clean(absTarget))
}
