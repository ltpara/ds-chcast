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
function app(fetch, stubEntry = false) {
  const saved = [];
  const runtime = {fetch, stubEntry, sessionStorage: {setItem: (_, value) => saved.push(value), removeItem() {}}, exported: {}, locks: 0};
  const source = fs.readFileSync(path.join(__dirname, 'assets/app.js'), 'utf8').replace(/\r\n/g, '\n');
  // Run the actual API/session functions without booting the DOM. Stubs only
  // remove rendering side effects; request and refresh implementations remain.
  assert.ok(source.includes('  boot();\n})();'));
  vm.runInNewContext(source.replace('  boot();\n})();', `
    if (stubEntry) enterConsole = async () => {};
    renderSession = () => {};
    applyPermissions = () => {};
    // Keep the real lock cleanup available to role/session regressions.
    const realLock = lock;
    lock = () => { locks++; state.auth = null; };
    Object.assign(exported, {loadPolicies, resetPolicyConsole, openClientStrategy, saveClientStrategy, openClientStrategyStatus, openConnectionDetail, refreshConnectionDetail, resetConnectionDetail, requestDisconnect, renderConnectionDetail});
    Object.assign(exported, {state, api, adoptAccountSession, canPermission, canDeviceCollection, canDeviceAction, tabVisible, applySessionPermissions, refreshConsolePermissions, refreshSession, connectionConfirmedDisconnected, connectionRequestKeyFor, releaseConnectionRequestKeyIfSettled, loadRoles, fmtTime, realLock, resetRoleConsole, renderPermissionOptions, renderRoles, renderRoleBindings, renderRoleSelects, applyRoleControlPermissions, syncRoleBindingScope, mutateRoles, clearRolePanels, editRole, saveRole, reloadRoleEditor, resetRoleEditor, deleteRole, openRoleAction, closeRoleAction, confirmRoleAction, roleActionKeydown, updateTabSelection, handleTabKeydown, initializeTableRegions});
  })();`), runtime);
  return {...runtime.exported, runtime, saved};
}

// Only the DOM surface is simulated. The actual role rendering, permission
// updates, request ownership and write completion functions run unchanged.
function roleDOM(loaded) {
  class Element {
    constructor() {
      this.children = [];
      this.value = '';
      this.disabled = false;
      this.checked = false;
      this.dataset = {};
      this.attributes = {};
      this.classList = {add() {}, remove() {}, toggle() {}};
    }
    appendChild(child) { this.children.push(child); }
    removeChild(child) { this.children.splice(this.children.indexOf(child), 1); }
    get firstChild() { return this.children[0]; }
    setAttribute(key, value) { this.attributes[key] = value; }
    removeAttribute(key) { delete this.attributes[key]; }
    addEventListener() {}
    scrollIntoView() {}
    focus() { if (!this.disabled) loaded.runtime.document.activeElement = this; }
    showModal() { this.open = true; }
    close() { this.open = false; }
    querySelectorAll(selector) {
      const all = this.children.flatMap(child => child instanceof Element ? [child, ...child.querySelectorAll('*')] : []);
      if (selector === '[data-role-focus]') return all.filter(child => child.dataset.roleFocus);
      return all;
    }
    reset() { this.children.forEach(child => { child.value = child.defaultValue || ''; child.checked = false; }); }
  }
  const nodes = {};
  const node = id => nodes[id] || (nodes[id] = new Element());
  node('roles-create').children = ['role-id', 'role-name', 'role-description', 'role-save'].map(node);
  node('role-binding-create').children = ['role-binding-role', 'role-binding-username', 'role-binding-scope', 'role-binding-value', 'role-binding-object-kind', 'binding-submit'].map(node);
  node('role-binding-scope').value = node('role-binding-scope').defaultValue = 'global';
  node('panel-roles').children = ['role-rows', 'role-binding-rows', 'role-permission-options'].map(node);
  loaded.runtime.Node = Element;
  loaded.runtime.setTimeout = () => 1;
  loaded.runtime.clearTimeout = () => {};
  loaded.runtime.document = {
    getElementById: node,
    createElement: () => new Element(),
    createTextNode: text => ({text}),
    querySelectorAll(selector) {
      assert.ok(selector.startsWith('#role-permission-options input'));
      return node('role-permission-options').children.flatMap(label => label.children)
        .filter(input => input.type === 'checkbox' && (!selector.endsWith(':checked') || input.checked));
    },
  };
  return node;
}

function roleManager(token = 'manager') {
  return {mode:'account', token, permissions:['roles.read', 'roles.write'], scoped_permissions:[]};
}

function managementDOM(loaded) {
  const node = roleDOM(loaded);
  node('connection-disconnect').children = ['connection-disconnect-reason', 'connection-disconnect-confirm', 'connection-disconnect-send'].map(node);
  loaded.state.auth = {mode:'account', token:'manager', permissions:['devices.policy.read','devices.policy.write','connections.read','connections.disconnect'], scoped_permissions:[]};
  const timers = new Map();
  let timerID = 0;
  loaded.runtime.setTimeout = fn => { timers.set(++timerID, fn); return timerID; };
  loaded.runtime.clearTimeout = id => timers.delete(id);
  return {node, timers};
}

function classList(initial = []) {
  const values = new Set(initial);
  return {
    add(value) { values.add(value); },
    remove(value) { values.delete(value); },
    contains(value) { return values.has(value); },
    toggle(value, force) {
      if (force === undefined ? !values.has(value) : force) values.add(value);
      else values.delete(value);
    },
  };
}

