package main

import (
	"errors"
	"flag"
	"log"
	"os"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/updater"
)

// runConfirmUpdate writes the health marker for an in-place update. The viewer
// calls it once its first frame is up; apply-update restores the previous
// image when the marker never arrives.
func runConfirmUpdate(args []string) error {
	fs := flag.NewFlagSet("confirm-update", flag.ExitOnError)
	target := fs.String("target", "", "binary that was just updated; required")
	logPath := fs.String("log", "", "append progress to this file; stderr is always written")
	fs.Parse(args)

	if *logPath != "" {
		if file, err := os.OpenFile(*logPath, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644); err == nil {
			log.SetOutput(file)
			defer file.Close()
		}
	}
	logf := func(format string, values ...any) {
		log.Printf("confirm-update: "+format, values...)
	}

	if strings.TrimSpace(*target) == "" {
		return errors.New("--target is required")
	}
	confirmed, err := updater.Confirm(*target)
	if err != nil {
		return err
	}
	if !confirmed {
		logf("no pending update for %s", *target)
		return nil
	}
	logf("confirmed the health of %s", *target)
	return nil
}
