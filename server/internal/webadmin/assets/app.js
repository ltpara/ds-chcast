'use strict';

(() => {
  const STORAGE_KEY = 'chast.admin.session';
  const LEGACY_KEY = 'chast.admin.key';
  const REFRESH_MS = 15000;
  const ROLE_RANK = { readonly: 1, operator: 2, admin: 3 };
  const GROUP_ROLE_RANK = { readonly: 1, write: 2, admin: 3 };

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
    auditTab: 'events',
    auditBefore: 0,
    auditRows: [],
    auditGen: 0,
    recordingBefore: 0,
    recordingRows: [],
    recordingGen: 0,
    groups: [],
    groupsDisabled: false,
    groupID: '',
    group: null,
    groupMembers: [],
    groupDevices: [],
    groupGen: 0,
    auditGroupID: '',
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

  function routeLabel(route) {
    switch (route) {
      case 'direct': return '直连';
      case 'relay': return '中继';
      case 'carrier': return '载体';
      case undefined:
      case null:
      case '': return '—';
      default: return String(route);
    }
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
    enterConsole();
  }

  async function unlockAccount(username, password, code) {
    const body = { username, password };
    if (code) {
      // Six digits is a TOTP code; anything else is treated as a printed
      // recovery code so a lost authenticator does not lock operators out.
      if (/^\d{6}$/.test(code)) body.totp_code = code;
      else body.recovery_code = code;
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
          : (data && data.error) || '用户名或密码不正确。';
      throw new ApiError(message, 401);
    }
    if (!resp.ok) {
      throw new ApiError((data && data.error) || resp.status + ' ' + resp.statusText, resp.status);
    }
    const user = data && data.user ? data.user : {};
    if (!ROLE_RANK[user.role]) {
      // The viewer session is valid but useless here; revoke it right away.
      fetch('/v1/account/logout', {
        method: 'POST',
        headers: { Authorization: 'Bearer ' + data.token },
        cache: 'no-store',
      }).catch(() => {});
      throw new ApiError('该账号没有管理台权限（需要 readonly、operator 或 admin 角色）。', 403);
    }
    state.auth = {
      mode: 'account',
      token: data.token,
      refresh: data.refresh_token,
      username: user.username || username,
      role: user.role,
    };
    writeSession(state.auth);
    enterConsole();
  }

  function enterConsole() {
    $('login-form').classList.add('hidden');
    $('session-bar').classList.remove('hidden');
    $('login-note').textContent = '';
    $('workspace').classList.remove('hidden');
    renderSession();
    applyPermissions();
    startTimer();
    switchTab('overview');
  }

  function renderSession() {
    const auth = state.auth;
    if (!auth) return;
    const role = auth.mode === 'key' ? 'server-key' : auth.role;
    const pill = $('session-role');
    pill.textContent = role;
    pill.className = 'pill ' + (role === 'admin' || role === 'server-key' ? 'ok'
      : role === 'operator' ? 'warn' : '');
    $('session-label').textContent = auth.mode === 'key' ? '服务器密钥' : auth.username;
  }

  function applyPermissions() {
    const admin = can('admin');
    const write = can('operator');
    document.querySelector('.tab[data-tab="accounts"]').classList.toggle('hidden', !admin);
    $('group-create-panel').classList.toggle('hidden', !admin);
    $('device-readonly').classList.toggle('hidden', write);
    $('recording-readonly').classList.toggle('hidden', write);
    if (!admin && state.tab === 'accounts') switchTab('overview');
  }

  function lock(message) {
    stopTimer();
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
    state.groups = [];
    state.groupsDisabled = false;
    state.auditGroupID = '';
    clearGroupDetail();
    const dialog = $('device-dialog');
    if (dialog.open) dialog.close();
    clearSession();
    $('workspace').classList.add('hidden');
    $('session-bar').classList.add('hidden');
    $('login-form').classList.remove('hidden');
    $('server-key').value = '';
    $('account-password').value = '';
    $('account-totp').value = '';
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
    else if (tab === 'accounts') loadAccounts().catch(reportError);
    else if (tab === 'groups') loadGroups().catch(reportError);
    else if (tab === 'recordings') loadRecordings(true).catch(reportError);
    else if (tab === 'audit') {
      if (!state.groups.length) loadGroups(true).catch(() => {});
      syncAuditControls();
      loadAudit(true).catch(reportError);
    }
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
      params.set('limit', '1000');
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
    const total = data.total === undefined ? peers.length : data.total;
    const online = data.online === undefined ? 0 : data.online;
    let summary = '共 ' + total + ' 台，在线 ' + online + ' 台';
    if (peers.length < total) summary += '（显示前 ' + peers.length + ' 台）';
    $('device-summary').textContent = summary;

    const body = $('device-rows');
    clear(body);
    peers.forEach((peer) => body.appendChild(deviceRow(peer)));
    $('device-empty').classList.toggle('hidden', peers.length > 0);
  }

  function deviceRow(peer) {
    const tr = document.createElement('tr');

    const id = cell(peer.id);
    id.className = 'mono';
    if (peer.fingerprint) id.title = '指纹 ' + peer.fingerprint;
    tr.appendChild(id);

    tr.appendChild(cell(peer.alias || '—'));

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
    if (peer.metadata_signed) {
      const signed = pill('已签名', 'ok');
      signed.title = '主机名、平台与版本已由主机密钥签名';
      tr.appendChild(cell(signed));
    } else {
      const unsigned = pill('未签名');
      unsigned.title = '旧版主机或注册时未携带 metadata_sig';
      tr.appendChild(cell(unsigned));
    }
    tr.appendChild(cell(peer.online ? pill('在线', 'ok') : pill('离线')));

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
    $('device-tags').value = Array.isArray(peer.tags) ? peer.tags.join(', ') : '';
    $('device-dialog').showModal();
    $('device-alias').focus();
  }

  async function saveDeviceLabels() {
    const peer = state.editing;
    if (!peer) return;
    const alias = $('device-alias').value.trim();
    const tags = $('device-tags').value.split(',')
      .map((tag) => tag.trim())
      .filter((tag) => tag.length > 0);
    await api('/v1/identities/' + encodeURIComponent(peer.id), {
      method: 'PATCH',
      body: { alias, tags },
    });
    $('device-dialog').close();
    state.editing = null;
    toast('已保存 ' + peer.id);
    await loadDevices();
  }

  // ---------- accounts ----------

  async function loadAccounts() {
    const data = await api('/v1/accounts');
    renderAccounts(Array.isArray(data && data.users) ? data.users : []);
  }

  function renderAccounts(users) {
    const body = $('account-rows');
    clear(body);
    users.forEach((user) => body.appendChild(accountRow(user)));
    $('account-empty').classList.toggle('hidden', users.length > 0);
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

    const actions = el('div', 'actions-wrap');

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

  // ---------- access groups ----------

  function groupRole() {
    if (can('admin')) return 'admin';
    return state.group && state.group.role ? state.group.role : '';
  }

  function groupCan(level) {
    return (GROUP_ROLE_RANK[groupRole()] || 0) >= GROUP_ROLE_RANK[level];
  }

  function fmtQuota(count, max) {
    return (count || 0) + ' / ' + (max ? max : '不限');
  }

  function fmtGroupTime(value) {
    const ms = Date.parse(value);
    return Number.isFinite(ms) ? fmtTime(ms) : '—';
  }

  function quotaValue(id) {
    const raw = $(id).value.trim();
    const value = raw === '' ? 0 : Number(raw);
    if (!Number.isInteger(value) || value < 0) {
      throw new Error('配额必须是不小于 0 的整数。');
    }
    return value;
  }

  async function loadGroups(silent) {
    try {
      const data = await api('/v1/access-groups');
      state.groups = Array.isArray(data && data.groups) ? data.groups : [];
      state.groupsDisabled = false;
      if (state.groupID && !state.groups.some((group) => group.id === state.groupID)) {
        clearGroupDetail();
      }
      renderGroups();
      renderAuditGroupOptions();
      syncAuditControls();
      return state.groups;
    } catch (err) {
      if (err && err.status === 503) {
        state.groups = [];
        state.groupsDisabled = true;
        clearGroupDetail();
        renderGroups();
        renderAuditGroupOptions();
        syncAuditControls();
        return state.groups;
      }
      if (silent) return state.groups;
      throw err;
    }
  }

  function renderGroups() {
    const body = $('group-rows');
    clear(body);
    state.groups.forEach((group) => body.appendChild(groupRow(group)));
    const empty = $('group-empty');
    empty.textContent = state.groupsDisabled ? '服务端未启用账号系统。'
      : can('admin') ? '还没有访问组。' : '你还没有加入任何访问组。';
    empty.classList.toggle('hidden', state.groups.length > 0);
  }

  function groupRow(group) {
    const tr = document.createElement('tr');
    const name = cell(group.name);
    name.title = 'ID ' + group.id;
    tr.appendChild(name);

    const description = cell(group.description || '—');
    description.className = 'wrap';
    tr.appendChild(description);

    tr.appendChild(cell(fmtQuota(group.device_count, group.max_devices)));
    tr.appendChild(cell(fmtQuota(group.member_count, group.max_members)));

    let role = group.role || '';
    let roleLabel = role;
    let roleKind = role === 'admin' ? 'ok' : role === 'write' ? 'warn' : '';
    if (!role && can('admin')) {
      roleLabel = '全局管理员';
      roleKind = 'ok';
    }
    tr.appendChild(cell(roleLabel ? pill(roleLabel, roleKind) : '—'));
    tr.appendChild(cell(fmtGroupTime(group.created_at)));

    const actions = el('div', 'actions-wrap');
    const manage = button('管理', 'small');
    manage.addEventListener('click', () => openGroup(group.id));
    actions.appendChild(manage);
    if (can('admin')) {
      const remove = button('删除', 'small danger');
      remove.addEventListener('click', () => withButton(remove, () => deleteGroup(group)));
      actions.appendChild(remove);
    }
    tr.appendChild(cell(actions));
    return tr;
  }

  function openGroup(id) {
    state.groupID = id;
    $('group-detail').classList.remove('hidden');
    loadGroupDetail(id).catch(handleGroupDetailError);
  }

  function handleGroupDetailError(err) {
    if (err && (err.status === 403 || err.status === 404)) {
      const missing = err.status === 404;
      clearGroupDetail();
      loadGroups(true).catch(() => {});
      toast(missing ? '访问组已不存在。' : '你已不是该访问组成员。', true);
      return;
    }
    reportError(err);
  }

  function clearGroupDetail() {
    state.groupGen++;
    state.groupID = '';
    state.group = null;
    state.groupMembers = [];
    state.groupDevices = [];
    $('group-detail').classList.add('hidden');
  }

  async function loadGroupDetail(id) {
    const generation = ++state.groupGen;
    const base = '/v1/access-groups/' + encodeURIComponent(id);
    const [group, members, devices] = await Promise.all([
      api(base),
      api(base + '/members'),
      api(base + '/devices'),
    ]);
    if (generation !== state.groupGen) return;
    state.group = group || null;
    state.groupMembers = Array.isArray(members && members.members) ? members.members : [];
    state.groupDevices = Array.isArray(devices && devices.devices) ? devices.devices : [];
    renderGroupDetail();
  }

  async function refreshGroupViews() {
    await loadGroups(true);
    if (!state.groupID) return;
    try {
      await loadGroupDetail(state.groupID);
    } catch (err) {
      handleGroupDetailError(err);
    }
  }

  function renderGroupDetail() {
    const group = state.group;
    if (!group) return;
    const admin = groupCan('admin');
    const write = groupCan('write');
    $('group-detail').classList.remove('hidden');
    $('group-detail-title').textContent = group.name;

    const bits = [
      'ID ' + group.id,
      '设备 ' + fmtQuota(group.device_count, group.max_devices),
      '成员 ' + fmtQuota(group.member_count, group.max_members),
    ];
    const role = groupRole();
    if (role) bits.push('我的权限 ' + (can('admin') && !group.role ? '全局管理员' : role));
    $('group-detail-summary').textContent = bits.join(' · ');

    $('group-settings-form').classList.toggle('hidden', !admin);
    if (admin) {
      $('group-settings-name').value = group.name;
      $('group-settings-description').value = group.description || '';
      $('group-settings-max-devices').value = String(group.max_devices || 0);
      $('group-settings-max-members').value = String(group.max_members || 0);
    }
    $('group-member-form').classList.toggle('hidden', !admin);
    $('group-device-form').classList.toggle('hidden', !write);

    renderGroupMembers(admin);
    renderGroupDevices(write);
  }

  function renderGroupMembers(editable) {
    const body = $('group-member-rows');
    clear(body);
    state.groupMembers.forEach((member) => body.appendChild(groupMemberRow(member, editable)));
    $('group-member-empty').classList.toggle('hidden', state.groupMembers.length > 0);
  }

  function groupMemberRow(member, editable) {
    const tr = document.createElement('tr');
    const name = cell(member.username);
    name.className = 'mono';
    if (member.banned) name.title = '账号已封禁，其组角色不可用';
    tr.appendChild(name);

    const select = document.createElement('select');
    ['readonly', 'write', 'admin'].forEach((role) => {
      const option = document.createElement('option');
      option.value = role;
      option.textContent = role;
      if (member.role === role) option.selected = true;
      select.appendChild(option);
    });
    select.disabled = !editable;
    select.addEventListener('change', () => withButton(select, async () => {
      try {
        await api('/v1/access-groups/' + encodeURIComponent(state.groupID) + '/members/' +
          encodeURIComponent(member.username), {
          method: 'PUT',
          body: { role: select.value },
        });
        toast('已将 ' + member.username + ' 设为 ' + select.value);
      } finally {
        await refreshGroupViews();
      }
    }));
    tr.appendChild(cell(select));

    tr.appendChild(cell(fmtGroupTime(member.created_at)));
    tr.appendChild(cell(member.banned ? pill('已封禁', 'bad') : pill('正常', 'ok')));

    const actions = el('div', 'actions-wrap');
    if (editable) {
      const remove = button('移除', 'small danger');
      remove.addEventListener('click', () => withButton(remove, async () => {
        if (!window.confirm('从访问组移除 ' + member.username + '？')) return;
        await api('/v1/access-groups/' + encodeURIComponent(state.groupID) + '/members/' +
          encodeURIComponent(member.username), { method: 'DELETE' });
        toast('已移除 ' + member.username);
        await refreshGroupViews();
      }));
      actions.appendChild(remove);
    } else {
      actions.appendChild(el('span', 'muted', '只读'));
    }
    tr.appendChild(cell(actions));
    return tr;
  }

  function renderGroupDevices(editable) {
    const body = $('group-device-rows');
    clear(body);
    state.groupDevices.forEach((device) => body.appendChild(groupDeviceRow(device, editable)));
    $('group-device-empty').classList.toggle('hidden', state.groupDevices.length > 0);
  }

  function groupDeviceRow(device, editable) {
    const tr = document.createElement('tr');
    const id = cell(device.device_id);
    id.className = 'mono';
    tr.appendChild(id);
    tr.appendChild(cell(fmtGroupTime(device.created_at)));

    const actions = el('div', 'actions-wrap');
    if (editable) {
      const remove = button('移除', 'small danger');
      remove.addEventListener('click', () => withButton(remove, async () => {
        if (!window.confirm('从访问组移除设备 ' + device.device_id + '？')) return;
        await api('/v1/access-groups/' + encodeURIComponent(state.groupID) + '/devices/' +
          encodeURIComponent(device.device_id), { method: 'DELETE' });
        toast('已移除 ' + device.device_id);
        await refreshGroupViews();
      }));
      actions.appendChild(remove);
    } else {
      actions.appendChild(el('span', 'muted', '只读'));
    }
    tr.appendChild(cell(actions));
    return tr;
  }

  async function createGroup() {
    const submit = $('group-create').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      const group = await api('/v1/access-groups', {
        method: 'POST',
        body: {
          name: $('group-name').value.trim(),
          description: $('group-description').value.trim(),
          max_devices: quotaValue('group-max-devices'),
          max_members: quotaValue('group-max-members'),
        },
      });
      $('group-name').value = '';
      $('group-description').value = '';
      $('group-max-devices').value = '0';
      $('group-max-members').value = '0';
      toast('已创建访问组 ' + (group && group.name ? group.name : ''));
      await loadGroups(true);
      if (group && group.id) openGroup(group.id);
    });
  }

  async function saveGroupSettings() {
    const group = state.group;
    if (!group) return;
    const submit = $('group-settings-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      await api('/v1/access-groups/' + encodeURIComponent(group.id), {
        method: 'PATCH',
        body: {
          name: $('group-settings-name').value.trim(),
          description: $('group-settings-description').value.trim(),
          max_devices: quotaValue('group-settings-max-devices'),
          max_members: quotaValue('group-settings-max-members'),
        },
      });
      toast('已保存访问组设置');
      await refreshGroupViews();
    });
  }

  async function deleteGroup(group) {
    if (!window.confirm('删除访问组 ' + group.name + '？成员与设备分配会一并移除。')) return;
    await api('/v1/access-groups/' + encodeURIComponent(group.id), { method: 'DELETE' });
    if (state.auditGroupID === group.id) {
      state.auditGroupID = '';
      if (state.tab === 'audit') loadAudit(true).catch(reportError);
    }
    if (state.groupID === group.id) clearGroupDetail();
    toast('已删除访问组 ' + group.name);
    await loadGroups(true);
  }

  async function addGroupMember() {
    const group = state.group;
    if (!group) return;
    const username = $('group-member-name').value.trim();
    if (!username) return;
    const submit = $('group-member-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      await api('/v1/access-groups/' + encodeURIComponent(group.id) + '/members/' +
        encodeURIComponent(username), {
        method: 'PUT',
        body: { role: $('group-member-role').value },
      });
      $('group-member-name').value = '';
      toast('已添加 ' + username);
      await refreshGroupViews();
    });
  }

  async function addGroupDevice() {
    const group = state.group;
    if (!group) return;
    const deviceID = $('group-device-id').value.trim();
    if (!deviceID) return;
    const submit = $('group-device-form').querySelector('button[type="submit"]');
    await withButton(submit, async () => {
      const reply = await api('/v1/access-groups/' + encodeURIComponent(group.id) + '/devices', {
        method: 'POST',
        body: { device_id: deviceID },
      });
      $('group-device-id').value = '';
      toast(reply && reply.added ? '已分配 ' + deviceID : deviceID + ' 已在组内');
      await refreshGroupViews();
    });
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
      columns: ['时间', '用户', '动作', '详情', '客户端 IP', '结果'],
      row(event) {
        return [
          fmtTime(event.created_at_ms),
          event.username || '—',
          event.action,
          event.detail || '—',
          event.client_ip || '—',
          pillBool(event.success),
        ];
      },
    },
    connections: {
      path: '/v1/audit/connections',
      filterKey: 'username',
      filterPlaceholder: '用户名（可选）',
      hasHost: true,
      columns: ['开始', '用户', '对端', '方向', '来源', '客户端 IP', '地点', '路由', '时长', '结果', '详情'],
      row(event) {
        return [
          fmtTime(event.started_at_ms),
          event.username || '—',
          event.peer_id || '—',
          event.direction || '—',
          event.source || '—',
          event.client_ip || '—',
          event.location || '—',
          routeLabel(event.route),
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
      columns: ['时间', '用户', '对端', '方向', '文件', '大小', '客户端 IP', '地点', '路由', '结果', '详情'],
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
          routeLabel(event.route),
          pillBool(event.success),
          event.detail || '—',
        ];
      },
    },
  };

  function selectAuditTab(tab) {
    state.auditTab = tab;
    document.querySelectorAll('.subtab').forEach((node) => {
      node.classList.toggle('active', node.dataset.audit === tab);
    });
    const spec = AUDIT[tab];
    $('audit-filter').placeholder = spec.filterPlaceholder;
    $('audit-filter').value = '';
    $('audit-host').value = '';
    syncAuditControls();
    loadAudit(true).catch(reportError);
  }

  function auditGrouped() {
    return Boolean(state.auditGroupID) &&
      (state.auditTab === 'connections' || state.auditTab === 'files');
  }

  function auditPath(spec) {
    if (!auditGrouped()) return spec.path;
    return '/v1/access-groups/' + encodeURIComponent(state.auditGroupID) +
      (state.auditTab === 'connections' ? '/connections' : '/file-transfers');
  }

  function renderAuditGroupOptions() {
    const select = $('audit-group');
    clear(select);
    const all = document.createElement('option');
    all.value = '';
    all.textContent = '全部访问组';
    select.appendChild(all);
    state.groups.forEach((group) => {
      const option = document.createElement('option');
      option.value = group.id;
      option.textContent = group.name;
      select.appendChild(option);
    });
    if (state.auditGroupID && !state.groups.some((group) => group.id === state.auditGroupID)) {
      state.auditGroupID = '';
    }
    select.value = state.auditGroupID;
  }

  // syncAuditControls hides the per-user filters when a group scope is active,
  // because the group endpoints page by device membership only.
  function syncAuditControls() {
    const spec = AUDIT[state.auditTab];
    const grouped = auditGrouped();
    $('audit-group').classList.toggle('hidden', state.auditTab === 'events' || state.groups.length === 0);
    $('audit-filter').classList.toggle('hidden', grouped);
    $('audit-host').classList.toggle('hidden', grouped || !spec.hasHost);
    $('audit-csv').disabled = grouped;
    $('audit-csv').title = grouped ? '按访问组筛选暂不支持 CSV 导出' : '';
  }

  function auditParams(spec) {
    const params = new URLSearchParams();
    if (!auditGrouped()) {
      const filter = $('audit-filter').value.trim();
      if (filter) params.set(spec.filterKey, filter);
      if (spec.hasHost) {
        const host = $('audit-host').value.trim();
        if (host) params.set('host', host);
      }
    }
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
    const params = auditParams(spec);
    params.set('limit', '50');
    const cursor = state.auditBefore;
    if (cursor) params.set('before_id', String(cursor));
    const data = await api(auditPath(spec) + '?' + params.toString()) || {};
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
    let summary = '已显示 ' + state.auditRows.length + ' 条' +
      (state.auditBefore ? '，还有更多' : state.auditRows.length ? '，已到末尾' : '');
    if (auditGrouped()) {
      const group = state.groups.find((item) => item.id === state.auditGroupID);
      if (group) summary += ' · 访问组 ' + group.name;
    }
    $('audit-summary').textContent = summary;
  }

  async function exportAudit() {
    if (auditGrouped()) {
      toast('按访问组筛选暂不支持 CSV 导出', true);
      return;
    }
    const spec = AUDIT[state.auditTab];
    const params = auditParams(spec);
    params.set('format', 'csv');
    const target = auditPath(spec) + '?' + params.toString();
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
      if (resp.status === 401) lock('服务器密钥无效或已更改，请重新连接。');
      throw new ApiError(message, resp.status);
    }
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');
    anchor.href = url;
    anchor.download = 'chast-' + state.auditTab + '-' + stamp + '.csv';
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    toast('已导出 CSV');
  }

  // ---------- events ----------

  let deviceInputTimer = 0;

  function syncLoginMode() {
    const account = $('login-mode').value === 'account';
    $('server-key').classList.toggle('hidden', account);
    $('login-account').classList.toggle('hidden', !account);
    if (account) $('account-username').focus();
    else $('server-key').focus();
  }

  function bindEvents() {
    $('login-mode').addEventListener('change', syncLoginMode);
    $('login-form').addEventListener('submit', async (event) => {
      event.preventDefault();
      const mode = $('login-mode').value;
      try {
        if (mode === 'account') {
          const username = $('account-username').value.trim();
          const password = $('account-password').value;
          if (!username || !password) {
            $('login-note').textContent = '请输入用户名与密码。';
            return;
          }
          await unlockAccount(username, password, $('account-totp').value.trim());
          $('account-password').value = '';
          $('account-totp').value = '';
        } else {
          await unlockKey($('server-key').value.trim());
        }
        toast('已连接');
      } catch (err) {
        // A rejected server key has already been explained by lock().
        if (err.status === 401 && mode === 'key') return;
        const message = err.message || String(err);
        if (err.status === 401 || err.status === 403) {
          $('login-note').textContent = message;
          toast(message, true);
          return;
        }
        lock('无法连接：' + message);
        toast(message, true);
      }
    });

    $('logout').addEventListener('click', () => {
      lock('已断开连接。');
      toast('已断开');
    });

    document.querySelectorAll('.tab').forEach((node) => {
      node.addEventListener('click', () => switchTab(node.dataset.tab));
    });

    $('device-query').addEventListener('input', () => {
      clearTimeout(deviceInputTimer);
      deviceInputTimer = setTimeout(() => loadDevices().catch(reportError), 250);
    });
    $('device-online').addEventListener('change', () => loadDevices().catch(reportError));
    $('device-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadDevices();
      toast('已刷新');
    }));

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

    $('account-create').addEventListener('submit', (event) => {
      event.preventDefault();
      createAccount();
    });
    $('account-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadAccounts();
      toast('已刷新');
    }));

    $('group-create').addEventListener('submit', (event) => {
      event.preventDefault();
      createGroup();
    });
    $('group-refresh').addEventListener('click', (event) => withButton(event.currentTarget, async () => {
      await loadGroups();
      if (state.groupID) await loadGroupDetail(state.groupID);
      toast('已刷新');
    }));
    $('group-settings-form').addEventListener('submit', (event) => {
      event.preventDefault();
      saveGroupSettings();
    });
    $('group-member-form').addEventListener('submit', (event) => {
      event.preventDefault();
      addGroupMember();
    });
    $('group-device-form').addEventListener('submit', (event) => {
      event.preventDefault();
      addGroupDevice();
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
    $('audit-csv').addEventListener('click', (event) => withButton(event.currentTarget, exportAudit));
    $('audit-group').addEventListener('change', () => {
      state.auditGroupID = $('audit-group').value;
      syncAuditControls();
      loadAudit(true).catch(reportError);
    });
    [$('audit-filter'), $('audit-host')].forEach((input) => {
      input.addEventListener('keydown', (event) => {
        if (event.key === 'Enter') loadAudit(true).catch(reportError);
      });
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

  // ---------- boot ----------

  function boot() {
    bindEvents();
    syncLoginMode();
    $('login-note').textContent = '使用服务器密钥，或以 readonly / operator / admin 角色的账号登录。';
    const saved = readSession();
    if (!saved) return;
    state.auth = saved;
    api('/v1/stats').then(() => {
      enterConsole();
    }).catch((err) => {
      if (err && err.status === 401) return;
      lock('自动连接失败：' + (err && err.message ? err.message : err));
    });
  }

  boot();
})();
