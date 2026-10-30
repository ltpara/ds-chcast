// Command chast-setup installs and removes chast on Windows. The release zip
// is either a separate file (--payload) or appended to this executable by
// cmd/chast-package.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/ltpara/ds-chcast/server/internal/setup"
)

var version = "dev"

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "chast-setup:", err)
		os.Exit(1)
	}
}

func run() error {
	var (
		payloadPath = flag.String("payload", "", "release zip to install; empty uses the payload appended to this executable")
		root        = flag.String("root", "", "install directory; defaults per-user or, with --machine, under Program Files")
		appID       = flag.String("app-id", "chast", "name of the Programs and Features entry")
		appVersion  = flag.String("version", version, "version recorded in Programs and Features")
		machine     = flag.Bool("machine", false, "install for all users under Program Files (requires elevation)")
		service     = flag.Bool("service", false, "register chast-host as a Windows service (requires --machine)")
		shortcut    = flag.Bool("shortcut", true, "create a Start Menu shortcut")
		register    = flag.Bool("register", true, "write the Programs and Features entry")
		uninstall   = flag.Bool("uninstall", false, "remove the installed files and registration")
		list        = flag.Bool("list", false, "list the payload entries and exit")
		quiet       = flag.Bool("quiet", false, "suppress progress output")
	)
	flag.Parse()

	install := setup.Options{
		Root:     *root,
		AppID:    *appID,
		Version:  *appVersion,
		Machine:  *machine,
		Service:  *service,
		Shortcut: *shortcut,
		Register: *register,
		Quiet:    *quiet,
	}
	install.Logf = func(format string, args ...any) {
		if !*quiet {
			fmt.Printf("chast-setup: "+format+"\n", args...)
		}
	}
	if install.Root == "" {
		install.Root = setup.DefaultRoot(*machine)
	}
	if *service && !*machine {
		return fmt.Errorf("--service requires --machine")
	}

	if *uninstall {
		if *machine && !setup.IsElevated() {
			return fmt.Errorf("--uninstall --machine requires an elevated prompt")
		}
		if err := setup.Uninstall(install); err != nil {
			return err
		}
		install.Logf("removed %s", install.Root)
		return nil
	}

	source := *payloadPath
	if source == "" {
		self, err := os.Executable()
		if err != nil {
			return fmt.Errorf("locate the installer: %w", err)
		}
		source = self
	}
	payload, err := setup.OpenPayload(source)
	if err != nil {
		return err
	}
	defer payload.Close()
	if *list {
		fmt.Printf("%d files, %d bytes\n", payload.Files, payload.Bytes)
		for _, name := range payload.Entries() {
			fmt.Println(name)
		}
		return nil
	}
	if *machine && !setup.IsElevated() {
		return fmt.Errorf("--machine requires an elevated prompt")
	}
	if err := setup.Install(payload, install); err != nil {
		return err
	}
	install.Logf("installed chast %s to %s", install.Version, install.Root)
	return nil
}
