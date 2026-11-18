'use strict';

const copy = {
  en: {
    appointments: 'Appointments', connections: 'Connect a device', newAppointment: 'New appointment',
    schedule: 'Schedule', previous: 'Previous', next: 'Next', details: 'Details', refresh: 'Refresh',
    loading: 'Loading appointments…', chooseAppointment: 'Select an appointment.',
    empty: 'No upcoming appointments.', enableAlerts: 'Enable browser alerts', close: 'Close',
    game: 'Game', gameId: 'Local communication ID (LCID)', appId: 'Application ID (optional)',
    localStart: 'Start time (local)', cancel: 'Cancel', create: 'Create', share: 'Share appointment',
    shareUrl: 'Appointment link', copy: 'Copy link', copied: 'Link copied.', selectLink: 'Link selected. Copy it to share.',
    chooseGame: 'Select a game', manualGame: 'Enter a game ID', following: 'Following', follow: 'Follow',
    unfollow: 'Unfollow', markReady: 'Mark ready', stopReady: 'Clear ready',
    readyCount: n => `${n} ready`, readyNow: n => `Ready lasts ${n} minutes.`,
    readyLater: n => `Ready opens ${n} minutes before the start.`, localTimezone: zone => `Times in ${zone}`,
    page: page => `Page ${page}`, rooms: 'Open rooms', roomsSoon: n => `Rooms show from ${n} minutes before the start.`,
    checkingRooms: 'Checking for rooms…', expired: 'This appointment has ended.',
    roomsEmpty: 'No open rooms yet.', roomError: 'Could not check rooms. Refresh to retry.',
    noEndpoints: 'No connection details are configured for this server.', unknownHost: 'Room',
    capacity: (n, max) => `${n} / ${max} players`, exactRoom: 'For this appointment',
    lcidMatch: 'LCID match', crossplay: 'Crossplay relay',
    hostNotice: game => `A room is open for ${game}.`, newRooms: 'New rooms',
    roomInstructions: 'Use Local Wireless in the game. Everyone needs the same game version.',
    appointmentCreated: 'Appointment saved.', failed: 'Could not reach the server. Refresh to retry.',
    unavailable: 'Appointments are temporarily unavailable.', invalid: 'This operation is invalid for the selected appointment.',
    not_found: 'This appointment is unavailable or has expired.', storage: 'The server could not save the change.',
    limit: 'Too many requests. Try again shortly.', timeInvalid: 'Choose a start time within the next seven days.',
    idInvalid: 'Enter a valid 16-digit local communication ID.',
    alertsDenied: 'Browser notifications are blocked in your site settings.',
    alertsEnabled: 'Browser alerts are enabled for this page.',
    alertsUnavailable: 'Browser alerts are unavailable. Open rooms still appear here.',
    rememberedLocally: 'Following and alerts are saved in this browser.',
    switchLabel: 'Switch', edenLabel: 'Eden 0.2.1', yuzuLabel: 'Original Yuzu / Eden 0.2.0',
    ryuldnLabel: 'Ryubing / Kenji-NX', lanplayLabel: 'LAN Play / supported Apple clients',
    switchHelp: 'Select this server in the RoomCircuit Switch app.',
    edenHelp: 'Multiplayer → Direct Connect.',
    yuzuHelp: 'Multiplayer → Direct Connect.',
    ryuldnHelp: 'Use RyuLDN with a supported build. Set the server address and disable P2P.',
    lanplayHelp: 'Use a LAN Play capable build or the routed Wi-Fi gateway. MeloNX requires the gateway patch.',
  },
  zh: {
    appointments: '约战', connections: '连接设备', newAppointment: '发起约战',
    schedule: '约战列表', previous: '上一页', next: '下一页', details: '约战详情', refresh: '刷新',
    loading: '正在加载约战…', chooseAppointment: '选择一个约战。', empty: '暂无近期约战。',
    enableAlerts: '开启浏览器提醒', close: '关闭', game: '游戏', gameId: '本地通信 ID（LCID）',
    appId: '应用 ID（选填）', localStart: '开战时间（本地）', cancel: '取消', create: '创建', share: '分享约战',
    shareUrl: '约战链接', copy: '复制链接', copied: '已复制链接。', selectLink: '链接已选中，可手动复制。',
    chooseGame: '选择游戏', manualGame: '手动输入游戏 ID', following: '已关注', follow: '关注',
    unfollow: '取消关注', markReady: '标记就绪', stopReady: '取消就绪',
    readyCount: n => `${n} 人就绪`, readyNow: n => `就绪状态有效期 ${n} 分钟。`,
    readyLater: n => `开战前 ${n} 分钟可标记就绪。`, localTimezone: zone => `时间以 ${zone} 显示`,
    page: page => `第 ${page} 页`, rooms: '已开放的房间', roomsSoon: n => `开战前 ${n} 分钟开始显示房间。`,
    checkingRooms: '正在查询开放房间…', expired: '此约战已结束。',
    roomsEmpty: '暂无开放房间。', roomError: '无法查询房间，请刷新重试。',
    noEndpoints: '服务器尚未配置连接信息。', unknownHost: '房间',
    capacity: (n, max) => `${n} / ${max} 人`, exactRoom: '此约战的房间',
    lcidMatch: '按联机 ID 匹配', crossplay: '跨端线路',
    hostNotice: game => `${game} 的房间已开放。`, newRooms: '有新房间',
    roomInstructions: '请在游戏中选择本地通信，并使用相同的游戏版本。',
    appointmentCreated: '约战已保存。', failed: '无法连接服务器，请刷新重试。',
    unavailable: '约战暂不可用。', invalid: '所选约战不支持这项操作。',
    not_found: '该约战已过期或不可用。', storage: '服务器未能保存此次更改。',
    limit: '请求过多，请稍后重试。', timeInvalid: '请选择未来七天内的开战时间。',
    idInvalid: '请输入有效的 16 位本地通信 ID。',
    alertsDenied: '此网站的浏览器通知已被禁用。', alertsEnabled: '本页面已开启浏览器提醒。',
    alertsUnavailable: '浏览器提醒不可用，仍可在此页面查看开放房间。',
    rememberedLocally: '关注和提醒保存在当前浏览器。',
    switchLabel: 'Switch', edenLabel: 'Eden 0.2.1', yuzuLabel: '原版柚子 / Eden 0.2.0',
    ryuldnLabel: 'Ryubing / Kenji-NX', lanplayLabel: 'LAN Play / 受支持的 Apple 客户端',
    switchHelp: '在 Switch 的 RoomCircuit 应用中选择此服务器。',
    edenHelp: '进入「多人 → 直连」。', yuzuHelp: '进入「多人 → 直连」。',
    ryuldnHelp: '使用受支持版本的 RyuLDN，填写服务器地址并关闭 P2P。',
    lanplayHelp: '使用支持 LAN Play 的客户端或 Wi-Fi 网关。MeloNX 需要网关补丁。',
  },
};