function navigationDOM(loaded) {
  const tabs = ['overview', 'devices', 'connections'].map((tab, index) => ({
    dataset: {tab},
    attributes: {},
    tabIndex: index === 0 ? 0 : -1,
    classList: classList(index === 0 ? ['active'] : []),
    setAttribute(key, value) { this.attributes[key] = value; },
    focus() { this.focused = true; },
  }));
  const panels = tabs.map((tab) => ({
    id: 'panel-' + tab.dataset.tab,
    classList: classList(tab.dataset.tab === 'overview' ? ['active'] : []),
  }));
  loaded.runtime.document = {
    querySelectorAll(selector) {
      if (selector === '.tab') return tabs;
      if (selector === '.panel-page') return panels;
      throw new Error('unexpected selector: ' + selector);
    },
  };
  return {tabs, panels};
}

test('admin tabs expose linked panels and standard keyboard navigation', () => {
  const html = fs.readFileSync(path.join(__dirname, 'assets/index.html'), 'utf8');
  const tabButtons = Array.from(html.matchAll(/<button\b([^>]*\brole="tab"[^>]*)>/g), match => match[1]);
  assert.equal(tabButtons.length, 12);
  for (const markup of tabButtons) {
    const tab = markup.match(/\bdata-tab="([^"]+)"/)[1];
    assert.match(markup, new RegExp('\\bid="tab-' + tab + '"'));
    assert.match(markup, new RegExp('\\baria-controls="panel-' + tab + '"'));
    assert.match(markup, new RegExp('\\baria-selected="' + (tab === 'overview' ? 'true' : 'false') + '"'));
    assert.match(markup, new RegExp('\\btabindex="' + (tab === 'overview' ? '0' : '-1') + '"'));
    assert.match(html, new RegExp('<section\\b(?=[^>]*\\bid="panel-' + tab + '")(?=[^>]*\\brole="tabpanel")(?=[^>]*\\baria-labelledby="tab-' + tab + '")[^>]*>'));
  }

  const loaded = app(async () => reply(200, {}));
  const {tabs, panels} = navigationDOM(loaded);
  const activate = tab => loaded.updateTabSelection(tab);
  function press(tabIndex, key) {
    let prevented = false;
    loaded.handleTabKeydown({
      currentTarget: tabs[tabIndex], key,
      preventDefault() { prevented = true; },
    }, activate);
    return prevented;
  }

  assert.equal(press(0, 'ArrowRight'), true);
  assert.equal(loaded.state.tab, 'devices');
  assert.equal(tabs[1].tabIndex, 0);
  assert.equal(tabs[1].attributes['aria-selected'], 'true');
  assert.equal(panels[1].classList.contains('active'), true);
  assert.equal(tabs[1].focused, true);
  assert.equal(press(1, 'ArrowRight'), true);
  assert.equal(loaded.state.tab, 'connections');
  assert.equal(press(2, 'ArrowRight'), true);
  assert.equal(loaded.state.tab, 'overview');
  assert.equal(press(0, 'ArrowLeft'), true);
  assert.equal(loaded.state.tab, 'connections');
  assert.equal(press(0, 'End'), true);
  assert.equal(loaded.state.tab, 'connections');
  assert.equal(press(2, 'Home'), true);
  assert.equal(loaded.state.tab, 'overview');
  tabs[1].classList.add('hidden');
  assert.equal(press(0, 'ArrowRight'), true);
  assert.equal(loaded.state.tab, 'connections');
  tabs[1].classList.remove('hidden');
  assert.equal(press(0, 'Enter'), false);
});

test('scrollable table regions are focusable and named by their columns', () => {
  const loaded = app(async () => reply(200, {}));
  const headers = ['用户名', '角色', '范围', '隐藏列'].map(textContent => ({textContent}));
  const region = {
    tabIndex: -1,
    clientWidth: 200,
    scrollWidth: 500,
    scrollLeft: 0,
    attributes: {},
    listeners: {},
    setAttribute(key, value) { this.attributes[key] = value; },
    hasAttribute(key) { return Object.hasOwn(this.attributes, key); },
    addEventListener(type, listener) { this.listeners[type] = listener; },
    querySelectorAll(selector) { assert.equal(selector, 'thead th'); return headers; },
  };
  const namedRegion = {
    ...region,
    attributes: {role: 'region', 'aria-label': '命名角色列表，可横向滚动'},
    listeners: {},
  };
  loaded.runtime.document = {querySelectorAll: selector => {
    assert.equal(selector, '.table-wrap');
    return [region, namedRegion];
  }};
  loaded.initializeTableRegions();
  assert.equal(region.tabIndex, 0);
  assert.equal(region.attributes.role, 'region');
  assert.equal(region.attributes['aria-label'], '表格列：用户名、角色、范围，可横向滚动');
  assert.equal(namedRegion.attributes['aria-label'], '命名角色列表，可横向滚动');
  let prevented = false;
  region.listeners.keydown({target: region, key: 'ArrowRight', preventDefault() { prevented = true; }});
  assert.equal(prevented, true);
  assert.equal(region.scrollLeft, 160);
  region.listeners.keydown({target: region, key: 'End', preventDefault() {}});
  assert.equal(region.scrollLeft, 300);
  region.listeners.keydown({target: {}, key: 'Home', preventDefault() { throw new Error('child controls keep their key behavior'); }});
  assert.equal(region.scrollLeft, 300);
});

const liveRow = (id = 'a') => ({host_id:'eastbox', runtime_id:'runtime', connection_id:id.repeat(32), state:'active', console_permissions:['connections.read','connections.disconnect']});
const strategyCatalog = {version:1, settings:[{key:'host.keep_awake', type:'boolean', apply:'restart'}]};

