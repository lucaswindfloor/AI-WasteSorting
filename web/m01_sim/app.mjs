import { CalibrationView } from './calibration.mjs';
import { distanceAt, drawAlignedPreview, drawBelt, drawMaterialCard, drawPreview, drawSprite, onMaterialsReady } from './drawing.mjs';
import { InferenceView, windowKey } from './inference.mjs';
import { createPickMotion, drawPickMotion, pickMotionLabels, samplePickMotion } from './pick-motion.mjs';

const $ = (id) => document.getElementById(id);
const names = {
  valid: '有效', missing: '缺行', invalid: '无效', stale: '过期', unknown: '待核',
  Late: 'HSI 晚到，已追加原始证据', OldGeneration: '旧世代回包，M01 已拒绝', WrongSession: '来源会话不符',
  Capacity: '采集缓冲不足', ConsumerBacklog: '消费端积压', MissingSync: '同步记录缺失',
  DuplicateConflict: '重复数据有冲突', InvalidPacket: '封包格式无效', UnknownAssociation: '触发归属待核',
};
const significant = new Set(['trigger', 'packet', 'result', 'depth', 'm02', 'm02-depth',
  'm03-model', 'm03-window', 'hsi-independent', 'm02-hsi', 'diagnostic', 'archive', 'reconnect', 'seal',
  'pick-window', 'pick-eligibility', 'pick-task', 'pick-action']);
const calibration = new CalibrationView();
const inference = new InferenceView(inspectSource);
const motionReduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
const resumeKey = 'm01-sim.last-run';
let report = null;
let lastRun = null;
let productionPreview = null;
let selected = 0;
let cursor = 0;
let index = 0;
let frame = 0;
let playing = false;
let rowSignature = '';
let lastColumns = -1;
let lastDepths = -1;
let currentRows = new Map();
let archiveRows = new Map();
let cameraRows = [];
let depthRows = [];
let independentHsi = [];
let lastHsi = -1;
let depthCount = 0;
let seenPackets = 0;
let latestTrigger = null;
let latestPacket = null;
let latestDiagnostic = null;
let health = {};
let firstGap = null;
let firstCandidate = null;
let picking = {};
let pickMotion = null;
const pickStages = { receipt: '控制器已收件', admitted: '运动已准入', contact: '已接触',
  holding: '末端已持物', 'placement-ended': '投放动作结束', released: '已释放到模拟测试箱',
  finished: '机器人动作结束', 'robot-ready': '机器人已回位', 'resources-released': '资源已归还',
  pending: '发令效果待核，资源保留' };

function status(text, kind = '') {
  $('run-status').textContent = text;
  $('run-status').className = 'tag ' + kind;
}

function showMessage(text) {
  $('message').hidden = !text;
  $('message').textContent = text;
}

function readLastRun() {
  try {
    return JSON.parse(localStorage.getItem(resumeKey));
  } catch {
    return null;
  }
}

function saveLastRun() {
  try {
    localStorage.setItem(resumeKey, JSON.stringify({ ...lastRun, time: cursor }));
  } catch {
    // A blocked storage API only disables resume.
  }
}

function reset() {
  index = 0;
  calibration.reset();
  inference.reset();
  seenPackets = 0;
  currentRows = new Map();
  archiveRows = new Map();
  cameraRows = [];
  depthRows = [];
  independentHsi = [];
  lastHsi = -1;
  depthCount = 0;
  latestTrigger = null;
  latestPacket = null;
  latestDiagnostic = null;
  health = {};
  rowSignature = '';
  lastColumns = -1;
  lastDepths = -1;
  picking = {};
}