const $ = id => document.getElementById(id);
const storageKey = 'roomcircuit.appointments.';
const readLocal = (key, fallback) => {
  try { return JSON.parse(localStorage.getItem(storageKey + key)) ?? fallback; }
  catch { return fallback; }
};
const writeLocal = (key, value) => {
  try { localStorage.setItem(storageKey + key, JSON.stringify(value)); }
  catch { /* The page still works if its browser restricts local storage. */ }
};
const hexId = value => typeof value === 'string' && /^[0-9a-f]{16}$/i.test(value);
const eventId = value => typeof value === 'string' && /^[1-9][0-9]{0,19}$/.test(value) &&
  BigInt(value) <= 0xffffffffffffffffn;
const knownGame = value => hexId(value) && !/^(0{16}|f{16})$/i.test(value);
const urlEvent = new URLSearchParams(location.search).get('event');
const identityValue = readLocal('identity', '');
const identity = typeof identityValue === 'string' && /^[a-f0-9]{32}$/.test(identityValue) &&
  !/^0+$/.test(identityValue)
  ? identityValue
  : Array.from(crypto.getRandomValues(new Uint8Array(16)), n => n.toString(16).padStart(2, '0')).join('');
writeLocal('identity', identity);
const state = {
  language: readLocal('language', navigator.language.startsWith('zh') ? 'zh' : 'en') === 'zh' ? 'zh' : 'en',
  options: { games: [], endpoints: [], readyBefore: 1800, readyLease: 300 },
  rows: null, selected: eventId(urlEvent) ? urlEvent : null, exact: null, rooms: new Map(),
  followed: new Map(), offset: 0, page: 1, next: null, pageOffsets: [0],
  clockShift: 0, roomIndex: 0, unseen: false, busy: false, polling: false, navigating: false, listError: null,
};
function loadFollows() {
  const value = readLocal('followed', []);
  state.followed = new Map(Array.isArray(value) ? value.filter(row =>
    row && eventId(row.id) && Number.isSafeInteger(row.starts)).slice(0, 24).map(row => [row.id, row]) : []);
}
loadFollows();

