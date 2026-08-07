// TPMS NRF52840 — Web BLE PWA

const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_TX_CHAR = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // Board → Phone (notifications)
const NUS_RX_CHAR = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // Phone → Board (write)
const CCCD_UUID = '00002902-0000-1000-8000-00805f9b34fb';

let device = null;
let server = null;
let txChar = null; // notifications from board
let rxChar = null; // write to board
let isConnected = false;
let nusReady = false;
let pendingQueue = [];
let rxBuffer = '';
let statusTimer = null;
let sniffRunning = false;
let snifferReport = [];
let lastDiscValid = [false, false, false, false];
let wasSniffActive = false;

const SENSOR_NAMES = ['Левое переднее', 'Правое переднее', 'Левое заднее', 'Правое заднее'];
let sensors = Array.from({length: 4}, () => ({id: '00000000', p: 0, t: 0, en: true}));
let settings = { freq: 433, tx_en: 0, tx_int: 60, tx_pkt: 2, license: 0, trial_rem: 0, serial: '', batt_mv: 0, batt_pct: 0, batt_raw: 0, batt_pin: 0, batt_cal: 0, usb: 0, sniff: 0, disc: [] };

// ========== Init ==========

function init() {
  // iOS warning
  if (/iphone|ipad|ipod/i.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1)) {
    document.getElementById('ios-warning').classList.add('show');
  }
  
  // Check Web Bluetooth support
  if (!navigator.bluetooth) {
    log('Web Bluetooth не поддерживается в этом браузере. Используйте Chrome или Edge.', 'err');
    document.getElementById('btn-scan').disabled = true;
    document.getElementById('btn-scan').textContent = 'BLE недоступен';
  }
  
  // Tab switching
  document.querySelectorAll('.tab').forEach(tab => {
    tab.addEventListener('click', () => switchTab(tab.dataset.tab));
  });
  
  // Scan button
  document.getElementById('btn-scan').addEventListener('click', scanAndConnect);
  
  // Sensor cards — short tap = TX, long press = editor
  document.querySelectorAll('.sensor-card').forEach((card, idx) => {
    let longPressTimer = null;
    let longHandled = false;
    card.addEventListener('pointerdown', () => {
      longHandled = false;
      longPressTimer = setTimeout(() => {
        longHandled = true;
        navigator.vibrate && navigator.vibrate(30);
        openSensorEditor(idx);
      }, 500);
    });
    card.addEventListener('pointerup', () => {
      clearTimeout(longPressTimer);
      if (!longHandled) {
        sendCmd(`"cmd":"tx${idx + 1}"`);
        flashCard(card);
      }
    });
    card.addEventListener('pointerleave', () => clearTimeout(longPressTimer));
  });
  
  // Buttons
  document.getElementById('btn-burst').addEventListener('click', () => sendCmd('"cmd":"burst"'));
  document.getElementById('btn-save-autotx').addEventListener('click', saveAutoTx);
  document.getElementById('btn-sniff-toggle').addEventListener('click', toggleSniff);
  document.getElementById('btn-sniff-apply').addEventListener('click', () => {
    sendCmd('"cmd":"sniff_apply"');
    const valid = lastDiscValid.filter(Boolean).length;
    snifferReport.push(`Применено: ${valid} из 4 найдено`);
    refreshSnifferStatus();
  });
  document.getElementById('btn-save-freq').addEventListener('click', saveFreq);
  document.getElementById('btn-activate').addEventListener('click', activateLicense);
  document.getElementById('btn-license-email').addEventListener('click', openLicenseEmail);
  document.getElementById('btn-batt-cal').addEventListener('click', calibrateBattery);
  document.getElementById('btn-reset-cal').addEventListener('click', () => sendCmd('"cmd":"battcal_reset"'));
  document.getElementById('btn-batt-cal').addEventListener('click', markCalibrated);
  document.getElementById('btn-reset-cal').addEventListener('click', () => {
    localStorage.removeItem('batt_calib_time');
    renderBattery();
  });

  ['input-capacity', 'input-interval', 'input-packets'].forEach(id => {
    document.getElementById(id).addEventListener('input', refreshBatteryLife);
  });
  
  // Freq radio buttons
  document.querySelectorAll('.freq-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.freq-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
    });
  });
  
  // Editor buttons
  document.getElementById('editor-cancel').addEventListener('click', closeEditor);
  document.getElementById('editor-close').addEventListener('click', closeEditor);
  document.getElementById('editor-save').addEventListener('click', saveSensorEditor);
  
  // Start status polling
  startStatusPolling();
  
  // Load saved prefs
  const cap = localStorage.getItem('batt_capacity_mah');
  if (cap) document.getElementById('input-capacity').value = cap;
  refreshBatteryLife();
  
  log('Приложение запущено. Нажмите «Поиск» для подключения.');
}

