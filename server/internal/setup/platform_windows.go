//go:build windows

package setup

import (
	"encoding/base64"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unicode/utf16"
	"unsafe"
)

const viewerExe = "chast_viewer.exe"

var (
	advapi32                = syscall.NewLazyDLL("advapi32.dll")
	procGetTokenInformation = advapi32.NewProc("GetTokenInformation")
)

// DefaultRoot returns the per-user or machine-wide install directory.
func DefaultRoot(machine bool) string {
	if machine {
		return filepath.Join(os.Getenv("ProgramFiles"), "chast")
	}
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		base, _ = os.UserHomeDir()
	}
	return filepath.Join(base, "Programs", "chast")
}

// IsElevated reports whether the current process holds an elevated token.
func IsElevated() bool {
	process, err := syscall.GetCurrentProcess()
	if err != nil {
		return false
	}
	var token syscall.Token
	if err := syscall.OpenProcessToken(process, syscall.TOKEN_QUERY, &token); err != nil {
		return false
	}
	defer token.Close()
	var elevated uint32
	var size uint32
	const tokenElevation = 20
	r, _, _ := procGetTokenInformation.Call(uintptr(token), tokenElevation,
		uintptr(unsafe.Pointer(&elevated)), unsafe.Sizeof(elevated), uintptr(unsafe.Pointer(&size)))
	return r != 0 && elevated != 0
}

// prepareInstall stops a registered chast-host service so its executable can
// be replaced. The returned function starts it again after the install.
func prepareInstall(opts Options) (func(), error) {
	if !opts.Machine || !serviceInstalled() {
		return nil, nil
	}
	status := serviceStatus()
	if status == "Stopped" || status == "" {
		return nil, nil
	}
	opts.logf("stopping the chast-host service")
	if out, err := runPowerShell("Stop-Service -Name 'chast-host' -Force -ErrorAction Stop"); err != nil {
		return nil, fmt.Errorf("stop the chast-host service: %w (%s)", err, firstLine(out))
	}
	for i := 0; i < 30; i++ {
		if serviceStatus() == "Stopped" {
			return func() {
				if out, err := runPowerShell("Start-Service -Name 'chast-host' -ErrorAction Stop"); err != nil {
					opts.logf("start the chast-host service: %v (%s)", err, firstLine(out))
				}
			}, nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return nil, errors.New("the chast-host service did not stop; stop it and rerun the installer")
}

func serviceStatus() string {
	out, err := runPowerShell("$s = Get-Service -Name 'chast-host' -ErrorAction SilentlyContinue; if ($s) { $s.Status.ToString() }")
	if err != nil {
		return ""
	}
	return firstLine(out)
}

func platformInstall(opts Options, payloadBytes int64) error {
	if err := checkElevation(opts); err != nil {
		return err
	}
	if opts.Service {
		service := filepath.Join(opts.Root, "chast-service.exe")
		if _, err := os.Stat(service); err != nil {
			return fmt.Errorf("--service needs %s in the payload: %w", filepath.Base(service), err)
		}
		opts.logf("registering the chast-host service")
		if out, err := run(service, "--install"); err != nil {
			return fmt.Errorf("chast-service --install: %w (%s)", err, firstLine(out))
		}
		opts.logf("chast-host service installed")
	}
	if opts.Register {
		if err := writeUninstallEntry(opts, payloadBytes); err != nil {
			return err
		}
		opts.logf("registered %s in Programs and Features", opts.AppID)
	}
	if opts.Shortcut {
		if err := createShortcut(opts); err != nil {
			return err
		}
		opts.logf("created the Start Menu shortcut")
	}
	return nil
}

func platformUninstall(opts Options) error {
	if err := checkElevation(opts); err != nil {
		return err
	}
	service := filepath.Join(opts.Root, "chast-service.exe")
	if _, err := os.Stat(service); err == nil && serviceInstalled() {
		opts.logf("removing the chast-host service")
		if out, err := run(service, "--uninstall"); err != nil {
			return fmt.Errorf("chast-service --uninstall: %w (%s)", err, firstLine(out))
		}
	}
	removeShortcut(opts)
	key, err := uninstallKey(opts)
	if err != nil {
		return err
	}
	if out, err := run("reg.exe", "query", key); err != nil {
		_ = out
		return nil
	}
	if out, err := run("reg.exe", "delete", key, "/f"); err != nil {
		return fmt.Errorf("remove the uninstall entry: %w (%s)", err, firstLine(out))
	}
	return nil
}

// finishUninstall removes the tree. When the uninstaller itself runs from the
// install root Windows keeps its image locked, so everything else is deleted
// now and a detached PowerShell retries the rest after this process exits.
func finishUninstall(root string) error {
	root, err := safeRoot(root)
	if err != nil {
		return err
	}
	self, selfErr := os.Executable()
	if selfErr != nil || !inside(self, root) {
		return RemoveTree(root)
	}
	if err := pruneTree(root, self); err != nil {
		return fmt.Errorf("remove %s: %w", root, err)
	}
	script := fmt.Sprintf(
		"Start-Sleep -Seconds 2\n"+
			"for ($i = 0; $i -lt 20; $i++) {\n"+
			"  try { Remove-Item -LiteralPath '%s' -Recurse -Force -ErrorAction Stop; break }\n"+
			"  catch { Start-Sleep -Milliseconds 500 }\n"+
			"}\n", psQuote(root))
	cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden",
		"-EncodedCommand", encodePowerShell(script))
	// DETACHED_PROCESS makes PowerShell exit without running the script, so
	// detach with CREATE_NO_WINDOW and its own process group instead.
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000 | 0x00000200}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("schedule the removal of %s: %w", root, err)
	}
	return cmd.Process.Release()
}

