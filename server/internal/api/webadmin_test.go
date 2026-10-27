package api_test

import (
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestAdminConsoleRoutes(t *testing.T) {
	s := testutil.Start(t)

	resp, err := http.Get(s.URL + "/admin/")
	if err != nil {
		t.Fatal(err)
	}
	body, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET /admin/ = %d %s", resp.StatusCode, body)
	}
	if got := resp.Header.Get("Content-Security-Policy"); !strings.Contains(got, "default-src 'self'") {
		t.Errorf("console CSP = %q", got)
	}
	if got := resp.Header.Get("X-Content-Type-Options"); got != "nosniff" {
		t.Errorf("X-Content-Type-Options = %q", got)
	}
	if !strings.Contains(string(body), "chast 管理台") {
		t.Error("console page does not carry its title")
	}

	noRedirect := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	redirect, err := noRedirect.Get(s.URL + "/admin")
	if err != nil {
		t.Fatal(err)
	}
	redirect.Body.Close()
	if redirect.StatusCode != http.StatusMovedPermanently || redirect.Header.Get("Location") != "/admin/" {
		t.Errorf("GET /admin = %d %q", redirect.StatusCode, redirect.Header.Get("Location"))
	}

	asset, err := http.Get(s.URL + "/admin/app.js")
	if err != nil {
		t.Fatal(err)
	}
	asset.Body.Close()
	if asset.StatusCode != http.StatusOK ||
		!strings.HasPrefix(asset.Header.Get("Content-Type"), "text/javascript") {
		t.Errorf("GET /admin/app.js = %d %q", asset.StatusCode, asset.Header.Get("Content-Type"))
	}

	posted, err := http.Post(s.URL+"/admin/app.js", "text/plain", strings.NewReader("x"))
	if err != nil {
		t.Fatal(err)
	}
	posted.Body.Close()
	if posted.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("POST /admin/app.js = %d", posted.StatusCode)
	}

	missing, err := http.Get(s.URL + "/admin/nothere.css")
	if err != nil {
		t.Fatal(err)
	}
	missing.Body.Close()
	if missing.StatusCode != http.StatusNotFound {
		t.Errorf("GET /admin/nothere.css = %d", missing.StatusCode)
	}
}