// ========== Tabs ==========

function switchTab(name) {
  document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.tab === name));
  document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.id === `page-${name}`));
}

// ========== Bluetooth ==========

async function scanAndConnect() {
  const btn = document.getElementById('btn-scan');
  btn.disabled = true;
  btn.textContent = 'Поиск...';
  setConnText('Поиск устройства...', 'scanning');
  
  try {
    device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: 'TPMS' }, { namePrefix: 'NRF' }],
      optionalServices: [NUS_SERVICE]
    });
    
    device.addEventListener('gattserverdisconnected', onDisconnected);
    
    setConnText(`Подключение к ${device.name}...`, 'scanning');
    log(`Выбрано: ${device.name}`);
    
    server = await device.gatt.connect();
    setConnText(`Подключение сервисов...`, 'scanning');
    
    const service = await server.getPrimaryService(NUS_SERVICE);
    txChar = await service.getCharacteristic(NUS_TX_CHAR);
    rxChar = await service.getCharacteristic(NUS_RX_CHAR);
    
    // Enable notifications on TX char (board → phone)
    await txChar.startNotifications();
    txChar.addEventListener('characteristicvaluechanged', onNotification);
    
    isConnected = true;
    nusReady = true;
    setConnText(device.name, 'connected');
    log('NUS готов к передаче');
    
    // Flush pending queue
    while (pendingQueue.length) {
      sendCmdInternal(pendingQueue.shift());
    }
    
    // Request initial status
    sendCmdInternal('{"cmd":"status"}\n');
    
  } catch (err) {
    if (err.name === 'NotFoundError') {
      log('Устройство не выбрано', 'err');
      setConnText('Устройство не выбрано', '');
    } else {
      log(`Ошибка: ${err.message}`, 'err');
      setConnText('Ошибка подключения', '');
    }
    console.error(err);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Поиск';
  }
}

function onDisconnected() {
  isConnected = false;
  nusReady = false;
  txChar = null;
  rxChar = null;
  setConnText('Отключено. Переподключение...', '');
  log('BLE отключен');
  
  // Auto-reconnect after 3s
  setTimeout(async () => {
    if (device && !isConnected) {
      try {
        log('Переподключение...');
        server = await device.gatt.connect();
        const service = await server.getPrimaryService(NUS_SERVICE);
        txChar = await service.getCharacteristic(NUS_TX_CHAR);
        rxChar = await service.getCharacteristic(NUS_RX_CHAR);
        await txChar.startNotifications();
        txChar.addEventListener('characteristicvaluechanged', onNotification);
        isConnected = true;
        nusReady = true;
        setConnText(device.name, 'connected');
        log('Переподключено');
        sendCmdInternal('{"cmd":"status"}\n');
      } catch (e) {
        log(`Переподключение не удалось: ${e.message}`, 'err');
        setConnText('Переподключение не удалось', '');
      }
    }
  }, 3000);
}

function onNotification(event) {
  const bytes = new Uint8Array(event.target.value.buffer);
  const text = new TextDecoder().decode(bytes);
  rxBuffer += text;
  
  let nlIdx = rxBuffer.indexOf('\n');
  while (nlIdx >= 0) {
    const line = rxBuffer.substring(0, nlIdx).trim();
    rxBuffer = rxBuffer.substring(nlIdx + 1);
    if (line) {
      onNusData(line);
    }
    nlIdx = rxBuffer.indexOf('\n');
  }
}

async function sendCmdInternal(msg) {
  if (!rxChar || !server || !server.connected) return;
  try {
    const encoder = new TextEncoder();
    const bytes = encoder.encode(msg);
    // Web BLE has a 512 byte MTU limit typically, but we chunk at 20 for compatibility
    const CHUNK = 20;
    for (let i = 0; i < bytes.length; i += CHUNK) {
      const chunk = bytes.slice(i, i + CHUNK);
      await rxChar.writeValue(chunk);
      if (i + CHUNK < bytes.length) {
        await sleep(30); // Small delay between chunks
      }
    }
  } catch (err) {
    log(`Ошибка отправки: ${err.message}`, 'err');
  }
}

