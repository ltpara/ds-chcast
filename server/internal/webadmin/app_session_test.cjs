const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return {promise, resolve};
}
function reply(status, value) {
  return {status, ok: status < 300, statusText: '', text: async () => JSON.stringify(value)};
}
function app(fetch) {
  const saved = [];
  const runtime = {fetch, sessionStorage: {setItem: (_, value) => saved.push(value), removeItem() {}}, exported: {}, locks: 0};
  const source = fs.readFileSync(path.join(__dirname, 'assets/app.js'), 'utf8').replace(/\r\n/g, '\n');
  // Run the actual API/session functions without booting the DOM. Stubs only
  // remove rendering side effects; request and refresh implementations remain.
  assert.ok(source.includes('  boot();\n})();'));
  vm.runInNewContext(source.replace('  boot();\n})();', `
    renderSession = () => {};
    applyPermissions = () => {};
    // Keep the real lock cleanup available to role/session regressions.
    const realLock = lock;
    lock = () => { locks++; state.auth = null; };
    Object.assign(exported, {state, api, refreshSession, connectionConfirmedDisconnected, connectionRequestKeyFor, releaseConnectionRequestKeyIfSettled, loadRoles, fmtTime, realLock, resetRoleConsole, renderPermissionOptions, renderRoles, renderRoleBindings, renderRoleSelects, applyRoleControlPermissions});
  })();`), runtime);
  return {...runtime.exported, runtime, saved};
}

test('fmtTime renders millisecond and RFC3339 API timestamps', () => {
  const loaded = app(async () => reply(200, {}));
  assert.equal(loaded.fmtTime(undefined), '—');
  assert.equal(loaded.fmtTime(null), '—');
  assert.equal(loaded.fmtTime(''), '—');
  assert.equal(loaded.fmtTime('not a timestamp'), '—');
  assert.equal(loaded.fmtTime(0), '—');
  assert.notEqual(loaded.fmtTime(1763320255000), '—');
  assert.notEqual(loaded.fmtTime('2026-11-16T21:50:55Z'), '—');
});

test('late 401 cannot retry an old write using the newly signed-in account', async () => {
  const response = deferred();
  const calls = [];
  const loaded = app(async (url, options) => {
    calls.push({url, options});
    if (calls.length === 1) return response.promise;
    return reply(200, {token:'new-rotated', refresh_token:'new-refresh-rotated'});
  });
  loaded.state.auth = {mode:'account', token:'old', refresh:'old-refresh'};
  const operation = loaded.api('/v1/policies/one/client-strategy', {method:'PUT', body:{if_revision:0, settings:{}}});
  loaded.state.auth = {mode:'account', token:'new', refresh:'new-refresh'};
  response.resolve(reply(401, {}));
  await assert.rejects(operation, /登录身份已变化/);
  assert.equal(calls.length, 1);
  assert.equal(loaded.runtime.locks, 0);
  assert.equal(loaded.state.auth.token, 'new');
});

test('refresh completing after logout cannot restore persisted credentials', async () => {
  const response = deferred();
  const loaded = app(() => response.promise);
  loaded.state.auth = {mode:'account', token:'old', refresh:'refresh'};
  const refreshing = loaded.refreshSession();
  loaded.state.auth = null;
  response.resolve(reply(200, {token:'rotated', refresh_token:'rotated-refresh'}));
  assert.equal(await refreshing, false);
  assert.deepEqual(loaded.saved, []);
});

test('simultaneous expired requests share one refresh rotation', async () => {
  const refreshResponse = deferred();
  let refreshCalls = 0;
  const loaded = app(async (url, options) => {
    if (url === '/v1/account/refresh') { refreshCalls++; return refreshResponse.promise; }
    return reply(options.headers.Authorization === 'Bearer fresh' ? 200 : 401, {ok:true});
  });
  loaded.state.auth = {mode:'account', token:'expired', refresh:'refresh'};
  const first = loaded.api('/v1/policies');
  const second = loaded.api('/v1/client-strategies/catalog');
  await new Promise(setImmediate);
  assert.equal(refreshCalls, 1);
  refreshResponse.resolve(reply(200, {token:'fresh', refresh_token:'next'}));
  await Promise.all([first, second]);
  assert.equal(loaded.runtime.locks, 0);
});