const t = (key, ...args) => typeof copy[state.language][key] === 'function'
  ? copy[state.language][key](...args) : copy[state.language][key];
const clock = () => Math.floor(Date.now() / 1000 + state.clockShift);
const locale = () => state.language === 'zh' ? 'zh-CN' : 'en';
const activeUntil = 2 * 60 * 60;
let titleFormatter;
function make(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}
function button(key, className, onClick) {
  const element = make('button', className, t(key));
  element.type = 'button';
  element.dataset.dynamicAction = key;
  element.addEventListener('click', onClick);
  return element;
}
function formatTime(value) {
  return titleFormatter.format(new Date(value * 1000));
}
function gameName(row) {
  const selected = state.options.games.find(game => game.game.toUpperCase() === row.game.toUpperCase() &&
    game.app.toUpperCase() === row.app.toUpperCase());
  return selected ? (state.language === 'zh' && selected.zh ? selected.zh : selected.name) : `LCID ${row.game.toUpperCase()}`;
}
function selected() {
  return state.rows?.find(row => row.id === state.selected) ??
    (state.exact?.id === state.selected ? state.exact : null);
}
function normalizeRows(rows) {
  return Array.isArray(rows) ? rows.filter(row => row && eventId(row.id) && Number.isSafeInteger(row.starts) &&
    row.starts >= 0 && row.starts <= 8_640_000_000_000 && hexId(row.game) && hexId(row.app) &&
    Number.isSafeInteger(row.interested) && row.interested >= 0 && typeof row.ready === 'boolean') : [];
}
function rememberFollows() {
  writeLocal('followed', [...state.followed.values()]);
}
function status(key, error = false, target = $('status'), args = []) {
  target.hidden = !key;
  target.classList.toggle('error', error);
  target.textContent = key ? (t(key, ...args) || t('failed')) : '';
}
function applyLanguage() {
  document.documentElement.lang = locale();
  $('language-button').textContent = state.language === 'zh' ? 'English' : '中文';
  $('language-button').lang = state.language === 'zh' ? 'en' : 'zh-CN';
  document.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = t(el.dataset.i18n); });
  titleFormatter = new Intl.DateTimeFormat(locale(), {
    month: 'short', day: 'numeric', weekday: 'short', hour: 'numeric', minute: '2-digit',
  });
  $('local-timezone').textContent = t('localTimezone', titleFormatter.resolvedOptions().timeZone);
  $('pages').setAttribute('aria-label', state.language === 'zh' ? '约战分页' : 'Appointment pages');
  $('game-id').placeholder = state.language === 'zh' ? '16 位十六进制数' : '16 hexadecimal digits';
  updateTitle();
  showAlertControl();
  fillGames();
  renderConnections();
  render();
}
function updateTitle() {
  document.title = `${state.unseen ? `${t('newRooms')} · ` : ''}${t('appointments')} · RoomCircuit`;
}
function showAlertControl() {
  $('notification-button').hidden = !('Notification' in window) || !window.isSecureContext ||
    Notification.permission === 'granted';
}
function fillGames() {
  const select = $('game-select'), oldValue = select.value;
  const options = state.options.games;
  const placeholder = make('option', '', t('chooseGame'));
  placeholder.value = '';
  placeholder.disabled = true;
  select.replaceChildren(placeholder, ...options.map((row, index) => {
    const option = make('option', '', state.language === 'zh' && row.zh ? row.zh : row.name);
    option.value = String(index);
    return option;
  }));
  const manual = make('option', '', t('manualGame'));
  manual.value = 'manual';
  select.append(manual);
  select.value = oldValue !== '' && [...select.options].some(option => option.value === oldValue)
    ? oldValue : options.length ? '' : 'manual';
  toggleManual();
}
function toggleManual() {
  const manual = $('game-select').value === 'manual';
  $('manual-game').hidden = !manual;
  $('game-id').required = manual;
  $('game-id').disabled = !manual;
  $('application-id').disabled = !manual;
}
function renderConnections() {
  const list = $('connection-list');
  const endpoints = state.options.endpoints;
  const labels = { switch: 'switch', eden: 'eden', yuzu: 'yuzu', ryuldn: 'ryuldn', lanplay: 'lanplay' };
  list.replaceChildren();
  if (!endpoints.length) list.append(make('p', 'rooms-status', t('noEndpoints')));
  for (const endpoint of endpoints) {
    if (!endpoint) continue;
    const key = labels[endpoint.id];
    if (!key || typeof endpoint.host !== 'string' || !endpoint.host || !Number.isInteger(endpoint.port) ||
      endpoint.port < 1 || endpoint.port > 65535) continue;
    const row = make('section', 'connection-item');
    const address = make('code', 'endpoint-address', `${endpoint.host}:${endpoint.port}`);
    row.append(make('h3', '', t(key + 'Label')), address, make('p', 'connection-instruction', t(key + 'Help')));
    list.append(row);
  }
  list.append(make('p', 'rooms-status', t('roomInstructions')));
}
let lastRender;
function render() {
  const list = $('appointment-list');
  const detail = $('appointment-detail');
  const row = selected();
  const current = clock();
  const expired = row && current >= row.starts + activeUntil;
  const open = row && !expired && current >= row.starts - state.options.readyBefore;
  const roomState = row ? state.rooms.get(row.id) : null;
  const key = JSON.stringify([
    state.language, state.rows, row, open, expired, roomState, [...state.followed.keys()], state.listError, state.busy,
    state.options.readyBefore, state.options.readyLease, state.navigating,
    state.page, state.next, state.options.games,
  ]);
  if (key === lastRender) return;
  lastRender = key;
  const focused = document.activeElement;
  const rowFocus = focused?.dataset.row;
  const actionFocus = focused?.dataset.dynamicAction;
  list.setAttribute('aria-busy', String(state.rows === null && !state.listError));
  list.replaceChildren();
  if (!state.rows?.length) {
    list.append(make('p', 'empty', t(state.listError || (state.rows ? 'empty' : 'loading'))));
  } else {
    for (const item of state.rows) {
      const container = make('article');
      const entry = make('button', 'appointment-row');
      entry.type = 'button';
      entry.dataset.row = item.id;
      entry.setAttribute('aria-pressed', String(item.id === state.selected));
      entry.setAttribute('aria-controls', 'appointment-detail');
      const time = make('time', 'time', formatTime(item.starts));
      time.dateTime = new Date(item.starts * 1000).toISOString();
      entry.append(make('span', 'game-title', gameName(item)),
        make('span', `ready-count${item.ready ? ' active' : ''}`, t('readyCount', item.interested)), time,
        make('span', 'following', state.followed.has(item.id) ? t('following') : ''));
      entry.addEventListener('click', () => {
        choose(item.id);
        revealDetails();
      });
      container.append(entry);
      list.append(container);
    }
  }
  $('pages').hidden = state.offset === 0 && state.next === null;
  $('previous-button').disabled = state.offset === 0 || state.busy || state.navigating;
  $('next-button').disabled = state.next === null || state.busy || state.navigating;
  $('page-number').textContent = t('page', state.page);
  detail.replaceChildren();
  if (row) {
    detail.append(make('h3', '', gameName(row)), make('time', 'time', formatTime(row.starts)));
    detail.lastChild.dateTime = new Date(row.starts * 1000).toISOString();
    const actions = make('div', 'actions');
    const ready = button(row.ready ? 'stopReady' : 'markReady', 'primary', () => setReady(row));
    ready.disabled = state.busy || !open;
    actions.append(ready, button(state.followed.has(row.id) ? 'unfollow' : 'follow', 'secondary', () => toggleFollow(row)),
      button('share', 'text-button', () => showShare(row)));
    detail.append(actions);
    const readyState = make('p', 'readiness', t('readyCount', row.interested));
    if (!expired && (!open || row.ready)) {
      const value = row.ready ? state.options.readyLease : state.options.readyBefore;
      readyState.append(document.createTextNode(` · ${t(row.ready ? 'readyNow' : 'readyLater', value / 60)}`));
    }
    detail.append(readyState);
    const rooms = make('section', 'rooms');
    rooms.append(make('h2', '', t('rooms')));
    if (!open || roomState?.error || !roomState?.open?.length) {
      const msg = expired ? 'expired' : !open ? 'roomsSoon' : !roomState ? 'checkingRooms' :
        roomState.error ? 'roomError' : 'roomsEmpty';
      rooms.append(make('p', 'rooms-status', t(msg, state.options.readyBefore / 60)));
    } else {
      for (const room of roomState.open) {
        const item = make('div', 'room-row');
        const labels = [t('capacity', room.players, room.max)];
        if (room.appointmentBound) labels.push(t('exactRoom'));
        else if (!room.titleConfirmed) labels.push(t('lcidMatch'));
        if (room.crossplay) labels.push(t('crossplay'));
        item.append(make('p', 'room-name', room.host || t('unknownHost')), make('p', 'room-info', labels.join(' · ')));
        rooms.append(item);
      }
      rooms.append(button('connections', 'text-button', openConnections));
    }
    detail.append(rooms);
  } else {
    detail.append(make('p', 'empty', t('chooseAppointment')));
  }
  if (rowFocus && !focused.isConnected) list.querySelector(`[data-row="${rowFocus}"]`)?.focus({ preventScroll: true });
  if (actionFocus && !focused.isConnected) {
    const alternatives = { stopReady: 'markReady', markReady: 'stopReady', follow: 'unfollow', unfollow: 'follow' };
    const match = detail.querySelector(`[data-dynamic-action="${actionFocus}"]`) ??
      detail.querySelector(`[data-dynamic-action="${alternatives[actionFocus]}"]`);
    if (match && !match.disabled) match.focus({ preventScroll: true });
  }
}
async function request(data) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch('/api/appointments', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, cache: 'no-store',
      body: JSON.stringify({ ...data, identity }), signal: controller.signal,
    });
    const result = await response.json();
    if (Number.isSafeInteger(result.now)) state.clockShift = result.now - Date.now() / 1000;
    if (!response.ok || result.error) throw Object.assign(new Error('Appointment request failed'), {
      code: typeof result.error === 'string' && Object.hasOwn(copy.en, result.error) ? result.error : 'failed',
    });
    return result;
  } finally { clearTimeout(timeout); }
}
async function loadOptions() {
  try {
    const response = await fetch('/api/appointments/options', { cache: 'no-store' });
    if (!response.ok) return;
    const options = await response.json();
    if (Array.isArray(options.games)) state.options.games = options.games.filter(row =>
      row && knownGame(row.game) && hexId(row.app) && typeof row.name === 'string');
    if (Array.isArray(options.endpoints)) state.options.endpoints = options.endpoints;
    if (Number.isInteger(options.readyBefore) && options.readyBefore > 0) state.options.readyBefore = options.readyBefore;
    if (Number.isInteger(options.readyLease) && options.readyLease > 0) state.options.readyLease = options.readyLease;
  } catch { /* Listing and a manually entered game ID still work. */ }
  applyLanguage();
}
let listRevision = 0;
async function refreshList(offset = state.offset, page = state.page) {
  const revision = ++listRevision;
  try {
    const result = await request({ action: 'list', offset });
    if (revision !== listRevision) return;
    state.listError = null;
    state.rows = normalizeRows(result.rows);
    state.next = Number.isSafeInteger(result.next) && result.next > offset ? result.next : null;
    state.offset = offset;
    state.page = page;
    state.pageOffsets[page - 1] = offset;
    if (state.rows.length === 0 && offset) return refreshList(0, 1);
    if (!state.selected) state.selected = state.rows[0]?.id ?? null;
    render();
  } catch (error) {
    if (revision !== listRevision) return;
    state.listError = error.code || 'failed';
    status(state.listError, true);
    render();
  }
}
async function refreshExact(id, inform = false) {
  try {
    const result = await request({ action: 'event', id });
    if (state.selected !== id) return;
    state.exact = normalizeRows(result.rows).find(row => row.id === id) || null;
  } catch (error) {
    if (state.selected !== id) return;
    if (error.code === 'not_found') {
      state.exact = null;
      state.selected = state.rows?.[0]?.id || null;
      if (inform) status('not_found', true);
    } else if (inform) status(error.code || 'failed', true);
  }
  render();
}
async function changePage(offset, page) {
  if (state.busy || state.navigating) return;
  state.navigating = true;
  render();
  try { await refreshList(offset, page); }
  finally { state.navigating = false; render(); }
}
function eventUrl(id) {
  const url = new URL(location.pathname, location.origin);
  url.searchParams.set('event', id);
  return url;
}
function choose(id) {
  state.selected = id;
  state.exact = null;
  state.unseen = false;
  updateTitle();
  status();
  history.replaceState(null, '', eventUrl(id));
  render();
  void refreshRooms(selected()).catch(() => {});
}
function revealDetails() {
  if (!matchMedia('(max-width: 760px)').matches) return;
  const heading = $('detail-heading');
  heading.tabIndex = -1;
  heading.focus({ preventScroll: true });
  $('appointment-detail').parentElement.scrollIntoView({ block: 'start' });
}
function showShare(row) {
  $('share-url').value = eventUrl(row.id).toString();
  $('copy-status').hidden = true;
  $('share-dialog').showModal();
  $('share-url').select();
}
function openConnections() {
  renderConnections();
  $('connection-dialog').showModal();
}
const roomChecks = new Set();
async function refreshRooms(row) {
  const now = clock();
  if (!row || row.starts - state.options.readyBefore > now || row.starts + activeUntil <= now ||
    roomChecks.has(row.id)) return;
  roomChecks.add(row.id);
  try {
    const result = await request({ action: 'rooms', id: row.id });
    const rooms = Array.isArray(result.rooms) ? result.rooms.filter(room => room && Number.isInteger(room.players) &&
      Number.isInteger(room.max) && (typeof room.host === 'string' || room.host === null)) : [];
    const firstOpen = !state.rooms.get(row.id)?.open?.length && rooms.length;
    state.rooms.set(row.id, { open: rooms });
    if (firstOpen && state.followed.has(row.id)) {
      if (document.hidden) { state.unseen = true; updateTitle(); }
      status('hostNotice', false, $('status'), [gameName(row)]);
      if ('Notification' in window && Notification.permission === 'granted') {
        try { new Notification('RoomCircuit', { body: t('hostNotice', gameName(row)), tag: `rc-${row.id}` }); }
        catch { /* Some mobile browsers expose Notification but require a worker. */ }
      }
    }
  } catch {
    state.rooms.set(row.id, { error: true, open: state.rooms.get(row.id)?.open || [] });
  } finally {
    roomChecks.delete(row.id);
    render();
  }
}
function toggleFollow(row) {
  if (state.followed.delete(row.id)) state.rooms.delete(row.id);
  else {
    if (state.followed.size >= 24) { status('limit', true); return; }
    state.followed.set(row.id, row);
  }
  rememberFollows();
  status('rememberedLocally');
  render();
}
async function poll() {
  if (state.busy || state.polling || state.navigating) return;
  state.polling = true;
  try {
    await refreshList();
    if (state.selected && !state.rows?.some(row => row.id === state.selected)) await refreshExact(state.selected);
    const now = clock();
    for (const [id, item] of state.followed) if (item.starts + activeUntil <= now) {
      state.followed.delete(id);
      state.rooms.delete(id);
      rememberFollows();
    }
    const open = [...state.followed.values()].filter(row => row.id !== state.selected &&
      row.starts - state.options.readyBefore <= now);
    const window = [];
    for (let n = 0; n < Math.min(3, open.length); n++) window.push(open[(state.roomIndex + n) % open.length]);
    if (open.length) state.roomIndex = (state.roomIndex + window.length) % open.length;
    await Promise.all([selected(), ...window].map(refreshRooms));
    const visibleIds = new Set([state.selected, ...state.followed.keys()]);
    for (const id of state.rooms.keys()) if (!visibleIds.has(id)) state.rooms.delete(id);
  } finally { state.polling = false; render(); }
}
function dateInputValue(seconds) {
  const time = new Date(seconds * 1000);
  const pair = n => String(n).padStart(2, '0');
  return `${time.getFullYear()}-${pair(time.getMonth() + 1)}-${pair(time.getDate())}T${pair(time.getHours())}:${pair(time.getMinutes())}`;
}
function openCreate() {
  const now = clock();
  const field = $('start-time');
  field.min = dateInputValue(Math.ceil(now / 60) * 60);
  field.max = dateInputValue(now + 7 * 24 * 60 * 60);
  field.value = dateInputValue(Math.ceil((now + 1800) / 60) * 60);
  status(null, false, $('create-error'));
  $('create-dialog').showModal();
}
async function createAppointment(event) {
  event.preventDefault();
  if (state.busy) return;
  const game = $('game-select').value === 'manual'
    ? { game: $('game-id').value, app: $('application-id').value }
    : state.options.games[Number($('game-select').value)];
  if (!game || !knownGame(game.game) || (game.app && !hexId(game.app))) {
    status('idInvalid', true, $('create-error'));
    return;
  }
  const value = $('start-time').value;
  const starts = Date.parse(value) / 1000;
  if (!Number.isSafeInteger(starts) || starts < clock() || starts - clock() > 7 * 86400 ||
    dateInputValue(starts) !== value) {
    status('timeInvalid', true, $('create-error'));
    return;
  }
  state.busy = true;
  $('create-fields').disabled = $('submit-appointment').disabled = true;
  try {
    const result = await request({ action: 'create', game: game.game.toUpperCase(),
      ...(game.app ? { app: game.app.toUpperCase() } : {}), starts });
    const created = result.created?.id || (typeof result.created === 'string' ? result.created : null);
    const row = normalizeRows(result.rows).find(item => item.starts === starts &&
      item.game.toUpperCase() === game.game.toUpperCase() &&
      item.app.toUpperCase() === (game.app || '0000000000000000').toUpperCase());
    $('create-dialog').close();
    const id = eventId(created) ? created : row?.id;
    if (id) choose(id);
    await refreshList(0, 1);
    if (id && !selected()) await refreshExact(id);
    status('appointmentCreated');
  } catch (error) { status(error.code || 'failed', true, $('create-error')); }
  finally {
    state.busy = false;
    $('create-fields').disabled = $('submit-appointment').disabled = false;
    render();
  }
}
async function setReady(row) {
  if (state.busy) return;
  status();
  state.busy = true;
  render();
  try {
    await request({ action: row.ready ? 'leave' : 'ready', id: row.id });
    await refreshList(0, 1);
    if (state.selected === row.id && !state.rows.some(item => item.id === row.id)) await refreshExact(row.id);
  } catch (error) { status(error.code || 'failed', true); }
  finally { state.busy = false; render(); }
}
$('language-button').addEventListener('click', () => {
  state.language = state.language === 'en' ? 'zh' : 'en';
  writeLocal('language', state.language);
  status();
  applyLanguage();
});
$('refresh-button').addEventListener('click', () => { status(); void poll(); });
$('create-button').addEventListener('click', openCreate);
$('game-select').addEventListener('change', toggleManual);
$('create-form').addEventListener('submit', createAppointment);
$('connection-button').addEventListener('click', openConnections);
$('previous-button').addEventListener('click', () => {
  if (state.page > 1) void changePage(state.pageOffsets[state.page - 2], state.page - 1);
});
$('next-button').addEventListener('click', () => {
  if (state.next !== null) void changePage(state.next, state.page + 1);
});
$('copy-button').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText($('share-url').value);
    status('copied', false, $('copy-status'));
  } catch {
    $('share-url').select();
    status('selectLink', false, $('copy-status'));
  }
});
$('notification-button').addEventListener('click', async () => {
  try {
    const permission = await Notification.requestPermission();
    status(permission === 'granted' ? 'alertsEnabled' : 'alertsDenied', permission !== 'granted');
    showAlertControl();
  } catch { status('alertsUnavailable', true); }
});
document.querySelectorAll('[data-close]').forEach(item =>
  item.addEventListener('click', () => $(item.dataset.close).close()));
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) {
    state.unseen = false;
    updateTitle();
    void poll();
  }
});
window.addEventListener('storage', event => {
  if (event.key === storageKey + 'followed') { loadFollows(); render(); }
});
applyLanguage();
void loadOptions();
void (async () => {
  await refreshList();
  if (state.selected && !selected()) await refreshExact(state.selected, true);
  if (urlEvent && selected()?.id === urlEvent) revealDetails();
  await refreshRooms(selected());
  setInterval(poll, 12000);
})();