function ingest(event) {
  const data = event.data;
  calibration.ingest(event);
  inference.ingest(event);
  if (event.type === 'trigger') {
    latestTrigger = { ...data, t: event.t };
  } else if (event.type === 'reconnect') {
    latestTrigger = { count: 0, generation: data.generation, t: -Infinity };
  } else if (event.type === 'packet') {
    seenPackets++;
    latestPacket = data;
  } else if (event.type === 'result') {
    currentRows.set(data.trigger, data);
    cameraRows[data.trigger - 1] = data;
  } else if (event.type === 'hsi-independent') {
    independentHsi.push(data);
  } else if (event.type === 'depth') {
    depthCount++;
    depthRows.push(data);
  } else if (event.type === 'diagnostic') {
    latestDiagnostic = { ...data, t: event.t };
  } else if (event.type === 'health') {
    health = data;
  } else if (event.type === 'archive') {
    for (const trigger of data.triggers) archiveRows.set(trigger, data.manifest);
  } else if (event.type === 'pick-position') {
    picking.position = data;
    if (data.valid) (picking.trail ??= []).push(data.point);
  } else if (event.type === 'pick-task') {
    picking.task = data;
  } else if (event.type === 'pick-eligibility') {
    picking.eligibility = data.state;
  } else if (event.type === 'pick-action') {
    if (data.state === 'holding') picking.removed = true;
    if (data.state in pickStages) picking.action = data.state;
  }
}

function paintPick(meta) {
  if (!meta?.scenario.startsWith('pet-')) return;
  const p = picking.position;
  const t = picking.task;
  const coordinates = (point, count) =>
    point.slice(0, count).map((value) => value.toFixed(1)).join(', ') + ' mm';
  $('pick-center').textContent = p?.valid ? 'M04 目标 #' + p.id + ' · 中心 (' +
    coordinates(p.point, 2) + ') · 参考计数 ' + p.referenceCount + ' · 当前计数 ' + p.count :
    picking.removed ? 'M04 目标已离开皮带' : p ? 'M04 位置失效' : 'M04 等待完整轮廓';
  $('pick-contact').textContent = t ? 'M05 抓点 (' + coordinates(t.point, 3) + ') · 参考计数 ' +
    t.referenceCount + ' · E05 源点 ' + t.sourceSamples + ' 个 · M06 接触窗 ' +
    t.openMs.toFixed(1) + '–' + t.closeMs.toFixed(1) + ' ms' :
    picking.eligibility === 'depth-missing' ? 'M05 抓区深度缺测' : 'M05 等待合格抓点';
  $('pick-progress').textContent = picking.action ? 'M07 ' + pickStages[picking.action] :
    p && !p.valid && !picking.removed ? '位置依据失效，停止发令' : t ? 'M07 等待发令' : '';
}

function paintTable() {
  const ids = [...currentRows.keys()].slice(-7).sort((a, b) => a - b);
  if (selected && currentRows.has(selected) && !ids.includes(selected)) ids.unshift(selected);
  const signature = ids.join(',') + '/' + ids.map((id) => archiveRows.get(id)).join(',') + '/' + selected;
  if (signature === rowSignature) return;
  rowSignature = signature;
  const body = $('rows');
  body.replaceChildren();
  if (!ids.length) {
    const cell = body.insertRow().insertCell();
    cell.colSpan = 4;
    cell.textContent = '等待 M01 发布第一批采集结果';
  }
  for (const id of ids) {
    const data = currentRows.get(id);
    const row = body.insertRow();
    row.classList.toggle('selected', id === selected);
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'row-button mono';
    btn.textContent = '#' + id;
    btn.setAttribute('aria-label', '查看触发 ' + id);
    btn.addEventListener('click', () => {
      selected = id;
      $('trigger-input').value = id;
      pause();
      paint();
    });
    row.insertCell().append(btn);
    for (const quality of [data.rgbQuality, data.hsiQuality]) {
      const cell = row.insertCell();
      const label = document.createElement('span');
      const independent = report?.meta.layout === 'production' && quality === 'unknown';
      label.textContent = independent ? '独立采集' : names[quality] ?? quality;
      label.className = 'quality' + (independent ? '' : quality === 'valid' ? ' ok' : ' warn');
      cell.append(label);
    }
    row.insertCell().textContent = archiveRows.has(id) ? 'v' + archiveRows.get(id) : '待提交';
  }
}

