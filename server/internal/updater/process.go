package updater

import (
	"errors"
	"os/exec"
	"time"
)

// Running is a started target image. The watchdog waits for either the health
// marker or Done, and stops the process before replacing it during a rollback.
type Running struct {
	cmd  *exec.Cmd
	done chan struct{}
	err  error
}

func newRunning(cmd *exec.Cmd) *Running {
	running := &Running{cmd: cmd, done: make(chan struct{})}
	go func() {
		running.err = cmd.Wait()
		close(running.done)
	}()
	return running
}

// PID identifies the started process.
func (r *Running) PID() int {
	return r.cmd.Process.Pid
}

// Done is closed when the process exits.
func (r *Running) Done() <-chan struct{} {
	return r.done
}

// Stop terminates the process and waits for it to exit. A process that has
// already finished is treated as stopped.
func (r *Running) Stop(timeout time.Duration) error {
	select {
	case <-r.done:
		return nil
	default:
	}
	if timeout <= 0 {
		timeout = 10 * time.Second
	}
	if err := Terminate(r.PID()); err != nil {
		return err
	}
	select {
	case <-r.done:
		return nil
	case <-time.After(timeout):
		return errors.New("the new image did not exit after termination")
	}
}
