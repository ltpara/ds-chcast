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

// TestConsoleExposesDevicePolicyReceipts keeps the policy page wired to the
// admin API and the receipt rule: a version is only shown as applied when the
// host-reported version matches the policy being displayed.
func TestConsoleExposesDevicePolicyReceipts(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`data-tab="policies"`,
		`id="panel-policies"`,
		`id="policy-rows"`,
		`id="policy-export-json"`,
		`id="policy-import-json"`,
		`id="policy-import-file"`,
		`id="policy-import-conflict"`,
		`id="policy-dialog"`,
		`id="policy-ceiling"`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"/v1/device-policies",
		"/v1/policies/export",
		"/v1/policies/import",
		"chast-policy-objects-",
		"receipt.policy_version !== policy.policy_version",
		"receipt.policy_version === policy.policy_version",
		"'已应用版本 '",
		"'待应用'",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesDeviceAdministration keeps the device table wired to the
// operator-managed state: the disabled block (with its own enable action) and
// the owner account field.
func TestConsoleExposesDeviceAdministration(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="device-owner"`,
		`所有者（账号用户名，留空表示不指定）`,
		`<th>所有者</th>`,
		`id="device-select-all"`,
		`id="device-bulk-disable"`,
		`id="device-bulk-enable"`,
		`id="device-bulk-clear"`,
		`id="device-selection"`,
		`id="device-export"`,
		`id="device-prev"`,
		`id="device-next"`,
		`id="device-page"`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		`peer.disabled`,
		`'已禁用'`,
		`'禁用设备'`,
		`'启用设备'`,
		`body: { disabled: !peer.disabled }`,
		`$('device-owner').value`,
		`body: { alias, tags, owner }`,
		`deviceSelection: new Set()`,
		`'/v1/identities/bulk'`,
		`body: { ids, disabled }`,
		`bulkSetDisabled(event.currentTarget, true)`,
		`bulkSetDisabled(event.currentTarget, false)`,
		`state.pendingBulk`,
		`updateDeviceSelection()`,
		`$('device-export').addEventListener`,
		`exportDevices`,
		`'chast-devices-' + csvStamp() + '.csv'`,
		`params.set('format', 'csv')`,
		`deviceOffset: 0`,
		`params.set('offset', String(state.deviceOffset))`,
		`function updateDevicePager`,
		`$('device-prev').addEventListener`,
		`$('device-next').addEventListener`,
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesPolicyAuthoring keeps the policy tab wired to the
// reusable policy objects and scoped assignments, and to the materialised
// per-device view that names the assignment it came from.
func TestConsoleExposesPolicyAuthoring(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="policy-object-rows"`,
		`id="policy-object-dialog"`,
		`id="policy-object-ceiling"`,
		`id="policy-assignment-rows"`,
		`id="policy-assignment-dialog"`,
		`id="policy-assignment-scope"`,
		`id="policy-assignment-priority"`,
		`device（一台或多台设备）`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"'/v1/policies'",
		"'/v1/policy-assignments'",
		"'/v1/policy-assignments/bulk'",
		"'/v1/device-policies'",
		"function policyObjectRow",
		"function policyAssignmentRow",
		"function sourceCell",
		"function splitDeviceIDs",
		"POLICY_ASSIGNMENT_BATCH_LIMIT = 200",
		"已有分配",
		"scope_kind",
		"devices: values,",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesAPITokenManagement keeps the token tab wired to the
// account-owned token API: creation carries role, resources, optional group
// scope and TTL, the secret is shown once, and listing/revocation stay on the
// same admin surface.
func TestConsoleExposesAPITokenManagement(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`data-tab="tokens"`,
		`id="panel-tokens"`,
		`id="token-create"`,
		`id="token-resources"`,
		`id="token-group"`,
		`id="token-ttl-days"`,
		`id="token-rows"`,
		`id="token-secret-dialog"`,
		`id="token-secret-value"`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"'/v1/api-tokens'",
		"function loadTokens",
		"function tokenRow",
		"function showTokenSecret",
		"expires_in_seconds",
		"GROUP_SCOPED_RESOURCES",
		"tokenAdmin",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesSecurityKeyLogin keeps the account login wired to the
// WebAuthn ceremony: password + key and passwordless discovery both start at
// the account API and finish by adopting the returned session.
func TestConsoleExposesSecurityKeyLogin(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="login-webauthn"`,
		`使用安全密钥登录`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"'/v1/account/login/webauthn/begin'",
		"'/v1/account/login/webauthn/finish'",
		"'/v1/account/login/webauthn/passwordless/begin'",
		"'/v1/account/login/webauthn/passwordless/finish'",
		"function unlockWithSecurityKey",
		"function adoptAccountSession",
		"navigator.credentials.get",
		"base64urlBytes",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesSingleSignOn keeps the console wired to the OIDC routes
// the account API already serves: probe, begin, then poll for the session the
// browser callback stored.
func TestConsoleExposesSingleSignOn(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="login-oidc"`,
		`使用单点登录`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"'/v1/account/oidc'",
		"'/v1/account/oidc/begin'",
		"'/v1/account/oidc/poll'",
		"function probeSingleSignOn",
		"function unlockWithSingleSignOn",
		"function adoptAccountSession",
		"window.open('', '_blank')",
		"client: 'console'",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesLoginCodes keeps the account login wired to the email and
// SMS second-factor routes: the kind selector decides which code field the
// login body carries, and the send button reuses the same password.
func TestConsoleExposesLoginCodes(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="login-code-kind"`,
		`id="login-send-code"`,
		`value="totp"`,
		`value="email"`,
		`value="sms"`,
		`value="recovery"`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"'/v1/account/login/email'",
		"'/v1/account/login/sms'",
		"body.email_code",
		"body.sms_code",
		"body.totp_code",
		"body.recovery_code",
		"function sendLoginCode",
		"$('login-code-kind').value",
		"'用户名或密码不正确。'",
		"'该账号已被封禁。'",
		"'尝试次数过多，请稍后再试。'",
		"'服务器未启用账号登录。'",
		"detail.includes('directory')",
		"'目录（LDAP/AD）服务暂时不可用，请稍后再试或联系管理员。'",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesAccountSelfService keeps the "我的账号" tab wired to the
// account self-service API: profile, password, TOTP enrollment, security keys
// and sessions.
// TestConsoleExposesAccountRecovery keeps the admin account table wired to the
// recovery-contact routes: an operator can set or clear another account's
// email and phone and text a reset code without that user's password.
func TestConsoleExposesAccountRecovery(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="account-recovery-dialog"`,
		`id="account-recovery-email"`,
		`id="account-recovery-email-send"`,
		`id="account-recovery-phone"`,
		`id="account-recovery-sms"`,
		`<th>找回方式</th>`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"function openAccountRecoveryDialog",
		"function saveAccountRecovery",
		"function sendAccountResetSMS",
		"function sendAccountResetEmail",
		"'/recover'",
		"'/email'",
		"'/phone'",
		"'/sms'",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesAccountSessions keeps the admin account table wired to the
// per-account login view: an operator can list another account's live sessions,
// revoke one device or sign the account out everywhere.
func TestConsoleExposesAccountSessions(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="account-sessions-dialog"`,
		`id="account-session-rows"`,
		`id="account-sessions-summary"`,
		`id="account-sessions-prev"`,
		`id="account-sessions-next"`,
		`id="account-sessions-revoke-all"`,
		`id="account-session-empty"`,
		`<th>登录 IP</th>`,
		`<th>最近活动</th>`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"function openAccountSessionsDialog",
		"function loadAccountSessions",
		"function adminSessionRow",
		"function renderSessionPager",
		"function sessionPageQuery",
		"function revokeAllAccountSessions",
		"state.accountSessionsLimit",
		"last_seen_at_ms",
		"session.online",
		"'/sessions/revoke-all'",
		"'登录会话'",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesAccountDirectory keeps the account table on the paged,
// searchable directory endpoint: username/contact search, role and sort
// controls, a pager and a CSV export that follows the same filter.
func TestConsoleExposesAccountDirectory(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="account-query"`,
		`id="account-role-filter"`,
		`id="account-sort"`,
		`id="account-order"`,
		`id="account-prev"`,
		`id="account-next"`,
		`id="account-page"`,
		`id="account-export"`,
		`<option value="operator">`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"function accountDirectoryParams",
		"function exportAccounts",
		"params.set('sort'",
		"params.set('order'",
		"state.accountLimit",
		"state.accountOffset",
		"params.set('limit', String(state.accountLimit))",
		"'/v1/accounts?'",
		"'chast-accounts-'",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

func TestConsoleExposesAccountSelfService(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`data-tab="mine"`,
		`id="mine-password-form"`,
		`id="mine-email-form"`,
		`id="mine-phone-form"`,
		`id="mine-2fa-start"`,
		`id="mine-key-form"`,
		`id="mine-key-rows"`,
		`id="mine-session-rows"`,
		`id="mine-sessions-summary"`,
		`id="mine-sessions-prev"`,
		`id="mine-sessions-next"`,
		`id="mine-recovery-download"`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		"'/v1/account'",
		"'/v1/account/password'",
		"'/v1/account/email'",
		"'/v1/account/phone'",
		"'/v1/account/2fa/start'",
		"'/v1/account/2fa/enable'",
		"'/v1/account/2fa/disable'",
		"'/v1/account/2fa/recovery'",
		"'/v1/account/webauthn/credentials'",
		"'/v1/account/webauthn/register/begin'",
		"'/v1/account/webauthn/register/finish'",
		"'/v1/account/webauthn/credentials/remove'",
		"'/v1/account/sessions'",
		"'/v1/account/sessions/revoke-others'",
		"function loadMine",
		"function saveMineContact",
		"function registerMineSecurityKey",
		"function renderMineKeys",
		"function renderMineSessions",
		"function renderSessionPager",
		"function sessionPageQuery",
		"state.mineSessionsLimit",
		"last_seen_at_ms",
		"session.online",
		"function downloadMineRecoveryCodes",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

// TestConsoleExposesReportTimeRange keeps the audit/connection/file tabs wired
// to the shared since/until window, including its CSV export path.
func TestConsoleExposesReportTimeRange(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="audit-since" type="datetime-local"`,
		`id="audit-until" type="datetime-local"`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		`[['since', 'audit-since'], ['until', 'audit-until']]`,
		`Date.parse(raw)`,
		`params.set(key, String(ms))`,
		`[$('audit-since'), $('audit-until')]`,
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
	}
}

func TestConsoleExposesAuditUserFilter(t *testing.T) {
	index, err := assets.ReadFile("assets/index.html")
	if err != nil {
		t.Fatal(err)
	}
	page := string(index)
	for _, want := range []string{
		`id="audit-user"`,
		`id="audit-host"`,
		`placeholder="账号（可选，精确匹配）"`,
		`placeholder="主机 ID（可选）"`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html is missing %q", want)
		}
	}
	script, err := assets.ReadFile("assets/app.js")
	if err != nil {
		t.Fatal(err)
	}
	js := string(script)
	for _, want := range []string{
		`hasUser: true`,
		`hasHost: true`,
		`$('audit-user').classList.toggle('hidden', !spec.hasUser)`,
		`$('audit-host').classList.toggle('hidden', !spec.hasHost)`,
		`params.set('user', user)`,
		`params.set('host', host)`,
		`'主机', '结果'`,
		`event.host_id || '—'`,
		`[$('audit-filter'), $('audit-host'), $('audit-user')]`,
	} {
		if !strings.Contains(js, want) {
			t.Errorf("app.js is missing %q", want)
		}
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
