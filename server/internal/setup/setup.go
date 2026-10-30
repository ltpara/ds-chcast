// Package setup installs a chast release tree from a zip payload, either as a
// plain archive or appended to the installer executable. The installer is
// per-user by default because the viewer updates itself in place; --machine
// switches to Program Files and all-users registration.
package setup

import (
	"archive/zip"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const (
	payloadMagic  = "CHASTPK1"
	payloadSuffix = 16
)

// Payload is an open release archive.
type Payload struct {
	reader *zip.Reader
	closer io.Closer
	Files  int
	Bytes  int64
}

// Options carries the installer and uninstaller settings.
type Options struct {
	Root     string
	AppID    string
	Version  string
	Machine  bool
	Service  bool
	Shortcut bool
	Register bool
	Quiet    bool
	Logf     func(format string, args ...any)
}

func (o Options) logf(format string, args ...any) {
	if !o.Quiet && o.Logf != nil {
		o.Logf(format, args...)
	}
}

// OpenPayload opens a release zip. A path that ends with an appended payload
// (a setup executable) is accepted as well.
func OpenPayload(path string) (*Payload, error) {
	if payload, err := OpenSelf(path); err == nil {
		return payload, nil
	}
	archive, err := zip.OpenReader(path)
	if err != nil {
		return nil, fmt.Errorf("open payload %s: %w", path, err)
	}
	return newPayload(&archive.Reader, archive)
}

// OpenSelf reads the zip appended by cmd/chast-package. A file without the
// trailer is reported as carrying no payload.
func OpenSelf(path string) (*Payload, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	info, err := file.Stat()
	if err != nil {
		file.Close()
		return nil, err
	}
	if info.Size() < payloadSuffix {
		file.Close()
		return nil, errors.New("the setup binary carries no payload")
	}
	trailer := make([]byte, payloadSuffix)
	if _, err := file.ReadAt(trailer, info.Size()-payloadSuffix); err != nil {
		file.Close()
		return nil, err
	}
	if string(trailer[:8]) != payloadMagic {
		file.Close()
		return nil, errors.New("the setup binary carries no payload")
	}
	size := int64(binary.LittleEndian.Uint64(trailer[8:]))
	if size <= 0 || size > info.Size()-payloadSuffix {
		file.Close()
		return nil, errors.New("the setup payload is truncated")
	}
	section := io.NewSectionReader(file, info.Size()-payloadSuffix-size, size)
	reader, err := zip.NewReader(section, size)
	if err != nil {
		file.Close()
		return nil, fmt.Errorf("open the appended payload: %w", err)
	}
	payload, err := newPayload(reader, file)
	if err != nil {
		file.Close()
		return nil, err
	}
	return payload, nil
}

func newPayload(reader *zip.Reader, closer io.Closer) (*Payload, error) {
	payload := &Payload{reader: reader, closer: closer, Files: len(reader.File)}
	for _, file := range reader.File {
		if !file.FileInfo().IsDir() {
			payload.Bytes += int64(file.UncompressedSize64)
		}
	}
	return payload, nil
}

// Close releases the underlying archive.
func (p *Payload) Close() error {
	if p.closer == nil {
		return nil
	}
	return p.closer.Close()
}

// Entries lists the files in install order.
func (p *Payload) Entries() []string {
	names := make([]string, 0, p.Files)
	for _, file := range p.reader.File {
		if file.FileInfo().IsDir() {
			continue
		}
		names = append(names, file.Name)
	}
	return names
}

// Install extracts the payload into opts.Root and then writes the platform
// registration (uninstall entry, shortcut and optional service).
func Install(payload *Payload, opts Options) error {
	if payload == nil {
		return errors.New("no install payload")
	}
	root, err := absoluteRoot(opts.Root)
	if err != nil {
		return err
	}
	opts.Root = root
	restart, err := prepareInstall(opts)
	if err != nil {
		return err
	}
	if restart != nil {
		defer restart()
	}
	if err := extract(payload, opts.Root, opts.logf); err != nil {
		return err
	}
	if err := copySelf(opts.Root); err != nil {
		return err
	}
	return platformInstall(opts, payload.Bytes)
}

// Uninstall removes the platform registration and then the installed tree.
func Uninstall(opts Options) error {
	root, err := absoluteRoot(opts.Root)
	if err != nil {
		return err
	}
	opts.Root = root
	if err := platformUninstall(opts); err != nil {
		return err
	}
	return finishUninstall(opts.Root)
}

// absoluteRoot canonicalizes the install directory so the registration, the
// uninstaller's own location and the cleanup all refer to the same path.
func absoluteRoot(root string) (string, error) {
	if strings.TrimSpace(root) == "" {
		return "", errors.New("install directory is empty")
	}
	root = filepath.Clean(root)
	if !filepath.IsAbs(root) {
		abs, err := filepath.Abs(root)
		if err != nil {
			return "", fmt.Errorf("resolve %s: %w", root, err)
		}
		root = abs
	}
	return root, nil
}

// RemoveTree deletes the installed files. User settings and logs live outside
// the install root and are kept.
func RemoveTree(root string) error {
	root, err := safeRoot(root)
	if err != nil {
		return err
	}
	if err := os.RemoveAll(root); err != nil {
		return fmt.Errorf("remove %s: %w (a running program may still hold files)", root, err)
	}
	return nil
}

// safeRoot rejects paths whose removal would escape an install directory.
func safeRoot(root string) (string, error) {
	root = filepath.Clean(root)
	if root == "" || root == "." || root == string(filepath.Separator) ||
		filepath.VolumeName(root) == root ||
		root == filepath.VolumeName(root)+string(filepath.Separator) ||
		root == ".." || strings.HasPrefix(root, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("refusing to remove %q", root)
	}
	return root, nil
}

// AppendPayload copies stub to output and appends the zip plus a trailer that
// OpenSelf understands.
func AppendPayload(stub, payload, output string) error {
	in, err := os.Open(stub)
	if err != nil {
		return fmt.Errorf("open the setup stub: %w", err)
	}
	defer in.Close()
	archive, err := os.Open(payload)
	if err != nil {
		return fmt.Errorf("open the payload: %w", err)
	}
	defer archive.Close()
	info, err := archive.Stat()
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 {
		return errors.New("the payload is not a regular file")
	}
	out, err := os.OpenFile(output, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o755)
	if err != nil {
		return fmt.Errorf("create %s: %w", output, err)
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		return fmt.Errorf("copy the setup stub: %w", err)
	}
	if _, err := io.Copy(out, archive); err != nil {
		return fmt.Errorf("append the payload: %w", err)
	}
	trailer := make([]byte, payloadSuffix)
	copy(trailer, payloadMagic)
	binary.LittleEndian.PutUint64(trailer[8:], uint64(info.Size()))
	if _, err := out.Write(trailer); err != nil {
		return fmt.Errorf("write the payload trailer: %w", err)
	}
	return out.Close()
}

func extract(payload *Payload, root string, logf func(string, ...any)) error {
	if err := os.MkdirAll(root, 0o755); err != nil {
		return fmt.Errorf("create %s: %w", root, err)
	}
	for _, file := range payload.reader.File {
		name := strings.TrimSuffix(file.Name, "/")
		if name == "" {
			continue
		}
		local := filepath.FromSlash(name)
		if !filepath.IsLocal(local) {
			return fmt.Errorf("payload entry %q escapes the install directory", file.Name)
		}
		if file.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("payload entry %q is a link", file.Name)
		}
		dst := filepath.Join(root, local)
		if file.FileInfo().IsDir() {
			if err := os.MkdirAll(dst, 0o755); err != nil {
				return fmt.Errorf("create %s: %w", dst, err)
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return fmt.Errorf("create %s: %w", filepath.Dir(dst), err)
		}
		if err := writeEntry(file, dst); err != nil {
			return err
		}
		logf("installed %s", dst)
	}
	return nil
}

// writeEntry stages the file next to its target and swaps it in so a failed
// copy never truncates the installed program.
func writeEntry(file *zip.File, dst string) error {
	perm := file.Mode().Perm()
	if perm == 0 {
		perm = 0o644
	}
	src, err := file.Open()
	if err != nil {
		return fmt.Errorf("read %s: %w", file.Name, err)
	}
	defer src.Close()
	tmp := dst + ".new"
	out, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, perm)
	if err != nil {
		return fmt.Errorf("create %s: %w", tmp, err)
	}
	if _, err := io.Copy(out, src); err != nil {
		out.Close()
		os.Remove(tmp)
		return fmt.Errorf("write %s: %w", dst, err)
	}
	if err := out.Close(); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("write %s: %w", dst, err)
	}
	if err := os.Remove(dst); err != nil && !os.IsNotExist(err) {
		os.Remove(tmp)
		return fmt.Errorf("replace %s: %w (close the running program and retry)", dst, err)
	}
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("replace %s: %w", dst, err)
	}
	return nil
}

