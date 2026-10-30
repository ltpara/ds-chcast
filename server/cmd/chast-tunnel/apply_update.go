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
	target := fs.String("target", "", "binary to replace, e.g. chast_viewer.exe")
	launch := fs.String("launch", "", "binary to start after the swap; defaults to --target")
	staging := fs.String("dir", "", "staging directory (default: system temp)")
	waitPID := fs.Int("wait-pid", 0, "wait for this process to exit before swapping")
	waitTimeout := fs.Duration("wait-timeout", 90*time.Second, "how long to wait for --wait-pid")
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
	backup, err := updater.Install(staged, *target)
	if err != nil {
		return err
	}
	launchPath := *launch
	if launchPath == "" {
		launchPath = *target
	}
	if err := updater.Launch(launchPath); err != nil {
		if restoreErr := updater.Restore(*target, backup); restoreErr != nil {
			return fmt.Errorf("start %s: %v (restoring the previous binary also failed: %v)", launchPath, err, restoreErr)
		}
		if relaunchErr := updater.Launch(*target); relaunchErr != nil {
			return fmt.Errorf("start %s: %v (the previous binary was restored but starting it also failed: %v)", launchPath, err, relaunchErr)
		}
		logf("restored and restarted the previous binary at %s", *target)
		return fmt.Errorf("start %s: %w", launchPath, err)
	}
	logf("started %s", launchPath)
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