func checkElevation(opts Options) error {
	if opts.Machine && !IsElevated() {
		return errors.New("installing for all users requires an elevated prompt")
	}
	if opts.Service && !opts.Machine {
		return errors.New("--service requires --machine")
	}
	return nil
}

func uninstallKey(opts Options) (string, error) {
	appID := strings.TrimSpace(opts.AppID)
	if appID == "" || strings.ContainsAny(appID, `\/`) {
		return "", fmt.Errorf("invalid application id %q", appID)
	}
	hive := "HKCU"
	if opts.Machine {
		hive = "HKLM"
	}
	return hive + `\Software\Microsoft\Windows\CurrentVersion\Uninstall\` + appID, nil
}

func writeUninstallEntry(opts Options, payloadBytes int64) error {
	key, err := uninstallKey(opts)
	if err != nil {
		return err
	}
	root := opts.Root
	target := filepath.Join(root, viewerExe)
	uninstall := fmt.Sprintf("%s --uninstall --root \"%s\"", quoteForCmd(filepath.Join(root, "chast-setup.exe")), root)
	if opts.Machine {
		uninstall += " --machine"
	}
	quiet := uninstall + " --quiet"
	size := payloadBytes / 1024
	if size < 1 {
		size = 1
	}
	values := []struct{ name, kind, data string }{
		{"DisplayName", "REG_SZ", "chast"},
		{"DisplayVersion", "REG_SZ", opts.Version},
		{"Publisher", "REG_SZ", "chast"},
		{"DisplayIcon", "REG_SZ", target},
		{"InstallLocation", "REG_SZ", root},
		{"UninstallString", "REG_SZ", uninstall},
		{"QuietUninstallString", "REG_SZ", quiet},
		{"NoModify", "REG_DWORD", "1"},
		{"NoRepair", "REG_DWORD", "1"},
		{"EstimatedSize", "REG_DWORD", strconv.FormatInt(size, 10)},
	}
	for _, value := range values {
		if out, err := run("reg.exe", "add", key, "/v", value.name, "/t", value.kind, "/d", value.data, "/f"); err != nil {
			return fmt.Errorf("write %s: %w (%s)", value.name, err, firstLine(out))
		}
	}
	return nil
}

func startMenuDir(machine bool) string {
	if machine {
		return filepath.Join(os.Getenv("ProgramData"), "Microsoft", "Windows", "Start Menu", "Programs")
	}
	return filepath.Join(os.Getenv("APPDATA"), "Microsoft", "Windows", "Start Menu", "Programs")
}

func shortcutPath(opts Options) string {
	return filepath.Join(startMenuDir(opts.Machine), "chast.lnk")
}

func createShortcut(opts Options) error {
	dir := startMenuDir(opts.Machine)
	if dir == "" {
		return errors.New("cannot locate the Start Menu directory")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("create the Start Menu directory: %w", err)
	}
	lnk := shortcutPath(opts)
	target := filepath.Join(opts.Root, viewerExe)
	if _, err := os.Stat(target); err != nil {
		return fmt.Errorf("create the shortcut: %w", err)
	}
	script := fmt.Sprintf(
		"$shell = New-Object -ComObject WScript.Shell\n"+
			"$shortcut = $shell.CreateShortcut('%s')\n"+
			"$shortcut.TargetPath = '%s'\n"+
			"$shortcut.WorkingDirectory = '%s'\n"+
			"$shortcut.Description = 'chast'\n"+
			"$shortcut.Save()\n",
		psQuote(lnk), psQuote(target), psQuote(opts.Root))
	if out, err := runPowerShell(script); err != nil {
		return fmt.Errorf("create the Start Menu shortcut: %w (%s)", err, firstLine(out))
	}
	return nil
}

func removeShortcut(opts Options) {
	os.Remove(shortcutPath(opts))
}

func psQuote(value string) string {
	return strings.ReplaceAll(value, "'", "''")
}

// runPowerShell passes the script as UTF-16LE base64 so paths with spaces,
// quotes or non-ASCII characters need no command-line escaping.
func runPowerShell(script string) (string, error) {
	cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encodePowerShell(script))
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	out, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

func encodePowerShell(script string) string {
	encoded := utf16.Encode([]rune(script))
	raw := make([]byte, len(encoded)*2)
	for i, unit := range encoded {
		raw[i*2] = byte(unit)
		raw[i*2+1] = byte(unit >> 8)
	}
	return base64.StdEncoding.EncodeToString(raw)
}

func run(name string, args ...string) (string, error) {
	cmd := exec.Command(name, args...)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	out, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

func serviceInstalled() bool {
	_, err := run("sc.exe", "query", "chast-host")
	return err == nil
}

// inside reports whether path is root or below it.
func inside(path, root string) bool {
	path = strings.ToLower(filepath.Clean(path))
	root = strings.ToLower(filepath.Clean(root))
	return path == root || strings.HasPrefix(path, root+string(filepath.Separator))
}

// pruneTree deletes everything under dir except keep.
func pruneTree(dir, keep string) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		path := filepath.Join(dir, entry.Name())
		if samePath(path, keep) {
			continue
		}
		if entry.IsDir() {
			if err := pruneTree(path, keep); err != nil {
				return err
			}
			if err := os.Remove(path); err != nil {
				return err
			}
			continue
		}
		if err := os.Remove(path); err != nil {
			return err
		}
	}
	return nil
}

func quoteForCmd(path string) string {
	return `"` + path + `"`
}

func firstLine(out string) string {
	if out == "" {
		return "no output"
	}
	if i := strings.IndexByte(out, '\n'); i >= 0 {
		out = out[:i]
	}
	return strings.TrimSpace(out)
}