test('policy responses cannot refill revoked data or release a new session loading guard', async () => {
  const old = deferred(), next = deferred();
  const loaded = app((url, options) => options.headers.Authorization === 'Bearer manager' ? old.promise : next.promise);
  const {node} = managementDOM(loaded);
  const first = loaded.loadPolicies();
  loaded.applySessionPermissions({permissions:[], scoped_permissions:[]});
  loaded.state.auth = {mode:'account',token:'new',permissions:['devices.policy.read'],scoped_permissions:[]};
  const second = loaded.loadPolicies();
  old.resolve(reply(200, {policies:[{id:'private'}]}));
  await first;
  assert.equal(loaded.state.policiesLoading, true);
  assert.equal(node('policy-object-rows').children.length, 0);
  next.resolve(reply(200, {policies:[],assignments:[]}));
  await second;
  assert.equal(loaded.state.policiesLoading, false);
});

test('strategy load and save are invalidated by same-account revocation', async () => {
  const pending = deferred();
  let phase = 'load';
  const loaded = app(async (url, options) => {
    if (url.endsWith('/catalog')) return reply(200, strategyCatalog);
    return phase === 'ready' ? reply(200, {revision:1,settings:{}}) : pending.promise;
  });
  const {node} = managementDOM(loaded);
  const opening = loaded.openClientStrategy({id:'p',name:'P'});
  loaded.applySessionPermissions({permissions:['connections.read'],scoped_permissions:[]});
  pending.resolve(reply(200, {revision:1,settings:{}}));
  await opening;
  assert.equal(loaded.state.clientStrategy, null);
  assert.equal(node('client-strategy-fields').children.length, 0);
  assert.equal(node('client-strategy-dialog').open, false);
  assert.equal(node('client-strategy-save').disabled, true);
});

test('strategy conflict preserves input and blocks repeats until explicit reload; readonly cannot save', async () => {
  let writes = 0;
  const loaded = app(async (url, options) => {
    if (url.endsWith('/catalog')) return reply(200, strategyCatalog);
    if (options.method === 'PUT') { writes++; return reply(409,{error:'conflict'}); }
    return reply(200,{revision:1,settings:{}});
  });
  const {node} = managementDOM(loaded);
  await loaded.openClientStrategy({id:'p',name:'P'});
  const field = loaded.state.clientStrategy.fields[0];
  field.mode.value = 'locked'; field.value.value = 'true';
  await loaded.saveClientStrategy();
  await loaded.saveClientStrategy();
  assert.equal(writes, 1);
  assert.equal(field.value.value, 'true');
  assert.equal(node('client-strategy-save').disabled, true);
  assert.match(node('client-strategy-error').textContent,/当前输入已保留/);
  await loaded.openClientStrategy({id:'p',name:'P'});
  assert.equal(node('client-strategy-save').disabled, false);
  loaded.applySessionPermissions({permissions:['devices.policy.read'],scoped_permissions:[]});
  await loaded.openClientStrategy({id:'p',name:'P'});
  await loaded.saveClientStrategy();
  assert.equal(writes, 1);
  assert.equal(node('client-strategy-save').disabled, true);
  assert.equal(node('client-strategy-fields').children[0].disabled, true);
});

test('late strategy save cannot enable or overwrite another editor after reset', async () => {
  const pending = deferred();
  let writes = 0;
  const loaded = app(async (url, options) => {
    if (options.method === 'PUT') { writes++; return pending.promise; }
    if (url.endsWith('/catalog')) return reply(200,strategyCatalog);
    return reply(200,{revision:1,settings:{}});
  });
  const {node} = managementDOM(loaded);
  await loaded.openClientStrategy({id:'p',name:'P'});
  const saving = loaded.saveClientStrategy();
  await loaded.saveClientStrategy();
  assert.equal(writes, 1);
  loaded.resetPolicyConsole();
  loaded.state.auth = {mode:'account',token:'new',permissions:['devices.policy.read']};
  await loaded.openClientStrategy({id:'q',name:'Q'});
  pending.resolve(reply(200,{revision:42}));
  await saving;
  assert.equal(loaded.state.clientStrategy.policy.id, 'q');
  assert.equal(loaded.state.clientStrategy.revision, 1);
  assert.equal(node('client-strategy-save').disabled, true);
});

test('connection detail refresh orders responses and rejects same-target reopen results', async () => {
  const first = deferred(), second = deferred();
  let requests = 0;
  const loaded = app(() => ++requests === 1 ? first.promise : second.promise);
  const {node} = managementDOM(loaded);
  const oldOpen = loaded.openConnectionDetail(liveRow());
  const newOpen = loaded.openConnectionDetail(liveRow());
  second.resolve(reply(200,{...liveRow(),command_status:'queued'}));
  await newOpen;
  first.resolve(reply(200,{...liveRow(),command_status:'failed'}));
  await oldOpen;
  assert.equal(loaded.state.connectionDetail.command_status,'queued');
  assert.equal(node('connection-disconnect-send').disabled,true);
});

test('late disconnect does not write into another detail or unlock its pending request', async () => {
  const old = deferred(), next = deferred();
  const loaded = app((url, options) => {
    if (options.method === 'POST') return url.includes('a'.repeat(32)) ? old.promise : next.promise;
    return reply(200,liveRow(url.includes('a'.repeat(32)) ? 'a' : 'b'));
  });
  const {node} = managementDOM(loaded);
  await loaded.openConnectionDetail(liveRow());
  node('connection-disconnect-confirm').checked = true;
  const first = loaded.requestDisconnect();
  await loaded.openConnectionDetail(liveRow('b'));
  node('connection-disconnect-confirm').checked = true;
  const second = loaded.requestDisconnect();
  old.resolve(reply(202,{command_id:'old-command',status:'queued'}));
  await first;
  assert.equal(loaded.state.connectionSending,true);
  assert.equal(node('connection-disconnect-note').textContent,'');
  assert.equal(node('connection-disconnect-send').disabled,true);
  next.resolve(reply(503,{error:'unavailable'}));
  await second;
  assert.equal(loaded.state.connectionSending,false);
  assert.match(node('connection-disconnect-note').textContent,/unavailable/);
});