function describe(sensor, data, quality) {
  if (!data) return sensor + ' ' + (names[quality] ?? quality);
  return sensor + ' 帧 ' + data.frame + ' · 行 ' + data.row + ' · 会话 ' + data.session +
    ' · ' + data.receivedMs.toFixed(2) + ' ms 收件';
}

function paintDetail() {
  const row = currentRows.get(selected);
  $('exposure').disabled = !report || !selected;
  if (!row) {
    $('detail').textContent = selected ? '触发 #' + selected + ' 的结果尚未回放到。'
      : '点击账本触发号查看源帧行和收件时刻。';
  } else {
    const version = archiveRows.get(selected);
    $('detail').textContent = '#' + selected + ' · 世代 ' + row.generation + ' · 编码器 ' + row.count +
      ' · M01 段 ' + row.segment + (version ? ' · M09 v' + version : ' · 归档待提交') + '\n' +
      describe('RGB', row.rgb, row.rgbQuality) + '\n' + (report?.meta.layout === 'production' ? 'HSI 独立采集' : describe('HSI', row.hsi, row.hsiQuality));
  }
  const incident = latestDiagnostic;
  $('diagnostic').hidden = !incident;
  if (incident) {
    $('diagnostic').textContent = (incident.trigger ? '#' + incident.trigger + ' · ' : '') +
      (names[incident.issue] ?? incident.issue);
  }
}

function paint() {
  const meta = report?.meta;
  const aligned = $('preview-mode').value === 'aligned';
  const outline = $('preview-outline').checked;
  const production = $('controls').elements.layout.value === 'production';
  const belt = meta ?? (production ? productionPreview : null);
  if (belt || !production) {
    drawBelt($('belt'), belt, cursor, latestTrigger, latestPacket, latestDiagnostic,
      $('inference-overlay').checked ? inference.current()?.evidence : undefined, $('plant-view').value, picking);
  }
  paintPick(meta);
  if (pickMotion) {
    const travel = distanceAt(meta, cursor);
    const contactTravel = pickMotion.contact ? distanceAt(meta, pickMotion.contact.time) : 0;
    const pose = samplePickMotion(pickMotion, cursor, travel, contactTravel);
    const stage = pickMotionLabels[pose.stage];
    if ($('pick-motion-stage').textContent !== stage) $('pick-motion-stage').textContent = stage;
    drawPickMotion($('pick-motion'), meta, pickMotion, pose, travel, drawSprite);
  }
  $('current-pulse').textContent = '触发 ' + (latestTrigger?.trigger ? '#' + latestTrigger.trigger : '—');
  $('time').textContent = cursor.toFixed(1) + ' ms';
  $('seek').value = cursor;
  $('count').textContent = latestTrigger?.count ?? '—';
  $('distance').textContent = distanceAt(meta, cursor).toFixed(1) + ' mm';
  $('packets').textContent = seenPackets;
  $('buffer').textContent = (health.bytes ?? 0).toLocaleString('zh-CN') + ' B';
  $('queue').textContent = health.archiveQueue ?? 0;
  paintTable();
  paintDetail();
  calibration.paint(selected, meta);
  inference.paint(meta, currentRows);
  if (lastColumns !== currentRows.size) {
    lastColumns = currentRows.size;
    for (const sensor of meta?.layout === 'production' ? ['rgb'] : ['rgb', 'hsi']) {
      if (aligned) {
        const samples = [...currentRows.values()].map((row) =>
          ({ displacementMm: sensor === 'hsi' ? row.hsiDisplacementMm ?? row.displacementMm : row.displacementMm,
            scanX: sensor === 'hsi' ? meta.hsiScanX : meta.scanX, pixels: row[sensor + 'Pixels'] }));
        drawAlignedPreview($(sensor), samples, sensor, meta, outline);
      } else {
        const pixels = Array.from(cameraRows, (row) => row?.[sensor + 'Pixels'] ?? null);
        drawPreview($(sensor), pixels, sensor, meta?.previewWidth);
      }
    }
  }
  if (meta?.layout === 'production' && lastHsi !== independentHsi.length) {
    lastHsi = independentHsi.length;
    if (aligned) {
      const samples = independentHsi.map((row) => ({ ...row, scanX: meta.hsiScanX, pitch: meta.hsiPitch }));
      drawAlignedPreview($('hsi'), samples, 'hsi', meta, outline);
    } else {
      drawPreview($('hsi'), independentHsi.map((row) => row.pixels), 'hsi', meta.previewWidth);
    }
  }
  if (lastDepths !== depthCount) {
    lastDepths = depthCount;
    if (aligned) drawAlignedPreview($('depth'), depthRows.map((row) => ({ ...row, scanX: meta.depthScanX })), 'depth', meta, outline);
    else drawPreview($('depth'), depthRows.map((row) => row.pixels), 'depth', meta?.previewWidth);
  }
}