// copySelf keeps the uninstaller inside the install root so the Add or Remove
// Programs entry does not depend on where the setup file was downloaded.
func copySelf(root string) error {
	self, err := os.Executable()
	if err != nil {
		return fmt.Errorf("locate the installer: %w", err)
	}
	dst := filepath.Join(root, "chast-setup.exe")
	if samePath(self, dst) {
		return nil
	}
	src, err := os.Open(self)
	if err != nil {
		return fmt.Errorf("read the installer: %w", err)
	}
	defer src.Close()
	tmp := dst + ".new"
	out, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o755)
	if err != nil {
		return fmt.Errorf("create %s: %w", tmp, err)
	}
	if _, err := io.Copy(out, src); err != nil {
		out.Close()
		os.Remove(tmp)
		return fmt.Errorf("copy the installer: %w", err)
	}
	if err := out.Close(); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("copy the installer: %w", err)
	}
	os.Remove(dst)
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("copy the installer: %w", err)
	}
	return nil
}

func samePath(a, b string) bool {
	absA, errA := filepath.Abs(a)
	absB, errB := filepath.Abs(b)
	if errA != nil || errB != nil {
		return filepath.Clean(a) == filepath.Clean(b)
	}
	return strings.EqualFold(filepath.Clean(absA), filepath.Clean(absB))
}