test('disconnect confirmation and retry retain exact key and reason across close/reopen and other targets', async () => {
  const sent = [];
  const loaded = app((url, options) => {
    if (options.method === 'POST') { sent.push(JSON.parse(options.body)); return reply(503,{error:'lost response'}); }
    return reply(200,liveRow());
  });
  const {node} = managementDOM(loaded);
  await loaded.openConnectionDetail(liveRow());
  await loaded.requestDisconnect();
  assert.equal(sent.length,0);
  node('connection-disconnect-confirm').checked = true;
  node('connection-disconnect-reason').value = 'first reason';
  await loaded.requestDisconnect();
  loaded.connectionRequestKeyFor(liveRow('b'));
  await loaded.openConnectionDetail(liveRow());
  assert.equal(node('connection-disconnect-reason').value,'first reason');
  node('connection-disconnect-confirm').checked = true;
  node('connection-disconnect-reason').value = 'edited during uncertainty';
  await loaded.requestDisconnect();
  assert.deepEqual(sent[0],sent[1]);
  loaded.state.connectionDetail.command_status = 'delivered';
  node('connection-disconnect-confirm').checked = true;
  await loaded.requestDisconnect();
  assert.equal(sent.length,2);
});

test('an older failed command cannot discard a retry key when the next response is lost', async () => {
  const sent = [];
  const previous = {...liveRow(), command_id:'previous-command', command_status:'failed'};
  const loaded = app((url, options) => {
    if (options.method === 'POST') {
      sent.push(JSON.parse(options.body));
      return reply(502,{error:'response lost'});
    }
    return reply(200,previous);
  });
  const {node} = managementDOM(loaded);
  await loaded.openConnectionDetail(previous);
  node('connection-disconnect-confirm').checked = true;
  await loaded.requestDisconnect();
  // A late list and a freshly opened detail can both still show the previous
  // terminal command. Neither proves that this new attempt has settled.
  loaded.releaseConnectionRequestKeyIfSettled(previous);
  await loaded.openConnectionDetail(previous);
  node('connection-disconnect-confirm').checked = true;
  await loaded.requestDisconnect();
  assert.equal(sent[0].request_key, sent[1].request_key);
  loaded.releaseConnectionRequestKeyIfSettled({...previous,command_id:'new-command'});
  assert.notEqual(loaded.connectionRequestKeyFor({...previous,command_id:'new-command'}),sent[0].request_key);
});

test('connection polling retries repeated query errors and stops on close or permission loss', async () => {
  let fail = true;
  const loaded = app(() => fail ? reply(503,{error:'offline'}) : reply(200,{...liveRow(),state:'closed',command_status:'applied',receipt_confirmed_absent:true}));
  const {node,timers} = managementDOM(loaded);
  await loaded.openConnectionDetail({...liveRow(),command_status:'queued'});
  for (let i=0;i<3;i++) {
    const callback = timers.get(loaded.state.connectionPollTimer);
    assert.ok(callback);
    await callback();
  }
  assert.match(node('connection-disconnect-note').textContent,/继续重试/);
  fail = false;
  await timers.get(loaded.state.connectionPollTimer)();
  assert.equal(node('connection-dialog-state').textContent,'已确认断开');
  assert.equal(loaded.state.connectionPollTimer,0);
  fail = true;
  await loaded.openConnectionDetail(liveRow());
  loaded.applySessionPermissions({permissions:[],scoped_permissions:[]});
  assert.equal(loaded.state.connectionDetail,null);
  assert.equal(loaded.state.connectionPollTimer,0);
  assert.equal(node('connection-dialog').open,false);
});

test('edit conflict preserves the draft and old revision through polling, then explicitly reloads', async () => {
  const writes = [];
  let latest = {id:'shared', name:'Other editor', description:'server description', permissions:['audit.read'], revision:'v2'};
  const loaded = app(async (url, options) => {
    if (url === '/v1/account') return reply(200, roleManager());
    if (options.method === 'PUT') {
      const body = JSON.parse(options.body); writes.push(body);
      if (body.if_revision !== latest.revision) return reply(409, {code:'role_revision_conflict', error:'conflict'});
      latest = {...body, revision:'v3'}; return reply(200, latest);
    }
    if (url === '/v1/admin-roles/shared') return reply(200, latest);
    if (url === '/v1/admin-roles') return reply(200, {roles:[latest], permissions:['devices.read','audit.read']});
    return reply(200, {bindings:[]});
  });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.state.rolePermissions = ['devices.read','audit.read']; loaded.renderPermissionOptions();
  loaded.editRole({id:'shared', name:'Initial', description:'original', permissions:['devices.read'], revision:'v1'});
  node('role-name').value = 'My unsaved draft';
  node('role-description').value = 'My explanation';
  await loaded.saveRole({preventDefault() {}});
  assert.equal(writes[0].if_revision, 'v1');
  assert.deepEqual(writes[0].permissions, ['devices.read']);
  assert.equal(node('role-name').value, 'My unsaved draft');
  assert.equal(node('role-description').value, 'My explanation');
  assert.equal(loaded.state.roleEditing.revision, 'v1');
  assert.equal(node('role-save').disabled, true);
  assert.match(node('role-save-note').textContent, /草稿已保留/);
  await loaded.loadRoles();
  assert.equal(loaded.state.roleEditing.revision, 'v1');
  assert.equal(node('role-name').value, 'My unsaved draft');
  assert.equal(node('role-save').disabled, true);
  await loaded.saveRole({preventDefault() {}});
  assert.equal(writes.length, 1);
  await loaded.reloadRoleEditor();
  assert.equal(node('role-name').value, 'Other editor');
  assert.equal(loaded.state.roleEditing.revision, 'v2');
  assert.equal(node('role-save').disabled, false);
  node('role-name').value = 'Reviewed change';
  await loaded.saveRole({preventDefault() {}});
  assert.equal(writes[1].if_revision, 'v2');
  assert.deepEqual(writes[1].permissions, ['audit.read']);
  assert.equal(latest.name, 'Reviewed change');
  assert.equal(loaded.state.roleEditing, null);
});