function seek(time) {
  if (!report) return;
  const position = Math.min(report.meta.durationMs, Math.max(0, time));
  if (position < cursor) reset();
  cursor = position;
  while (index < report.events.length && report.events[index].t <= cursor) ingest(report.events[index++]);
  paint();
}

function pause() {
  playing = false;
  cancelAnimationFrame(frame);
  $('play').textContent = '播放';
}

function play(rate, stopAt) {
  if (!report || playing) return;
  const end = Math.min(report.meta.durationMs, stopAt ?? report.meta.durationMs);
  const speed = rate ?? (report.meta.layout === 'production' ? 0.25 : 0.043);
  if (cursor >= end) seek(0);
  playing = true;
  $('play').textContent = '暂停';
  let previous;
  const animate = (now) => {
    if (!playing) return;
    const realDelta = previous === undefined ? 0 : Math.min(now - previous, 50);
    previous = now;
    seek(Math.min(end, cursor + realDelta * speed));
    if (cursor >= end) pause();
    else frame = requestAnimationFrame(animate);
  };
  frame = requestAnimationFrame(animate);
}

function jump(id) {
  if (!report) return;
  const result = report.events.find((event) => event.type === 'result' && event.data.trigger === id);
  if (!result) return showMessage('该次运行没有这条触发的结果。');
  const relevant = report.events.filter((row) => (row.type === 'diagnostic' && row.data.trigger === id)
      || (row.type === 'archive' && row.data.triggers.includes(id)));
  const time = Math.max(result.t, ...relevant.map((event) => event.t));
  pause();
  selected = id;
  $('trigger-input').value = id;
  seek(time);
  showMessage('');
}

function inspectSource(trigger) {
  pause();
  if (currentRows.has(trigger)) {
    selected = trigger;
    $('trigger-input').value = trigger;
    paint();
  } else {
    jump(trigger);
  }
  $('calibration').scrollIntoView();
}

function paintStation(meta) {
  const production = meta?.layout === 'production';
  $('plant-tools').hidden = !production;
  if (!production) return;
  const shared = meta.scanX === meta.hsiScanX && meta.scanX === meta.depthScanX;
  const stations = shared ? 'RGB / HSI / 深度共用截面 ' + meta.scanX + ' mm' :
    'RGB ' + meta.scanX + ' mm · HSI ' + meta.hsiScanX + ' mm · 深度 ' + meta.depthScanX + ' mm';
  $('plant-stations').textContent = stations + ' · 抓取区 ' + meta.pickBegin + '–' + meta.pickEnd + ' mm';
}

function paintStationPreview() {
  if (report) return;
  const production = $('controls').elements.layout.value === 'production';
  const preview = production ? productionPreview : null;
  paintStation(preview);
  $('plant-view').value = 'line';
  $('belt').height = 380;
  $('geometry').textContent = preview
    ? preview.beltLength + ' × ' + preview.beltWidth + ' mm · 台架预览'
    : production ? '运行后显示生产台架尺寸。' : '演示台架预览';
  paint();
}