function sendCmd(payload, quiet = false) {
  const msg = `{${payload}}\n`;
  if (!isConnected || !nusReady) {
    if (!quiet) {
      pendingQueue.push(msg);
      log('В очереди: ' + payload);
    }
    return;
  }
  if (pendingQueue.length > 0) {
    if (!quiet) pendingQueue.push(msg);
    return;
  }
  if (!quiet) log('→ ' + payload, 'tx');
  sendCmdInternal(msg);
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ========== Data Handling ==========

function onNusData(data) {
  try {
    const json = JSON.parse(data);
    switch (json.t) {
      case 'status':
        if (json.data) applyStatus(json.data);
        break;
      case 'log':
        log(json.m || data);
        break;
      case 'pkt':
        log(`TX PKT #${json.s}: id=${json.id} p=${json.p} t=${json.tmp} cnt=${json.c} crc=${json.crc}`, 'pkt');
        break;
      case 'disc':
        log(`DISC: id=${json.id} p=${json.p} t=${json.tmp} rssi=${json.rssi}`, 'pkt');
        break;
      default:
        log(data);
    }
  } catch {
    log(data);
  }
}

function applyStatus(data) {
  // Sensors
  if (data.sensors) {
    for (let i = 0; i < Math.min(data.sensors.length, 4); i++) {
      const s = data.sensors[i];
      sensors[i] = {
        id: s.id || '00000000',
        p: s.p || 0,
        t: s.t || 0,
        en: s.en !== 0
      };
    }
    renderSensors();
  }
  
  // Frequency
  const freq = data.freq || 315;
  document.querySelectorAll('.freq-btn').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.freq) === freq);
  });
  settings.freq = freq;
  
  // Auto TX
  settings.tx_en = data.tx_en || 0;
  settings.tx_int = data.tx_int || 60;
  settings.tx_pkt = data.tx_pkt || 2;
  const elInt = document.getElementById('input-interval');
  const elPkt = document.getElementById('input-packets');
  if (document.activeElement !== elInt) elInt.value = settings.tx_int;
  if (document.activeElement !== elPkt) elPkt.value = settings.tx_pkt;
  
  // License
  settings.license = data.license || 0;
  settings.trial_rem = data.trial_rem || 0;
  if (data.serial) settings.serial = data.serial;
  updateLicenseUi(settings.license, settings.trial_rem, settings.serial);
  
  // Battery
  settings.batt_mv = data.batt_mv || 0;
  settings.batt_pct = data.batt_pct || 0;
  settings.batt_raw = data.batt_raw || 0;
  settings.batt_pin = data.batt_pin || 0;
  settings.batt_cal = data.batt_cal || 0;
  settings.usb = data.usb || 0;
  renderBattery();
  
  const calInput = document.getElementById('input-batt-cal');
  if (document.activeElement !== calInput && settings.batt_cal > 0) {
    calInput.value = settings.batt_cal;
  }
  
  // Sniffer
  settings.sniff = data.sniff || 0;
  if (sniffRunning !== !!settings.sniff) {
    sniffRunning = !!settings.sniff;
    document.getElementById('btn-sniff-toggle').textContent = sniffRunning ? 'Остановить' : 'Старт сниффера';
  }
  settings.disc = data.disc || [];
  updateSnifferUI(!!settings.sniff, settings.disc);
}

// ========== UI Rendering ==========

function renderSensors() {
  document.querySelectorAll('.sensor-card').forEach((card, i) => {
    const s = sensors[i];
    card.querySelector('.s-id').textContent = s.id;
    card.querySelector('.s-pressure span').textContent = s.p || '--';
    card.querySelector('.s-temp span').textContent = s.t || '--';
    card.classList.toggle('disabled', !s.en);
  });
}

function renderBattery() {
  const mv = settings.batt_mv;
  const pct = settings.batt_pct;
  const usb = settings.usb;
  const raw = settings.batt_raw;
  const pin = settings.batt_pin;
  const cal = settings.batt_cal;
  
  const icon = document.querySelector('.battery-icon');
  const fill = icon.querySelector('.battery-fill');
  const usbMode = usb || (mv === 0 && raw > 0);

  icon.className = 'battery-icon' + (usbMode ? ' usb' : (pct < 15 ? ' low' : ''));
  fill.style.width = usbMode ? '100%' : Math.max(2, pct) + '%';

  const pctEl = document.querySelector('.battery-pct');
  pctEl.textContent = usbMode ? 'USB' : pct + '%';
  pctEl.style.color = usbMode ? 'var(--accent)' : (pct < 15 ? 'var(--red)' : 'var(--green)');

  let details = `Напряжение: ${(mv / 1000).toFixed(2)} В (${pct}%)\n`;
  if (usbMode) details += 'Питание: USB (АКБ не измеряется)\n';
  const calibTime = parseInt(localStorage.getItem('batt_calib_time') || '0', 10);
  details += `Последняя калибровка: ${formatAgo(calibTime)}`;
  document.querySelector('.battery-details').textContent = details;
}