test('failed conflict reload keeps the draft; late reload cannot overwrite a new session', async () => {
  const pending = deferred();
  let delay = false;
  const loaded = app(async () => delay ? pending.promise : reply(503, {error:'unavailable'}));
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.editRole({id:'shared', name:'Draft', revision:'v1'});
  loaded.state.roleConflict = true;
  await loaded.reloadRoleEditor();
  assert.equal(node('role-name').value, 'Draft');
  assert.equal(loaded.state.roleEditing.revision, 'v1');
  assert.equal(node('role-save').disabled, true);
  delay = true;
  const reloading = loaded.reloadRoleEditor();
  loaded.resetRoleConsole(); loaded.clearRolePanels();
  loaded.state.auth = roleManager('new');
  node('role-name').value = 'New account draft';
  pending.resolve(reply(200, {id:'shared', name:'Old response', revision:'v2'}));
  await reloading;
  assert.equal(node('role-name').value, 'New account draft');
  assert.equal(loaded.state.roleEditing, null);
});

test('deleted role leaves a recoverable draft and resetting restores create mode', async () => {
  const loaded = app(async () => reply(404, {error:'unknown admin role'}));
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.editRole({id:'gone', name:'Draft', revision:'v1'});
  await loaded.saveRole({preventDefault() {}});
  assert.equal(node('role-name').value, 'Draft');
  assert.match(node('role-save-note').textContent, /已被删除/);
  assert.equal(node('role-save').disabled, true);
  loaded.resetRoleEditor();
  assert.equal(node('role-save').disabled, false);
  assert.equal(loaded.state.roleConflict, false);
});

test('delete sends the displayed revision and reports stale deletion without clearing the editor', async () => {
  let request;
  const loaded = app(async (url, options) => { request={url,options}; return reply(409, {code:'role_revision_conflict',error:'conflict'}); });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  const role = {id:'shared', name:'Draft', revision:'v1'};
  loaded.editRole(role);
  await loaded.deleteRole(role);
  assert.equal(request, undefined);
  assert.equal(node('role-action-dialog').open, true);
  assert.equal(loaded.runtime.document.activeElement, node('role-action-cancel'));
  await loaded.confirmRoleAction();
  assert.equal(request.url, '/v1/admin-roles/shared?if_revision=v1');
  assert.equal(request.options.method, 'DELETE');
  assert.match(node('role-action-error').textContent, /未删除/);
  assert.equal(node('role-action-confirm').disabled, true);
  assert.equal(loaded.state.roleEditing, role);
  loaded.closeRoleAction();
  request = null;
  await loaded.deleteRole(role);
  loaded.closeRoleAction();
  assert.equal(request, null);
});

test('overview requires the ops permission used by its stats API', () => {
  const loaded = app(async () => reply(200, {}));
  loaded.state.auth = {mode:'account', permissions:['devices.read', 'roles.read']};
  assert.equal(loaded.tabVisible('devices'), true);
  assert.equal(loaded.tabVisible('overview'), false);
  loaded.state.auth.permissions = ['ops.read'];
  assert.equal(loaded.tabVisible('overview'), true);
});

test('binding confirmation identifies its exact scope; cancellation restores the refreshed row focus without writes', async () => {
  const binding = {id:'east-binding', username:'ordinary', role_id:'reader', role_name:'Reader', scope_kind:'object', scope_value:'eastbox', object_kind:'device'};
  let writes = 0;
  const loaded = app(async (url, options) => {
    if (options.method === 'DELETE') writes++;
    return reply(200, {roles:[], bindings:[binding], permissions:[]});
  });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  await loaded.loadRoles();
  loaded.openRoleAction('binding', binding);
  assert.match(node('role-action-target').textContent, /ordinary.*\n.*reader.*\n.*object · eastbox\n对象类型：device/s);
  assert.equal(loaded.runtime.document.activeElement, node('role-action-cancel'));
  await loaded.loadRoles(); // replaces the triggering button while the modal is open
  loaded.closeRoleAction();
  assert.equal(writes, 0);
  assert.equal(node('role-action-dialog').open, false);
  assert.equal(loaded.runtime.document.activeElement.dataset.roleFocus, 'binding:east-binding');
});

test('pending destructive action prevents repeat or cancel, then preserves failure and allows one retry', async () => {
  const pending = deferred();
  let writes = 0;
  const loaded = app(async (url, options) => {
    if (options.method === 'DELETE') return ++writes === 1 ? pending.promise : reply(200, {});
    if (url === '/v1/account') return reply(200, roleManager());
    return reply(200, {roles:[], bindings:[], permissions:[]});
  });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.openRoleAction('binding', {id:'b', username:'ordinary', role_id:'reader', scope_kind:'global'});
  const saving = loaded.confirmRoleAction();
  await loaded.confirmRoleAction();
  loaded.closeRoleAction();
  assert.equal(writes, 1);
  assert.equal(node('role-action-dialog').open, true);
  assert.equal(node('role-action-cancel').disabled, true);
  assert.equal(node('role-action-confirm').disabled, true);
  pending.resolve(reply(503, {error:'temporarily unavailable'}));
  await saving;
  assert.match(node('role-action-error').textContent, /temporarily unavailable/);
  assert.equal(node('role-action-confirm').disabled, false);
  assert.equal(loaded.runtime.document.activeElement, node('role-action-cancel'));
  await loaded.confirmRoleAction();
  assert.equal(writes, 2);
  assert.equal(node('role-action-dialog').open, false);
  assert.equal(loaded.runtime.document.activeElement, node('role-binding-refresh'));
});