async function loadProductionPreview() {
  try {
    const response = await fetch('/api/production-layout', { signal: AbortSignal.timeout(5000) });
    if (!response.ok) return;
    const data = await response.json();
    if (data.layout === 'production') {
      productionPreview = { ...data, objects: [] };
      paintStationPreview();
    }
  } catch {
    // A missing preview does not prevent loading a saved run.
  }
}

function install(data, { at = 0, autoplay = !motionReduced } = {}) {
  report = data.report;
  const { meta, summary } = report;
  selected = 0;
  reset();
  inference.setBcaf(data.id, data.bcaf);
  cursor = 0;
  for (const id of ['seek', 'play', 'step', 'jump', 'trigger-input']) $(id).disabled = false;
  $('seek').max = meta.durationMs;
  $('trigger-input').max = summary.expected;
  firstGap = report.events.find((event) => event.type === 'result' && (meta.layout === 'production' ? event.data.rgbQuality : event.data.hsiQuality) === 'missing');
  $('first-gap').disabled = !firstGap;
  firstCandidate = report.events.find((event) => event.type === 'm03-window' && event.data.state === 'completed');
  $('inference-first').disabled = !firstCandidate;
  const production = meta.layout === 'production';
  pickMotion = meta.scenario.startsWith('pet-') ? createPickMotion(report) : null;
  $('pick-replay').hidden = !pickMotion;
  $('pick-jump').disabled = !pickMotion?.contact;
  $('pick-play').disabled = pickMotion?.admitted === undefined || pickMotion?.ready === undefined;
  $('material-al-card').hidden = !meta.objects.some((item) => item.material === 'AL');
  $('ledger-description').textContent = production ? 'RGB 有效行与编码器一一核对' : '使用 M01 的实际配对结果';
  paintStation(meta);
  $('plant-materials').hidden = !production;
  $('plant-view').value = 'line';
  $('belt').height = 380;
  paintMaterialCards();
  $('geometry').textContent = (production ? meta.beltLength + ' × ' + meta.beltWidth + ' mm · RGB ' +
    meta.rgbWidth + ' px × ' + meta.packetLines + ' 行/帧 · ' : '') + '每 ' + meta.pitch + ' mm 触发一次 · ' + (meta.speed / 1000).toFixed(1) + ' m/s';
  $('band').textContent = meta.hsiPreviewBand + ' nm 模拟波段 · 伪彩';
  status(summary.passed ? '校验通过' : '校验失败',
    summary.passed ? 'ok' : 'warn');
  $('summary').textContent = summary.passed
    ? '已核对原始字节、源帧行和归档引用 · 封存清单 v' + summary.manifest +
      ' · 归档队列峰值 ' + summary.peakArchiveQueue
    : summary.errors.join('；');
  $('pair-title').textContent = production ? 'M01 RGB 有效行' : 'M01 完整配对';
  $('coverage-title').textContent = production ? '独立 HSI 采集行' : 'M02 共同覆盖';
  $('gap-title').textContent = production ? 'RGB 缺行' : 'HSI 缺口';
  $('pairs').textContent = (production ? summary.rgbRows : summary.paired) + ' / ' + summary.expected;
  $('m02-total').textContent = production ? summary.independentHsi :
    summary.m02 ? summary.m02.covered + ' / ' + summary.m02.processed : '—';
  $('gaps').textContent = summary.missing;
  $('late').textContent = summary.late;
  $('depth-total').textContent = summary.depth;
  $('verified').textContent = summary.verifiedBlocks;
  const m03 = summary.m03;
  $('inference-total').hidden = !m03;
  $('inference-total').textContent = m03 ? 'M03 窗口 · 有候选 ' + m03.detected + ' · 无候选 ' + m03.empty +
    ' · 输入无效 ' + m03.invalid + ' · 超期 ' + m03.expired + ' · 队列满 ' + m03.queueFull +
    ' · 尾窗退役 ' + m03.retired : '';
  for (const [anchor, path] of [['csv', 'ledger.csv'], ['json', 'report.json']]) {
    $(anchor).href = '/runs/' + encodeURIComponent(data.id) + '/' + path;
    $(anchor).hidden = false;
  }
  seek(at);
  if (data.bcafError) showMessage(data.bcafError);
  if (autoplay) play();
}

