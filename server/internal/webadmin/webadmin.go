// Package webadmin serves the embedded operator console. The console is a
// static single page that talks to the same origin API with the server key, so
// it needs no build step and no extra credentials beyond the key an operator
// would already pass to the loopback CLI.
package webadmin

import (
	"embed"
	"io/fs"
	"net/http"
	"strconv"
	"strings"
)

// Prefix is the mount point of the console. Requests below it are resolved
// against a fixed allowlist, so the route cannot expose arbitrary files.
const Prefix = "/admin/"

const contentSecurityPolicy = "default-src 'self'; script-src 'self'; style-src 'self'; " +
	"connect-src 'self'; img-src 'self' data:; base-uri 'none'; " +
	"frame-ancestors 'none'; form-action 'self'"

//go:embed assets
var assets embed.FS

var allowed = map[string]string{
	"index.html": "text/html; charset=utf-8",
	"app.js":     "text/javascript; charset=utf-8",
	"style.css":  "text/css; charset=utf-8",
}

// Handler serves the console. index.html is returned for the mount point
// itself; every other name must match the allowlist exactly.
func Handler() http.Handler {
	files, err := fs.Sub(assets, "assets")
	if err != nil {
		panic("webadmin: embedded assets: " + err.Error())
	}
	return &handler{files: files}
}

type handler struct {
	files fs.FS
}

func (h *handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	header := w.Header()
	header.Set("Content-Security-Policy", contentSecurityPolicy)
	header.Set("X-Content-Type-Options", "nosniff")
	header.Set("X-Frame-Options", "DENY")
	header.Set("Referrer-Policy", "no-referrer")
	header.Set("Cache-Control", "no-store")

	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		header.Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if !strings.HasPrefix(r.URL.Path, Prefix) {
		http.NotFound(w, r)
		return
	}
	name := strings.TrimPrefix(r.URL.Path, Prefix)
	if name == "" {
		name = "index.html"
	}
	contentType, ok := allowed[name]
	if !ok {
		http.NotFound(w, r)
		return
	}
	data, err := fs.ReadFile(h.files, name)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	header.Set("Content-Type", contentType)
	header.Set("Content-Length", strconv.Itoa(len(data)))
	w.WriteHeader(http.StatusOK)
	if r.Method == http.MethodHead {
		return
	}
	_, _ = w.Write(data)
}
