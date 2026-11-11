package geoip

import "sync"

// Table is the read side shared by the startup table and a reloadable one.
type Table interface {
	Lookup(addr string) string
	Len() int
}

// Reloader wraps a table loaded from a fixed path so the admin console can
// replace it at runtime, mirroring hbbs's reload-geo. Lookups take a read lock
// and never wait on the file; a failed reload keeps the previous table.
type Reloader struct {
	path string
	mu   sync.RWMutex
	cur  *Locator
}

func NewReloader(path string, current *Locator) *Reloader {
	return &Reloader{path: path, cur: current}
}

func (r *Reloader) Lookup(addr string) string {
	if r == nil {
		return ""
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.cur.Lookup(addr)
}

func (r *Reloader) Len() int {
	if r == nil {
		return 0
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.cur.Len()
}

// Path returns the file reloaded by Reload.
func (r *Reloader) Path() string {
	if r == nil {
		return ""
	}
	return r.path
}

// Reload reads the path again and swaps the table on success. It returns the
// new prefix count; a parse or read error leaves the previous table in place.
func (r *Reloader) Reload() (int, error) {
	next, err := Load(r.path)
	if err != nil {
		return 0, err
	}
	r.mu.Lock()
	r.cur = next
	r.mu.Unlock()
	return next.Len(), nil
}
