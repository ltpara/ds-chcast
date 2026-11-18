'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawn, execFileSync} = require('node:child_process');
const {setTimeout: delay} = require('node:timers/promises');

const [url, targetConnectionID, artifactDirectory] = process.argv.slice(2);
const username = process.env.CHAST_BROWSER_USERNAME || '';
const password = process.env.CHAST_BROWSER_PASSWORD || '';
if (!url || !targetConnectionID || !artifactDirectory || !username || !password) {
  throw new Error('missing browser acceptance inputs');
}

function findChrome() {
  if (process.env.CHAST_CHROME_PATH && fs.existsSync(process.env.CHAST_CHROME_PATH)) {
    return process.env.CHAST_CHROME_PATH;
  }
  const candidates = [
    path.join(process.env.PROGRAMFILES || 'C:\\Program Files', 'Google/Chrome/Application/chrome.exe'),
    path.join(process.env['PROGRAMFILES(X86)'] || 'C:\\Program Files (x86)', 'Google/Chrome/Application/chrome.exe'),
    path.join(process.env['LOCALAPPDATA'] || '', 'Google/Chrome/Application/chrome.exe'),
    path.join(process.env['PROGRAMFILES(X86)'] || 'C:\\Program Files (x86)', 'Microsoft/Edge/Application/msedge.exe'),
  ];
  return candidates.find(candidate => fs.existsSync(candidate));
}

async function waitFor(description, predicate, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const result = await predicate();
    if (result) return result;
    await delay(100);
  }
  throw new Error(`timed out waiting for ${description}`);
}

