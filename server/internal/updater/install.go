package updater

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Install replaces target with the verified release at staged and returns the
// path of the previous image. Windows allows renaming a running executable, so
// the helper can swap the viewer while the viewer is still exiting; the caller
// restores the backup if the new image cannot start.
func Install(staged, target string) (string, error) {
	info, err := os.Stat(staged)
	if err != nil {
		return "", fmt.Errorf("staged release: %w", err)
	}
	if !info.Mode().IsRegular() {
		return "", errors.New("the staged release is not a regular file")
	}
	if samePath(staged, target) {
		return "", errors.New("the staged release and the target are the same file")
	}
	backup := ""
	if _, err := os.Stat(target); err == nil {
		backup = fmt.Sprintf("%s.old-%d", target, time.Now().UnixNano())
		if err := os.Rename(target, backup); err != nil {
			return "", fmt.Errorf("move the current binary aside: %w", err)
		}
	} else if !os.IsNotExist(err) {
		return "", fmt.Errorf("inspect the target: %w", err)
	}
	if err := moveFile(staged, target); err != nil {
		if backup != "" {
			_ = os.Rename(backup, target)
		}
		return "", fmt.Errorf("place the new binary: %w", err)
	}
	return backup, nil
}

// Restore puts the previous image back after a failed launch. The new image is
// removed first because Windows refuses to rename over an existing file.
func Restore(target, backup string) error {
	if backup == "" {
		return nil
	}
	if err := os.Remove(target); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("remove the rejected binary: %w", err)
	}
	if err := os.Rename(backup, target); err != nil {
		return fmt.Errorf("restore the previous binary: %w", err)
	}
	return nil
}

// Retire removes the backup once the old image is no longer running. The
// previous process may still hold the file for a moment, so removal is retried
// until the timeout expires; a leftover backup is harmless and is replaced by
// the next update's timestamp.
func Retire(backup string, timeout time.Duration) {
	if strings.TrimSpace(backup) == "" {
		return
	}
	deadline := time.Now().Add(timeout)
	for {
		err := os.Remove(backup)
		if err == nil || os.IsNotExist(err) {
			return
		}
		if time.Now().After(deadline) {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
}

// WaitForPID blocks until pid exits or the timeout expires. A process that has
// already exited is treated as done.
func WaitForPID(pid int, timeout time.Duration) error {
	if pid <= 0 {
		return nil
	}
	if timeout <= 0 {
		timeout = 60 * time.Second
	}
	return waitForPID(pid, timeout)
}

func samePath(a, b string) bool {
	absA, errA := filepath.Abs(a)
	absB, errB := filepath.Abs(b)
	if errA != nil || errB != nil {
		return filepath.Clean(a) == filepath.Clean(b)
	}
	return strings.EqualFold(filepath.Clean(absA), filepath.Clean(absB))
}

// moveFile renames within a volume and falls back to copying so a staging
// directory on another drive still works.
func moveFile(staged, target string) error {
	if err := os.Rename(staged, target); err == nil {
		return nil
	}
	tmp := target + ".new"
	if err := copyFile(staged, tmp); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, target); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Remove(staged)
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o755)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}
