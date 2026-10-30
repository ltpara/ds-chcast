// Command chast-package appends a release zip to the chast-setup stub so one
// executable carries both the installer and its payload.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/ltpara/ds-chcast/server/internal/setup"
)

func main() {
	stub := flag.String("stub", "", "chast-setup.exe built without a payload")
	payload := flag.String("payload", "", "release zip created by tools/package-windows.ps1")
	output := flag.String("output", "", "setup executable to write")
	flag.Parse()
	if *stub == "" || *payload == "" || *output == "" {
		fmt.Fprintln(os.Stderr, "usage: chast-package --stub chast-setup.exe --payload release.zip --output chast-setup-full.exe")
		os.Exit(2)
	}
	if err := setup.AppendPayload(*stub, *payload, *output); err != nil {
		fmt.Fprintln(os.Stderr, "chast-package:", err)
		os.Exit(1)
	}
	info, err := os.Stat(*output)
	if err != nil {
		fmt.Fprintln(os.Stderr, "chast-package:", err)
		os.Exit(1)
	}
	fmt.Printf("packed %s (%d bytes)\n", *output, info.Size())
}