test('confirmation keeps its displayed revision across polling and cannot execute after revocation or account switch', async () => {
  const requests = [];
  const loaded = app(async (url, options) => {
    requests.push({url, method:options.method});
    return reply(200, {roles:[{id:'same', name:'new', revision:'v2'}], bindings:[], permissions:[]});
  });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.openRoleAction('role', {id:'same', name:'old', revision:'v1'});
  await loaded.loadRoles();
  assert.equal(loaded.state.roleAction.target.revision, 'v1');
  assert.match(node('role-action-target').textContent, /old/);
  loaded.applySessionPermissions({permissions:['roles.read'], scoped_permissions:[]});
  await loaded.confirmRoleAction();
  assert.equal(node('role-action-dialog').open, false);
  assert.equal(node('role-action-target').textContent, '');
  loaded.state.auth = roleManager();
  loaded.openRoleAction('role', {id:'same', revision:'v2'});
  loaded.state.auth = roleManager('another-account');
  await loaded.confirmRoleAction();
  assert.equal(node('role-action-dialog').open, false);
  assert.equal(requests.filter(r => r.method === 'DELETE').length, 0);
});

test('self-unbind refreshes grants immediately and clears privileged rows before reloading', async () => {
  const requests = [];
  const loaded = app(async (url, options) => {
    requests.push(url);
    if (options.method === 'DELETE') return reply(200, {});
    if (url === '/v1/account') return reply(200, {permissions:[], scoped_permissions:[]});
    assert.fail('privileged list requested after self-unbind: ' + url);
  });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.state.roles = [{id:'manager'}];
  loaded.openRoleAction('binding', {id:'self', username:'manager', role_id:'manager'});
  await loaded.confirmRoleAction();
  assert.deepEqual(requests, ['/v1/role-bindings/self', '/v1/account']);
  assert.equal(loaded.state.roles.length, 0);
  assert.equal(loaded.state.roleWriteBusy, false);
  assert.equal(node('role-action-dialog').open, false);
  assert.equal(loaded.canPermission('roles.write'), false);
});

test('an old destructive response cannot close a new account confirmation', async () => {
  const pending = deferred();
  const loaded = app(() => pending.promise);
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager('old');
  loaded.openRoleAction('binding', {id:'old', username:'old-user', role_id:'reader'});
  const deleting = loaded.confirmRoleAction();
  loaded.resetRoleConsole();
  loaded.state.auth = roleManager('new');
  loaded.openRoleAction('binding', {id:'new', username:'new-user', role_id:'reader'});
  pending.resolve(reply(200, {}));
  await deleting;
  assert.equal(node('role-action-dialog').open, true);
  assert.equal(loaded.state.roleAction.target.id, 'new');
  assert.match(node('role-action-target').textContent, /new-user/);
  assert.equal(node('role-action-confirm').disabled, false);
});

test('a successful deletion followed by refresh failure does not offer a destructive retry', async () => {
  let writes = 0;
  const loaded = app(async (url, options) => {
    if (options.method === 'DELETE') { writes++; return reply(200, {}); }
    return reply(503, {error:'refresh unavailable'});
  });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.openRoleAction('role', {id:'gone', revision:'v1'});
  await loaded.confirmRoleAction();
  await loaded.confirmRoleAction();
  assert.equal(writes, 1);
  assert.equal(node('role-action-dialog').open, false);
  assert.equal(loaded.state.roleAction, null);
  assert.match(node('toast').textContent, /refresh unavailable/);
});

test('polling preserves keyboard focus and permission drafts, and editing focuses the name', async () => {
  const role = {id:'reader', permissions:['devices.read'], revision:'v1'};
  const loaded = app(async () => reply(200, {roles:[role], permissions:['devices.read'], bindings:[]}));
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  await loaded.loadRoles();
  const before = node('role-permission-options').children[0].children[0];
  before.checked = true;
  before.focus();
  await loaded.loadRoles();
  const after = node('role-permission-options').children[0].children[0];
  assert.notEqual(before, after);
  assert.equal(loaded.runtime.document.activeElement, after);
  assert.equal(after.checked, true);
  loaded.editRole(role);
  assert.equal(loaded.runtime.document.activeElement, node('role-name'));
});

test('confirmation Tab cycles in both directions and skips unavailable actions', () => {
  const loaded = app(() => assert.fail('keyboard navigation must not send requests'));
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  loaded.openRoleAction('role', {id:'reader', revision:'v1'});
  let prevented = 0;
  const key = (shiftKey = false) => loaded.roleActionKeydown({key:'Tab', shiftKey, preventDefault() { prevented++; }});
  key(true);
  assert.equal(loaded.runtime.document.activeElement, node('role-action-confirm'));
  key();
  assert.equal(loaded.runtime.document.activeElement, node('role-action-cancel'));
  node('role-action-confirm').disabled = true;
  key();
  assert.equal(loaded.runtime.document.activeElement, node('role-action-cancel'));
  node('role-action-cancel').disabled = true;
  key(); // no focusable actions while a request is pending
  assert.equal(prevented, 4);
  assert.equal(node('role-action-dialog').open, true);
});

test('same-account revocation invalidates a pending role list and its drafts', async () => {
  const pending = deferred();
  const loaded = app(() => pending.promise);
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  node('role-binding-username').value = 'old-recipient';
  const loading = loaded.loadRoles();
  loaded.applySessionPermissions({permissions:[], scoped_permissions:[]});
  pending.resolve(reply(200, {roles:[{id:'private-role'}], bindings:[{id:'private-binding'}]}));
  assert.equal(await loading, false);
  assert.equal(loaded.state.roles.length, 0);
  assert.equal(loaded.state.roleBindings.length, 0);
  assert.equal(node('role-rows').children.length, 0);
  assert.equal(node('role-binding-username').value, '');
});

