package webadmin

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func serve(t *testing.T, method, target string) *httptest.ResponseRecorder {
	t.Helper()
	rec := httptest.NewRecorder()
	Handler().ServeHTTP(rec, httptest.NewRequest(method, target, nil))
	return rec
}

func checkSecurityHeaders(t *testing.T, rec *httptest.ResponseRecorder) {
	t.Helper()
	csp := rec.Header().Get("Content-Security-Policy")
	for _, want := range []string{"default-src 'self'", "script-src 'self'", "style-src 'self'",
		"connect-src 'self'", "frame-ancestors 'none'"} {
		if !strings.Contains(csp, want) {
			t.Errorf("CSP %q is missing %q", csp, want)
		}
	}
	if strings.Contains(csp, "unsafe-inline") {
		t.Errorf("CSP allows inline code: %q", csp)
	}
	for name, want := range map[string]string{
		"X-Content-Type-Options": "nosniff",
		"X-Frame-Options":        "DENY",
		"Referrer-Policy":        "no-referrer",
		"Cache-Control":          "no-store",
	} {
		if got := rec.Header().Get(name); got != want {
			t.Errorf("%s = %q, want %q", name, got, want)
		}
	}
}

func TestServesConsoleAssets(t *testing.T) {
	cases := []struct {
		name        string
		target      string
		contentType string
		marker      string
	}{
		{"mount point", "/admin/", "text/html", "chast 管理台"},
		{"index", "/admin/index.html", "text/html", "<!doctype html>"},
		{"script", "/admin/app.js", "text/javascript", "'use strict'"},
		{"style", "/admin/style.css", "text/css", "--bg"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rec := serve(t, http.MethodGet, tc.target)
			if rec.Code != http.StatusOK {
				t.Fatalf("GET %s = %d", tc.target, rec.Code)
			}
			if got := rec.Header().Get("Content-Type"); !strings.HasPrefix(got, tc.contentType) {
				t.Errorf("Content-Type = %q, want %q", got, tc.contentType)
			}
			if !strings.Contains(rec.Body.String(), tc.marker) {
				t.Errorf("body does not contain %q", tc.marker)
			}
			checkSecurityHeaders(t, rec)
		})
	}
}

func TestHeadServesHeadersWithoutBody(t *testing.T) {
	rec := serve(t, http.MethodHead, "/admin/app.js")
	if rec.Code != http.StatusOK {
		t.Fatalf("HEAD = %d", rec.Code)
	}
	if rec.Body.Len() != 0 {
		t.Errorf("HEAD body = %d bytes", rec.Body.Len())
	}
	if rec.Header().Get("Content-Length") == "" {
		t.Error("HEAD did not report Content-Length")
	}
	checkSecurityHeaders(t, rec)
}

func TestRejectsUnlistedRequests(t *testing.T) {
	cases := []struct {
		method string
		target string
		status int
	}{
		{http.MethodPost, "/admin/", http.StatusMethodNotAllowed},
		{http.MethodPut, "/admin/app.js", http.StatusMethodNotAllowed},
		{http.MethodGet, "/admin/missing.js", http.StatusNotFound},
		{http.MethodGet, "/admin/../secret", http.StatusNotFound},
		{http.MethodGet, "/admin/index.html/", http.StatusNotFound},
		{http.MethodGet, "/assets/app.js", http.StatusNotFound},
	}
	for _, tc := range cases {
		rec := serve(t, tc.method, tc.target)
		if rec.Code != tc.status {
			t.Errorf("%s %s = %d, want %d", tc.method, tc.target, rec.Code, tc.status)
		}
		if tc.status == http.StatusMethodNotAllowed {
			if got := rec.Header().Get("Allow"); got != "GET, HEAD" {
				t.Errorf("Allow = %q", got)
			}
		}
		checkSecurityHeaders(t, rec)
	}
}

// TestAssetsStayCSPCompatible guards the policy that the console contains no
// inline script or style and never assigns data through HTML parsing helpers.
func TestAssetsStayCSPCompatible(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := strings.ToLower(string(index))
	for _, banned := range []string{"onclick=", "onload=", "<style", " style="} {
		if strings.Contains(page, banned) {
			t.Errorf("index.html contains %q", banned)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, banned := range []string{".innerHTML", "document.write", "insertAdjacentHTML", "eval("} {
		if strings.Contains(js, banned) {
			t.Errorf("app.js contains %q", banned)
		}
	}
}
