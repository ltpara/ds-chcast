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
    lock = () => { locks++; state.auth = null; };
    Object.assign(exported, {state, api, refreshSession});
  })();`), runtime);
  return {...runtime.exported, runtime, saved};
}

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