test('an old role request cannot release the new account loading guard', async () => {
  const oldResponse = deferred();
  const newResponse = deferred();
  const loaded = app((url, options) => options.headers.Authorization === 'Bearer old' ? oldResponse.promise : newResponse.promise);
  roleDOM(loaded);
  loaded.state.auth = roleManager('old');
  const oldLoad = loaded.loadRoles();
  loaded.resetRoleConsole();
  loaded.state.auth = roleManager('new');
  const newLoad = loaded.loadRoles();
  oldResponse.resolve(reply(200, {}));
  assert.equal(await oldLoad, false);
  assert.equal(loaded.state.rolesLoading, true);
  newResponse.resolve(reply(200, {roles:[{id:'new-role'}], bindings:[]}));
  assert.equal(await newLoad, true);
  assert.equal(loaded.state.rolesLoading, false);
  assert.equal(loaded.state.roles[0].id, 'new-role');
});

test('permission polling cannot enable a pending write or allow duplicate submission', async () => {
  const pending = deferred();
  let writes = 0;
  let success = 0;
  const loaded = app(() => { writes++; return pending.promise; });
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  node('role-name').value = 'unsaved draft';
  node('role-binding-scope').value = 'object';
  const write = loaded.mutateRoles('/v1/admin-roles', {method:'POST'}, () => { success++; });
  loaded.applyRoleControlPermissions();
  assert.equal(node('role-save').disabled, true);
  assert.equal(node('role-editor-reset').disabled, true);
  assert.equal(node('role-binding-object-kind').disabled, true);
  await loaded.mutateRoles('/v1/admin-roles', {method:'POST'}, () => { success++; });
  assert.equal(writes, 1);
  pending.resolve(reply(503, {error:'unavailable'}));
  await write;
  assert.equal(success, 0);
  assert.equal(node('role-name').value, 'unsaved draft');
  assert.equal(node('role-save').disabled, false);
});

test('a write finishing after revocation cannot re-enable controls or reset new state', async () => {
  const pending = deferred();
  const loaded = app(() => pending.promise);
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager();
  let success = false;
  const write = loaded.mutateRoles('/v1/admin-roles', {method:'POST'}, () => { success = true; });
  loaded.applySessionPermissions({permissions:['roles.read'], scoped_permissions:[]});
  loaded.applyRoleControlPermissions();
  pending.resolve(reply(200, {}));
  await write;
  assert.equal(success, false);
  assert.equal(node('role-save').disabled, true);
  assert.equal(node('binding-submit').disabled, true);
});

test('an old account write cannot release the new account write guard', async () => {
  const oldResponse = deferred();
  const newResponse = deferred();
  const loaded = app((url, options) => options.headers.Authorization === 'Bearer old' ? oldResponse.promise : newResponse.promise);
  const node = roleDOM(loaded);
  loaded.state.auth = roleManager('old');
  const oldWrite = loaded.mutateRoles('/v1/admin-roles', {method:'POST'}, () => assert.fail('old success callback'));
  loaded.resetRoleConsole();
  loaded.state.auth = roleManager('new');
  const newWrite = loaded.mutateRoles('/v1/admin-roles', {method:'POST'}, () => {});
  oldResponse.resolve(reply(200, {}));
  await oldWrite;
  assert.equal(loaded.state.roleWriteBusy, true);
  assert.equal(node('role-save').disabled, true);
  newResponse.resolve(reply(503, {error:'retry'}));
  await newWrite;
  assert.equal(loaded.state.roleWriteBusy, false);
});

test('write completion reloads roles even when a pre-write list is still pending', async () => {
  const oldList = deferred();
  let reads = 0;
  const loaded = app((url, options) => {
    if (url === '/v1/account') return reply(200, roleManager());
    if (options.method === 'POST') return reply(200, {});
    reads++;
    return reads <= 2 ? oldList.promise : reply(200, {roles:[{id:'saved-role'}], bindings:[]});
  });
  roleDOM(loaded);
  loaded.state.auth = roleManager();
  const loading = loaded.loadRoles();
  await loaded.mutateRoles('/v1/admin-roles', {method:'POST'}, () => {});
  assert.equal(loaded.state.roles[0].id, 'saved-role');
  oldList.resolve(reply(200, {roles:[{id:'stale-role'}], bindings:[]}));
  assert.equal(await loading, false);
  assert.equal(loaded.state.roles[0].id, 'saved-role');
});

test('role panel cleanup clears binding recipient, scope and cached role choices', () => {
  const loaded = app(async () => reply(200, {}));
  const node = roleDOM(loaded);
  node('role-binding-username').value = 'old-recipient';
  node('role-binding-scope').value = 'object';
  node('role-binding-value').value = 'old-device';
  node('role-binding-object-kind').value = 'device';
  node('role-binding-role').children.push({value:'old-role'});
  loaded.clearRolePanels();
  assert.equal(node('role-binding-username').value, '');
  assert.equal(node('role-binding-scope').value, 'global');
  assert.equal(node('role-binding-value').value, '');
  assert.equal(node('role-binding-object-kind').value, '');
  assert.equal(node('role-binding-role').children.length, 0);
});

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