async function restoreLastRun() {
  const saved = readLastRun();
  if (typeof saved?.id !== 'string' || !saved.options) return;
  const form = $('controls');
  for (const select of form.querySelectorAll('select')) {
    const value = String(saved.options[select.name]);
    if (Array.from(select.options).some((option) => option.value === value)) select.value = value;
  }
  changeLayout();
  paintStationPreview();
  $('run').disabled = true;
  status('读取上次回放…');
  try {
    const base = '/runs/' + encodeURIComponent(saved.id) + '/';
    const response = await fetch(base + 'report.json', { signal: AbortSignal.timeout(10000) });
    if (!response.ok) throw new Error('回放报告不可用');
    const restored = await response.json();
    if (restored.schema !== 1 || !restored.summary?.passed) throw new Error('回放报告格式无效');
    let bcaf;
    if (restored.meta.bcafCapture) {
      bcaf = await fetch(base + 'bcaf/bcaf-report.json', { signal: AbortSignal.timeout(5000) })
        .then((result) => result.ok ? result.json() : undefined)
        .catch(() => undefined);
    }
    install({ id: saved.id, report: restored, bcaf }, {
      at: Number.isFinite(saved.time) ? saved.time : 0,
      autoplay: false,
    });
    lastRun = saved;
  } catch {
    status('待运行');
    showMessage('上次的回放文件暂时无法读取。可重新运行当前场景。');
  } finally {
    $('run').disabled = false;
  }
}

window.addEventListener('pagehide', () => {
  if (lastRun && readLastRun()?.id === lastRun.id) saveLastRun();
});

$('controls').addEventListener('submit', async (event) => {
  event.preventDefault();
  pause();
  showMessage('');
  $('run').disabled = true;
  $('run').textContent = '正在运行…';
  status('C++ 仿真与归档运行中');
  const opts = Object.fromEntries(new FormData(event.currentTarget));
  opts.speed = Number(opts.speed);
  opts.packetLines = Number(opts.packetLines);
  if (opts.inference === 'bcaf') opts.masks = 'strict';
  if (opts.scenario.startsWith('pet-')) opts.masks = 'strict';
  try {
    const response = await fetch('/api/run', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(opts),
      signal: AbortSignal.timeout(opts.inference === 'bcaf' ? 370000 : 125000),
    });
    const data = await response.json();
    if (!response.ok || data.report?.schema !== 1) throw new Error(data.error || '无法读取仿真事件。');
    install(data);
    if (data.report.summary.passed) {
      lastRun = { id: data.id, options: opts };
      saveLastRun();
    } else {
      lastRun = null;
    }
  } catch (error) {
    status('运行失败', 'warn');
    showMessage(error.message);
  } finally {
    $('run').disabled = false;
    $('run').textContent = '运行仿真';
  }
});
$('play').addEventListener('click', () => playing ? pause() : play());
$('pick-jump').addEventListener('click', () => {
  const contact = pickMotion?.contact;
  if (contact) {
    pause();
    seek(contact.time);
  }
});
$('pick-play').addEventListener('click', () => {
  pause();
  seek(pickMotion.admitted);
  play(0.05, pickMotion.ready);
});
$('step').addEventListener('click', () => {
  pause();
  const next = report?.events.slice(index).find((event) => significant.has(event.type));
  if (report) seek(next?.t ?? report.meta.durationMs);
});
$('seek').addEventListener('input', () => {
  pause();
  seek(Number($('seek').value));
});
$('lookup').addEventListener('submit', (event) => {
  event.preventDefault();
  jump(Number($('trigger-input').value));
});
$('exposure').addEventListener('click', () => {
  const event = report?.events.find((entry) => entry.type === 'trigger' && entry.data.trigger === selected);
  if (event) {
    pause();
    seek(event.t);
  }
});
$('first-gap').addEventListener('click', () => {
  if (firstGap) jump(firstGap.data.trigger);
});
$('inference-first').addEventListener('click', () => {
  if (!firstCandidate) return;
  pause();
  seek(firstCandidate.t);
  inference.selected = windowKey(firstCandidate.data);
  paint();
});
$('inference-window').addEventListener('change', (event) => {
  pause();
  inference.selected = event.target.value;
  paint();
});
function changePreview() {
  const aligned = $('preview-mode').value === 'aligned';
  $('preview-outline').disabled = !aligned;
  $('preview-note').textContent = aligned
    ? '三图按皮带毫米坐标等比例显示，朝向与顶视图一致。虚线是仿真轮廓；深度只标出实测位置，不插值。'
    : '从左向右按触发或观测顺序拼接，先扫到的一端在左；此模式不按毫米等比例绘制。';
  lastColumns = lastDepths = lastHsi = -1;
  paint();
}
$('calibration-mode').addEventListener('change', paint);
$('inference-overlay').addEventListener('change', paint);
$('preview-mode').addEventListener('change', changePreview);
$('preview-outline').addEventListener('change', changePreview);
function paintMaterialCards() {
  if (report?.meta.layout !== 'production') return;
  for (const item of report.meta.objects) {
    const id = 'material-' + item.material.toLowerCase();
    $(id + '-size').textContent = item.length + ' × ' + item.width + ' mm';
    drawMaterialCard($(id), item);
  }
}