function setConnText(text, state) {
  document.getElementById('conn-text').textContent = text;
  const dot = document.querySelector('.dot');
  dot.className = 'dot' + (state ? ' ' + state : '');
}

function log(msg, cls = '') {
  const el = document.getElementById('log');
  const line = document.createElement('span');
  if (cls) line.className = 'log-' + cls;
  line.textContent = msg + '\n';
  el.appendChild(line);
  // Auto-scroll
  el.scrollTop = el.scrollHeight;
  // Limit lines
  while (el.children.length > 500) el.removeChild(el.firstChild);
}

function flashCard(card) {
  card.style.transform = 'scale(1.06)';
  setTimeout(() => { card.style.transform = ''; }, 200);
}

// ========== Actions ==========

function saveAutoTx() {
  const interval = parseInt(document.getElementById('input-interval').value) || 60;
  const packets = parseInt(document.getElementById('input-packets').value) || 2;
  sendCmd(`"cmd":"autotx","interval":${interval},"packets":${packets}`);
  settings.tx_int = interval;
  settings.tx_pkt = packets;
}

function toggleSniff() {
  sniffRunning = !sniffRunning;
  document.getElementById('btn-sniff-toggle').textContent = sniffRunning ? 'Остановить' : 'Старт сниффера';
  sendCmd(sniffRunning ? '"cmd":"sniff_start"' : '"cmd":"sniff_stop"');
  if (sniffRunning) {
    snifferReport = [];
    lastDiscValid = [false, false, false, false];
    wasSniffActive = false;
  }
  refreshSnifferStatus();
}

function saveFreq() {
  const active = document.querySelector('.freq-btn.active');
  const freq = active ? parseInt(active.dataset.freq) : 433;
  sendCmd(`"cmd":"settings","freq":${freq}`);
}

function activateLicense() {
  const key = document.getElementById('input-license').value.trim();
  if (key.length === 8) {
    sendCmd(`"cmd":"license","key":"${key}"`);
  } else {
    log('Ключ должен быть 8 символов', 'err');
  }
}

function updateLicenseUi(license, trialRem, serial) {
  const badge = document.getElementById('license-badge');
  const inputs = document.getElementById('lic-inputs');
  inputs.style.display = license === 2 ? 'none' : 'block';
  if (license === 2) {
    badge.textContent = 'Лицензия пройдена';
    badge.className = 'license-badge license-active';
  } else if (license === 1) {
    const h = trialRem / 3600.0;
    const left = h >= 48 ? Math.round(h / 24) + ' дн' : h.toFixed(1) + ' ч';
    badge.textContent = 'Пробный период: осталось ~' + left;
    badge.className = 'license-badge license-trial';
  } else {
    badge.textContent = 'Лицензия не активна — отправка TX отключена';
    badge.className = 'license-badge license-inactive';
  }
}

function openLicenseEmail() {
  let body = 'Добрый день!\n\n';
  body += 'Прошу выдать лицензионный ключ для устройства:\n';
  body += 'Устройство: TPMS-NRF52840\n';
  if (settings.serial) {
    body += 'ID устройства (serial): ' + settings.serial + '\n';
  } else {
    body += '(ID устройства появится после подключения по Bluetooth — пришли письмо повторно)\n';
  }
  body += 'Спасибо!';
  const subject = 'Запрос лицензии TPMS-NRF52840';
  location.href = 'mailto:wargaelanor@ya.ru?subject=' + encodeURIComponent(subject) + '&body=' + encodeURIComponent(body);
}

function calibrateBattery() {
  const mv = parseInt(document.getElementById('input-batt-cal').value);
  if (mv && mv > 0) {
    sendCmd(`"cmd":"battcal","mv":${mv}`);
    localStorage.setItem('batt_calib_time', String(Date.now()));
    renderBattery();
  } else {
    log('Введите реальное напряжение АКБ (мВ)', 'err');
  }
}

function markCalibrated() {
  localStorage.setItem('batt_calib_time', String(Date.now()));
  renderBattery();
}