test('refreshing the role catalogue preserves an unsaved permission selection', () => {
  const loaded = app(async () => reply(200, {}));
  function node() {
    return {
      children: [], checked: false, dataset: {},
      appendChild(child) { this.children.push(child); },
      removeChild(child) { this.children.splice(this.children.indexOf(child), 1); },
      get firstChild() { return this.children[0]; },
    };
  }
  const box = node();
  const inputs = () => box.children.flatMap(label => label.children).filter(child => child.type === 'checkbox');
  loaded.runtime.document = {
    getElementById(id) { assert.equal(id, 'role-permission-options'); return box; },
    createElement: node,
    createTextNode(text) { return {text}; },
    querySelectorAll(selector) {
      assert.ok(['#role-permission-options input', '#role-permission-options input:checked'].includes(selector));
      return inputs().filter(input => !selector.endsWith(':checked') || input.checked);
    },
  };
  loaded.state.rolePermissions = ['devices.read', 'connections.read', 'audit.read'];
  loaded.renderPermissionOptions();
  inputs().find(input => input.value === 'devices.read').checked = true;
  inputs().find(input => input.value === 'connections.read').checked = true;
  // The editable draft differs from the persisted role; a list refresh must
  // preserve that draft rather than reset it or reload the saved permissions.
  loaded.state.roleEditing = {id:'reader', permissions:['audit.read']};
  loaded.renderPermissionOptions();
  assert.deepEqual(inputs().filter(input => input.checked).map(input => input.value), ['devices.read', 'connections.read']);
  // A deliberately empty selection must remain empty too.
  inputs().forEach(input => { input.checked = false; });
  loaded.renderPermissionOptions();
  assert.equal(inputs().filter(input => input.checked).length, 0);
});

test('object binding controls stay disabled after roles.write is revoked', () => {
  const loaded = app(async () => reply(200, {}));
  const nodes = {
    'role-binding-scope': {value:'object'},
    'role-binding-value': {value:'eastbox', setAttribute() {}, removeAttribute() {}},
    'role-binding-object-kind': {value:'device'},
    'role-binding-note': {},
  };
  loaded.runtime.document = {getElementById: id => nodes[id]};
  loaded.state.auth = {mode:'account', permissions:['roles.read', 'roles.write']};
  loaded.syncRoleBindingScope();
  assert.equal(nodes['role-binding-object-kind'].disabled, false);
  loaded.state.auth.permissions = ['roles.read'];
  loaded.syncRoleBindingScope();
  assert.equal(nodes['role-binding-value'].disabled, true);
  assert.equal(nodes['role-binding-object-kind'].disabled, true);
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

for (const scope of [{scope_kind:'group', scope_value:'east'}, {scope_kind:'object', scope_value:'eastbox', object_kind:'device'}]) {
  test('scoped login enters the console without making global grants: ' + scope.scope_kind, async () => {
    const calls = [];
    const loaded = app(async (url) => { calls.push(url); return reply(200, {}); }, true);
    await loaded.adoptAccountSession({token:'scoped', refresh_token:'refresh', permissions:[],
      scoped_permissions:[{...scope, permissions:['devices.read','devices.write','connections.read','connections.disconnect','devices.policy.read','accounts.write']}],
      user:{username:'limited', role:'user'}}, 'limited');
    assert.equal(loaded.state.auth.token, 'scoped');
    assert.deepEqual(calls, []);
    assert.equal(loaded.canPermission('devices.read'), false);
    assert.equal(loaded.tabVisible('devices'), true);
    assert.equal(loaded.tabVisible('connections'), true);
    for (const tab of ['overview','policies','accounts','roles','tokens','groups','audit']) assert.equal(loaded.tabVisible(tab), false, tab);
    assert.equal(loaded.canDeviceAction({console_permissions:['devices.write']}, 'devices.write'), true);
    assert.equal(loaded.canDeviceAction({console_permissions:['devices.read']}, 'devices.write'), false);
    assert.equal(loaded.canDeviceAction({}, 'devices.write'), false);
  });
}

test('permission polling removes revoked rows, selections and navigation', async () => {
  const loaded = app(async (url) => { assert.equal(url, '/v1/account'); return reply(200, {permissions:[], scoped_permissions:[]}); });
  loaded.state.auth = {mode:'account', token:'scoped', permissions:[], scoped_permissions:[{scope_kind:'group',scope_value:'east',permissions:['devices.read','connections.read','connections.disconnect']}]};
  loaded.state.deviceList = [{id:'eastbox'}];
  loaded.state.deviceSelection.add('eastbox');
  loaded.state.connectionRows = [{host_id:'eastbox'}];
  loaded.state.connectionDetail = {host_id:'eastbox'};
  const generation = loaded.state.connectionGen;
  await loaded.refreshConsolePermissions();
  assert.equal(loaded.state.deviceList.length, 0);
  assert.equal(loaded.state.deviceSelection.size, 0);
  assert.equal(loaded.state.connectionRows.length, 0);
  assert.equal(loaded.state.connectionDetail, null);
  assert.ok(loaded.state.connectionGen > generation);
  assert.equal(loaded.tabVisible('connections'), false);
  assert.equal(loaded.canDeviceAction({console_permissions:['connections.disconnect']}, 'connections.disconnect'), false);
});

test('permission refresh ignores a response from the previous account', async () => {
  const pending = deferred();
  const loaded = app(() => pending.promise);
  loaded.state.auth = {mode:'account',token:'old',permissions:[]};
  const polling = loaded.refreshConsolePermissions();
  loaded.state.auth = {mode:'account',token:'new',permissions:['roles.read']};
  pending.resolve(reply(200, {permissions:['devices.write'], scoped_permissions:[]}));
  await assert.rejects(polling, /登录身份已变化/);
  assert.deepEqual(loaded.state.auth.permissions, ['roles.read']);
});

test('token refresh retains separate scoped permissions', async () => {
  const loaded = app(async () => reply(200, {token:'fresh',refresh_token:'next',permissions:[],scoped_permissions:[{scope_kind:'object',scope_value:'one',object_kind:'device',permissions:['connections.read']}]}));
  loaded.state.auth = {mode:'account',token:'old',refresh:'refresh',permissions:[]};
  assert.equal(await loaded.refreshSession(), true);
  assert.equal(loaded.canPermission('connections.read'), false);
  assert.equal(loaded.tabVisible('connections'), true);
  assert.equal(JSON.parse(loaded.saved.at(-1)).scoped_permissions[0].scope_value, 'one');
});
