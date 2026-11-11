'use strict';

(() => {
  const STORAGE_KEY = 'chast.admin.session';
  const LEGACY_KEY = 'chast.admin.key';
  const REFRESH_MS = 15000;
  const ROLE_RANK = { readonly: 1, operator: 2, admin: 3 };

  const $ = (id) => document.getElementById(id);

  const state = {
    // {mode:'key', key} for the server key, or
    // {mode:'account', token, refresh, username, role} for an operator login.
    auth: null,
    tab: 'overview',
    publicKey: '',
    editing: null,
    timer: 0,
    devicesLoading: false,
    deviceSelection: new Set(),
    deviceList: [],
    deviceOffset: 0,
    deviceLimit: 200,
    deviceTotal: 0,
    pendingBulk: '',
    bulkTimer: 0,
    auditTab: 'events',
    auditBefore: 0,
    auditRows: [],
    auditGen: 0,
    recordingBefore: 0,
    recordingRows: [],
    recordingGen: 0,
    groups: [],
    groupsLoaded: false,
    groupsLoading: false,
    groupEditing: null,
    tokens: [],
    tokensLoading: false,
    tokenSecret: '',
    policies: [],
    policiesLoaded: false,
    policiesLoading: false,
    policyEditing: null,
    policyObjects: [],
    policyAssignments: [],
    policyObjectEditing: null,
    accountCapUser: null,
    accountRecoveryUser: null,
    accountSessionsUser: null,
    accountLimit: 50,
    accountOffset: 0,
    // Set by probeSingleSignOn when the server has an identity provider.
    oidc: { enabled: false, name: '' },
    // Bumped to abandon an in-flight SSO poll (logout, another login).
    oidcGen: 0,
    // Self-service state: GET /v1/account, its sessions, and the pending
    // TOTP secret between start and enable.
    accountInfo: null,
    accountSessions: [],
    account2faPending: null,
    // Security keys: null until loaded; enabled=false when the server has no
    // WebAuthn relying party configured (the list route answers 503).
    accountKeys: null,
  };

  class ApiError extends Error {
    constructor(message, status) {
      super(message);
      this.status = status;
    }
  }

  // ---------- small DOM and formatting helpers ----------

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function button(text, className) {
    const node = el('button', className || '', text);
    node.type = 'button';
    return node;
  }

  function clear(node) {
    while (node.firstChild) node.removeChild(node.firstChild);
  }

  function cell(value) {
    const td = document.createElement('td');
    if (value instanceof Node) td.appendChild(value);
    else td.textContent = value === undefined || value === null || value === '' ? '—' : String(value);
    return td;
  }

  function pill(text, kind) {
    return el('span', 'pill' + (kind ? ' ' + kind : ''), text);
  }

  function pillBool(ok) {
    return ok ? pill('成功', 'ok') : pill('失败', 'bad');
  }

  const ROUTE_LABELS = {
    direct: '直连',
    punch: '打洞',
    relay: '中继',
    carrier: '共享承载',
  };

  function routeCell(value) {
    if (!value) return '—';
    const node = el('span', '', ROUTE_LABELS[value] || value);
    node.title = value;
    return node;
  }

  function refCell(value) {
    if (!value) return '—';
    const node = el('span', 'mono', value.length > 18 ? value.slice(0, 18) + '…' : value);
    node.title = value;
    return node;
  }

  function fmtBytes(value) {
    const n = Number(value);
    if (value === undefined || value === null || !Number.isFinite(n)) return '—';
    if (n < 1024) return n + ' B';
    const units = ['KiB', 'MiB', 'GiB', 'TiB', 'PiB'];
    let out = n / 1024;
    let unit = 0;
    while (out >= 1024 && unit < units.length - 1) {
      out /= 1024;
      unit++;
    }
    return out.toFixed(out >= 100 ? 0 : 1) + ' ' + units[unit];
  }

  function fmtRate(bps) {
    return bps ? fmtBytes(bps) + '/s' : '不限';
  }

  function fmtSeconds(sec) {
    const n = Number(sec);
    if (sec === undefined || sec === null || !Number.isFinite(n)) return '—';
    if (n < 60) return n + ' 秒';
    if (n < 3600) return (n / 60).toFixed(1) + ' 分钟';
    return (n / 3600).toFixed(1) + ' 小时';
  }

  function fmtDurationMS(ms) {
    const n = Number(ms);
    if (!Number.isFinite(n) || n < 0) return '—';
    const sec = Math.floor(n / 1000);
    if (sec < 60) return sec + ' 秒';
    const min = Math.floor(sec / 60);
    if (min < 60) return min + ' 分 ' + (sec % 60) + ' 秒';
    return Math.floor(min / 60) + ' 小时 ' + (min % 60) + ' 分';
  }

  function fmtAge(ms) {
    const n = Number(ms);
    if (ms === undefined || ms === null || !Number.isFinite(n) || n < 0) return '—';
    const sec = Math.floor(n / 1000);
    if (sec < 5) return '刚刚';
    if (sec < 60) return sec + ' 秒前';
    const min = Math.floor(sec / 60);
    if (min < 60) return min + ' 分钟前';
    const hour = Math.floor(min / 60);
    if (hour < 24) return hour + ' 小时前';
    return Math.floor(hour / 24) + ' 天前';
  }

  function fmtTime(ms) {
    const n = Number(ms);
    if (!Number.isFinite(n) || n <= 0) return '—';
    return new Date(n).toLocaleString('zh-CN', { hour12: false });
  }

  function fmtPercent(ratio) {
    const n = Number(ratio);
    if (!Number.isFinite(n) || n <= 0) return '未启用';
    return (n * 100).toFixed(n < 0.1 ? 1 : 0) + '%';
  }

  let toastTimer = 0;
  function toast(message, isError) {
    const node = $('toast');
    node.textContent = message;
    node.classList.toggle('error', Boolean(isError));
    node.classList.remove('hidden');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => node.classList.add('hidden'), isError ? 6000 : 3000);
  }

  function reportError(err) {
    if (err && err.status === 401) return;
    toast(err && err.message ? err.message : String(err), true);
  }

  async function withButton(control, action) {
    if (!control || control.disabled) return;
    control.disabled = true;
    try {
      await action();
    } catch (err) {
      reportError(err);
    } finally {
      control.disabled = false;
    }
  }

  // ---------- session and API ----------

  function readSession() {
    try {
      const raw = sessionStorage.getItem(STORAGE_KEY);
      if (raw) {
        const parsed = JSON.parse(raw);
        if (parsed && (parsed.mode === 'key' || parsed.mode === 'account')) return parsed;
      }
      // Sessions saved before account logins existed stored the raw server key.
      const legacy = sessionStorage.getItem(LEGACY_KEY);
      if (legacy !== null) return { mode: 'key', key: legacy };
    } catch {
      /* storage can be unavailable in locked-down browsers */
    }
    return null;
  }

  function writeSession(auth) {
    try {
      sessionStorage.setItem(STORAGE_KEY, JSON.stringify(auth));
      sessionStorage.removeItem(LEGACY_KEY);
    } catch {
      /* ignore */
    }
  }

  function clearSession() {
    try {
      sessionStorage.removeItem(STORAGE_KEY);
      sessionStorage.removeItem(LEGACY_KEY);
    } catch {
      /* ignore */
    }
  }

  function can(level) {
    const auth = state.auth;
    if (!auth) return false;
    if (auth.mode === 'key') return true;
    return (ROLE_RANK[auth.role] || 0) >= ROLE_RANK[level];
  }

  // groupOnly is an account whose console access comes from group membership
  // alone; it may only use the Groups tab and group-scoped audit views.
  function groupOnly() {
    const auth = state.auth;
    return Boolean(auth && auth.mode === 'account' && !ROLE_RANK[auth.role]);
  }

  function groupByID(id) {
    return state.groups.find((group) => group.id === id) || null;
  }

  // group.role already carries the stronger of the global console role and
  // the membership role, so this mirrors the server's groupActor decision.
  function canGroup(group, level) {
    const auth = state.auth;
    if (!auth || !group) return false;
    if (auth.mode === 'key') return true;
    return (ROLE_RANK[group.role] || 0) >= ROLE_RANK[level];
  }

  function authToken() {
    const auth = state.auth;
    if (!auth) return '';
    return auth.mode === 'key' ? auth.key : auth.token;
  }

  function authHeaders() {
    const token = authToken();
    return token ? { Authorization: 'Bearer ' + token } : {};
  }

  function readJSON(text) {
    if (!text) return null;
    try {
      return JSON.parse(text);
    } catch {
      return null;
    }
  }

  // WebAuthn carries binary fields as unpadded base64url; the browser API
  // wants ArrayBuffers in both directions.
  function base64urlBytes(value) {
    let text = String(value || '').replace(/-/g, '+').replace(/_/g, '/');
    while (text.length % 4) text += '=';
    const raw = atob(text);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
    return bytes;
  }

  function bytesBase64url(value) {
    const bytes = new Uint8Array(value);
    let text = '';
    for (let i = 0; i < bytes.length; i += 1) text += String.fromCharCode(bytes[i]);
    return btoa(text).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }

  // postJSON is the unauthenticated login-side request: it keeps the error
  // body so a caller can word a refusal itself instead of locking the console.
  async function postJSON(path, body) {
    const resp = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
      cache: 'no-store',
    });
    const data = readJSON(await resp.text());
    if (!resp.ok) {
      const message = (data && data.error) || resp.status + ' ' + resp.statusText;
      const error = new ApiError(message, resp.status);
      // OIDC poll failures carry a machine-readable code next to the text.
      if (data && data.code) error.code = data.code;
      throw error;
    }
    return data;
  }

  // refreshSession rotates the account pair. It runs on every expired access
  // token, so a console stays usable for the refresh token's lifetime.
  async function refreshSession() {
    const auth = state.auth;
    if (!auth || auth.mode !== 'account' || !auth.refresh) return false;
    try {
      const resp = await fetch('/v1/account/refresh', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ refresh_token: auth.refresh }),
        cache: 'no-store',
      });
      if (!resp.ok) return false;
      const data = readJSON(await resp.text());
      if (!data || !data.token || !data.refresh_token) return false;
      auth.token = data.token;
      auth.refresh = data.refresh_token;
      if (data.user) {
        auth.username = data.user.username || auth.username;
        auth.role = data.user.role || auth.role;
      }
      writeSession(auth);
      renderSession();
      applyPermissions();
      return true;
    } catch {
      return false;
    }
  }

  async function api(path, options, retried) {
    const opts = options || {};
    const headers = authHeaders();
    if (opts.body !== undefined) headers['Content-Type'] = 'application/json';
    const resp = await fetch(path, {
      method: opts.method || 'GET',
      headers,
      body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
      cache: 'no-store',
    });
    const text = await resp.text();
    const data = readJSON(text);
    if (resp.status === 401) {
      if (!retried && await refreshSession()) {
        return api(path, options, true);
      }
      lock('登录已失效或凭据被拒绝，请重新登录。');
      throw new ApiError('unauthorized', 401);
    }
    if (!resp.ok) {
      const message = data && data.error ? data.error : resp.status + ' ' + resp.statusText;
      throw new ApiError(message, resp.status);
    }
    return data;
  }

  async function unlockKey(key) {
    state.auth = { mode: 'key', key };
    await api('/v1/stats');
    writeSession(state.auth);
    await enterConsole();
  }

  async function hasGroupConsoleAccess(token) {
    try {
      const resp = await fetch('/v1/groups', {
        headers: { Authorization: 'Bearer ' + token },
        cache: 'no-store',
      });
      if (!resp.ok) return false;
      const data = readJSON(await resp.text());
      return Boolean(data && Array.isArray(data.groups) && data.groups.length > 0);
    } catch {
      return false;
    }
  }

  // adoptAccountSession applies a completed account login (password or
  // security key): a session without a console role or group grant is revoked
  // instead of being kept around unused.
  async function adoptAccountSession(data, username) {
    const user = data && data.user ? data.user : {};
    if (!ROLE_RANK[user.role] && !(await hasGroupConsoleAccess(data.token))) {
      // The session is valid but useless here; revoke it right away.
      fetch('/v1/account/logout', {
        method: 'POST',
        headers: { Authorization: 'Bearer ' + data.token },
        cache: 'no-store',
      }).catch(() => {});
      throw new ApiError('该账号没有管理台权限（需要 readonly、operator 或 admin 角色，或至少一个分组授权）。', 403);
    }
    state.auth = {
      mode: 'account',
      token: data.token,
      refresh: data.refresh_token,
      username: user.username || username,
      role: user.role,
    };
    writeSession(state.auth);
    await enterConsole();
  }

  async function unlockAccount(username, password, code, codeKind) {
    const body = { username, password };
    const value = (code || '').trim();
    if (value) {
      // "auto" keeps the original guess: six digits is a TOTP code and
      // anything else is a printed recovery code. An explicit choice lets the
      // operator use the email or SMS second factor instead.
      const kind = codeKind || 'auto';
      if (kind === 'totp') body.totp_code = value;
      else if (kind === 'email') body.email_code = value;
      else if (kind === 'sms') body.sms_code = value;
      else if (kind === 'recovery') body.recovery_code = value;
      else if (/^\d{6}$/.test(value)) body.totp_code = value;
      else body.recovery_code = value;
    }
    const resp = await fetch('/v1/account/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
      cache: 'no-store',
    });
    const data = readJSON(await resp.text());
    if (resp.status === 401) {
      const factor = resp.headers.get('X-Chast-2FA');
      const message = factor === 'required' ? '该账号启用了两步验证，请输入验证码。'
        : factor === 'invalid' ? '两步验证码不正确。'
          : '用户名或密码不正确。';
      throw new ApiError(message, 401);
    }
    // Account login keeps its operator-facing wording in the console instead of
    // echoing the server's English sentences.
    if (resp.status === 403) throw new ApiError('该账号已被封禁。', 403);
    if (resp.status === 429) throw new ApiError('尝试次数过多，请稍后再试。', 429);
    if (resp.status === 503) {
      // Accounts missing and an unreachable directory share the status but not
      // the fix, so the server's sentence picks the wording.
      const detail = data && data.error ? String(data.error) : '';
      if (detail.includes('directory')) {
        throw new ApiError('目录（LDAP/AD）服务暂时不可用，请稍后再试或联系管理员。', 503);
      }
      throw new ApiError('服务器未启用账号登录。', 503);
    }
    if (!resp.ok) {
      throw new ApiError('登录失败（' + resp.status + '）。', resp.status);
    }
    await adoptAccountSession(data, username);
  }

  // sendLoginCode asks the server to mail or text a one-time second-factor
  // code. The password is verified server-side before anything is sent, so
  // this only saves typing when the second factor is not an authenticator.
  async function sendLoginCode() {
    const kind = $('login-code-kind').value;
    if (kind !== 'email' && kind !== 'sms') {
      $('login-note').textContent = '先在验证码类型中选择“邮箱验证码”或“短信验证码”。';
      return;
    }
    const username = $('login-username').value.trim();
    const password = $('login-password').value;
    if (!username || !password) {
      $('login-note').textContent = '请先填写用户名和密码。';
      return;
    }
    try {
      await postJSON(
        kind === 'email' ? '/v1/account/login/email' : '/v1/account/login/sms',
        { username, password },
      );
      $('login-note').textContent = '如果该账号登记了找回' +
        (kind === 'email' ? '邮箱' : '手机') + '并启用了两步验证，验证码已发送。';
    } catch (err) {
      if (err.status === 401) {
        $('login-note').textContent = '用户名或密码不正确。';
        return;
      }
      if (err.status === 403) {
        $('login-note').textContent = '该账号已被封禁。';
        return;
      }
      if (err.status === 429) {
        $('login-note').textContent = '验证码请求过于频繁，请稍后再试。';
        return;
      }
      if (err.status === 503) {
        $('login-note').textContent = kind === 'email'
          ? '服务器未配置邮件发送，无法发送验证码。'
          : '服务器未配置短信发送，无法发送验证码。';
        return;
      }
      throw err;
    }
  }

  // unlockWithSecurityKey runs the browser half of the WebAuthn ceremony the
  // account API already serves. A password turns it into password + key; an
  // empty password asks for a passwordless (discoverable) credential instead.
  async function unlockWithSecurityKey() {
    const username = $('login-username').value.trim();
    const password = $('login-password').value;
    if (!username) {
      $('login-note').textContent = '请先填写用户名。';
      return;
    }
    if (!window.isSecureContext || !window.PublicKeyCredential || !navigator.credentials) {
      $('login-note').textContent = '当前页面不能使用安全密钥（需要 HTTPS 或 localhost）。';
      return;
    }
    const twoFactor = Boolean(password);
    const beginPath = twoFactor
      ? '/v1/account/login/webauthn/begin'
      : '/v1/account/login/webauthn/passwordless/begin';
    let begin;
    try {
      begin = await postJSON(beginPath, twoFactor ? { username, password } : { username });
    } catch (err) {
      if (err.status === 401) throw new ApiError('用户名或密码不正确。', 401);
      if (err.status === 503) throw new ApiError('服务器未配置安全密钥登录。', 503);
      if (err.status === 429) throw new ApiError('尝试次数过多，请稍后再试。', 429);
      throw err;
    }
    if (!begin || !begin.challenge) {
      // The server answers 202 without a challenge when the account has no
      // registered key, so this route cannot be used to enumerate accounts.
      $('login-note').textContent = '该账号没有可用的安全密钥，请用密码登录。';
      return;
    }
    if (!Array.isArray(begin.credentials) || begin.credentials.length === 0) {
      $('login-note').textContent = '该账号没有可用的安全密钥，请用密码登录。';
      return;
    }
    let assertion = null;
    try {
      assertion = await navigator.credentials.get({
        publicKey: {
          challenge: base64urlBytes(begin.challenge),
          rpId: begin.rp_id,
          timeout: Number(begin.timeout_ms) || 60000,
          userVerification: 'preferred',
          allowCredentials: begin.credentials.map((cred) => ({
            type: 'public-key',
            id: base64urlBytes(cred.id),
          })),
        },
      });
    } catch (err) {
      if (err && err.name === 'NotAllowedError') {
        $('login-note').textContent = '已取消或超时，未使用安全密钥。';
        return;
      }
      $('login-note').textContent = '安全密钥失败：' + ((err && err.message) || String(err));
      return;
    }
    if (!assertion || !assertion.response) return;
    const response = assertion.response;
    const finishPath = twoFactor
      ? '/v1/account/login/webauthn/finish'
      : '/v1/account/login/webauthn/passwordless/finish';
    let data;
    try {
      data = await postJSON(finishPath, {
        username,
        credential_id: bytesBase64url(assertion.rawId),
        client_data_json: bytesBase64url(response.clientDataJSON),
        authenticator_data: bytesBase64url(response.authenticatorData),
        signature: bytesBase64url(response.signature),
        device_name: 'webadmin',
      });
    } catch (err) {
      if (err.status === 401) throw new ApiError('安全密钥验证未被接受。', 401);
      if (err.status === 429) throw new ApiError('尝试次数过多，请稍后再试。', 429);
      throw err;
    }
    $('login-password').value = '';
    $('login-totp').value = '';
    await adoptAccountSession(data, username);
  }

  // probeSingleSignOn reveals the SSO button only when the server has an
  // identity provider configured; an unconfigured server answers 200 with
  // enabled=false, so any failure here just leaves the button hidden.
  async function probeSingleSignOn() {
    try {
      const resp = await fetch('/v1/account/oidc', { cache: 'no-store' });
      if (!resp.ok) return;
      const data = readJSON(await resp.text());
      if (!data || !data.enabled) return;
      state.oidc = { enabled: true, name: String(data.name || '') };
      const control = $('login-oidc');
      control.textContent = state.oidc.name
        ? '使用 ' + state.oidc.name + ' 登录'
        : '使用单点登录';
      control.classList.remove('hidden');
    } catch {
      /* the button simply stays hidden */
    }
  }

  function showSingleSignOnLink(url) {
    const note = $('login-note');
    note.textContent = '浏览器拦截了登录窗口，请点击此链接打开单点登录页面：';
    const link = el('a', '', '打开单点登录');
    link.href = url;
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
    note.appendChild(link);
  }

  function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  function singleSignOnError(err) {
    switch (err && err.code) {
      case 'oidc_expired': return '单点登录已过期，请重试。';
      case 'no_account': return '该身份未关联任何管理台账号，请联系管理员。';
      case 'account_banned': return '该账号已被封禁。';
      case 'email_taken': return '该身份的邮箱已被其他账号使用，请联系管理员。';
      default: return '单点登录未能完成，请改用密码登录。';
    }
  }

  // unlockWithSingleSignOn opens the provider page in a new tab, then polls
  // for the session the browser callback created. The tab is opened before the
  // first request so the click still counts as the popup-blocker gesture.
  async function unlockWithSingleSignOn() {
    if (!state.oidc.enabled) {
      $('login-note').textContent = '服务器未配置单点登录。';
      return;
    }
    const gen = ++state.oidcGen;
    let tab = null;
    try {
      tab = window.open('', '_blank');
    } catch {
      tab = null;
    }
    let begin;
    try {
      begin = await postJSON('/v1/account/oidc/begin', {
        device_id: '',
        device_name: 'webadmin',
        client: 'console',
      });
    } catch (err) {
      if (tab) tab.close();
      if (err.status === 503) {
        throw new ApiError('单点登录暂不可用，请稍后再试或改用密码登录。', 503);
      }
      throw err;
    }
    let target = null;
    try {
      target = new URL(begin && begin.auth_url ? begin.auth_url : '');
    } catch {
      target = null;
    }
    if (!begin || !begin.state || !target ||
        (target.protocol !== 'https:' && target.protocol !== 'http:')) {
      if (tab) tab.close();
      $('login-note').textContent = '单点登录地址无效，请联系管理员。';
      return;
    }
    $('login-note').textContent = '正在等待浏览器中的单点登录完成…';
    if (tab) {
      // The provider page must not be able to script this console.
      try {
        tab.opener = null;
      } catch {
        /* about:blank always allows this */
      }
      tab.location.replace(target.toString());
    } else {
      showSingleSignOnLink(target.toString());
    }
    const deadline = Date.now() + (Number(begin.expires_in_ms) || 600000);
    while (state.oidcGen === gen && Date.now() < deadline) {
      await sleep(1500);
      if (state.oidcGen !== gen) return;
      if (tab && tab.closed) {
        $('login-note').textContent = '登录窗口已关闭，单点登录已取消。';
        return;
      }
      let data;
      try {
        data = await postJSON('/v1/account/oidc/poll', { state: begin.state });
      } catch (err) {
        if (err.status === 503) {
          throw new ApiError('单点登录暂不可用，请稍后再试。', 503);
        }
        if (err.status === 404 || err.status === 401) {
          $('login-note').textContent = singleSignOnError(err);
          return;
        }
        throw err;
      }
      if (data && data.status === 'pending') continue;
      if (data && data.token) {
        if (tab && !tab.closed) tab.close();
        await adoptAccountSession(data, '');
        return;
      }
    }
    if (state.oidcGen === gen) {
      $('login-note').textContent = '单点登录超时，请重试。';
    }
  }

  async function enterConsole() {
    // Any login that wins the console cancels a still-running SSO poll.
    state.oidcGen++;
    $('login-form').classList.add('hidden');
    $('session-bar').classList.remove('hidden');
    $('login-note').textContent = '';
    $('workspace').classList.remove('hidden');
    renderSession();
    try {
      await loadGroups();
    } catch (err) {
      // A server without the account store still has a key-only console.
      if (!err || err.status !== 503) throw err;
      state.groups = [];
      state.groupsLoaded = true;
    }
    if (groupOnly() && !state.groups.length) {
      lock('该账号没有分组授权。');
      return;
    }
    applyPermissions();
    startTimer();
    switchTab(groupOnly() ? 'groups' : 'overview');
  }

  function renderSession() {
    const auth = state.auth;
    if (!auth) return;
    const role = auth.mode === 'key' ? 'server-key' : (ROLE_RANK[auth.role] ? auth.role : 'group');
    const pill = $('session-role');
    pill.textContent = role;
    pill.className = 'pill ' + (role === 'admin' || role === 'server-key' ? 'ok'
      : role === 'operator' ? 'warn' : '');
    $('session-label').textContent = auth.mode === 'key' ? '服务器密钥' : auth.username;
  }

  function applyPermissions() {
    const admin = can('admin');
    const write = can('operator');
    const read = can('readonly');
    const restricted = groupOnly();
    const tokenAdmin = admin && !restricted && Boolean(state.auth && state.auth.mode === 'account');
    const visibility = {
      overview: read && !restricted,
      devices: read && !restricted,
      policies: admin,
      accounts: admin,
      mine: Boolean(state.auth && state.auth.mode === 'account'),
      tokens: tokenAdmin,
      groups: true,
      recordings: read && !restricted,
      audit: read || restricted,
      server: read && !restricted,
    };
    document.querySelectorAll('.tab').forEach((node) => {
      node.classList.toggle('hidden', !visibility[node.dataset.tab]);
    });
    $('device-readonly').classList.toggle('hidden', write);
    $('recording-readonly').classList.toggle('hidden', write);
    $('group-create-panel').classList.toggle('hidden', !admin);
    $('group-readonly').classList.toggle('hidden', admin);
    if (!visibility[state.tab]) switchTab(restricted ? 'groups' : 'overview');
  }

  function lock(message) {
    stopTimer();
    // A pending SSO poll must not sign the operator back in after a logout.
    state.oidcGen++;
    const auth = state.auth;
    if (auth && auth.mode === 'account' && auth.token) {
      fetch('/v1/account/logout', {
        method: 'POST',
        headers: { Authorization: 'Bearer ' + auth.token },
        cache: 'no-store',
      }).catch(() => {});
    }
    state.auth = null;
    state.publicKey = '';
    state.auditRows = [];
    state.auditGen++;
    state.recordingRows = [];
    state.recordingGen++;
    state.deviceOffset = 0;
    state.deviceTotal = 0;
    state.deviceSelection.clear();
    state.groups = [];
    state.groupsLoaded = false;
    state.groupEditing = null;
    state.policies = [];
    state.policiesLoaded = false;
    state.policyEditing = null;
    state.policyObjects = [];
    state.policyAssignments = [];
    state.policyObjectEditing = null;
    state.tokens = [];
    state.tokenSecret = '';
    resetMine();
    clear($('group-rows'));
    clear($('policy-rows'));
    clear($('policy-object-rows'));
    clear($('policy-assignment-rows'));
    clear($('token-rows'));
    clear($('audit-group'));
    const dialog = $('device-dialog');
    if (dialog.open) dialog.close();
    const policyDialog = $('policy-dialog');
    if (policyDialog.open) policyDialog.close();
    const policyObjectDialog = $('policy-object-dialog');
    if (policyObjectDialog.open) policyObjectDialog.close();
    const policyAssignmentDialog = $('policy-assignment-dialog');
    if (policyAssignmentDialog.open) policyAssignmentDialog.close();
    const groupDialog = $('group-dialog');
    if (groupDialog.open) groupDialog.close();
    const tokenSecretDialog = $('token-secret-dialog');
    if (tokenSecretDialog.open) tokenSecretDialog.close();
    const accountCapDialog = $('account-cap-dialog');
    if (accountCapDialog.open) accountCapDialog.close();
    $('token-secret-value').textContent = '';
    clearSession();
    $('workspace').classList.add('hidden');
    $('session-bar').classList.add('hidden');
    $('login-form').classList.remove('hidden');
    $('server-key').value = '';
    $('login-password').value = '';
    $('login-totp').value = '';
    $('login-code-kind').value = 'auto';
    $('login-note').textContent = message || '已断开连接。';
    syncLoginMode();
  }

  function startTimer() {
    stopTimer();
    state.timer = setInterval(() => {
      if (document.hidden) return;
      if (state.tab === 'overview') {
        refreshOverview().catch(() => {});
      } else if (state.tab === 'devices') {
        loadDevices(true).catch(() => {});
      } else if (state.tab === 'policies') {
        loadPolicies(true).catch(() => {});
      } else if (state.tab === 'groups') {
        loadGroups(true).catch(() => {});
      }
    }, REFRESH_MS);
  }

  function stopTimer() {
    if (state.timer) {
      clearInterval(state.timer);
      state.timer = 0;
    }
  }

  // ---------- tabs ----------

  function switchTab(tab) {
    state.tab = tab;
    document.querySelectorAll('.tab').forEach((node) => {
      node.classList.toggle('active', node.dataset.tab === tab);
    });
    document.querySelectorAll('.panel-page').forEach((node) => {
      node.classList.toggle('active', node.id === 'panel-' + tab);
    });
    if (tab === 'overview') refreshOverview().catch(reportError);
    else if (tab === 'devices') loadDevices().catch(reportError);
    else if (tab === 'policies') loadPolicies().catch(reportError);
    else if (tab === 'accounts') loadAccounts().catch(reportError);
    else if (tab === 'mine') loadMine().catch(reportError);
    else if (tab === 'tokens') loadTokens().catch(reportError);
    else if (tab === 'groups') loadGroups().catch(reportError);
    else if (tab === 'recordings') loadRecordings(true).catch(reportError);
    else if (tab === 'audit') loadAudit(true).catch(reportError);
    else if (tab === 'server') loadServerPanel().catch(reportError);
  }

  // ---------- overview ----------

  async function refreshOverview() {
    const [stats, info] = await Promise.all([api('/v1/stats'), api('/v1/server')]);
    renderStats(stats || {});
    renderServer(info || {});
  }

  function card(label, value, hint) {
    const node = el('div', 'card');
    node.appendChild(el('div', 'label', label));
    node.appendChild(el('div', 'value', value));
    if (hint) node.appendChild(el('div', 'hint', hint));
    return node;
  }

  function renderStats(stats) {
    const relay = stats.relay || {};
    const carrier = relay.shared_carrier || null;
    const pool = stats.relay_nodes || null;
    const nodes = pool && Array.isArray(pool.nodes) ? pool.nodes : [];
    const blocker = stats.registration_blocked || null;
    const punch = stats.punch_requests || null;
    const changes = stats.ip_changes || null;

    const cards = $('stat-cards');
    clear(cards);
    cards.appendChild(card('在线主机', stats.hosts === undefined ? '—' : stats.hosts,
      '身份总数 ' + (stats.identities === undefined ? '—' : stats.identities)));
    cards.appendChild(card('中继节点', pool ? pool.healthy + '/' + nodes.length : '未配置',
      stats.relay_only ? '仅中继模式' : 'P2P 优先'));
    cards.appendChild(card('共享载体查看者', carrier ? carrier.viewers : '—',
      carrier ? '主机 ' + carrier.hosts + ' · 链路 ' + carrier.host_links : '共享载体未启用'));
    cards.appendChild(card('击穿请求', punch ? punch.total : '—',
      punch ? '去重 ' + punch.deduped : '击穿日志未启用'));
    cards.appendChild(card('IP 变更', changes ? changes.changes : '—',
      changes ? '跟踪 ' + changes.tracked + ' 台' : 'IP 变更跟踪未启用'));
    cards.appendChild(card('注册阻断', blocker ? blocker.rate_blocked + blocker.id_blocked : '未启用',
      blocker ? '跟踪 IP ' + blocker.tracked_ips : '滥用阻断未启用'));

    renderRelayNodes(nodes);
    renderRelayPolicy(relay.policy || {});
    renderRelayPorts(relay, carrier);
  }

  function renderRelayNodes(nodes) {
    const box = $('relay-nodes');
    clear(box);
    if (!nodes.length) {
      box.appendChild(el('p', 'muted', '未配置中继节点池。'));
      return;
    }
    nodes.forEach((node) => {
      const row = el('div', 'row');
      const left = el('div', 'stack');
      left.appendChild(el('span', 'mono', node.url || '—'));
      const bits = ['租约 ' + (node.leases || 0), '链接 ' + (node.linked || 0)];
      if (node.rtt_ms) bits.push('RTT ' + node.rtt_ms + ' ms');
      if (node.error) bits.push(node.error);
      left.appendChild(el('span', 'muted', bits.join(' · ')));
      row.appendChild(left);
      row.appendChild(node.healthy ? pill('健康', 'ok')
        : node.checked ? pill('异常', 'bad') : pill('待探测', 'warn'));
      box.appendChild(row);
    });
  }

  function renderRelayPolicy(policy) {
    const dl = $('relay-policy');
    clear(dl);
    const rows = [
      ['单会话带宽', fmtRate(policy.single_bandwidth_bps)],
      ['总带宽', fmtRate(policy.total_bandwidth_bps)],
      ['降级阈值', fmtPercent(policy.downgrade_threshold)],
      ['降级观察', policy.downgrade_after_sec ? fmtSeconds(policy.downgrade_after_sec) : '—'],
      ['拉黑后限速', fmtRate(policy.blacklist_speed_bps)],
      ['黑名单 / 阻名单', (policy.blacklist || 0) + ' / ' + (policy.blocklist || 0)],
      ['活动会话', policy.sessions || 0],
      ['限速丢弃', (policy.rate_dropped_datagrams || 0) + ' 包 / ' + fmtBytes(policy.rate_dropped_bytes || 0)],
      ['阻断 / 降级会话', (policy.blocked_sessions || 0) + ' / ' + (policy.downgraded_sessions || 0)],
    ];
    rows.forEach(([name, value]) => {
      dl.appendChild(el('dt', '', name));
      dl.appendChild(el('dd', '', value));
    });
  }

  function renderRelayPorts(relay, carrier) {
    const body = $('relay-ports');
    clear(body);
    const ports = Array.isArray(relay.ports) ? relay.ports : [];
    ports.forEach((port) => {
      const tr = document.createElement('tr');
      tr.appendChild(cell(port.port));
      tr.appendChild(cell(port.host_id || '—'));
      tr.appendChild(cell(port.viewer || '—'));
      tr.appendChild(cell(fmtBytes(port.to_viewer_bytes)));
      tr.appendChild(cell(fmtBytes(port.to_host_bytes)));
      tr.appendChild(cell(port.tcp_viewers ? port.tcp_viewers + ' 会话 / ' +
        fmtBytes((port.tcp_to_viewer_bytes || 0) + (port.tcp_to_host_bytes || 0)) : '—'));
      tr.appendChild(cell(port.dropped || 0));
      body.appendChild(tr);
    });
    $('relay-ports-empty').classList.toggle('hidden', ports.length > 0);

    const bits = [];
    if (carrier) {
      const addrs = Array.isArray(carrier.addrs) && carrier.addrs.length
        ? carrier.addrs.join(', ') : carrier.addr;
      if (addrs) bits.push('共享载体 ' + addrs);
      bits.push('查看者 ' + (carrier.viewers || 0), '拒绝 ' + (carrier.rejected || 0));
    }
    if (relay.udp_read_buffer_bytes) bits.push('UDP 读缓冲 ' + fmtBytes(relay.udp_read_buffer_bytes));
    bits.push('更新 ' + new Date().toLocaleTimeString('zh-CN', { hour12: false }));
    $('relay-updated').textContent = bits.join(' · ');
  }

  // ---------- server panel ----------

  async function loadServerPanel() {
    renderServer(await api('/v1/server') || {});
  }

  function renderServer(info) {
    state.publicKey = info.public_key || '';

    const dl = $('server-info');
    clear(dl);
    const rows = [
      ['版本', info.version],
      ['服务器公钥', info.public_key],
      ['会合地址', (info.punch || '—') + (info.punch_v6 ? ' / ' + info.punch_v6 : '')],
      ['中继载体', info.carrier || '未启用'],
      ['载体 IPv6', info.carrier_v6 || '未启用'],
      ['TLS', info.tls ? '已启用' : '未启用'],
      ['租约 TTL', fmtSeconds(info.host_ttl_sec)],
      ['relay-only', info.relay_only ? '是' : '否'],
      ['中继服务器', Array.isArray(info.relay_servers) && info.relay_servers.length
        ? info.relay_servers.join(', ') : '—'],
      ['会合 UDP 读缓冲', info.punch_udp_read_buffer ? fmtBytes(info.punch_udp_read_buffer) : '系统默认'],
      ['中继 UDP 读缓冲', info.relay_udp_read_buffer ? fmtBytes(info.relay_udp_read_buffer) : '系统默认'],
      ['账号系统', info.accounts ? '已启用' : '未启用'],
      ['配置序号', info.config_serial || '—'],
    ];
    rows.forEach(([name, value]) => {
      dl.appendChild(el('dt', '', name));
      dl.appendChild(el('dd', '', value === undefined || value === null || value === '' ? '—' : value));
    });

    renderSoftware(info.software || null);
    renderRendezvous(info);
  }

  function renderSoftware(software) {
    const box = $('software-info');
    clear(box);
    if (!software) {
      box.appendChild(el('p', 'muted', '未配置软件更新通告。设置 --software-version/--software-url 后客户端才会收到提示。'));
      return;
    }
    const dl = el('dl', 'kv');
    const rows = [
      ['版本', software.version],
      ['下载地址', software.url],
      ['SHA-256', software.sha256 || '未签名托管'],
      ['大小', software.size ? fmtBytes(software.size) : '—'],
      ['签名', software.signature || '—'],
    ];
    rows.forEach(([name, value]) => {
      dl.appendChild(el('dt', '', name));
      dl.appendChild(el('dd', '', value === undefined || value === null || value === '' ? '—' : value));
    });
    box.appendChild(dl);
  }

  function renderRendezvous(info) {
    const box = $('rendezvous-info');
    clear(box);
    const servers = Array.isArray(info.rendezvous_servers) ? info.rendezvous_servers : [];
    if (!servers.length) {
      box.appendChild(el('p', 'muted', '未配置下发的会合服务器列表。'));
      return;
    }
    const row = el('div', 'row');
    row.appendChild(el('span', 'muted', '配置序号 ' + (info.config_serial || '—')));
    row.appendChild(el('span', '', servers.length + ' 个地址'));
    box.appendChild(row);
    servers.forEach((url) => {
      const item = el('div', 'row');
      item.appendChild(el('span', 'mono', url));
      box.appendChild(item);
    });
  }

  // ---------- devices ----------

  async function loadDevices(silent) {
    if (state.devicesLoading) return;
    state.devicesLoading = true;
    try {
      const params = new URLSearchParams();
      const query = $('device-query').value.trim();
      if (query) params.set('q', query);
      if ($('device-online').checked) params.set('online', '1');
      params.set('limit', String(state.deviceLimit));
      params.set('offset', String(state.deviceOffset));
      const data = await api('/v1/identities?' + params.toString());
      renderDevices(data || {});
    } catch (err) {
      if (!silent) throw err;
    } finally {
      state.devicesLoading = false;
    }
  }

  function renderDevices(data) {
    const peers = Array.isArray(data.identities) ? data.identities : [];
    state.deviceList = peers;
    const known = new Set(peers.map((peer) => peer.id));
    Array.from(state.deviceSelection).forEach((id) => {
      if (!known.has(id)) state.deviceSelection.delete(id);
    });
    const total = data.total === undefined ? peers.length : data.total;
    const online = data.online === undefined ? 0 : data.online;
    state.deviceTotal = total;
    // A deletion or filter change can leave the offset past the last page;
    // fall back once instead of showing an empty table with a positive total.
    if (!peers.length && total > 0 && state.deviceOffset > 0) {
      state.deviceOffset = Math.max(0, Math.floor((total - 1) / state.deviceLimit) * state.deviceLimit);
      loadDevices().catch(reportError);
      return;
    }
    $('device-summary').textContent = '共 ' + total + ' 台，在线 ' + online + ' 台';
    updateDevicePager(total, peers.length);

    const body = $('device-rows');
    clear(body);
    peers.forEach((peer) => body.appendChild(deviceRow(peer)));
    $('device-empty').classList.toggle('hidden', peers.length > 0);
    updateDeviceSelection();
  }

  function updateDevicePager(total, shown) {
    if (total === 0) {
      $('device-page').textContent = '没有设备';
    } else {
      $('device-page').textContent = '第 ' + (state.deviceOffset + 1) + '–' +
        (state.deviceOffset + shown) + ' 台，共 ' + total + ' 台';
    }
    $('device-prev').disabled = state.deviceOffset <= 0;
    $('device-next').disabled = state.deviceOffset + state.deviceLimit >= total;
  }

  function updateDeviceSelection() {
    resetBulkPrompt();
    const count = state.deviceSelection.size;
    $('device-selection').textContent = count ? '已选择 ' + count + ' 台' : '未选择设备';
    const writable = can('operator');
    $('device-bulk-disable').disabled = !writable || count === 0;
    $('device-bulk-enable').disabled = !writable || count === 0;
    $('device-bulk-clear').disabled = count === 0;
    const all = $('device-select-all');
    const visible = state.deviceList.length;
    const selected = state.deviceList.filter((peer) => state.deviceSelection.has(peer.id)).length;
    all.checked = visible > 0 && selected === visible;
    all.indeterminate = selected > 0 && selected < visible;
    all.disabled = !writable || visible === 0;
  }

  function resetBulkPrompt() {
    state.pendingBulk = '';
    clearTimeout(state.bulkTimer);
    $('device-bulk-disable').textContent = '批量禁用';
    $('device-bulk-enable').textContent = '批量启用';
  }

  // The second click confirms: a native confirm() blocks the page, and a bulk
  // disable is destructive enough to want the explicit repeat.
  async function bulkSetDisabled(button, disabled) {
    const ids = Array.from(state.deviceSelection);
    if (!ids.length) return;
    const action = disabled ? '禁用' : '启用';
    if (state.pendingBulk !== (disabled ? 'disable' : 'enable')) {
      resetBulkPrompt();
      state.pendingBulk = disabled ? 'disable' : 'enable';
      button.textContent = '确认' + action + ' ' + ids.length + ' 台';
      state.bulkTimer = setTimeout(() => resetBulkPrompt(), 6000);
      return;
    }
    resetBulkPrompt();
    const result = await api('/v1/identities/bulk', {
      method: 'POST',
      body: { ids, disabled },
    });
    const updated = Array.isArray(result && result.updated) ? result.updated.length : 0;
    const missing = Array.isArray(result && result.missing) ? result.missing.length : 0;
    state.deviceSelection.clear();
    toast('已' + action + ' ' + updated + ' 台设备' + (missing ? '，' + missing + ' 台不存在' : ''));
    await loadDevices();
  }

  function deviceRow(peer) {
    const tr = document.createElement('tr');

    const pick = document.createElement('input');
    pick.type = 'checkbox';
    pick.checked = state.deviceSelection.has(peer.id);
    pick.title = '选择 ' + peer.id;
    pick.addEventListener('change', () => {
      if (pick.checked) {
        state.deviceSelection.add(peer.id);
      } else {
        state.deviceSelection.delete(peer.id);
      }
      updateDeviceSelection();
    });
    const pickCell = cell(pick);
    pickCell.className = 'check-col';
    tr.appendChild(pickCell);

    const id = cell(peer.id);
    id.className = 'mono';
    if (peer.fingerprint) id.title = '指纹 ' + peer.fingerprint;
    tr.appendChild(id);

    tr.appendChild(cell(peer.alias || '—'));
    tr.appendChild(cell(peer.owner || '—'));

    const tags = el('span');
    if (Array.isArray(peer.tags) && peer.tags.length) {
      peer.tags.forEach((tag) => tags.appendChild(el('span', 'tag', tag)));
    } else {
      tags.textContent = '—';
    }
    tr.appendChild(cell(tags));

    tr.appendChild(cell(peer.hostname || '—'));
    tr.appendChild(cell(peer.platform || '—'));
    tr.appendChild(cell(peer.version || '—'));
    tr.appendChild(cell(peer.metadata_signature ? pill('已签名', 'ok') : pill('未签名')));
    if (peer.disabled) {
      tr.appendChild(cell(pill('已禁用', 'bad')));
    } else {
      tr.appendChild(cell(peer.online ? pill('在线', 'ok') : pill('离线')));
    }

    const where = el('div', 'stack');
    if (peer.relay) where.appendChild(el('div', 'mono', '中继 ' + peer.relay));
    if (peer.candidate) where.appendChild(el('div', 'mono', peer.candidate));
    if (!peer.relay && !peer.candidate) where.appendChild(el('span', 'muted', '—'));
    tr.appendChild(cell(where));

    tr.appendChild(cell(fmtAge(peer.last_seen_ms)));

    const actions = el('div', 'actions-wrap');
    if (!can('operator')) {
      tr.appendChild(cell(el('span', 'muted', '只读')));
      return tr;
    }
    const edit = button('编辑', 'small');
    edit.addEventListener('click', () => openDeviceDialog(peer));
    actions.appendChild(edit);

    const toggle = button(peer.disabled ? '启用设备' : '禁用设备',
      'small' + (peer.disabled ? '' : ' danger'));
    toggle.addEventListener('click', () => withButton(toggle, async () => {
      if (!peer.disabled && !window.confirm('禁用设备 ' + peer.id +
        '？\n将立即释放当前租约并拒绝再次注册，直到重新启用。')) return;
      await api('/v1/identities/' + encodeURIComponent(peer.id), {
        method: 'PATCH',
        body: { disabled: !peer.disabled },
      });
      toast((peer.disabled ? '已启用 ' : '已禁用 ') + peer.id);
      await loadDevices();
    }));
    actions.appendChild(toggle);

    const reset = button('重置身份', 'small danger');
    reset.addEventListener('click', () => withButton(reset, async () => {
      if (!window.confirm('重置设备 ' + peer.id + '？\n将删除密钥绑定、别名和标签，并释放当前租约，此操作不可撤销。')) return;
      await api('/v1/identities/' + encodeURIComponent(peer.id), { method: 'DELETE' });
      toast('已重置 ' + peer.id);
      await loadDevices();
    }));
    actions.appendChild(reset);

    if (peer.online) {
      const drop = button('移除租约', 'small');
      drop.addEventListener('click', () => withButton(drop, async () => {
        if (!window.confirm('移除 ' + peer.id + ' 的当前租约？主机需要重新注册才会恢复在线。')) return;
        await api('/v1/hosts/' + encodeURIComponent(peer.id), { method: 'DELETE' });
        toast('已移除租约 ' + peer.id);
        await loadDevices();
      }));
      actions.appendChild(drop);
    }

    tr.appendChild(cell(actions));
    return tr;
  }

  function openDeviceDialog(peer) {
    state.editing = peer;
    $('device-dialog-id').textContent = peer.id + (peer.fingerprint ? ' · ' + peer.fingerprint : '');
    $('device-alias').value = peer.alias || '';
    $('device-owner').value = peer.owner || '';
    $('device-tags').value = Array.isArray(peer.tags) ? peer.tags.join(', ') : '';
    $('device-dialog').showModal();
    $('device-alias').focus();
  }

  async function saveDeviceLabels() {
    const peer = state.editing;
    if (!peer) return;
    const alias = $('device-alias').value.trim();
    const owner = $('device-owner').value.trim();
    const tags = $('device-tags').value.split(',')
      .map((tag) => tag.trim())
      .filter((tag) => tag.length > 0);
    await api('/v1/identities/' + encodeURIComponent(peer.id), {
      method: 'PATCH',
      body: { alias, tags, owner },
    });
    $('device-dialog').close();
    state.editing = null;
    toast('已保存 ' + peer.id);
    await loadDevices();
  }

  // ---------- device policies ----------

  const PERMISSION_LABELS = [
    [0x0001, '输入'],
    [0x0002, '剪贴板'],
    [0x0004, '音频'],
    [0x0008, '文件传输'],
    [0x0010, '麦克风'],
    [0x0020, '隐私模式'],
    [0x0080, '端口转发'],
    [0x0100, '终端'],
    [0x0200, '录制'],
    [0x0400, '摄像头'],
    [0x0800, '重启'],
    [0x1000, '阻断输入'],
    [0x2000, '系统登录'],
    [0x4000, '会话切换'],
    [0x8000, '远程配置'],
  ];

  function fmtCeiling(mask) {
    const value = Number(mask) >>> 0;
    const names = PERMISSION_LABELS.filter(([bit]) => (value & bit) !== 0).map(([, name]) => name);
    const known = PERMISSION_LABELS.reduce((bits, [bit]) => bits | bit, 0);
    const unknown = value & ~known;
    if (unknown) names.push('未知位 0x' + unknown.toString(16).toUpperCase());
    return names.length ? names.join('、') : '不限';
  }

  function ceilingCell(mask) {
    const value = Number(mask) >>> 0;
    if (!value) return '不限';
    const node = el('span', '', fmtCeiling(value));
    node.title = '0x' + value.toString(16).toUpperCase();
    return node;
  }

  // receiptState reports what the console knows about a host applying the
  // current policy. "已应用" needs a signed receipt whose version matches the
  // policy; an older success or a failure never counts as the current version.
  function receiptState(policy) {
    const receipt = policy.receipt;
    if (!receipt) {
      return { label: '待应用', kind: 'warn', detail: '主机尚未上报该策略。' };
    }
    if (receipt.policy_version !== policy.policy_version) {
      return {
        label: '待应用',
        kind: 'warn',
        detail: '主机上报版本 v' + receipt.policy_version + '（' +
          (receipt.applied ? '已应用' : '应用失败') + '），当前版本 v' + policy.policy_version +
          ' 尚未确认。' + (receipt.detail ? ' ' + receipt.detail : ''),
      };
    }
    if (!receipt.applied) {
      return { label: '应用失败', kind: 'bad', detail: receipt.detail || '主机未能应用该版本。' };
    }
    return {
      label: '已应用版本 ' + receipt.policy_version,
      kind: 'ok',
      detail: (receipt.detail ? receipt.detail + ' · ' : '') + '上报于 ' + fmtTime(receipt.reported_at_ms),
    };
  }

  async function loadPolicies(silent) {
    if (state.policiesLoading) return;
    state.policiesLoading = true;
    try {
      const [devices, objects, assignments] = await Promise.all([
        api('/v1/device-policies'),
        api('/v1/policies'),
        api('/v1/policy-assignments'),
      ]);
      state.policies = Array.isArray(devices && devices.policies) ? devices.policies : [];
      state.policyObjects = Array.isArray(objects && objects.policies) ? objects.policies : [];
      state.policyAssignments = Array.isArray(assignments && assignments.assignments)
        ? assignments.assignments : [];
      state.policiesLoaded = true;
      renderPolicies();
    } catch (err) {
      if (err && err.status === 503) {
        state.policies = [];
        state.policyObjects = [];
        state.policyAssignments = [];
        state.policiesLoaded = true;
        renderPolicies();
        $('policy-summary').textContent = '服务端未启用账号库（缺少 --accounts-db），设备策略不可用。';
        return;
      }
      if (!silent) throw err;
    } finally {
      state.policiesLoading = false;
    }
  }

  function renderPolicies() {
    const objectBody = $('policy-object-rows');
    clear(objectBody);
    state.policyObjects.forEach((policy) => objectBody.appendChild(policyObjectRow(policy)));
    $('policy-object-empty').classList.toggle('hidden', state.policyObjects.length > 0);
    $('policy-object-readonly').classList.toggle('hidden', can('admin'));

    const assignmentBody = $('policy-assignment-rows');
    clear(assignmentBody);
    state.policyAssignments.forEach((assignment) =>
      assignmentBody.appendChild(policyAssignmentRow(assignment)));
    $('policy-assignment-empty').classList.toggle('hidden', state.policyAssignments.length > 0);

    const body = $('policy-rows');
    clear(body);
    state.policies.forEach((policy) => body.appendChild(policyRow(policy)));
    $('policy-empty').classList.toggle('hidden', state.policies.length > 0);
    $('policy-readonly').classList.toggle('hidden', can('admin'));
    const applied = state.policies.filter((policy) => {
      const receipt = policy.receipt;
      return Boolean(receipt && receipt.applied && receipt.policy_version === policy.policy_version);
    }).length;
    $('policy-summary').textContent = '生效设备 ' + state.policies.length +
      ' 台；策略对象 ' + state.policyObjects.length + ' 个，分配 ' +
      state.policyAssignments.length + ' 条；主机已确认当前版本 ' + applied + ' 台。';
  }

  function policyObjectRow(policy) {
    const tr = document.createElement('tr');
    tr.appendChild(cell(policy.name));
    tr.appendChild(cell(pill(policy.mode === 'restricted' ? '受限' : '开放',
      policy.mode === 'restricted' ? 'warn' : 'ok')));
    tr.appendChild(cell(ceilingCell(policy.permission_ceiling)));

    const allow = (list, placeholder) => {
      if (policy.mode !== 'restricted') return '不适用';
      return Array.isArray(list) && list.length ? list.join(', ') : placeholder;
    };
    tr.appendChild(cell(allow(policy.allowed_users, '无（仅分组）')));
    tr.appendChild(cell(allow(policy.allowed_groups, '无（仅用户）')));
    tr.appendChild(cell('v' + policy.policy_version));
    tr.appendChild(cell(fmtTime(policy.updated_at_ms)));

    const actions = el('div', 'actions-wrap');
    const edit = button('编辑', 'small');
    edit.addEventListener('click', () => openPolicyObjectDialog(policy));
    actions.appendChild(edit);
    tr.appendChild(cell(actions));
    return tr;
  }

  function policyScopeText(assignment) {
    if (assignment.scope_kind === 'all') return '全部设备';
    if (assignment.scope_kind === 'group') return '分组 · ' + assignment.scope_value;
    return '设备 · ' + assignment.scope_value;
  }

  function policyAssignmentRow(assignment) {
    const tr = document.createElement('tr');
    tr.appendChild(cell(assignment.policy_name || assignment.policy_id));
    tr.appendChild(cell(policyScopeText(assignment)));
    tr.appendChild(cell(String(assignment.priority)));
    tr.appendChild(cell(fmtTime(assignment.created_at_ms)));

    const actions = el('div', 'actions-wrap');
    const remove = button('删除', 'small danger');
    remove.addEventListener('click', (event) =>
      withButton(event.currentTarget, () => deletePolicyAssignment(assignment)));
    actions.appendChild(remove);
    tr.appendChild(cell(actions));
    return tr;
  }

  function sourceCell(policy) {
    const source = policy.source;
    if (!source) return el('span', 'muted', '手动覆盖');
    const node = el('span', '', '策略 ' + source.name + ' · v' + source.version);
    node.title = '来源 ' + source.kind + ' ' + source.id + ' · 优先级 ' + source.priority;
    return node;
  }

  function policyRow(policy) {
    const tr = document.createElement('tr');
    const peer = cell(policy.peer_id);
    peer.className = 'mono';
    tr.appendChild(peer);
    tr.appendChild(cell(pill(policy.mode === 'restricted' ? '受限' : '受管开放',
      policy.mode === 'restricted' ? 'warn' : 'ok')));
    tr.appendChild(cell(ceilingCell(policy.permission_ceiling)));

    const allow = (list, placeholder) => {
      if (policy.mode !== 'restricted') return '不适用';
      return Array.isArray(list) && list.length ? list.join(', ') : placeholder;
    };
    tr.appendChild(cell(allow(policy.allowed_users, '无（仅分组）')));
    tr.appendChild(cell(allow(policy.allowed_groups, '无（仅用户）')));
    tr.appendChild(cell('v' + policy.policy_version));
    tr.appendChild(cell(sourceCell(policy)));

    const receipt = receiptState(policy);
    const receiptBox = el('div', 'stack');
    const badge = pill(receipt.label, receipt.kind);
    badge.title = receipt.detail;
    receiptBox.appendChild(badge);
    receiptBox.appendChild(el('span', 'muted', receipt.detail));
    const receiptCell = document.createElement('td');
    receiptCell.className = 'wrap';
    receiptCell.appendChild(receiptBox);
    tr.appendChild(receiptCell);

    tr.appendChild(cell(fmtTime(policy.updated_at_ms)));

    const actions = el('div', 'actions-wrap');
    const edit = button('编辑', 'small');
    edit.addEventListener('click', () => openPolicyDialog(policy));
    actions.appendChild(edit);
    tr.appendChild(cell(actions));
    return tr;
  }

  function splitNames(value) {
    return value.split(',').map((item) => item.trim()).filter((item) => item.length > 0);
  }

  function applyPolicyMode(modeEl, usersEl, groupsEl, hintEl, note) {
    const restricted = modeEl.value === 'restricted';
    usersEl.disabled = !restricted;
    groupsEl.disabled = !restricted;
    hintEl.textContent = (restricted
      ? '只有下列用户与分组能获得接入票据；两列都为空时无人可连。'
      : 'open 允许所有未被封禁的账号获得接入票据，允许列表不生效。') + note;
  }

  function syncPolicyMode() {
    applyPolicyMode($('policy-mode'), $('policy-users'), $('policy-groups'),
      $('policy-mode-hint'), '当前主机版本对所有新会话失败关闭。');
  }

  function syncPolicyObjectMode() {
    applyPolicyMode($('policy-object-mode'), $('policy-object-users'), $('policy-object-groups'),
      $('policy-object-mode-hint'), '对象生效后仍受设备本机设置收紧。');
  }

  function applyPolicyCeiling(inputEl, hintEl) {
    const value = Number(inputEl.value || 0);
    if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
      hintEl.textContent = '请输入 0..4294967295 之间的整数。';
      return;
    }
    hintEl.textContent = value === 0
      ? '0 表示不限制权限。'
      : '上限包含：' + fmtCeiling(value);
  }

  function syncPolicyCeiling() {
    applyPolicyCeiling($('policy-ceiling'), $('policy-ceiling-hint'));
  }

  function syncPolicyObjectCeiling() {
    applyPolicyCeiling($('policy-object-ceiling'), $('policy-object-ceiling-hint'));
  }

  function openPolicyDialog(policy) {
    state.policyEditing = policy || null;
    const editing = Boolean(policy);
    $('policy-dialog-title').textContent = editing ? '设备策略 · ' + policy.peer_id : '新建设备策略';
    const peer = $('policy-peer');
    peer.value = editing ? policy.peer_id : '';
    peer.readOnly = editing;
    $('policy-mode').value = editing ? policy.mode : 'open';
    $('policy-ceiling').value = editing ? String(policy.permission_ceiling) : '0';
    $('policy-users').value = editing && Array.isArray(policy.allowed_users)
      ? policy.allowed_users.join(', ') : '';
    $('policy-groups').value = editing && Array.isArray(policy.allowed_groups)
      ? policy.allowed_groups.join(', ') : '';
    $('policy-delete').classList.toggle('hidden', !editing);
    if (editing) {
      const current = receiptState(policy);
      $('policy-dialog-receipt').textContent = '当前版本 v' + policy.policy_version + '：' +
        current.label + '。' + current.detail;
    } else {
      $('policy-dialog-receipt').textContent =
        '保存后主机在下次注册时收到策略；在签名回执确认相同版本前，控制台显示“待应用”。';
    }
    syncPolicyMode();
    syncPolicyCeiling();
    $('policy-dialog').showModal();
    if (!editing) peer.focus();
  }

  async function savePolicy() {
    const peer = $('policy-peer').value.trim();
    if (!peer) {
      toast('请填写设备 ID', true);
      return;
    }
    const ceiling = Number($('policy-ceiling').value || 0);
    if (!Number.isInteger(ceiling) || ceiling < 0 || ceiling > 0xffffffff) {
      toast('权限上限必须是 0..4294967295 的整数', true);
      return;
    }
    const submit = $('policy-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      const reply = await api('/v1/device-policies/' + encodeURIComponent(peer), {
        method: 'PUT',
        body: {
          mode: $('policy-mode').value,
          permission_ceiling: ceiling,
          allowed_users: splitNames($('policy-users').value),
          allowed_groups: splitNames($('policy-groups').value),
        },
      });
      const version = reply && reply.policy ? reply.policy.policy_version : 0;
      $('policy-dialog').close();
      state.policyEditing = null;
      toast('已保存 ' + peer + (version ? '（版本 v' + version + '，待主机确认）' : ''));
      await loadPolicies();
    });
  }

  async function deletePolicy() {
    const policy = state.policyEditing;
    if (!policy) return;
    if (!window.confirm('删除设备 ' + policy.peer_id + ' 的策略？\n设备会回到未受管状态或回落到匹配的分配；此操作会写入审计。')) return;
    await api('/v1/device-policies/' + encodeURIComponent(policy.peer_id), { method: 'DELETE' });
    $('policy-dialog').close();
    state.policyEditing = null;
    toast('已删除策略 ' + policy.peer_id);
    await loadPolicies();
  }

  // ---------- policy objects and assignments ----------

  function openPolicyObjectDialog(policy) {
    state.policyObjectEditing = policy || null;
    const editing = Boolean(policy);
    $('policy-object-dialog-title').textContent = editing
      ? '策略对象 · ' + policy.name : '新建策略对象';
    $('policy-object-name').value = editing ? policy.name : '';
    $('policy-object-mode').value = editing ? policy.mode : 'open';
    $('policy-object-ceiling').value = editing ? String(policy.permission_ceiling) : '0';
    $('policy-object-users').value = editing && Array.isArray(policy.allowed_users)
      ? policy.allowed_users.join(', ') : '';
    $('policy-object-groups').value = editing && Array.isArray(policy.allowed_groups)
      ? policy.allowed_groups.join(', ') : '';
    $('policy-object-delete').classList.toggle('hidden', !editing);
    $('policy-object-dialog-note').textContent = editing
      ? '当前版本 v' + policy.policy_version +
        '。保存会提升对象版本并同步所有覆盖设备；删除前必须先删除它的分配。'
      : '保存后可在“策略分配”中把对象绑定到全部设备、分组或单台设备。';
    syncPolicyObjectMode();
    syncPolicyObjectCeiling();
    $('policy-object-dialog').showModal();
    $('policy-object-name').focus();
  }

  async function savePolicyObject() {
    const name = $('policy-object-name').value.trim();
    if (!name) {
      toast('请填写策略对象名称', true);
      return;
    }
    const ceiling = Number($('policy-object-ceiling').value || 0);
    if (!Number.isInteger(ceiling) || ceiling < 0 || ceiling > 0xffffffff) {
      toast('权限上限必须是 0..4294967295 的整数', true);
      return;
    }
    const editing = state.policyObjectEditing;
    const submit = $('policy-object-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      const reply = await api(
        editing ? '/v1/policies/' + encodeURIComponent(editing.id) : '/v1/policies',
        {
          method: editing ? 'PUT' : 'POST',
          body: {
            name,
            mode: $('policy-object-mode').value,
            permission_ceiling: ceiling,
            allowed_users: splitNames($('policy-object-users').value),
            allowed_groups: splitNames($('policy-object-groups').value),
          },
        },
      );
      const version = reply && reply.policy_version ? reply.policy_version : 0;
      $('policy-object-dialog').close();
      state.policyObjectEditing = null;
      toast((editing ? '已更新 ' : '已创建 ') + name +
        (version ? '（v' + version + '）' : ''));
      await loadPolicies();
    });
  }

  async function deletePolicyObject() {
    const policy = state.policyObjectEditing;
    if (!policy) return;
    if (!window.confirm('删除策略对象 ' + policy.name +
      '？\n存在分配时服务器会拒绝删除；请先删除相关分配。')) return;
    await api('/v1/policies/' + encodeURIComponent(policy.id), { method: 'DELETE' });
    $('policy-object-dialog').close();
    state.policyObjectEditing = null;
    toast('已删除策略对象 ' + policy.name);
    await loadPolicies();
  }

  function fillPolicyGroupOptions() {
    const list = $('policy-assignment-groups');
    clear(list);
    state.groups.forEach((group) => {
      const option = document.createElement('option');
      option.value = group.id;
      if (group.name) option.label = group.name;
      list.appendChild(option);
    });
  }

  const POLICY_ASSIGNMENT_BATCH_LIMIT = 200;

  // splitDeviceIDs accepts the separators an operator is likely to paste from
  // a spreadsheet and drops duplicates so one pasted row cannot create two
  // assignments for the same device.
  function splitDeviceIDs(text) {
    const ids = String(text || '').split(/[\s,，、]+/).map((id) => id.trim()).filter(Boolean);
    return Array.from(new Set(ids));
  }

  async function openPolicyAssignmentDialog() {
    if (!state.policyObjects.length) {
      toast('请先创建策略对象', true);
      return;
    }
    if (!state.groupsLoaded) {
      try {
        await loadGroups(true);
      } catch {
        /* free-text group ids still work without the picker */
      }
    }
    fillPolicyGroupOptions();
    const select = $('policy-assignment-policy');
    clear(select);
    state.policyObjects.forEach((policy) => {
      const option = document.createElement('option');
      option.value = policy.id;
      option.textContent = policy.name + '（v' + policy.policy_version + '）';
      select.appendChild(option);
    });
    $('policy-assignment-scope').value = 'all';
    $('policy-assignment-value').value = '';
    $('policy-assignment-priority').value = '0';
    syncPolicyAssignmentScope();
    $('policy-assignment-dialog').showModal();
  }

  function syncPolicyAssignmentScope() {
    const kind = $('policy-assignment-scope').value;
    const input = $('policy-assignment-value');
    const label = $('policy-assignment-value-text');
    const hint = $('policy-assignment-hint');
    if (kind === 'all') {
      input.value = '';
      input.disabled = true;
      input.removeAttribute('list');
      input.placeholder = '';
      label.textContent = '范围值（全部设备不需要填写）';
      hint.textContent = '全部设备是兜底范围；分组与设备分配优先于它，优先级相同时也不会覆盖更具体的范围。';
      return;
    }
    input.disabled = false;
    if (kind === 'group') {
      input.setAttribute('list', 'policy-assignment-groups');
      input.placeholder = 'lab-a';
      label.textContent = '设备分组 ID';
      hint.textContent = '组内设备会匹配该分配；分组在“分组”页签维护。';
    } else {
      input.removeAttribute('list');
      input.placeholder = 'devbox, lab-pc-2';
      label.textContent = '设备 ID（可填多个）';
      hint.textContent = '多台设备用逗号、空格或换行分隔，每台生成一条分配；已有设备分配的会跳过并提示，一次最多 ' +
        POLICY_ASSIGNMENT_BATCH_LIMIT + ' 台。手动设置的设备策略仍然优先。';
    }
  }

  async function savePolicyAssignment() {
    const policyID = $('policy-assignment-policy').value;
    if (!policyID) {
      toast('请选择策略对象', true);
      return;
    }
    const kind = $('policy-assignment-scope').value;
    const values = kind === 'device'
      ? splitDeviceIDs($('policy-assignment-value').value)
      : [kind === 'all' ? '' : $('policy-assignment-value').value.trim()];
    if (kind !== 'all' && !values[0]) {
      toast('请填写范围值', true);
      return;
    }
    if (kind === 'device' && values.length > POLICY_ASSIGNMENT_BATCH_LIMIT) {
      toast('一次最多为 ' + POLICY_ASSIGNMENT_BATCH_LIMIT + ' 台设备创建分配', true);
      return;
    }
    const priority = Number($('policy-assignment-priority').value || 0);
    if (!Number.isInteger(priority) || priority < -10000 || priority > 10000) {
      toast('优先级必须是 -10000..10000 的整数', true);
      return;
    }
    const submit = $('policy-assignment-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      if (kind === 'device') {
        // One transactional call for the whole pasted list; devices that
        // already hold an assignment come back in skipped instead of failing
        // the batch. The server keeps the same 200-device cap as the form.
        const result = await api('/v1/policy-assignments/bulk', {
          method: 'POST',
          body: { policy_id: policyID, devices: values, priority },
        });
        const created = (result && result.created) || [];
        const skipped = (result && result.skipped) || [];
        if (created.length > 0) $('policy-assignment-dialog').close();
        if (!skipped.length) {
          toast(created.length > 1
            ? '已为 ' + created.length + ' 台设备创建策略分配'
            : '已创建设备策略分配');
        } else {
          const shown = skipped.slice(0, 3).join('、') + (skipped.length > 3 ? ' 等' : '');
          toast(created.length > 0
            ? '创建 ' + created.length + ' 条、跳过 ' + skipped.length + ' 条：' + shown
            : '这些设备已有分配：' + shown, true);
        }
        if (created.length > 0) await loadPolicies();
        return;
      }
      await api('/v1/policy-assignments', {
        method: 'POST',
        body: { policy_id: policyID, scope_kind: kind, scope_value: values[0], priority },
      });
      $('policy-assignment-dialog').close();
      toast('已创建策略分配');
      await loadPolicies();
    });
  }

  async function deletePolicyAssignment(assignment) {
    if (!window.confirm('删除分配：' + assignment.policy_name + ' → ' +
      policyScopeText(assignment) + '？\n被覆盖设备会重新解析策略或回到未受管状态。')) return;
    await api('/v1/policy-assignments/' + encodeURIComponent(assignment.id), { method: 'DELETE' });
    toast('已删除分配');
    await loadPolicies();
  }

  // ---------- accounts ----------

  async function loadAccounts() {
    const params = new URLSearchParams();
    const query = ($('account-query').value || '').trim();
    if (query) params.set('q', query);
    if ($('account-role-filter').value) params.set('role', $('account-role-filter').value);
    params.set('limit', String(state.accountLimit));
    params.set('offset', String(state.accountOffset));
    const data = await api('/v1/accounts?' + params.toString());
    renderAccounts(Array.isArray(data && data.users) ? data.users : [],
      data && data.total === undefined ? null : data.total);
  }

  function renderAccounts(users, total) {
    const body = $('account-rows');
    clear(body);
    users.forEach((user) => body.appendChild(accountRow(user)));
    $('account-empty').classList.toggle('hidden', users.length > 0);
    if (total === null) total = users.length;
    // A deleted account or a narrower filter can leave the offset past the
    // last page; fall back once instead of showing an empty page.
    if (!users.length && total > 0 && state.accountOffset > 0) {
      state.accountOffset = Math.max(0, Math.floor((total - 1) / state.accountLimit) * state.accountLimit);
      loadAccounts().catch(reportError);
      return;
    }
    if (total === 0) {
      $('account-page').textContent = '没有账号';
    } else {
      $('account-page').textContent = '第 ' + (state.accountOffset + 1) + '–' +
        (state.accountOffset + users.length) + ' 个，共 ' + total + ' 个';
    }
    $('account-prev').disabled = state.accountOffset <= 0;
    $('account-next').disabled = state.accountOffset + state.accountLimit >= total;
  }

  async function exportAccounts() {
    const params = new URLSearchParams();
    const query = ($('account-query').value || '').trim();
    if (query) params.set('q', query);
    if ($('account-role-filter').value) params.set('role', $('account-role-filter').value);
    params.set('format', 'csv');
    await downloadCSV('/v1/accounts?' + params.toString(),
      'chast-accounts-' + csvStamp() + '.csv', '已导出账号 CSV');
  }

  function accountRow(user) {
    const tr = document.createElement('tr');
    const name = cell(user.username);
    name.className = 'mono';
    tr.appendChild(name);

    const select = document.createElement('select');
    ['user', 'readonly', 'operator', 'admin'].forEach((role) => {
      const option = document.createElement('option');
      option.value = role;
      option.textContent = role;
      if (user.role === role) option.selected = true;
      select.appendChild(option);
    });
    select.addEventListener('change', () => withButton(select, async () => {
      try {
        await api('/v1/accounts/' + encodeURIComponent(user.username), {
          method: 'PATCH',
          body: { role: select.value },
        });
        toast('已将 ' + user.username + ' 设为 ' + select.value);
      } finally {
        await loadAccounts();
      }
    }));
    tr.appendChild(cell(select));

    tr.appendChild(cell(fmtTime(user.created_at_ms)));
    tr.appendChild(cell(user.banned ? pill('已封禁', 'bad') : pill('正常', 'ok')));
    tr.appendChild(cell(ceilingCell(user.permission_ceiling || 0)));
    const contact = [user.email, user.phone].filter(Boolean).join(' / ') || '—';
    tr.appendChild(cell(contact));

    const actions = el('div', 'actions-wrap');

    const recovery = button('找回方式', 'small');
    recovery.addEventListener('click', () => openAccountRecoveryDialog(user));
    actions.appendChild(recovery);

    const permissions = button('会话权限', 'small');
    permissions.addEventListener('click', () => openAccountCapDialog(user));
    actions.appendChild(permissions);

    const sessions = button('登录会话', 'small');
    sessions.addEventListener('click', () => openAccountSessionsDialog(user));
    actions.appendChild(sessions);

    const ban = button(user.banned ? '解封' : '封禁', 'small' + (user.banned ? '' : ' danger'));
    ban.addEventListener('click', () => withButton(ban, async () => {
      if (!user.banned && !window.confirm('封禁账号 ' + user.username + '？现有会话会被吊销。')) return;
      await api('/v1/accounts/' + encodeURIComponent(user.username) + '/ban', {
        method: user.banned ? 'DELETE' : 'POST',
      });
      toast((user.banned ? '已解封 ' : '已封禁 ') + user.username);
      await loadAccounts();
    }));
    actions.appendChild(ban);

    const password = button('重置密码', 'small');
    password.addEventListener('click', () => withButton(password, async () => {
      const value = window.prompt('为 ' + user.username + ' 设置新密码（留空自动生成）：');
      if (value === null) return;
      const reply = await api('/v1/accounts/' + encodeURIComponent(user.username) + '/password', {
        method: 'POST',
        body: { password: value },
      });
      if (reply && reply.password) {
        window.prompt('已生成一次性密码，请立即记录：', reply.password);
      } else {
        toast('密码已重置');
      }
      await loadAccounts();
    }));
    actions.appendChild(password);

    const twoFactor = button('重置 2FA', 'small');
    twoFactor.addEventListener('click', () => withButton(twoFactor, async () => {
      if (!window.confirm('重置 ' + user.username + ' 的两步验证与恢复码？')) return;
      await api('/v1/accounts/' + encodeURIComponent(user.username) + '/2fa/reset', { method: 'POST' });
      toast('已重置 2FA ' + user.username);
      await loadAccounts();
    }));
    actions.appendChild(twoFactor);

    const remove = button('删除', 'small danger');
    remove.addEventListener('click', () => withButton(remove, async () => {
      if (!window.confirm('删除账号 ' + user.username + '？该账号的会话与地址簿数据会随之移除。')) return;
      await api('/v1/accounts/' + encodeURIComponent(user.username), { method: 'DELETE' });
      toast('已删除 ' + user.username);
      await loadAccounts();
    }));
    actions.appendChild(remove);

    tr.appendChild(cell(actions));
    return tr;
  }

  async function createAccount() {
    const username = $('account-username').value.trim();
    const password = $('account-password').value;
    const role = $('account-role').value;
    const submit = $('account-create').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      const reply = await api('/v1/accounts', {
        method: 'POST',
        body: { username, password, role },
      });
      $('account-username').value = '';
      $('account-password').value = '';
      if (reply && reply.password) {
        window.prompt('账号已创建，一次性密码：', reply.password);
      } else {
        toast('账号已创建');
      }
      await loadAccounts();
    });
  }

  // openAccountCapDialog edits the account-wide session ceiling. It intersects
  // with each device policy, so the effective mask can only get narrower.
  function openAccountCapDialog(user) {
    state.accountCapUser = user;
    $('account-cap-title').textContent = '会话权限 · ' + user.username;
    $('account-cap-note').textContent =
      '该账号在所有设备上的新会话都受此上限约束；设备策略更严格时取交集。' +
      '已签发的接入票据最长 60 秒后失效。';
    $('account-cap-ceiling').value = String(user.permission_ceiling || 0);
    applyPolicyCeiling($('account-cap-ceiling'), $('account-cap-hint'));
    $('account-cap-dialog').showModal();
    $('account-cap-ceiling').focus();
  }

  async function saveAccountCap() {
    const user = state.accountCapUser;
    if (!user) return;
    const ceiling = Number($('account-cap-ceiling').value || 0);
    if (!Number.isInteger(ceiling) || ceiling < 0 || ceiling > 0xffffffff) {
      toast('权限上限必须是 0..4294967295 的整数', true);
      return;
    }
    const submit = $('account-cap-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      await api('/v1/accounts/' + encodeURIComponent(user.username) + '/permissions', {
        method: 'POST',
        body: { permission_ceiling: ceiling },
      });
      $('account-cap-dialog').close();
      toast('已更新 ' + user.username + ' 的会话权限');
      await loadAccounts();
    });
  }

  // openAccountSessionsDialog shows every live login of one account so an
  // operator can cut a stale device without banning the account. Revoking a
  // session deletes its access and refresh token; the device signs in again
  // on its next unlock unless the account is banned.
  function openAccountSessionsDialog(user) {
    state.accountSessionsUser = user;
    $('account-sessions-title').textContent = '登录会话 · ' + user.username;
    $('account-sessions-note').textContent =
      '这里列出该账号当前仍然有效的登录（管理台与其他设备）。撤销后该设备的令牌立即失效；' +
      '封禁账号会让全部会话一次性失效。';
    clear($('account-session-rows'));
    $('account-session-empty').classList.add('hidden');
    $('account-sessions-revoke-all').disabled = true;
    $('account-sessions-dialog').showModal();
    loadAccountSessions().catch(reportError);
  }

  async function loadAccountSessions() {
    const user = state.accountSessionsUser;
    if (!user) return [];
    const data = await api('/v1/accounts/' + encodeURIComponent(user.username) + '/sessions');
    const sessions = data && Array.isArray(data.sessions) ? data.sessions : [];
    const body = $('account-session-rows');
    clear(body);
    sessions.forEach((session) => body.appendChild(adminSessionRow(user, session)));
    $('account-session-empty').classList.toggle('hidden', sessions.length > 0);
    $('account-sessions-revoke-all').disabled = sessions.length === 0;
    return sessions;
  }

  function adminSessionRow(user, session) {
    const tr = document.createElement('tr');
    tr.appendChild(cell(session.device_name || session.device_id || '未知设备'));
    tr.appendChild(cell(session.login_ip || '—'));
    tr.appendChild(cell(session.location || '—'));
    tr.appendChild(cell(fmtTime(session.created_at_ms)));
    tr.appendChild(cell(fmtTime(session.expires_at_ms)));
    tr.appendChild(cell(session.current ? pill('当前会话', 'ok') : pill('有效', '')));

    const actions = el('div', 'actions-wrap');
    const revoke = button('撤销', 'small danger');
    revoke.disabled = Boolean(session.current);
    revoke.addEventListener('click', () => withButton(revoke, async () => {
      await api('/v1/accounts/' + encodeURIComponent(user.username) + '/sessions/' +
        encodeURIComponent(session.id), { method: 'DELETE' });
      toast('已撤销该设备的登录会话');
      await loadAccountSessions();
    }));
    actions.appendChild(revoke);
    tr.appendChild(cell(actions));
    return tr;
  }

  // revokeAllAccountSessions signs the account out everywhere. When the
  // operator is viewing their own account the server keeps the calling
  // session, so the console does not lock itself out.
  async function revokeAllAccountSessions(event) {
    const user = state.accountSessionsUser;
    if (!user) return;
    await withButton(event.currentTarget, async () => {
      const reply = await api('/v1/accounts/' + encodeURIComponent(user.username) +
        '/sessions/revoke-all', { method: 'POST' });
      const revoked = (reply && reply.revoked) || 0;
      toast(revoked > 0 ? '已退出 ' + revoked + ' 个登录会话' : '没有需要退出的会话');
      await loadAccountSessions();
    });
  }

  // openAccountRecoveryDialog repairs an account's recovery address or texts
  // a reset code without the user's password. The server audits every change
  // under the operator's name.
  function openAccountRecoveryDialog(user) {
    state.accountRecoveryUser = user;
    $('account-recovery-title').textContent = '找回方式 · ' + user.username;
    $('account-recovery-note').textContent =
      '清空输入即移除该找回方式。发送重置短信使用已保存的手机号，' +
      '要求账号登记了手机且服务器配置了短信网关；先保存再发送。';
    $('account-recovery-email').value = user.email || '';
    $('account-recovery-phone').value = user.phone || '';
    $('account-recovery-email-send').disabled = !user.email;
    $('account-recovery-sms').disabled = !user.phone;
    $('account-recovery-dialog').showModal();
  }

  async function saveAccountRecovery() {
    const user = state.accountRecoveryUser;
    if (!user) return;
    const email = $('account-recovery-email').value.trim();
    const phone = $('account-recovery-phone').value.trim();
    const submit = $('account-recovery-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      const changed = [];
      if (email !== (user.email || '')) {
        await api('/v1/accounts/' + encodeURIComponent(user.username) + '/email', {
          method: 'POST',
          body: { email },
        });
        changed.push('邮箱');
      }
      if (phone !== (user.phone || '')) {
        await api('/v1/accounts/' + encodeURIComponent(user.username) + '/phone', {
          method: 'POST',
          body: { phone },
        });
        changed.push('手机');
      }
      $('account-recovery-dialog').close();
      toast(changed.length
        ? '已更新 ' + user.username + ' 的找回' + changed.join('与')
        : '没有改变');
      await loadAccounts();
    });
  }

  async function sendAccountResetEmail() {
    const user = state.accountRecoveryUser;
    if (!user) return;
    try {
      await api('/v1/accounts/' + encodeURIComponent(user.username) + '/recover', { method: 'POST' });
    } catch (err) {
      if (err.status === 503) throw new ApiError('服务器未配置邮件发送，无法发送重置邮件。', 503);
      if (err.status === 400) throw new ApiError('该账号还没有找回邮箱，请先保存邮箱。', 400);
      if (err.status === 429) throw new ApiError('重置邮件请求过于频繁，请稍后再试。', 429);
      if (err.status === 502) throw new ApiError('重置邮件发送失败，请稍后再试。', 502);
      throw err;
    }
    toast('重置邮件已发送到 ' + user.username + ' 的找回邮箱');
  }

  async function sendAccountResetSMS() {
    const user = state.accountRecoveryUser;
    if (!user) return;
    try {
      await api('/v1/accounts/' + encodeURIComponent(user.username) + '/sms', { method: 'POST' });
    } catch (err) {
      if (err.status === 503) throw new ApiError('服务器未配置短信网关，无法发送重置短信。', 503);
      if (err.status === 400) throw new ApiError('该账号还没有找回手机，请先保存手机号。', 400);
      if (err.status === 502) throw new ApiError('短信发送失败，请稍后再试。', 502);
      throw err;
    }
    toast('重置短信已发送到 ' + user.username + ' 的找回手机');
  }

  // ---------- API tokens ----------

  const TOKEN_RESOURCE_LABELS = {
    devices: '设备',
    recordings: '录制',
    audit: '审计',
    groups: '分组',
    accounts: '账号',
    ops: '运维',
    all: '全部',
  };
  const GROUP_SCOPED_RESOURCES = new Set(['devices', 'recordings', 'audit', 'groups']);

  function tokenExpired(token) {
    const expires = Number(token.expires_at_ms);
    return Number.isFinite(expires) && expires > 0 && expires <= Date.now();
  }

  function tokenResources() {
    return Array.from(document.querySelectorAll('#token-resources input[type="checkbox"]:checked'))
      .map((node) => node.value);
  }

  function syncTokenScope() {
    const group = $('token-group').value;
    document.querySelectorAll('#token-resources input[type="checkbox"]').forEach((node) => {
      node.disabled = Boolean(group) && !GROUP_SCOPED_RESOURCES.has(node.value);
      if (node.disabled) node.checked = false;
    });
    $('token-resource-note').textContent = group
      ? '已选择分组：设备、录制与审计按该分组成员/设备过滤；账号、运维与全部资源不可用于分组令牌。'
      : '';
  }

  function renderTokenGroups() {
    const select = $('token-group');
    const current = select.value;
    clear(select);
    const none = document.createElement('option');
    none.value = '';
    none.textContent = '（不限定分组）';
    select.appendChild(none);
    state.groups.forEach((group) => {
      const option = document.createElement('option');
      option.value = group.id;
      option.textContent = group.id + ' · ' + (group.name || group.id);
      select.appendChild(option);
    });
    if (state.groups.some((group) => group.id === current)) select.value = current;
    syncTokenScope();
  }

  async function loadTokens(silent) {
    if (!state.auth || state.auth.mode !== 'account') {
      state.tokens = [];
      renderTokens();
      return state.tokens;
    }
    if (state.tokensLoading) return state.tokens;
    state.tokensLoading = true;
    try {
      const data = await api('/v1/api-tokens') || {};
      state.tokens = Array.isArray(data.tokens) ? data.tokens : [];
      renderTokenGroups();
      renderTokens();
      return state.tokens;
    } catch (err) {
      if (!silent) throw err;
      return state.tokens;
    } finally {
      state.tokensLoading = false;
    }
  }

  function renderTokens() {
    const body = $('token-rows');
    clear(body);
    const account = Boolean(state.auth && state.auth.mode === 'account');
    $('token-create-panel').classList.toggle('hidden', !account);
    $('token-key-note').classList.toggle('hidden', account);
    if (!account) {
      $('token-empty').classList.add('hidden');
      $('token-summary').textContent = '';
      return;
    }
    state.tokens.forEach((token) => body.appendChild(tokenRow(token)));
    const active = state.tokens.filter((token) => !token.revoked && !tokenExpired(token)).length;
    $('token-summary').textContent = state.tokens.length
      ? '共 ' + state.tokens.length + ' 个令牌，' + active + ' 个有效（每个账号最多保留 20 个有效令牌）'
      : '';
    $('token-empty').classList.toggle('hidden', state.tokens.length > 0);
  }

  function tokenRow(token) {
    const tr = document.createElement('tr');
    const name = cell(token.name);
    name.className = 'mono';
    tr.appendChild(name);
    tr.appendChild(cell(rolePill(token.role)));
    const resources = Array.isArray(token.resources) ? token.resources : [];
    tr.appendChild(cell(resources.map((value) => TOKEN_RESOURCE_LABELS[value] || value).join('、') || '—'));
    tr.appendChild(cell(token.group_id || '全局'));
    tr.appendChild(cell(fmtTime(token.created_at_ms)));
    tr.appendChild(cell(fmtTime(token.expires_at_ms)));
    tr.appendChild(cell(token.last_used_at_ms ? fmtTime(token.last_used_at_ms) : '从未'));
    tr.appendChild(cell(token.revoked ? pill('已撤销', 'bad')
      : tokenExpired(token) ? pill('已过期', 'warn') : pill('有效', 'ok')));

    const actions = el('div', 'actions-wrap');
    const revoke = button('撤销', 'small danger');
    revoke.disabled = token.revoked || tokenExpired(token);
    revoke.addEventListener('click', () => withButton(revoke, async () => {
      if (!window.confirm('撤销令牌 ' + token.name + '？使用该密钥的集成会立即失效。')) return;
      await api('/v1/api-tokens/' + encodeURIComponent(token.id), { method: 'DELETE' });
      toast('已撤销令牌 ' + token.name);
      await loadTokens();
    }));
    actions.appendChild(revoke);
    tr.appendChild(cell(actions));
    return tr;
  }

  async function createToken() {
    const submit = $('token-create').querySelector('button[type="submit"]');
    const resources = tokenResources();
    if (!resources.length) {
      toast('至少选择一个资源', true);
      return;
    }
    const days = Number($('token-ttl-days').value || 0);
    if (!Number.isFinite(days) || days < 1 || days > 365) {
      toast('有效期需在 1 到 365 天之间', true);
      return;
    }
    const body = {
      name: $('token-name').value.trim(),
      role: $('token-role').value,
      resources,
      group_id: $('token-group').value,
      expires_in_seconds: Math.round(days * 86400),
    };
    await withButton(submit, async () => {
      const reply = await api('/v1/api-tokens', { method: 'POST', body });
      $('token-name').value = '';
      if (reply && reply.secret) showTokenSecret(reply.secret);
      toast('令牌已创建');
      await loadTokens();
    });
  }

  function showTokenSecret(secret) {
    state.tokenSecret = secret || '';
    $('token-secret-value').textContent = state.tokenSecret;
    const dialog = $('token-secret-dialog');
    if (typeof dialog.showModal === 'function') dialog.showModal();
    else window.prompt('令牌密钥（只显示一次）：', state.tokenSecret);
  }

  // ---------- groups ----------

  async function loadGroups(silent) {
    if (state.groupsLoading) return state.groups;
    state.groupsLoading = true;
    try {
      const data = await api('/v1/groups') || {};
      state.groups = Array.isArray(data.groups) ? data.groups : [];
      state.groupsLoaded = true;
      if (state.auth && state.auth.mode === 'account') {
        const role = data.global_role || 'user';
        if (state.auth.role !== role) {
          state.auth.role = role;
          writeSession(state.auth);
          renderSession();
          applyPermissions();
        }
      }
      renderGroups();
      renderAuditGroups();
      return state.groups;
    } catch (err) {
      if (!silent) throw err;
      return state.groups;
    } finally {
      state.groupsLoading = false;
    }
  }

  function rolePill(role) {
    if (!role) return el('span', 'muted', '—');
    return pill(role, role === 'admin' ? 'ok' : role === 'operator' ? 'warn' : '');
  }

  function renderGroups() {
    const body = $('group-rows');
    clear(body);
    state.groups.forEach((group) => body.appendChild(groupRow(group)));
    $('group-empty').classList.toggle('hidden', state.groups.length > 0);
    $('group-readonly').classList.toggle('hidden', can('admin'));
    renderTokenGroups();
  }

  function groupRow(group) {
    const tr = document.createElement('tr');
    const id = cell(group.id);
    id.className = 'mono';
    tr.appendChild(id);
    tr.appendChild(cell(group.name || '—'));
    tr.appendChild(cell(group.description || '—'));
    tr.appendChild(cell(group.member_count + ' / ' + (group.member_quota || '∞')));
    tr.appendChild(cell(group.device_count + ' / ' + (group.device_quota || '∞')));
    tr.appendChild(cell(rolePill(group.role)));
    tr.appendChild(cell(fmtTime(group.updated_at_ms)));

    const actions = el('div', 'actions-wrap');
    const manage = button('管理', 'small');
    manage.addEventListener('click', () => withButton(manage, () => openGroupDialog(group)));
    actions.appendChild(manage);
    tr.appendChild(cell(actions));
    return tr;
  }

  async function createGroup() {
    const submit = $('group-create').querySelector('button[type="submit"]');
    const body = {
      id: $('group-id').value.trim(),
      name: $('group-name').value.trim(),
      description: $('group-description').value.trim(),
      member_quota: Number($('group-member-quota').value || 0),
      device_quota: Number($('group-device-quota').value || 0),
    };
    await withButton(submit, async () => {
      const group = await api('/v1/groups', { method: 'POST', body });
      $('group-id').value = '';
      $('group-name').value = '';
      $('group-description').value = '';
      $('group-member-quota').value = '0';
      $('group-device-quota').value = '0';
      toast('已创建分组 ' + group.id);
      await loadGroups(true);
      await openGroupDialog(group);
    });
  }

  async function openGroupDialog(group) {
    state.groupEditing = group;
    $('group-dialog-title').textContent = group.name || group.id;
    $('group-dialog-id').textContent = group.id;
    $('group-dialog-role').textContent = group.role || '';
    $('group-dialog-role').className = 'pill ' +
      (group.role === 'admin' ? 'ok' : group.role === 'operator' ? 'warn' : '');
    const admin = canGroup(group, 'admin');
    const write = canGroup(group, 'operator');
    $('group-edit-name').value = group.name || '';
    $('group-edit-description').value = group.description || '';
    $('group-edit-member-quota').value = group.member_quota || 0;
    $('group-edit-device-quota').value = group.device_quota || 0;
    ['group-edit-name', 'group-edit-description', 'group-edit-member-quota', 'group-edit-device-quota']
      .forEach((id) => { $(id).disabled = !admin; });
    $('group-edit').querySelector('button[type="submit"]').classList.toggle('hidden', !admin);
    $('group-member-add').classList.toggle('hidden', !admin);
    $('group-device-add').classList.toggle('hidden', !write);
    $('group-delete').classList.toggle('hidden', !admin);
    clear($('group-member-rows'));
    clear($('group-device-rows'));
    $('group-member-empty').classList.add('hidden');
    $('group-device-empty').classList.add('hidden');
    $('group-dialog').showModal();
    await loadGroupDetail(group.id);
  }

  async function loadGroupDetail(groupID) {
    const data = await api('/v1/groups/' + encodeURIComponent(groupID)) || {};
    if (!state.groupEditing || state.groupEditing.id !== groupID) return;
    const group = data.group || {};
    const merged = Object.assign({}, state.groupEditing, {
      name: group.name || state.groupEditing.name,
      description: group.description || '',
      member_quota: group.member_quota || 0,
      device_quota: group.device_quota || 0,
      member_count: group.member_count || 0,
      device_count: group.device_count || 0,
      updated_at_ms: group.updated_at_ms || state.groupEditing.updated_at_ms,
    });
    state.groupEditing = merged;
    const admin = canGroup(merged, 'admin');
    const write = canGroup(merged, 'operator');
    $('group-dialog-title').textContent = merged.name || merged.id;
    $('group-edit-name').value = merged.name || '';
    $('group-edit-description').value = merged.description || '';
    $('group-edit-member-quota').value = merged.member_quota;
    $('group-edit-device-quota').value = merged.device_quota;
    ['group-edit-name', 'group-edit-description', 'group-edit-member-quota', 'group-edit-device-quota']
      .forEach((id) => { $(id).disabled = !admin; });
    $('group-edit').querySelector('button[type="submit"]').classList.toggle('hidden', !admin);
    $('group-member-add').classList.toggle('hidden', !admin);
    $('group-device-add').classList.toggle('hidden', !write);
    $('group-delete').classList.toggle('hidden', !admin);
    renderGroupMembers(Array.isArray(data.members) ? data.members : [], admin);
    renderGroupDevices(Array.isArray(data.devices) ? data.devices : [], write);
  }

  async function refreshGroupDialog() {
    const group = state.groupEditing;
    if (!group) return;
    await loadGroups(true);
    const current = groupByID(group.id);
    if (!current) {
      $('group-dialog').close();
      state.groupEditing = null;
      return;
    }
    state.groupEditing = current;
    await loadGroupDetail(current.id);
  }

  function renderGroupMembers(members, admin) {
    const body = $('group-member-rows');
    clear(body);
    members.forEach((member) => {
      const tr = document.createElement('tr');
      const name = cell(member.username);
      name.className = 'mono';
      tr.appendChild(name);
      if (admin) {
        const select = document.createElement('select');
        ['readonly', 'operator', 'admin'].forEach((role) => {
          const option = document.createElement('option');
          option.value = role;
          option.textContent = role;
          if (member.role === role) option.selected = true;
          select.appendChild(option);
        });
        select.addEventListener('change', () => withButton(select, async () => {
          try {
            await updateGroupMember(member.username, select.value);
          } finally {
            await refreshGroupDialog();
          }
        }));
        tr.appendChild(cell(select));
      } else {
        tr.appendChild(cell(rolePill(member.role)));
      }
      tr.appendChild(cell(fmtTime(member.created_at_ms)));

      const actions = el('div', 'actions-wrap');
      if (admin) {
        const remove = button('移除', 'small danger');
        remove.addEventListener('click', () => withButton(remove, async () => {
          if (!window.confirm('从分组中移除 ' + member.username + '？')) return;
          await api('/v1/groups/' + encodeURIComponent(state.groupEditing.id) +
            '/members/' + encodeURIComponent(member.username), { method: 'DELETE' });
          toast('已移除 ' + member.username);
          await refreshGroupDialog();
        }));
        actions.appendChild(remove);
      }
      tr.appendChild(cell(actions));
      body.appendChild(tr);
    });
    $('group-member-empty').classList.toggle('hidden', members.length > 0);
  }

  function renderGroupDevices(devices, write) {
    const body = $('group-device-rows');
    clear(body);
    devices.forEach((device) => {
      const tr = document.createElement('tr');
      const peer = cell(device.peer_id);
      peer.className = 'mono';
      tr.appendChild(peer);
      tr.appendChild(cell(device.added_by || '—'));
      tr.appendChild(cell(fmtTime(device.created_at_ms)));

      const actions = el('div', 'actions-wrap');
      if (write) {
        const remove = button('移除', 'small danger');
        remove.addEventListener('click', () => withButton(remove, async () => {
          if (!window.confirm('从分组中移除设备 ' + device.peer_id + '？')) return;
          await api('/v1/groups/' + encodeURIComponent(state.groupEditing.id) +
            '/devices/' + encodeURIComponent(device.peer_id), { method: 'DELETE' });
          toast('已移除设备 ' + device.peer_id);
          await refreshGroupDialog();
        }));
        actions.appendChild(remove);
      }
      tr.appendChild(cell(actions));
      body.appendChild(tr);
    });
    $('group-device-empty').classList.toggle('hidden', devices.length > 0);
  }

  async function updateGroupMember(username, role) {
    await api('/v1/groups/' + encodeURIComponent(state.groupEditing.id) +
      '/members/' + encodeURIComponent(username), { method: 'PATCH', body: { role } });
    toast('已将 ' + username + ' 设为 ' + role);
  }

  async function addGroupMember() {
    const group = state.groupEditing;
    if (!group) return;
    const username = $('group-member-username').value.trim();
    const role = $('group-member-role').value;
    const submit = $('group-member-add').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      await api('/v1/groups/' + encodeURIComponent(group.id) + '/members', {
        method: 'POST',
        body: { username, role },
      });
      $('group-member-username').value = '';
      toast('已添加 ' + username);
      await refreshGroupDialog();
    });
  }

  async function addGroupDevice() {
    const group = state.groupEditing;
    if (!group) return;
    const peerID = $('group-device-peer').value.trim();
    const submit = $('group-device-add').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      await api('/v1/groups/' + encodeURIComponent(group.id) + '/devices', {
        method: 'POST',
        body: { peer_id: peerID },
      });
      $('group-device-peer').value = '';
      toast('已添加设备 ' + peerID);
      await refreshGroupDialog();
    });
  }

  async function saveGroupSettings() {
    const group = state.groupEditing;
    if (!group) return;
    const submit = $('group-edit').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      await api('/v1/groups/' + encodeURIComponent(group.id), {
        method: 'PATCH',
        body: {
          name: $('group-edit-name').value.trim(),
          description: $('group-edit-description').value.trim(),
          member_quota: Number($('group-edit-member-quota').value || 0),
          device_quota: Number($('group-edit-device-quota').value || 0),
        },
      });
      toast('已保存分组设置');
      await refreshGroupDialog();
    });
  }

  async function deleteGroup() {
    const group = state.groupEditing;
    if (!group) return;
    if (!window.confirm('删除分组 ' + group.id + '？成员与设备归属会一并移除，此操作不可撤销。')) return;
    await api('/v1/groups/' + encodeURIComponent(group.id), { method: 'DELETE' });
    $('group-dialog').close();
    state.groupEditing = null;
    toast('已删除分组 ' + group.id);
    await loadGroups(true);
  }

  // ---------- recordings ----------

  async function loadRecordings(reset) {
    const generation = ++state.recordingGen;
    if (reset) {
      state.recordingBefore = 0;
      state.recordingRows = [];
    }
    const username = $('recording-user').value.trim();
    const params = new URLSearchParams();
    if (username) params.set('username', username);
    params.set('limit', '50');
    const cursor = state.recordingBefore;
    if (cursor) params.set('before_id', String(cursor));
    let data;
    try {
      data = await api('/v1/recordings?' + params.toString()) || {};
    } catch (err) {
      if (err && err.status === 503) {
        state.recordingRows = [];
        state.recordingBefore = 0;
        renderRecordings();
        $('recording-summary').textContent = '服务端未启用录制归档（缺少 --recordings-dir）。';
        return;
      }
      throw err;
    }
    if (generation !== state.recordingGen) return;
    const list = Array.isArray(data.recordings) ? data.recordings : [];
    state.recordingRows = state.recordingRows.concat(list);
    state.recordingBefore = data.next_before_id || 0;
    renderRecordings();
    const total = data.total === undefined ? list.length : data.total;
    const bits = ['共 ' + total + ' 个', '合计 ' + fmtBytes(data.usage_bytes || 0)];
    if (data.max_upload_bytes) bits.push('单文件上限 ' + fmtBytes(data.max_upload_bytes));
    if (username) bits.push('用户 ' + username);
    if (state.recordingRows.length < total) bits.push('已显示 ' + state.recordingRows.length + ' 个');
    $('recording-summary').textContent = bits.join(' · ');
  }

  function renderRecordings() {
    const body = $('recording-rows');
    clear(body);
    state.recordingRows.forEach((rec) => body.appendChild(recordingRow(rec)));
    $('recording-empty').classList.toggle('hidden', state.recordingRows.length > 0);
    $('recording-more').classList.toggle('hidden', !state.recordingBefore);
  }

  function recordingRow(rec) {
    const tr = document.createElement('tr');
    tr.appendChild(cell(fmtTime(rec.created_at_ms)));

    const owner = cell(rec.username || '—');
    owner.className = 'mono';
    tr.appendChild(owner);

    const name = cell(rec.name || '—');
    if (rec.sha256) name.title = 'SHA-256 ' + rec.sha256;
    tr.appendChild(name);
    tr.appendChild(cell(rec.peer_id || '—'));
    tr.appendChild(cell(fmtBytes(rec.size)));
    tr.appendChild(cell(fmtDurationMS(rec.duration_ms)));
    tr.appendChild(cell(fmtTime(rec.started_at_ms)));

    const actions = el('div', 'actions-wrap');
    const download = button('下载', 'small');
    download.addEventListener('click', () => withButton(download, () => downloadRecording(rec)));
    actions.appendChild(download);

    if (can('operator')) {
      const remove = button('删除', 'small danger');
      remove.addEventListener('click', () => withButton(remove, async () => {
        if (!window.confirm('删除 ' + (rec.username || '') + ' 的录制「' + (rec.name || rec.id) +
          '」？\n文件与元数据会一并移除，此操作不可撤销。')) return;
        await api('/v1/recordings/' + encodeURIComponent(rec.id), { method: 'DELETE' });
        toast('已删除录制');
        await loadRecordings(true);
      }));
      actions.appendChild(remove);
    }

    tr.appendChild(cell(actions));
    return tr;
  }

  async function downloadRecording(rec) {
    const target = '/v1/recordings/' + encodeURIComponent(rec.id);
    let resp = await fetch(target, { headers: authHeaders(), cache: 'no-store' });
    if (resp.status === 401 && await refreshSession()) {
      resp = await fetch(target, { headers: authHeaders(), cache: 'no-store' });
    }
    if (!resp.ok) {
      let message = resp.status + ' ' + resp.statusText;
      try {
        const data = await resp.json();
        if (data && data.error) message = data.error;
      } catch {
        /* not JSON */
      }
      if (resp.status === 401) lock('登录已失效或凭据被拒绝，请重新登录。');
      throw new ApiError(message, resp.status);
    }
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = rec.name || rec.id + '.mp4';
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    toast('已开始下载 ' + (rec.name || rec.id));
  }

  // ---------- audit ----------

  const AUDIT = {
    events: {
      path: '/v1/audit',
      filterKey: 'action',
      filterPlaceholder: '动作（可选，如 login）',
      hasHost: false,
      hasUser: true,
      columns: ['时间', '用户', '动作', '详情', '客户端 IP', '地点', '结果'],
      row(event) {
        return [
          fmtTime(event.created_at_ms),
          event.username || '—',
          event.action,
          event.detail || '—',
          event.client_ip || '—',
          event.location || '—',
          pillBool(event.success),
        ];
      },
    },
    connections: {
      path: '/v1/audit/connections',
      filterKey: 'username',
      filterPlaceholder: '用户名（可选）',
      hasHost: true,
      columns: ['开始', '用户', '对端', '方向', '来源', '客户端 IP', '地点', '路由',
        '关联', '时长', '结果', '详情'],
      row(event) {
        return [
          fmtTime(event.started_at_ms),
          event.username || '—',
          event.peer_id || '—',
          event.direction || '—',
          event.source || '—',
          event.client_ip || '—',
          event.location || '—',
          routeCell(event.route),
          refCell(event.audit_ref),
          fmtDurationMS(event.duration_ms),
          pillBool(event.success),
          event.detail || '—',
        ];
      },
    },
    files: {
      path: '/v1/audit/file-transfers',
      filterKey: 'username',
      filterPlaceholder: '用户名（可选）',
      hasHost: true,
      columns: ['时间', '用户', '对端', '方向', '文件', '大小', '客户端 IP', '地点',
        '路由', '关联', '结果', '详情'],
      row(event) {
        const size = fmtBytes(event.size) + (event.files > 1 ? '（' + event.files + ' 个）' : '');
        return [
          fmtTime(event.occurred_at_ms),
          event.username || '—',
          event.peer_id || '—',
          event.direction || '—',
          event.name || '—',
          size,
          event.client_ip || '—',
          event.location || '—',
          routeCell(event.route),
          refCell(event.audit_ref),
          pillBool(event.success),
          event.detail || '—',
        ];
      },
    },
  };

  function renderAuditGroups() {
    const select = $('audit-group');
    const previous = select.value;
    clear(select);
    const all = document.createElement('option');
    all.value = '';
    all.textContent = '全部分组';
    select.appendChild(all);
    state.groups.forEach((group) => {
      const option = document.createElement('option');
      option.value = group.id;
      option.textContent = group.name && group.name !== group.id
        ? group.id + ' · ' + group.name : group.id;
      select.appendChild(option);
    });
    if (groupOnly()) {
      all.disabled = true;
      all.hidden = true;
    }
    if (previous && state.groups.some((group) => group.id === previous)) {
      select.value = previous;
    } else if (groupOnly()) {
      select.value = state.groups.length ? state.groups[0].id : '';
    } else {
      select.value = '';
    }
  }

  function selectAuditTab(tab) {
    state.auditTab = tab;
    document.querySelectorAll('.subtab').forEach((node) => {
      node.classList.toggle('active', node.dataset.audit === tab);
    });
    const spec = AUDIT[tab];
    $('audit-filter').placeholder = spec.filterPlaceholder;
    $('audit-host').classList.toggle('hidden', !spec.hasHost);
    $('audit-user').classList.toggle('hidden', !spec.hasUser);
    $('audit-filter').value = '';
    $('audit-host').value = '';
    $('audit-user').value = '';
    loadAudit(true).catch(reportError);
  }

  function auditParams(spec) {
    const params = new URLSearchParams();
    const group = $('audit-group').value;
    if (group) params.set('group', group);
    const filter = $('audit-filter').value.trim();
    if (filter) params.set(spec.filterKey, filter);
    if (spec.hasUser) {
      const user = $('audit-user').value.trim();
      if (user) params.set('user', user);
    }
    if (spec.hasHost) {
      const host = $('audit-host').value.trim();
      if (host) params.set('host', host);
    }
    // datetime-local is wall-clock time in the operator's zone; the report API
    // takes unix milliseconds, so convert here and let the server stay zone-free.
    [['since', 'audit-since'], ['until', 'audit-until']].forEach(([key, id]) => {
      const raw = $(id).value;
      if (!raw) return;
      const ms = Date.parse(raw);
      if (Number.isFinite(ms)) params.set(key, String(ms));
    });
    return params;
  }

  async function loadAudit(reset) {
    const spec = AUDIT[state.auditTab];
    const generation = ++state.auditGen;
    if (reset) {
      state.auditBefore = 0;
      state.auditRows = [];
    }
    renderAuditHeader(spec);
    if (groupOnly() && !$('audit-group').value) {
      state.auditRows = [];
      state.auditBefore = 0;
      renderAuditRows(spec);
      $('audit-summary').textContent = state.groups.length
        ? '请选择要查看的分组。' : '当前账号还没有分组授权。';
      return;
    }
    const params = auditParams(spec);
    params.set('limit', '50');
    const cursor = state.auditBefore;
    if (cursor) params.set('before_id', String(cursor));
    const data = await api(spec.path + '?' + params.toString()) || {};
    if (generation !== state.auditGen) return;
    const list = data.events || data.connections || data.transfers || [];
    state.auditRows = state.auditRows.concat(list);
    state.auditBefore = data.next_before_id || 0;
    renderAuditRows(spec);
  }

  function renderAuditHeader(spec) {
    const head = $('audit-head');
    clear(head);
    const tr = document.createElement('tr');
    spec.columns.forEach((name) => tr.appendChild(el('th', '', name)));
    head.appendChild(tr);
  }

  function renderAuditRows(spec) {
    const body = $('audit-rows');
    clear(body);
    state.auditRows.forEach((record) => {
      const tr = document.createElement('tr');
      spec.row(record).forEach((value) => tr.appendChild(cell(value)));
      body.appendChild(tr);
    });
    $('audit-empty').classList.toggle('hidden', state.auditRows.length > 0);
    $('audit-more').classList.toggle('hidden', !state.auditBefore);
    $('audit-summary').textContent = '已显示 ' + state.auditRows.length + ' 条' +
      (state.auditBefore ? '，还有更多' : state.auditRows.length ? '，已到末尾' : '');
  }

  async function downloadCSV(target, filename, message) {
    let resp = await fetch(target, { headers: authHeaders(), cache: 'no-store' });
    if (resp.status === 401 && await refreshSession()) {
      resp = await fetch(target, { headers: authHeaders(), cache: 'no-store' });
    }
    if (!resp.ok) {
      let detail = resp.status + ' ' + resp.statusText;
      try {
        const data = await resp.json();
        if (data && data.error) detail = data.error;
      } catch {
        /* not JSON */
      }
      if (resp.status === 401) lock('服务器密钥无效或已更改，请重新连接。');
      throw new ApiError(detail, resp.status);
    }
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = filename;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    toast(message);
  }

  function csvStamp() {
    return new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');
  }

  async function exportAudit() {
    const spec = AUDIT[state.auditTab];
    const params = auditParams(spec);
    params.set('format', 'csv');
    await downloadCSV(spec.path + '?' + params.toString(),
      'chast-' + state.auditTab + '-' + csvStamp() + '.csv', '已导出 CSV');
  }

  // The device list pages 200 rows at a time; the export deliberately ignores
  // paging and carries every filtered identity (online and offline).
  async function exportDevices() {
    const params = new URLSearchParams();
    const query = $('device-query').value.trim();
    if (query) params.set('q', query);
    if ($('device-online').checked) params.set('online', '1');
    params.set('format', 'csv');
    await downloadCSV('/v1/identities?' + params.toString(),
      'chast-devices-' + csvStamp() + '.csv', '已导出设备 CSV');
  }

  // ---------- events ----------

  let deviceInputTimer = 0;
  let accountInputTimer = 0;

  function syncLoginMode() {
    const account = $('login-mode').value === 'account';
    $('server-key').classList.toggle('hidden', account);
    $('login-account').classList.toggle('hidden', !account);
    if (account) $('login-username').focus();
    else $('server-key').focus();
  }

  function bindEvents() {
    $('login-mode').addEventListener('change', syncLoginMode);
    $('login-form').addEventListener('submit', async (event) => {
      event.preventDefault();
      const mode = $('login-mode').value;
      try {
        if (mode === 'account') {
          const username = $('login-username').value.trim();
          const password = $('login-password').value;
          if (!username || !password) {
            $('login-note').textContent = '请输入用户名与密码。';
            return;
          }
          await unlockAccount(
            username, password,
            $('login-totp').value, $('login-code-kind').value,
          );
          $('login-password').value = '';
          $('login-totp').value = '';
          $('login-code-kind').value = 'auto';
        } else {
          await unlockKey($('server-key').value.trim());
        }
        toast('已连接');
      } catch (err) {
        // A rejected server key has already been explained by lock().
        if (err.status === 401 && mode === 'key') return;
        const message = err.message || String(err);
        if (err.status === 401 || err.status === 403 || err.status === 429 || err.status === 503) {
          $('login-note').textContent = message;
          toast(message, true);
          return;
        }
        lock('无法连接：' + message);
        toast(message, true);
      }
    });

    $('login-webauthn').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      try {
        await unlockWithSecurityKey();
        toast('已连接');
      } catch (err) {
        const message = err.message || String(err);
        if (err.status === 401 || err.status === 403 || err.status === 429 || err.status === 503) {
          $('login-note').textContent = message;
          toast(message, true);
          return;
        }
        lock('无法完成安全密钥登录：' + message);
        toast(message, true);
      }
    }));

    $('login-oidc').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      try {
        await unlockWithSingleSignOn();
        toast('已连接');
      } catch (err) {
        const message = err.message || String(err);
        if (err.status === 401 || err.status === 403 || err.status === 429 || err.status === 503) {
          $('login-note').textContent = message;
          toast(message, true);
          return;
        }
        lock('无法完成单点登录：' + message);
        toast(message, true);
      }
    }));

    $('login-send-code').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      try {
        await sendLoginCode();
      } catch (err) {
        const message = err.message || String(err);
        $('login-note').textContent = message;
        toast(message, true);
      }
    }));

    $('logout').addEventListener('click', () => {
      lock('已断开连接。');
      toast('已断开');
    });

    document.querySelectorAll('.tab').forEach((node) => {
      node.addEventListener('click', () => switchTab(node.dataset.tab));
    });

    $('device-query').addEventListener('input', () => {
      clearTimeout(deviceInputTimer);
      state.deviceOffset = 0;
      deviceInputTimer = setTimeout(() => loadDevices().catch(reportError), 250);
    });
    $('device-online').addEventListener('change', () => {
      state.deviceOffset = 0;
      loadDevices().catch(reportError);
    });
    $('device-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadDevices();
      toast('已刷新');
    }));
    $('device-prev').addEventListener('click', () => {
      state.deviceOffset = Math.max(0, state.deviceOffset - state.deviceLimit);
      loadDevices().catch(reportError);
    });
    $('device-next').addEventListener('click', () => {
      state.deviceOffset += state.deviceLimit;
      loadDevices().catch(reportError);
    });
    $('device-export').addEventListener('click', (event) => withButton(event.currentTarget, exportDevices));
    $('device-select-all').addEventListener('change', (event) => {
      if (event.currentTarget.checked) {
        state.deviceList.forEach((peer) => state.deviceSelection.add(peer.id));
      } else {
        state.deviceSelection.clear();
      }
      $('device-rows').querySelectorAll('input[type=checkbox]').forEach((box, index) => {
        if (state.deviceList[index]) box.checked = state.deviceSelection.has(state.deviceList[index].id);
      });
      updateDeviceSelection();
    });
    $('device-bulk-disable').addEventListener('click', (event) =>
      withButton(event.currentTarget, () => bulkSetDisabled(event.currentTarget, true)));
    $('device-bulk-enable').addEventListener('click', (event) =>
      withButton(event.currentTarget, () => bulkSetDisabled(event.currentTarget, false)));
    $('device-bulk-clear').addEventListener('click', () => {
      state.deviceSelection.clear();
      $('device-rows').querySelectorAll('input[type=checkbox]').forEach((box) => {
        box.checked = false;
      });
      updateDeviceSelection();
    });

    $('device-form').addEventListener('submit', (event) => {
      event.preventDefault();
      saveDeviceLabels().catch(reportError);
    });
    $('device-cancel').addEventListener('click', () => {
      $('device-dialog').close();
      state.editing = null;
    });
    $('device-dialog').addEventListener('close', () => {
      state.editing = null;
    });

    $('policy-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadPolicies();
      toast('已刷新');
    }));
    $('policy-create').addEventListener('click', () => openPolicyDialog(null));
    $('policy-form').addEventListener('submit', (event) => {
      event.preventDefault();
      savePolicy();
    });
    $('policy-mode').addEventListener('change', syncPolicyMode);
    $('policy-ceiling').addEventListener('input', syncPolicyCeiling);
    $('policy-cancel').addEventListener('click', () => $('policy-dialog').close());
    $('policy-delete').addEventListener('click', (event) => withButton(event.currentTarget, deletePolicy));
    $('policy-dialog').addEventListener('close', () => {
      state.policyEditing = null;
    });

    $('account-cap-form').addEventListener('submit', (event) => {
      event.preventDefault();
      saveAccountCap();
    });
    $('account-cap-ceiling').addEventListener('input', () =>
      applyPolicyCeiling($('account-cap-ceiling'), $('account-cap-hint')));
    $('account-cap-cancel').addEventListener('click', () => $('account-cap-dialog').close());
    $('account-cap-dialog').addEventListener('close', () => {
      state.accountCapUser = null;
    });

    $('account-sessions-revoke-all').addEventListener('click', revokeAllAccountSessions);
    $('account-sessions-close').addEventListener('click', () =>
      $('account-sessions-dialog').close());
    $('account-sessions-dialog').addEventListener('close', () => {
      state.accountSessionsUser = null;
    });

    $('account-recovery-form').addEventListener('submit', (event) => {
      event.preventDefault();
      saveAccountRecovery();
    });
    $('account-recovery-cancel').addEventListener('click', () =>
      $('account-recovery-dialog').close());
    $('account-recovery-email-send').addEventListener('click', (event) =>
      withButton(event.currentTarget, sendAccountResetEmail));
    $('account-recovery-sms').addEventListener('click', (event) =>
      withButton(event.currentTarget, sendAccountResetSMS));
    $('account-recovery-dialog').addEventListener('close', () => {
      state.accountRecoveryUser = null;
    });

    $('policy-object-create').addEventListener('click', () => openPolicyObjectDialog(null));
    $('policy-object-form').addEventListener('submit', (event) => {
      event.preventDefault();
      savePolicyObject();
    });
    $('policy-object-mode').addEventListener('change', syncPolicyObjectMode);
    $('policy-object-ceiling').addEventListener('input', syncPolicyObjectCeiling);
    $('policy-object-cancel').addEventListener('click', () => $('policy-object-dialog').close());
    $('policy-object-delete').addEventListener('click', (event) =>
      withButton(event.currentTarget, deletePolicyObject));
    $('policy-object-dialog').addEventListener('close', () => {
      state.policyObjectEditing = null;
    });

    $('policy-assignment-create').addEventListener('click', (event) =>
      withButton(event.currentTarget, openPolicyAssignmentDialog));
    $('policy-assignment-form').addEventListener('submit', (event) => {
      event.preventDefault();
      savePolicyAssignment();
    });
    $('policy-assignment-scope').addEventListener('change', syncPolicyAssignmentScope);
    $('policy-assignment-cancel').addEventListener('click', () =>
      $('policy-assignment-dialog').close());

    $('account-create').addEventListener('submit', (event) => {
      event.preventDefault();
      createAccount();
    });
    $('account-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadAccounts();
      toast('已刷新');
    }));
    $('account-query').addEventListener('input', () => {
      clearTimeout(accountInputTimer);
      state.accountOffset = 0;
      accountInputTimer = setTimeout(() => loadAccounts().catch(reportError), 250);
    });
    $('account-role-filter').addEventListener('change', () => {
      state.accountOffset = 0;
      loadAccounts().catch(reportError);
    });
    $('account-prev').addEventListener('click', () => {
      state.accountOffset = Math.max(0, state.accountOffset - state.accountLimit);
      loadAccounts().catch(reportError);
    });
    $('account-next').addEventListener('click', () => {
      state.accountOffset += state.accountLimit;
      loadAccounts().catch(reportError);
    });
    $('account-export').addEventListener('click', (event) =>
      withButton(event.currentTarget, exportAccounts));

    $('token-create').addEventListener('submit', (event) => {
      event.preventDefault();
      createToken();
    });
    $('token-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadTokens();
      toast('已刷新');
    }));
    $('token-group').addEventListener('change', syncTokenScope);
    $('token-secret-copy').addEventListener('click', () => {
      if (!state.tokenSecret) {
        toast('没有可复制的密钥', true);
        return;
      }
      copyText(state.tokenSecret);
    });

    $('mine-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadMine();
      toast('已刷新');
    }));
    $('mine-sessions-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadMine();
      toast('已刷新');
    }));
    $('mine-password-form').addEventListener('submit', (event) => {
      event.preventDefault();
      changeMinePassword(event).catch(reportError);
    });
    $('mine-email-form').addEventListener('submit', (event) => {
      saveMineContact('email', false, event).catch(reportError);
    });
    $('mine-email-clear').addEventListener('click', (event) => {
      withButton(event.currentTarget, () => saveMineContact('email', true, null).catch(reportError));
    });
    $('mine-phone-form').addEventListener('submit', (event) => {
      saveMineContact('phone', false, event).catch(reportError);
    });
    $('mine-phone-clear').addEventListener('click', (event) => {
      withButton(event.currentTarget, () => saveMineContact('phone', true, null).catch(reportError));
    });
    $('mine-2fa-start').addEventListener('submit', (event) => {
      event.preventDefault();
      startMineTwoFactor(event).catch(reportError);
    });
    $('mine-2fa-enable').addEventListener('submit', (event) => {
      event.preventDefault();
      enableMineTwoFactor(event).catch(reportError);
    });
    $('mine-2fa-cancel').addEventListener('click', cancelMineTwoFactor);
    $('mine-2fa-disable').addEventListener('submit', (event) => {
      event.preventDefault();
      disableMineTwoFactor(event).catch(reportError);
    });
    $('mine-recovery').addEventListener('submit', (event) => {
      event.preventDefault();
      generateMineRecoveryCodes(event).catch(reportError);
    });
    $('mine-recovery-copy').addEventListener('click', () => {
      copyText($('mine-recovery-list').textContent);
    });
    $('mine-recovery-download').addEventListener('click', () => {
      downloadMineRecoveryCodes();
    });
    $('mine-key-form').addEventListener('submit', (event) => {
      event.preventDefault();
      registerMineSecurityKey(event).catch(reportError);
    });
    $('mine-sessions-revoke-others').addEventListener('click', (event) => {
      withButton(event.currentTarget, revokeOtherMineSessions);
    });
    $('token-secret-dialog').addEventListener('close', () => {
      state.tokenSecret = '';
      $('token-secret-value').textContent = '';
    });

    $('group-create').addEventListener('submit', (event) => {
      event.preventDefault();
      createGroup();
    });
    $('group-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadGroups();
      toast('已刷新');
    }));
    $('group-edit').addEventListener('submit', (event) => {
      event.preventDefault();
      saveGroupSettings();
    });
    $('group-member-add').addEventListener('submit', (event) => {
      event.preventDefault();
      addGroupMember();
    });
    $('group-device-add').addEventListener('submit', (event) => {
      event.preventDefault();
      addGroupDevice();
    });
    $('group-close').addEventListener('click', () => $('group-dialog').close());
    $('group-delete').addEventListener('click', (event) => withButton(event.currentTarget, deleteGroup));
    $('group-dialog').addEventListener('close', () => {
      state.groupEditing = null;
    });

    $('recording-reload').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadRecordings(true);
    }));
    $('recording-more').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadRecordings(false);
    }));
    $('recording-user').addEventListener('keydown', (event) => {
      if (event.key === 'Enter') loadRecordings(true).catch(reportError);
    });

    document.querySelectorAll('.subtab').forEach((node) => {
      node.addEventListener('click', () => selectAuditTab(node.dataset.audit));
    });
    $('audit-reload').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadAudit(true);
    }));
    $('audit-more').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadAudit(false);
    }));
    $('audit-emergency').addEventListener('click', () => {
      // Emergency grants are account audit events; switch to that subtab and
      // filter by the dedicated action so an operator sees every override.
      state.auditTab = 'events';
      document.querySelectorAll('.subtab').forEach((node) => {
        node.classList.toggle('active', node.dataset.audit === 'events');
      });
      $('audit-filter').placeholder = AUDIT.events.filterPlaceholder;
      $('audit-host').classList.add('hidden');
      $('audit-host').value = '';
      $('audit-user').classList.remove('hidden');
      $('audit-user').value = '';
      $('audit-filter').value = 'emergency_access';
      loadAudit(true).catch(reportError);
    });
    $('audit-csv').addEventListener('click', (event) => withButton(event.currentTarget, exportAudit));
    $('audit-group').addEventListener('change', () => loadAudit(true).catch(reportError));
    [$('audit-filter'), $('audit-host'), $('audit-user')].forEach((input) => {
      input.addEventListener('keydown', (event) => {
        if (event.key === 'Enter') loadAudit(true).catch(reportError);
      });
    });
    [$('audit-since'), $('audit-until')].forEach((input) => {
      input.addEventListener('change', () => loadAudit(true).catch(reportError));
    });

    $('server-copy').addEventListener('click', () => {
      if (!state.publicKey) {
        toast('尚未读取服务器公钥', true);
        return;
      }
      copyText(state.publicKey);
    });

    document.addEventListener('visibilitychange', () => {
      if (document.hidden) return;
      if ($('workspace').classList.contains('hidden')) return;
      if (state.tab === 'overview') refreshOverview().catch(() => {});
      else if (state.tab === 'devices') loadDevices(true).catch(() => {});
      else if (state.tab === 'policies') loadPolicies(true).catch(() => {});
      else if (state.tab === 'groups') loadGroups(true).catch(() => {});
    });
  }

  async function copyText(text) {
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(text);
      } else {
        const area = document.createElement('textarea');
        area.value = text;
        area.setAttribute('readonly', '');
        area.className = 'sr-only';
        document.body.appendChild(area);
        area.select();
        document.execCommand('copy');
        area.remove();
      }
      toast('已复制');
    } catch {
      toast('复制失败，请手动选择文本', true);
    }
  }

  // ---------- my account ----------

  function resetMine() {
    state.accountInfo = null;
    state.accountSessions = [];
    state.account2faPending = null;
    state.accountKeys = null;
    $('mine-2fa-secret').textContent = '';
    $('mine-2fa-uri').textContent = '';
    $('mine-recovery-list').textContent = '';
    $('mine-recovery-codes').classList.add('hidden');
    $('mine-2fa-recovery-note').textContent = '';
    $('mine-2fa-state').textContent = '';
    $('mine-keys-state').textContent = '';
    $('mine-email').value = '';
    $('mine-phone').value = '';
    $('mine-email-password').value = '';
    $('mine-phone-password').value = '';
    clear($('mine-summary'));
    clear($('mine-session-rows'));
    clear($('mine-key-rows'));
  }

  async function loadMine(silent) {
    if (!state.auth || state.auth.mode !== 'account') {
      renderMine();
      return;
    }
    try {
      const [info, sessions, keys] = await Promise.all([
        api('/v1/account'),
        api('/v1/account/sessions'),
        api('/v1/account/webauthn/credentials')
          .then((data) => ({ enabled: true, credentials: (data && data.credentials) || [] }))
          .catch((err) => {
            if (err && err.status === 503) return { enabled: false, credentials: [] };
            throw err;
          }),
      ]);
      state.accountInfo = info || null;
      state.accountSessions = sessions && Array.isArray(sessions.sessions) ? sessions.sessions : [];
      state.accountKeys = keys;
      renderMine();
    } catch (err) {
      if (!silent) throw err;
    }
  }

  function renderMine() {
    const info = state.accountInfo;
    const twoFactor = Boolean(info && info.two_factor);
    const pending = state.account2faPending;
    $('mine-2fa-off').classList.toggle('hidden', twoFactor || Boolean(pending));
    $('mine-2fa-pending').classList.toggle('hidden', twoFactor || !pending);
    $('mine-2fa-on').classList.toggle('hidden', !twoFactor);
    $('mine-2fa-state').textContent = twoFactor ? '已启用' : (pending ? '待确认' : '未启用');

    const user = info && info.user ? info.user : {};
    const keys = state.accountKeys;
    const summary = $('mine-summary');
    clear(summary);
    const rows = [
      ['用户名', user.username || (state.auth && state.auth.username) || '—'],
      ['角色', rolePill(user.role || (state.auth && state.auth.role) || '')],
      ['邮箱', user.email || '—'],
      ['手机', user.phone || '—'],
      ['两步验证', twoFactor ? '已启用' : '未启用'],
      ['恢复码', twoFactor ? (Number(info.recovery_codes) || 0) + ' 枚未使用' : '—'],
      ['安全密钥', keys && keys.enabled ? (keys.credentials.length ? keys.credentials.length + ' 枚' : '未登记') : '—'],
    ];
    rows.forEach(([label, value]) => {
      summary.appendChild(el('dt', '', label));
      const dd = el('dd');
      if (value instanceof Node) dd.appendChild(value);
      else dd.textContent = String(value);
      summary.appendChild(dd);
    });

    if (twoFactor) {
      $('mine-2fa-recovery-note').textContent = (Number(info.recovery_codes) || 0)
        ? '恢复码剩余 ' + info.recovery_codes + ' 枚。'
        : '还没有恢复码；建议生成一组，验证器丢失时用它登录。';
    }
    $('mine-email').value = user.email || '';
    $('mine-phone').value = user.phone || '';
    if (pending) {
      $('mine-2fa-secret').textContent = pending.secret;
      $('mine-2fa-uri').textContent = pending.otpauth_uri;
    }
    renderMineKeys();
    renderMineSessions();
  }

  function keyAlgorithmLabel(alg) {
    switch (Number(alg)) {
      case -7: return 'ES256';
      case -257: return 'RS256';
      case -8: return 'EdDSA';
      default: return 'alg ' + (alg || 0);
    }
  }

  function renderMineKeys() {
    const keys = state.accountKeys;
    const enabled = Boolean(keys && keys.enabled);
    $('mine-keys-on').classList.toggle('hidden', !enabled);
    $('mine-keys-off').classList.toggle('hidden', enabled);
    $('mine-keys-state').textContent = enabled
      ? (keys.credentials.length ? keys.credentials.length + ' 枚' : '未登记')
      : '服务器未配置';
    const body = $('mine-key-rows');
    clear(body);
    if (!enabled) return;
    keys.credentials.forEach((cred) => body.appendChild(mineKeyRow(cred)));
    $('mine-key-empty').classList.toggle('hidden', keys.credentials.length !== 0);
  }

  function mineKeyRow(cred) {
    const tr = document.createElement('tr');
    tr.appendChild(cell(cred.name || '未命名安全密钥'));
    tr.appendChild(cell(keyAlgorithmLabel(cred.alg)));
    tr.appendChild(cell(fmtTime(cred.created_at_ms)));
    tr.appendChild(cell(cred.last_used_at_ms ? fmtTime(cred.last_used_at_ms) : '—'));
    const actions = el('div', 'actions-wrap');
    const remove = button('删除', 'small danger');
    remove.addEventListener('click', () => withButton(remove, async () => {
      const password = $('mine-key-password').value;
      if (!password) {
        toast('请先在下方输入当前密码，再删除安全密钥', true);
        return;
      }
      if (!window.confirm('删除安全密钥“' + (cred.name || '未命名安全密钥') + '”？删除后它将不能再用于登录。')) return;
      try {
        await api('/v1/account/webauthn/credentials/remove', {
          method: 'POST',
          body: { password, credential_id: cred.id },
        });
      } catch (err) {
        if (err.status === 403) throw new ApiError('当前密码不正确。', 403);
        if (err.status === 404) throw new ApiError('该安全密钥已不存在。', 404);
        throw err;
      }
      $('mine-key-password').value = '';
      toast('安全密钥已删除');
      await loadMine();
    }));
    actions.appendChild(remove);
    tr.appendChild(cell(actions));
    return tr;
  }

  async function registerMineSecurityKey(event) {
    event.preventDefault();
    const keys = state.accountKeys;
    if (!keys || !keys.enabled) {
      toast('服务器未配置 WebAuthn，无法登记安全密钥', true);
      return;
    }
    const password = $('mine-key-password').value;
    if (!password) {
      toast('请输入当前密码', true);
      return;
    }
    let begin;
    try {
      begin = await api('/v1/account/webauthn/register/begin', {
        method: 'POST',
        body: { password },
      });
    } catch (err) {
      if (err.status === 403) throw new ApiError('当前密码不正确。', 403);
      if (err.status === 503) throw new ApiError('服务器未配置 WebAuthn。', 503);
      throw err;
    }
    if (!begin || !begin.challenge || !begin.rp_id || !begin.user_id) {
      throw new Error('服务器返回的安全密钥参数不完整');
    }
    let credential = null;
    try {
      credential = await navigator.credentials.create({
        publicKey: {
          challenge: base64urlBytes(begin.challenge),
          rp: { id: begin.rp_id, name: 'chast' },
          user: {
            id: base64urlBytes(begin.user_id),
            name: begin.username,
            displayName: begin.display_name || begin.username,
          },
          pubKeyCredParams: [
            { type: 'public-key', alg: -7 },
            { type: 'public-key', alg: -257 },
            { type: 'public-key', alg: -8 },
          ],
          timeout: Number(begin.timeout_ms) || 60000,
          attestation: 'none',
          authenticatorSelection: { userVerification: 'preferred', residentKey: 'preferred' },
          excludeCredentials: (begin.exclude_credentials || []).map((cred) => ({
            type: 'public-key',
            id: base64urlBytes(cred.id),
          })),
        },
      });
    } catch (err) {
      if (err && err.name === 'NotAllowedError') {
        toast('已取消或超时，未登记安全密钥', true);
        return;
      }
      if (err && err.name === 'InvalidStateError') {
        toast('这枚安全密钥已经登记过了', true);
        return;
      }
      throw new Error('安全密钥失败：' + ((err && err.message) || String(err)));
    }
    if (!credential || !credential.response) {
      toast('浏览器没有返回安全密钥凭据', true);
      return;
    }
    const name = $('mine-key-name').value.trim().slice(0, 64);
    try {
      await api('/v1/account/webauthn/register/finish', {
        method: 'POST',
        body: {
          name,
          client_data_json: bytesBase64url(credential.response.clientDataJSON),
          attestation_object: bytesBase64url(credential.response.attestationObject),
        },
      });
    } catch (err) {
      if (err.status === 403) throw new ApiError('安全密钥验证未被接受，请重试。', 403);
      if (err.status === 409) throw new ApiError('这枚安全密钥已经登记过了。', 409);
      throw err;
    }
    $('mine-key-password').value = '';
    $('mine-key-name').value = '';
    toast('安全密钥已登记');
    await loadMine();
  }

  function renderMineSessions() {
    const body = $('mine-session-rows');
    clear(body);
    state.accountSessions.forEach((session) => body.appendChild(mineSessionRow(session)));
    $('mine-session-empty').classList.toggle('hidden', state.accountSessions.length !== 0);
    $('mine-sessions-revoke-others').disabled =
      !state.accountSessions.some((session) => !session.current);
  }

  function mineSessionRow(session) {
    const tr = document.createElement('tr');
    tr.appendChild(cell(session.device_name || session.device_id || '未知设备'));
    tr.appendChild(cell(session.login_ip || '—'));
    tr.appendChild(cell(session.location || '—'));
    tr.appendChild(cell(fmtTime(session.created_at_ms)));
    tr.appendChild(cell(fmtTime(session.expires_at_ms)));
    tr.appendChild(cell(session.current ? pill('当前会话', 'ok') : pill('其他设备', '')));

    const actions = el('div', 'actions-wrap');
    const rename = button('重命名', 'small');
    rename.addEventListener('click', () => withButton(rename, async () => {
      const name = window.prompt('设备名称（用于在会话列表里区分设备）', session.device_name || '');
      if (name === null) return;
      const trimmed = name.trim();
      if (!trimmed) {
        toast('设备名称不能为空', true);
        return;
      }
      await api('/v1/account/sessions/' + encodeURIComponent(session.id), {
        method: 'PATCH',
        body: { device_name: trimmed.slice(0, 64) },
      });
      toast('已重命名会话');
      await loadMine();
    }));
    actions.appendChild(rename);
    if (!session.current) {
      const revoke = button('撤销', 'small danger');
      revoke.addEventListener('click', () => withButton(revoke, async () => {
        if (!window.confirm('撤销这台设备的登录会话？')) return;
        await api('/v1/account/sessions/' + encodeURIComponent(session.id), { method: 'DELETE' });
        toast('已撤销会话');
        await loadMine();
      }));
      actions.appendChild(revoke);
    }
    tr.appendChild(cell(actions));
    return tr;
  }

  async function saveMineContact(kind, clear, event) {
    if (event) event.preventDefault();
    const isEmail = kind === 'email';
    const value = $(isEmail ? 'mine-email' : 'mine-phone').value.trim();
    const password = $(isEmail ? 'mine-email-password' : 'mine-phone-password').value;
    if (!password) {
      toast('请输入当前密码', true);
      return;
    }
    if (!clear && !value) {
      toast(isEmail ? '请输入找回邮箱' : '请输入找回手机', true);
      return;
    }
    if (clear && !window.confirm(isEmail ? '清除找回邮箱？' : '清除找回手机？')) return;
    try {
      await api(isEmail ? '/v1/account/email' : '/v1/account/phone', {
        method: 'POST',
        body: isEmail
          ? { email: clear ? '' : value, password }
          : { phone: clear ? '' : value, password },
      });
    } catch (err) {
      if (err.status === 403) throw new ApiError('当前密码不正确。', 403);
      if (err.status === 409) {
        throw new ApiError(isEmail ? '该邮箱已被其他账号使用。' : '该手机号已被其他账号使用。', 409);
      }
      if (err.status === 400) {
        throw new ApiError((isEmail ? '邮箱' : '手机号') + '格式不正确：' + err.message, 400);
      }
      throw err;
    }
    $(isEmail ? 'mine-email-password' : 'mine-phone-password').value = '';
    if (clear) toast(isEmail ? '已清除找回邮箱' : '已清除找回手机');
    else toast(isEmail ? '已保存找回邮箱' : '已保存找回手机');
    await loadMine();
  }

  async function changeMinePassword(event) {
    event.preventDefault();
    const current = $('mine-password-current').value;
    const next = $('mine-password-new').value;
    if (!current || !next) {
      toast('请填写当前密码与新密码', true);
      return;
    }
    if (next !== $('mine-password-confirm').value) {
      toast('两次输入的新密码不一致', true);
      return;
    }
    try {
      await api('/v1/account/password', {
        method: 'POST',
        body: { current_password: current, new_password: next },
      });
    } catch (err) {
      if (err.status === 403) throw new ApiError('当前密码不正确。', 403);
      if (err.status === 400) throw new ApiError('新密码不符合要求：' + err.message, 400);
      throw err;
    }
    $('mine-password-current').value = '';
    $('mine-password-new').value = '';
    $('mine-password-confirm').value = '';
    toast('密码已修改，其他会话已撤销');
    await loadMine();
  }

  async function startMineTwoFactor(event) {
    event.preventDefault();
    const password = $('mine-2fa-password').value;
    if (!password) {
      toast('请输入当前密码', true);
      return;
    }
    let data;
    try {
      data = await api('/v1/account/2fa/start', { method: 'POST', body: { password } });
    } catch (err) {
      if (err.status === 403) throw new ApiError('当前密码不正确。', 403);
      if (err.status === 409) throw new ApiError('两步验证已启用。', 409);
      throw err;
    }
    $('mine-2fa-password').value = '';
    state.account2faPending = {
      secret: (data && data.secret) || '',
      otpauth_uri: (data && data.otpauth_uri) || '',
    };
    renderMine();
  }

  function cancelMineTwoFactor() {
    state.account2faPending = null;
    $('mine-2fa-enable-password').value = '';
    $('mine-2fa-enable-code').value = '';
    renderMine();
  }

  async function enableMineTwoFactor(event) {
    event.preventDefault();
    const password = $('mine-2fa-enable-password').value;
    const code = $('mine-2fa-enable-code').value.trim();
    if (!password || !code) {
      toast('请输入当前密码与验证码', true);
      return;
    }
    try {
      await api('/v1/account/2fa/enable', { method: 'POST', body: { password, code } });
    } catch (err) {
      if (err.status === 400) throw new ApiError('绑定已失效，请重新开始。', 400);
      if (err.status === 403) throw new ApiError('当前密码或验证码不正确。', 403);
      throw err;
    }
    state.account2faPending = null;
    $('mine-2fa-enable-password').value = '';
    $('mine-2fa-enable-code').value = '';
    $('mine-2fa-password').value = '';
    toast('两步验证已启用');
    await loadMine();
  }

  async function disableMineTwoFactor(event) {
    event.preventDefault();
    const password = $('mine-2fa-disable-password').value;
    const code = $('mine-2fa-disable-code').value.trim();
    if (!password || !code) {
      toast('请输入当前密码与验证码', true);
      return;
    }
    try {
      await api('/v1/account/2fa/disable', { method: 'POST', body: { password, code } });
    } catch (err) {
      if (err.status === 400) throw new ApiError('两步验证未启用。', 400);
      if (err.status === 403) throw new ApiError('密码或验证码不正确。', 403);
      throw err;
    }
    $('mine-2fa-disable-password').value = '';
    $('mine-2fa-disable-code').value = '';
    $('mine-recovery-list').textContent = '';
    $('mine-recovery-codes').classList.add('hidden');
    toast('两步验证已停用');
    await loadMine();
  }

  async function generateMineRecoveryCodes(event) {
    event.preventDefault();
    const password = $('mine-recovery-password').value;
    const code = $('mine-recovery-code').value.trim();
    if (!password || !code) {
      toast('请输入当前密码与验证码', true);
      return;
    }
    let data;
    try {
      data = await api('/v1/account/2fa/recovery', { method: 'POST', body: { password, code } });
    } catch (err) {
      if (err.status === 400) throw new ApiError('两步验证未启用。', 400);
      if (err.status === 403) throw new ApiError('密码或验证码不正确。', 403);
      throw err;
    }
    $('mine-recovery-password').value = '';
    $('mine-recovery-code').value = '';
    const codes = data && Array.isArray(data.codes) ? data.codes : [];
    $('mine-recovery-list').textContent = codes.join('\n');
    $('mine-recovery-codes').classList.toggle('hidden', codes.length === 0);
    toast('已生成 ' + codes.length + ' 枚恢复码');
    await loadMine();
  }

  // downloadMineRecoveryCodes saves the one-time list as a text file so it can
  // be printed or stored offline before the panel is hidden again.
  function downloadMineRecoveryCodes() {
    const text = $('mine-recovery-list').textContent;
    if (!text.trim()) {
      toast('没有可下载的恢复码', true);
      return;
    }
    const username = state.auth.username || 'account';
    const header = 'chast recovery codes for ' + username +
      '\nEach code works once. Keep this file offline.\n\n';
    const blob = new Blob([header + text + '\n'], { type: 'text/plain' });
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = 'chast-recovery-codes-' + username + '.txt';
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(link.href);
    toast('恢复码已下载');
  }

  async function revokeOtherMineSessions() {
    const others = state.accountSessions.filter((session) => !session.current).length;
    if (!others) {
      toast('没有其他会话', true);
      return;
    }
    if (!window.confirm('撤销其他 ' + others + ' 个登录会话？')) return;
    const data = await api('/v1/account/sessions/revoke-others', { method: 'POST' });
    toast('已撤销 ' + ((data && data.revoked) || 0) + ' 个会话');
    await loadMine();
  }

  // ---------- boot ----------

  function boot() {
    bindEvents();
    syncLoginMode();
    probeSingleSignOn();
    $('login-note').textContent = '使用服务器密钥，或以账号登录：readonly / operator / admin 可使用完整管理台，仅有分组授权的账号只能查看所属分组；已注册安全密钥的账号可点“使用安全密钥登录”，服务器配置了单点登录时还可点“使用单点登录”。';
    const saved = readSession();
    if (!saved) return;
    state.auth = saved;
    const probe = saved.mode === 'key' || ROLE_RANK[saved.role] ? '/v1/stats' : '/v1/groups';
    api(probe).then(() => enterConsole()).catch((err) => {
      if (err && err.status === 401) return;
      lock('自动连接失败：' + (err && err.message ? err.message : err));
    });
  }

  boot();
})();