function refreshBatteryLife() {
  const capacity = parseInt(document.getElementById('input-capacity').value) || 0;
  const interval = parseInt(document.getElementById('input-interval').value) || 0;
  const packets = parseInt(document.getElementById('input-packets').value) || 0;
  const el = document.getElementById('calc-result');

  localStorage.setItem('batt_capacity_mah', capacity);

  if (capacity <= 0 || interval <= 0 || packets < 1) {
    el.textContent = 'Укажи ёмкость, интервал и пакеты';
    el.className = 'calc-result bad';
    return;
  }

  // Режим сна: между передачами BLE выключен, CC1101 обесточен, CPU спит.
  const sleepMa = 0.05;
  const wakeMaS = 15.0 * 0.03;
  const pktMaS = 25.0 * 0.06;
  const gapMaS = 3.0 * 0.1;
  const burstMaS = wakeMaS + packets * pktMaS + (packets - 1) * gapMaS;
  const avgMa = sleepMa + burstMaS / interval;
  const hours = capacity / avgMa;

  el.textContent = 'Время работы от АКБ: ≈ ' + formatDuration(hours);
  el.className = 'calc-result good';
}

function formatDuration(hours) {
  if (hours >= 8760) return (hours / 8760).toFixed(1) + ' лет';
  if (hours >= 720) return (hours / 720).toFixed(1) + ' мес';
  if (hours >= 48) return Math.floor(hours / 24) + ' дн ' + Math.floor(hours) % 24 + ' ч';
  if (hours >= 1) return hours.toFixed(1) + ' ч';
  return Math.round(hours * 60) + ' мин';
}

function formatAgo(timeMillis) {
  if (!timeMillis) return 'нет';
  const diff = Date.now() - timeMillis;
  const d = diff / 86400000.0;
  const h = diff / 3600000.0;
  if (diff < 60000) return 'только что';
  if (d >= 2) return Math.round(d) + ' дн назад';
  if (d >= 1) return d.toFixed(1) + ' дн назад';
  if (h >= 1) return h.toFixed(1) + ' ч назад';
  return Math.round(diff / 60000) + ' мин назад';
}

// ========== Sniffer ==========

function updateSnifferUI(sniffActive, disc) {
  if (disc) {
    for (let i = 0; i < disc.length; i++) {
      const obj = disc[i];
      if (obj && !lastDiscValid[i] && obj.id && obj.id !== '?') {
        lastDiscValid[i] = true;
        snifferReport.push(`#${i + 1}: ${obj.id}`);
      }
    }
  }
  const allValid = lastDiscValid.every(Boolean);
  if (wasSniffActive && !sniffActive && allValid) {
    snifferReport.push('Все 4 датчика найдены!');
  }
  wasSniffActive = sniffActive;
  refreshSnifferStatus();
}

function refreshSnifferStatus() {
  const el = document.getElementById('sniff-status');
  let text = sniffRunning ? 'Сниффер: активен' : 'Сниффер: выкл';
  if (snifferReport.length) text += '\n' + snifferReport.join('\n');
  el.textContent = text;
  el.style.color = sniffRunning ? 'var(--accent)' : 'var(--text3)';
}

// ========== Sensor Editor ==========

let editorIdx = -1;

function openSensorEditor(idx) {
  editorIdx = idx;
  const s = sensors[idx];
  document.getElementById('editor-title').textContent = SENSOR_NAMES[idx];
  document.getElementById('editor-id').value = s.id;
  document.getElementById('editor-pressure').value = s.p || '';
  document.getElementById('editor-temp').value = s.t || '';
  document.getElementById('editor-enabled').checked = s.en;
  document.getElementById('modal-overlay').classList.add('open');
}

function closeEditor() {
  document.getElementById('modal-overlay').classList.remove('open');
  editorIdx = -1;
}

function saveSensorEditor() {
  if (editorIdx < 0) return;
  const id = document.getElementById('editor-id').value.trim();
  const pressure = parseInt(document.getElementById('editor-pressure').value) || 230;
  const temp = parseInt(document.getElementById('editor-temp').value) || 20;
  const enabled = document.getElementById('editor-enabled').checked;
  
  sensors[editorIdx] = { id, p: pressure, t: temp, en: enabled };
  renderSensors();
  
  sendCmd(`"cmd":"sensor","sensor":${editorIdx},"id":"${id}","pressure":${pressure},"temp":${temp},"enabled":${enabled ? 1 : 0}`);
  closeEditor();
}

// ========== Status Polling ==========

function startStatusPolling() {
  statusTimer = setInterval(() => {
    if (isConnected && nusReady) {
      sendCmd('"cmd":"status"', true);
    }
  }, 4000);
}

// ========== Boot ==========

document.addEventListener('DOMContentLoaded', init);

// Register service worker
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('sw.js').catch(() => {});
}