test('a stale role response is rejected before it can render after account switch', async () => {
  const roleResponse = deferred();
  const bindingResponse = deferred();
  const loaded = app((url) => {
    if (url === '/v1/admin-roles') return roleResponse.promise;
    if (url === '/v1/role-bindings') return bindingResponse.promise;
    return reply(200, {});
  });
  const calls = {permissions: 0, roles: 0, bindings: 0, selects: 0};
  loaded.renderPermissionOptions = () => { calls.permissions++; };
  loaded.renderRoles = () => { calls.roles++; };
  loaded.renderRoleBindings = () => { calls.bindings++; };
  loaded.renderRoleSelects = () => { calls.selects++; };
  loaded.applyRoleControlPermissions = () => {};
  loaded.state.auth = {mode:'account', token:'old', refresh:'old-refresh', permissions:['roles.read']};
  const loading = loaded.loadRoles();
  loaded.state.auth = {mode:'account', token:'new', refresh:'new-refresh', permissions:['roles.read']};
  roleResponse.resolve(reply(200, {roles:[{id:'old-role', permissions:[]}], permissions:[]}));
  bindingResponse.resolve(reply(200, {bindings:[{id:'old-binding', username:'old-user', role_id:'old-role'}]}));
  await assert.rejects(loading, /登录身份已变化/);
  assert.equal(loaded.state.roles.length, 0);
  assert.equal(loaded.state.roleBindings.length, 0);
  assert.deepEqual(calls, {permissions:0, roles:0, bindings:0, selects:0});
});

test('logout role state reset clears in-flight data for the next account', () => {
  const loaded = app(async () => reply(200, {}));
  loaded.state.auth = {mode:'account', token:'old', refresh:'old-refresh', permissions:['roles.read']};
  loaded.state.rolesLoading = true;
  loaded.state.roles = [{id:'old-role'}];
  loaded.state.roleBindings = [{id:'old-binding'}];
  loaded.state.roleEditing = {id:'old-role'};
  loaded.resetRoleConsole();
  assert.equal(loaded.state.rolesLoading, false);
  assert.equal(loaded.state.roles.length, 0);
  assert.equal(loaded.state.roleBindings.length, 0);
  assert.equal(loaded.state.roleEditing, null);
});

test('live connection success claims require closed, applied and confirmed absence', async () => {
  const loaded = app(async () => reply(200, {}));
  const confirmed = loaded.connectionConfirmedDisconnected;
  assert.ok(confirmed, 'the console exposes the confirmation predicate for testing');
  assert.equal(confirmed({state:'closed', command_status:'applied', receipt_confirmed_absent:true}), true);
  // A closed row alone is not proof; neither is an applied command without the
  // host's confirmed_absent receipt, nor a stale heartbeat.
  assert.equal(confirmed({state:'closed', command_status:'applied', receipt_confirmed_absent:false}), false);
  assert.equal(confirmed({state:'closed', command_status:'applied'}), false);
  assert.equal(confirmed({state:'closed'}), false);
  assert.equal(confirmed({state:'stale', command_status:'applied', receipt_confirmed_absent:true}), false);
  assert.equal(confirmed({state:'active', command_status:'applied', receipt_confirmed_absent:true}), false);
  assert.equal(confirmed(null), false);
});

test('a settled failed or expired disconnect releases the retry key', async () => {
  const loaded = app(async () => reply(200, {}));
  const conn = {host_id:'devbox', connection_id:'a'.repeat(32)};
  loaded.state.connectionRequestKey = {connection:'devbox/' + conn.connection_id, key:'request-key'};
  loaded.releaseConnectionRequestKeyIfSettled({...conn, command_status:'queued'});
  assert.equal(loaded.state.connectionRequestKey.key, 'request-key');
  loaded.releaseConnectionRequestKeyIfSettled({...conn, command_status:'failed'});
  assert.equal(loaded.state.connectionRequestKey, null);

  loaded.state.connectionRequestKey = {connection:'devbox/' + conn.connection_id, key:'request-key'};
  loaded.releaseConnectionRequestKeyIfSettled({...conn, command_status:'expired'});
  assert.equal(loaded.state.connectionRequestKey, null);

  const confirmed = {...conn, state:'closed', command_status:'applied', receipt_confirmed_absent:true};
  loaded.state.connectionRequestKey = {connection:'devbox/' + conn.connection_id, key:'request-key'};
  loaded.releaseConnectionRequestKeyIfSettled(confirmed);
  assert.equal(loaded.state.connectionRequestKey, null);
});

test('one disconnect action reuses its request key for a retried request', async () => {
  const sent = [];
  const loaded = app(async (url, options) => {
    sent.push({url, body: options && options.body ? JSON.parse(options.body) : null});
    if (sent.length === 1) return reply(500, {error:'temporary failure'});
    return reply(202, {command_id:'cmd-1', status:'queued'});
  });
  const conn = {host_id:'devbox', connection_id:'a'.repeat(32)};
  const first = loaded.connectionRequestKeyFor(conn);
  const second = loaded.connectionRequestKeyFor(conn);
  assert.equal(first, second);
  assert.ok(first.length >= 8 && first.length <= 128);
  // A different target gets its own key so two viewers are never conflated.
  const other = loaded.connectionRequestKeyFor(
    {host_id:'devbox', connection_id:'b'.repeat(32)});
  assert.notEqual(first, other);
});