function changeLayout() {
  const form = $('controls');
  const production = form.elements.layout.value === 'production';
  const filter = (name, values) => {
    const select = form.elements[name];
    const current = select.value;
    for (const option of select.options) {
      option.hidden = Boolean(values && !values.includes(option.value)) ||
        (!production && (['rgb-frame-loss', 'bcaf'].includes(option.value) || option.value.startsWith('pet-')));
      option.disabled = option.hidden;
    }
    if (select.selectedOptions[0]?.disabled || !current) {
      select.value = name === 'scenario' ? 'normal' : name === 'speed' ? '1000' : 'rgb';
    }
  };
  filter('scenario', production ? ['normal', 'rgb-frame-loss', 'pet-pick', 'pet-depth-loss', 'pet-encoder-loss', 'pet-receipt-loss'] : null);
  const pet = production && form.elements.scenario.value.startsWith('pet-');
  filter('speed', production ? pet ? ['1000'] : ['1000', '2000', '3000'] : null);
  filter('inference', production ? pet ? ['rgb'] : ['rgb', 'off', 'bcaf'] : null);
  if (pet) form.elements.depth.value = 'independent';
  form.elements.depth.options[1].disabled = pet;
  const bcaf = form.elements.inference.value === 'bcaf';
  if (bcaf || pet) form.elements.masks.value = 'strict';
  form.elements.masks.disabled = bcaf || pet;
  for (const name of ['profile', 'packetLines']) {
    form.elements[name].disabled = production;
    form.elements[name].closest('label').hidden = production;
  }
}

$('controls').elements.layout.addEventListener('change', () => {
  changeLayout();
  paintStationPreview();
});
$('controls').elements.scenario.addEventListener('change', changeLayout);
$('controls').elements.inference.addEventListener('change', changeLayout);
$('plant-view').addEventListener('change', () => {
  $('belt').height = $('plant-view').value === 'infeed' ? 555 : 380;
  paint();
});
onMaterialsReady(() => {
  paintMaterialCards();
  paint();
});
changeLayout();
paintStationPreview();
void loadProductionPreview();
void restoreLastRun();
