'use strict';

let devices = [];
let inboxItems = [];
let activeTransfers = {}; // transfer_id -> TransferProgress, incoming only
let pendingPairingRequest = null;

function escapeHtml(s) {
  return String(s ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function formatBytes(n) {
  if (!n || n < 1024) return (n || 0) + ' B';
  const units = ['KB', 'MB', 'GB'];
  let v = n / 1024, i = 0;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return v.toFixed(1) + ' ' + units[i];
}

function formatTime(ms) {
  if (!ms) return '';
  const d = new Date(ms);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

// ---- devices ----

async function refreshDevices() {
  devices = await listDevices();
  renderDevices();
  renderComposeDeviceOptions();
}

function renderDevices() {
  const el = document.getElementById('deviceList');
  if (devices.length === 0) {
    el.innerHTML = '<div class="empty">No paired devices.</div>';
    return;
  }
  el.innerHTML = devices.map((d) => `
    <div class="device">
      <span class="dot ${d.online ? 'online' : ''}"></span>
      <span class="device-name">${escapeHtml(d.display_name)}</span>
      <button class="device-remove" data-uuid="${escapeHtml(d.device_uuid)}" title="Remove device">&times;</button>
    </div>
  `).join('');
  el.querySelectorAll('.device-remove').forEach((btn) => {
    btn.addEventListener('click', async () => {
      await removeDevice(btn.dataset.uuid);
      refreshDevices();
    });
  });
}

function renderComposeDeviceOptions() {
  const select = document.getElementById('composeDevice');
  const prev = select.value;
  select.innerHTML = devices.length
    ? devices.map((d) => `<option value="${escapeHtml(d.device_uuid)}">${escapeHtml(d.display_name)}${d.online ? '' : ' (offline)'}</option>`).join('')
    : '<option value="">No paired devices</option>';
  if (devices.some((d) => d.device_uuid === prev)) select.value = prev;
  const hasDevices = devices.length > 0;
  document.getElementById('sendTextBtn').disabled = !hasDevices;
  document.getElementById('sendFileBtn').disabled = !hasDevices;
}

// ---- inbox ----

const trashIconSvg = `<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor" aria-hidden="true">
  <path d="M6,19c0,1.1 0.9,2 2,2h8c1.1,0 2,-0.9 2,-2L18,7L6,7v12zM19,4h-3.5l-1,-1h-5l-1,1H5v2h14V4z"/>
</svg>`;

function renderInbox() {
  const el = document.getElementById('inboxList');
  const transferRows = Object.values(activeTransfers).map((p) => `
    <div class="item">
      <div class="item-meta"><span>Receiving &ldquo;${escapeHtml(p.file_name)}&rdquo;&hellip;</span><span>${p.chunks_done}/${p.total_chunks}</span></div>
    </div>
  `);
  const itemRows = inboxItems.map((item) => {
    const from = escapeHtml(item.from_display_name || 'Unknown device');
    const when = formatTime(item.received_at_unix_ms);
    let body;
    if (item.kind === 'file') {
      body = `📎 ${escapeHtml(item.file_name)} &mdash; ${formatBytes(item.file_size_bytes)}`;
    } else {
      body = escapeHtml(item.text);
    }
    const actions = item.kind === 'file'
      ? `<button data-action="save" data-id="${escapeHtml(item.envelope_id)}" data-name="${escapeHtml(item.file_name)}">Save&hellip;</button>
         <button class="item-trash" data-action="reject" data-id="${escapeHtml(item.envelope_id)}" title="Remove">${trashIconSvg}</button>`
      : `<button data-action="copy" data-id="${escapeHtml(item.envelope_id)}" data-text="${escapeHtml(item.text)}">Copy</button>
         <button class="item-trash" data-action="reject" data-id="${escapeHtml(item.envelope_id)}" title="Remove">${trashIconSvg}</button>`;
    return `
      <div class="item" data-envelope="${escapeHtml(item.envelope_id)}">
        <div class="item-meta"><span>${from}</span><span>${when}</span></div>
        <div class="item-body">${body}</div>
        <div class="item-actions">${actions}</div>
      </div>
    `;
  });

  const rows = transferRows.join('') + itemRows.join('');
  el.innerHTML = rows || '<div class="empty">Nothing received yet.</div>';

  el.querySelectorAll('button[data-action]').forEach((btn) => {
    btn.addEventListener('click', () => handleItemAction(btn));
  });
}

async function handleItemAction(btn) {
  const action = btn.dataset.action;
  const id = btn.dataset.id;
  if (action === 'copy') {
    try {
      await navigator.clipboard.writeText(btn.dataset.text);
    } catch (e) {
      // Fallback for webviews without navigator.clipboard permission.
      const ta = document.createElement('textarea');
      ta.value = btn.dataset.text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      ta.remove();
    }
    await actOnItem(id, 'copy');
  } else if (action === 'reject') {
    await actOnItem(id, 'reject');
    inboxItems = inboxItems.filter((i) => i.envelope_id !== id);
    renderInbox();
  } else if (action === 'save') {
    const result = await saveItem(id, btn.dataset.name);
    if (result && result.ok) {
      inboxItems = inboxItems.filter((i) => i.envelope_id !== id);
      renderInbox();
    }
  }
}

// ---- compose / send ----

// WebKitGTK sometimes only partially repaints a textarea's placeholder
// after its value is cleared via JS (e.g. shows just "P" of "Paste or
// type..."). Toggling display forces a fresh layout/paint so the full
// placeholder renders correctly.
function forceRepaint(el) {
  el.style.display = 'none';
  void el.offsetHeight;
  el.style.display = '';
}

async function sendCurrentText() {
  const textarea = document.getElementById('composeText');
  const text = textarea.value.trim();
  const deviceUuid = document.getElementById('composeDevice').value;
  if (!text || !deviceUuid) return;
  const result = await sendText(deviceUuid, text);
  if (result && result.ok) {
    textarea.value = '';
    forceRepaint(textarea);
  } else {
    alert('Failed to send: ' + (result && result.error ? result.error : 'unknown error'));
  }
}

async function sendCurrentFile() {
  const deviceUuid = document.getElementById('composeDevice').value;
  if (!deviceUuid) return;
  const result = await pickAndSendFile(deviceUuid);
  if (result && !result.ok && !result.cancelled) {
    alert('Failed to send file: ' + (result.error || 'unknown error'));
  }
}

// ---- pairing ----

async function openAddDeviceModal() {
  document.getElementById('addDeviceModal').hidden = false;
  // beginPairing() resolves to an object (webview.js deserializes the
  // bind() call's JSON return value) -- re-stringify it to get the QR
  // payload text to encode.
  const qrPayload = await beginPairing();
  const qrText = JSON.stringify(qrPayload);
  const qr = qrcode(0, 'M');
  qr.addData(qrText);
  qr.make();
  document.getElementById('qrContainer').innerHTML = qr.createSvgTag(4, 8);
}

async function closeAddDeviceModal() {
  document.getElementById('addDeviceModal').hidden = true;
  await cancelPairing();
}

async function submitManualPair() {
  const raw = document.getElementById('manualPairInput').value.trim();
  if (!raw) return;
  const result = await requestPairing(raw);
  if (!result || !result.ok) {
    alert('Pairing failed: ' + (result && result.error ? result.error : 'invalid QR data'));
    return;
  }
  document.getElementById('manualPairInput').value = '';
  document.getElementById('addDeviceModal').hidden = true;
}

function showPairingRequestModal(req) {
  pendingPairingRequest = req;
  document.getElementById('pairingRequestName').textContent =
    `${req.display_name} (${req.platform}) wants to pair with this PC.`;
  document.getElementById('pairingRequestFingerprint').textContent =
    'Fingerprint: ' + req.fingerprint;
  document.getElementById('pairingRequestModal').hidden = false;
}

// ---- Core -> JS push events ----

window.onIncomingItem = function (item) {
  inboxItems.unshift(item);
  if (item.kind === 'file') {
    const match = Object.keys(activeTransfers).find(
      (id) => activeTransfers[id].file_name === item.file_name
    );
    if (match) delete activeTransfers[match];
  }
  renderInbox();
};

window.onPairingRequest = function (req) {
  showPairingRequestModal(req);
};

window.onTransferProgress = function (p) {
  if (p.direction === 'incoming') {
    activeTransfers[p.transfer_id] = p;
    renderInbox();
  }
};

window.onPeerStatusChanged = function (d) {
  const existing = devices.find((x) => x.device_uuid === d.device_uuid);
  if (existing) {
    existing.online = d.online;
  } else {
    devices.push(d);
  }
  renderDevices();
  renderComposeDeviceOptions();
};

// ---- wiring ----

document.addEventListener('DOMContentLoaded', async () => {
  const self = await getSelfInfo();
  document.getElementById('selfInfo').textContent = self.display_name;

  await refreshDevices();
  renderInbox();

  document.getElementById('sendTextBtn').addEventListener('click', sendCurrentText);
  document.getElementById('sendFileBtn').addEventListener('click', sendCurrentFile);
  document.getElementById('addDeviceBtn').addEventListener('click', openAddDeviceModal);
  document.getElementById('closeAddDeviceBtn').addEventListener('click', closeAddDeviceModal);
  document.getElementById('manualPairBtn').addEventListener('click', submitManualPair);
  document.getElementById('acceptPairingBtn').addEventListener('click', async () => {
    if (!pendingPairingRequest) return;
    await acceptPairing(pendingPairingRequest.device_uuid);
    document.getElementById('pairingRequestModal').hidden = true;
    pendingPairingRequest = null;
    refreshDevices();
  });
  document.getElementById('rejectPairingBtn').addEventListener('click', async () => {
    if (!pendingPairingRequest) return;
    await rejectPairing(pendingPairingRequest.device_uuid);
    document.getElementById('pairingRequestModal').hidden = true;
    pendingPairingRequest = null;
  });

  const composeText = document.getElementById('composeText');
  composeText.addEventListener('dragover', (e) => {
    e.preventDefault();
    composeText.classList.add('dragover');
  });
  composeText.addEventListener('dragleave', () => composeText.classList.remove('dragover'));
  composeText.addEventListener('drop', (e) => {
    e.preventDefault();
    composeText.classList.remove('dragover');
    const text = e.dataTransfer.getData('text/plain');
    if (text) composeText.value = text;
  });
});
