// Package logging provides the size-rotated log file sink used by the server
// binaries. Rotation keeps the operator's disk bounded without an external
// logrotate: the active file is renamed to a timestamped name, optionally
// compressed, and old backups are pruned by count and age.
package logging

import (
	"compress/gzip"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	// DefaultMaxSize is used when Config.MaxSize is zero.
	DefaultMaxSize = 100 << 20
	// DefaultMaxBackups is used by the binaries when the flag is not set.
	DefaultMaxBackups = 5

	backupLayout = "20060102-150405.000000000"
)

// Config describes the rotating file sink. An empty Path disables the sink.
type Config struct {
	// Path is the active log file.
	Path string
	// MaxSize is the rotation threshold in bytes; zero uses DefaultMaxSize.
	MaxSize int64
	// MaxBackups is the number of rotated files to keep; zero keeps all.
	MaxBackups int
	// MaxAge deletes rotated files older than this; zero keeps all.
	MaxAge time.Duration
	// Compress gzips rotated files.
	Compress bool
}

func (c Config) validate() error {
	if c.MaxSize < 0 {
		return errors.New("log max size must not be negative")
	}
	if c.MaxBackups < 0 {
		return errors.New("log max backups must not be negative")
	}
	if c.MaxAge < 0 {
		return errors.New("log max age must not be negative")
	}
	return nil
}

// Open starts the sink. It returns nil when cfg.Path is empty so callers can
// keep stderr-only logging unchanged.
func Open(cfg Config) (*Writer, error) {
	cfg.Path = strings.TrimSpace(cfg.Path)
	if cfg.Path == "" {
		return nil, nil
	}
	if err := cfg.validate(); err != nil {
		return nil, err
	}
	if cfg.MaxSize == 0 {
		cfg.MaxSize = DefaultMaxSize
	}
	w := &Writer{
		path:       cfg.Path,
		maxSize:    cfg.MaxSize,
		maxBackups: cfg.MaxBackups,
		maxAge:     cfg.MaxAge,
		compress:   cfg.Compress,
		now:        time.Now,
	}
	if err := w.open(); err != nil {
		return nil, err
	}
	return w, nil
}

// Writer is a concurrency-safe io.Writer that rotates by size.
type Writer struct {
	mu         sync.Mutex
	path       string
	maxSize    int64
	maxBackups int
	maxAge     time.Duration
	compress   bool
	now        func() time.Time

	file *os.File
	size int64
}

func (w *Writer) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.file == nil {
		if err := w.open(); err != nil {
			return 0, err
		}
	}
	if w.maxSize > 0 && w.size > 0 && w.size+int64(len(p)) > w.maxSize {
		if err := w.rotate(); err != nil {
			return 0, err
		}
	}
	n, err := w.file.Write(p)
	w.size += int64(n)
	return n, err
}

// Close flushes and closes the active file. It is safe on a nil Writer.
func (w *Writer) Close() error {
	if w == nil {
		return nil
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.closeFile()
}

func (w *Writer) open() error {
	if dir := filepath.Dir(w.path); dir != "." && dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return fmt.Errorf("log directory: %w", err)
		}
	}
	file, err := os.OpenFile(w.path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return fmt.Errorf("log file: %w", err)
	}
	info, err := file.Stat()
	if err != nil {
		file.Close()
		return fmt.Errorf("log file: %w", err)
	}
	w.file = file
	w.size = info.Size()
	return nil
}

func (w *Writer) closeFile() error {
	if w.file == nil {
		return nil
	}
	err := w.file.Close()
	w.file = nil
	return err
}

func (w *Writer) rotate() error {
	now := w.now()
	if err := w.closeFile(); err != nil {
		return err
	}
	target := w.path + "." + now.Format(backupLayout)
	for {
		if _, err := os.Stat(target); err != nil {
			break
		}
		now = now.Add(time.Nanosecond)
		target = w.path + "." + now.Format(backupLayout)
	}
	if err := os.Rename(w.path, target); err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			_ = w.open()
			return fmt.Errorf("rotate log: %w", err)
		}
	} else if w.compress {
		_, _ = compressFile(target)
	}
	w.prune(now)
	return w.open()
}

type backup struct {
	name string
	at   time.Time
}

// prune removes backups by age and count. It scans the directory instead of
// keeping in-memory state so restarts still enforce the retention policy.
func (w *Writer) prune(now time.Time) {
	dir := filepath.Dir(w.path)
	base := filepath.Base(w.path) + "."
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	var backups []backup
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		rest, ok := strings.CutPrefix(entry.Name(), base)
		if !ok {
			continue
		}
		rest = strings.TrimSuffix(rest, ".gz")
		at, err := time.Parse(backupLayout, rest)
		if err != nil {
			continue
		}
		backups = append(backups, backup{name: entry.Name(), at: at})
	}
	sort.Slice(backups, func(i, j int) bool { return backups[i].at.After(backups[j].at) })

	kept := backups[:0]
	for _, item := range backups {
		if w.maxAge > 0 && now.Sub(item.at) > w.maxAge {
			os.Remove(filepath.Join(dir, item.name))
			continue
		}
		kept = append(kept, item)
	}
	if w.maxBackups > 0 {
		for i := w.maxBackups; i < len(kept); i++ {
			os.Remove(filepath.Join(dir, kept[i].name))
		}
	}
}

func compressFile(path string) (string, error) {
	source, err := os.Open(path)
	if err != nil {
		return "", err
	}
	target := path + ".gz"
	out, err := os.OpenFile(target, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		source.Close()
		return "", err
	}
	zipper := gzip.NewWriter(out)
	_, err = io.Copy(zipper, source)
	source.Close()
	if closeErr := zipper.Close(); err == nil {
		err = closeErr
	}
	if closeErr := out.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		os.Remove(target)
		return "", err
	}
	// Windows refuses to delete a file that still has an open handle, so both
	// handles above are closed before the plain file is removed.
	if err := os.Remove(path); err != nil {
		return "", err
	}
	return target, nil
}
