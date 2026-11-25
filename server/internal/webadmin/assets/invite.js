'use strict';

const form = document.getElementById('invitation-accept');
const token = document.getElementById('invitation-token');
const status = document.getElementById('invitation-status');

const fragment = new URLSearchParams(window.location.hash.slice(1));
const linkedToken = fragment.get('token');
if (linkedToken) {
  token.value = linkedToken;
  window.history.replaceState(null, '', window.location.pathname);
}

form.addEventListener('submit', async (event) => {
  event.preventDefault();
  const submit = form.querySelector('button[type="submit"]');
  submit.disabled = true;
  status.textContent = '正在创建账号…';
  try {
    const response = await fetch('/v1/account-invitations/accept', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        token: token.value.trim(),
        username: document.getElementById('invitation-username').value.trim(),
        password: document.getElementById('invitation-password').value,
      }),
    });
    const reply = await response.json();
    if (!response.ok) throw new Error(reply.error || '无法接受邀请');
    form.classList.add('hidden');
    status.textContent = '账号已创建，邮箱已验证。';
    document.getElementById('invitation-login').classList.remove('hidden');
  } catch (error) {
    status.textContent = error.message || String(error);
  } finally {
    submit.disabled = false;
  }
});