async function main() {
  const chromePath = findChrome();
  if (!chromePath) throw new Error('Chrome or Edge was not found');
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'chast-console-native-'));
  let chrome;
  let socket;
  const pending = new Map();
  let nextRequestID = 0;

  try {
    chrome = spawn(chromePath, [
      '--headless=new',
      '--disable-gpu',
      '--disable-extensions',
      '--disable-background-networking',
      '--no-first-run',
      '--no-default-browser-check',
      '--remote-debugging-address=127.0.0.1',
      '--remote-debugging-port=0',
      `--user-data-dir=${profile}`,
      'about:blank',
    ], {stdio: 'ignore', windowsHide: true});
    await new Promise((resolve, reject) => {
      chrome.once('spawn', resolve);
      chrome.once('error', reject);
    });

    const portFile = path.join(profile, 'DevToolsActivePort');
    await waitFor('Chrome DevTools port', () => {
      if (chrome.exitCode !== null) throw new Error(`browser exited with code ${chrome.exitCode}`);
      return fs.existsSync(portFile);
    }, 20000);
    const port = Number(fs.readFileSync(portFile, 'utf8').split(/\r?\n/)[0]);
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const page = targets.find(target => target.type === 'page');
    if (!page) throw new Error('Chrome did not create a page target');

    socket = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {
      socket.addEventListener('open', resolve, {once: true});
      socket.addEventListener('error', reject, {once: true});
    });
    socket.addEventListener('message', event => {
      let message;
      try {
        message = JSON.parse(String(event.data));
      } catch {
        return;
      }
      if (!message.id) return;
      const request = pending.get(message.id);
      if (!request) return;
      pending.delete(message.id);
      if (message.error) request.reject(new Error(message.error.message || 'CDP request failed'));
      else request.resolve(message.result || {});
    });
    socket.addEventListener('close', () => {
      for (const request of pending.values()) request.reject(new Error('browser connection closed'));
      pending.clear();
    });

    const send = (method, params = {}) => new Promise((resolve, reject) => {
      const id = ++nextRequestID;
      pending.set(id, {resolve, reject});
      socket.send(JSON.stringify({id, method, params}));
    });
    const evaluate = async expression => {
      const response = await send('Runtime.evaluate', {
        expression,
        awaitPromise: true,
        returnByValue: true,
      });
      if (response.exceptionDetails) {
        throw new Error(response.exceptionDetails.text || 'page script failed');
      }
      return response.result && response.result.value;
    };

    await send('Page.enable');
    await send('Runtime.enable');
    await send('Page.navigate', {url});
    await waitFor('management login page', () => evaluate(
      "document.readyState === 'complete' && Boolean(document.querySelector('#login-form'))"));

    const loginSource = `(() => {
      const mode = document.querySelector('#login-mode');
      mode.value = 'account';
      mode.dispatchEvent(new Event('change', {bubbles: true}));
      document.querySelector('#login-username').value = ${JSON.stringify(username)};
      document.querySelector('#login-password').value = ${JSON.stringify(password)};
      document.querySelector('#login-form').requestSubmit();
      return true;
    })()`;
    await evaluate(loginSource);
    await waitFor('manager login', () => evaluate(
      "!document.querySelector('#workspace').classList.contains('hidden')"), 45000);

    await evaluate(`(() => {
      window.__connectionApiResponses = [];
      const originalFetch = window.fetch.bind(window);
      window.fetch = async (...args) => {
        const response = await originalFetch(...args);
        const url = String(args[0]);
        if (url.includes('/v1/live-connections')) {
          const body = await response.clone().json().catch(() => null);
          const connections = Array.isArray(body && body.connections)
            ? body.connections : [body];
          window.__connectionApiResponses.push({
            url,
            status: response.status,
            connections: connections.filter(Boolean).map(connection => ({
              host_id: connection.host_id,
              connection_id: connection.connection_id,
              state: connection.state,
              command_status: connection.command_status,
              command_id: connection.command_id,
              console_permissions: connection.console_permissions,
            })),
          });
        }
        return response;
      };
      return true;
    })()`);

    await evaluate("document.querySelector('#tab-connections').click()");
    await waitFor('active connection list', () => evaluate(
      "document.querySelectorAll('#connection-rows tr').length > 0"), 20000);
    const rowCount = await evaluate("document.querySelectorAll('#connection-rows tr').length");
    let targetIndex = -1;
    for (let index = 0; index < rowCount; index++) {
      const opened = await evaluate(`(() => {
        const row = document.querySelectorAll('#connection-rows tr')[${index}];
        const button = row && row.querySelector('button');
        if (!button) return false;
        button.click();
        return true;
      })()`);
      if (!opened) continue;
      await waitFor('connection detail', () => evaluate(
        "document.querySelector('#connection-dialog').open && document.querySelector('#connection-dialog-id').textContent.trim() !== ''"));
      const currentConnectionID = await evaluate(
        "document.querySelector('#connection-dialog-id').textContent.trim()");
      if (currentConnectionID === targetConnectionID) {
        targetIndex = index;
        break;
      }
      await evaluate("document.querySelector('#connection-dialog-close').click()");
      await waitFor('connection detail close', () => evaluate(
        "!document.querySelector('#connection-dialog').open"));
    }
    if (targetIndex < 0) throw new Error('the console did not open the requested connection ID');

    // Exercise rapid switching even when the target happens to sort first.
    // Close and reopen in one task to make the old dialog's queued close observable.
    await evaluate(`(async () => {
      const dialog = document.querySelector('#connection-dialog');
      const close = document.querySelector('#connection-dialog-close');
      const rows = document.querySelectorAll('#connection-rows tr');
      const target = rows[${targetIndex}].querySelector('button');
      const other = rows[${targetIndex === 0 ? 1 : 0}].querySelector('button');
      const nextClose = () => new Promise(resolve => dialog.addEventListener('close', resolve, {once: true}));
      const firstClose = nextClose();
      close.click();
      await firstClose;
      other.click();
      const currentID = () => document.querySelector('#connection-dialog-id').textContent.trim();
      if (!dialog.open || !currentID() || currentID() === ${JSON.stringify(targetConnectionID)}) {
        throw new Error('expected a different connection before reopening target');
      }
      const previousClose = nextClose();
      close.click();
      target.click();
      await previousClose;
      if (!dialog.open || currentID() !== ${JSON.stringify(targetConnectionID)}) {
        throw new Error('target dialog changed after previous close event');
      }
      return true;
    })()`);

    const before = await evaluate("document.querySelector('#connection-dialog-state').textContent.trim()");
    if (before !== '活动中') throw new Error(`target was not active before disconnect: ${before}`);
    await evaluate(`(() => {
      window.__disconnectFetches = [];
      window.__disconnectTrace = [];
      const dialog = document.querySelector('#connection-dialog');
      const form = document.querySelector('#connection-disconnect');
      form.addEventListener('submit', event => {
        window.__disconnectTrace.push('submit event');
        queueMicrotask(() => window.__disconnectTrace.push('submit prevented=' + event.defaultPrevented));
      }, true);
      dialog.addEventListener('close', () => window.__disconnectTrace.push('dialog closed'));
      new MutationObserver(() => window.__disconnectTrace.push({
        state: document.querySelector('#connection-dialog-state').textContent.trim(),
        command: document.querySelector('#connection-command-detail').innerText,
        note: document.querySelector('#connection-disconnect-note').textContent.trim(),
      })).observe(dialog, {subtree: true, childList: true, characterData: true, attributes: true});
      const originalFetch = window.fetch;
      window.fetch = async (...args) => {
        const response = await originalFetch(...args);
        if (String(args[0]).includes('/v1/live-connections/')) {
          window.__disconnectFetches.push({
            url: String(args[0]),
            status: response.status,
            body: await response.clone().text(),
          });
        }
        return response;
      };
      return true;
    })()`);
    await evaluate(`(() => {
      const confirmation = document.querySelector('#connection-disconnect-confirm');
      if (confirmation.checked) throw new Error('new target unexpectedly kept previous confirmation');
      confirmation.click();
      const form = document.querySelector('#connection-disconnect');
      const button = document.querySelector('#connection-disconnect-send');
      window.__disconnectControlsBeforeClick = {
        confirmationChecked: confirmation.checked,
        confirmationDisabled: confirmation.disabled,
        buttonDisabled: button.disabled,
        formHidden: form.classList.contains('hidden'),
        disabledControls: [...form.querySelectorAll('input, button')]
          .filter(control => control.disabled)
          .map(control => control.id),
      };
      document.querySelector('#connection-disconnect-send').click();
      return true;
    })()`);
    try {
      await waitFor('UI-confirmed exact disconnect', () => evaluate(
        "document.querySelector('#connection-dialog-state').textContent.trim() === '已确认断开'"), 30000);
    } catch (error) {
      const diagnostic = await evaluate(`(() => ({
        state: document.querySelector('#connection-dialog-state').textContent.trim(),
        note: document.querySelector('#connection-command-note').textContent.trim(),
        requestNote: document.querySelector('#connection-disconnect-note').textContent.trim(),
        command: document.querySelector('#connection-command-detail').innerText,
        disconnectControls: (() => {
          const form = document.querySelector('#connection-disconnect');
          const confirmation = document.querySelector('#connection-disconnect-confirm');
          const button = document.querySelector('#connection-disconnect-send');
          return {
            hidden: form.classList.contains('hidden'),
            confirmationChecked: confirmation.checked,
            confirmationDisabled: confirmation.disabled,
            buttonDisabled: button.disabled,
            buttonText: button.textContent.trim(),
          };
        })(),
        controlsBeforeClick: window.__disconnectControlsBeforeClick || null,
        connectionApiResponses: window.__connectionApiResponses || [],
        dialogConnectionID: document.querySelector('#connection-dialog-id').textContent.trim(),
        fetches: window.__disconnectFetches,
        trace: window.__disconnectTrace,
      }))()`);
      const screenshot = await send('Page.captureScreenshot', {format: 'png', captureBeyondViewport: true});
      const screenshotPath = path.join(artifactDirectory, 'browser-disconnect-timeout.png');
      fs.writeFileSync(screenshotPath, Buffer.from(screenshot.data, 'base64'));
      throw new Error(`${error.message}; final page state=${JSON.stringify(diagnostic)}; screenshot=${screenshotPath}`);
    }
    process.stdout.write(`browser UI confirmed target disconnect ${targetConnectionID}\n`);
  } finally {
    if (socket && socket.readyState === WebSocket.OPEN) socket.close();
    if (chrome && chrome.pid && chrome.exitCode === null) {
      try {
        if (process.platform === 'win32') {
          execFileSync('taskkill.exe', ['/PID', String(chrome.pid), '/T', '/F'],
            {stdio: 'ignore', windowsHide: true});
        } else {
          chrome.kill('SIGTERM');
        }
      } catch {
        chrome.kill();
      }
      await Promise.race([
        new Promise(resolve => chrome.once('exit', resolve)),
        delay(5000),
      ]);
    }
    fs.rmSync(profile, {recursive: true, force: true});
  }
}

main().catch(error => {
  process.stderr.write(`native browser acceptance failed: ${error.message}\n`);
  process.exitCode = 1;
});
