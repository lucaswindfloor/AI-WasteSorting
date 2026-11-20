import { distanceAt, drawBelt, drawPreview } from './drawing.mjs';

const $ = (id) => document.getElementById(id);
const names = {
  valid: '有效', missing: '缺行', invalid: '无效', stale: '过期', unknown: '待核',
  Late: 'HSI 晚到，已追加原始证据', OldGeneration: '旧世代回包，M01 已拒绝', WrongSession: '来源会话不符',
  Capacity: '采集缓冲不足', ConsumerBacklog: '消费端积压', MissingSync: '同步记录缺失',
  DuplicateConflict: '重复数据有冲突', InvalidPacket: '封包格式无效', UnknownAssociation: '触发归属待核',
};
const significant = new Set(['trigger', 'packet', 'result', 'depth', 'diagnostic', 'archive', 'reconnect', 'seal']);
const motionReduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
let report = null;
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
let depthCount = 0;
let seenPackets = 0;
let latestTrigger = null;
let latestPacket = null;
let latestDiagnostic = null;
let health = {};
let firstGap = null;

function status(text, kind = '') {
  $('run-status').textContent = text;
  $('run-status').className = 'tag ' + kind;
}

function showMessage(text) {
  $('message').hidden = !text;
  $('message').textContent = text;
}

function reset() {
  index = 0;
  seenPackets = 0;
  currentRows = new Map();
  archiveRows = new Map();
  cameraRows = [];
  depthRows = [];
  depthCount = 0;
  latestTrigger = null;
  latestPacket = null;
  latestDiagnostic = null;
  health = {};
  rowSignature = '';
  lastColumns = -1;
  lastDepths = -1;
}

function ingest(event) {
  const data = event.data;
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
  } else if (event.type === 'depth') {
    depthCount++;
    depthRows.push(data.pixels);
  } else if (event.type === 'diagnostic') {
    latestDiagnostic = { ...data, t: event.t };
  } else if (event.type === 'health') {
    health = data;
  } else if (event.type === 'archive') {
    for (const trigger of data.triggers) archiveRows.set(trigger, data.manifest);
  }
}

function paintTable() {
  const ids = [...currentRows.keys()].sort((a, b) => a - b).slice(-7);
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
      label.textContent = names[quality] ?? quality;
      label.className = 'quality ' + (quality === 'valid' ? 'ok' : 'warn');
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
      describe('RGB', row.rgb, row.rgbQuality) + '\n' + describe('HSI', row.hsi, row.hsiQuality);
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
  drawBelt($('belt'), meta, cursor, latestTrigger, latestPacket, latestDiagnostic);
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
  if (lastColumns !== currentRows.size) {
    lastColumns = currentRows.size;
    const rgb = Array.from(cameraRows, (row) => row?.rgbPixels ?? null);
    const hsi = Array.from(cameraRows, (row) => row?.hsiPixels ?? null);
    drawPreview($('rgb'), rgb, 'rgb', meta?.previewWidth);
    drawPreview($('hsi'), hsi, 'hsi', meta?.previewWidth);
  }
  if (lastDepths !== depthCount) {
    lastDepths = depthCount;
    drawPreview($('depth'), depthRows, 'depth', meta?.previewWidth);
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

function play() {
  if (!report || playing) return;
  if (cursor >= report.meta.durationMs) seek(0);
  playing = true;
  $('play').textContent = '暂停';
  let previous;
  const animate = (now) => {
    if (!playing) return;
    const realDelta = previous === undefined ? 0 : Math.min(now - previous, 50);
    previous = now;
    seek(cursor + realDelta * 0.043);
    if (cursor >= report.meta.durationMs) pause();
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

function install(data) {
  report = data.report;
  const { meta, summary } = report;
  selected = 0;
  reset();
  cursor = 0;
  for (const id of ['seek', 'play', 'step', 'jump', 'trigger-input']) $(id).disabled = false;
  $('seek').max = meta.durationMs;
  $('trigger-input').max = summary.expected;
  firstGap = report.events.find((event) => event.type === 'result' && event.data.hsiQuality === 'missing');
  $('first-gap').disabled = !firstGap;
  $('geometry').textContent = '每 ' + meta.pitch + ' mm 触发一次 · ' + (meta.speed / 1000).toFixed(1) + ' m/s';
  $('band').textContent = meta.hsiPreviewBand + ' nm 模拟波段 · 伪彩';
  status(summary.passed ? 'M01 / M09 校验通过' : '校验失败', summary.passed ? 'ok' : 'warn');
  $('summary').textContent = summary.passed
    ? '已核对原始字节、源帧行和归档引用 · 封存清单 v' + summary.manifest +
      ' · 归档队列峰值 ' + summary.peakArchiveQueue
    : summary.errors.join('；');
  $('pairs').textContent = summary.paired + ' / ' + summary.expected;
  $('gaps').textContent = summary.missing;
  $('late').textContent = summary.late;
  $('depth-total').textContent = summary.depth;
  $('verified').textContent = summary.verifiedBlocks;
  for (const [anchor, path] of [['csv', 'ledger.csv'], ['json', 'report.json']]) {
    $(anchor).href = '/runs/' + encodeURIComponent(data.id) + '/' + path;
    $(anchor).hidden = false;
  }
  seek(0);
  if (!motionReduced) play();
}

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
  try {
    const response = await fetch('/api/run', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(opts),
      signal: AbortSignal.timeout(65000),
    });
    const data = await response.json();
    if (!response.ok || data.report?.schema !== 1) throw new Error(data.error || '无法读取仿真事件。');
    install(data);
  } catch (error) {
    status('运行失败', 'warn');
    showMessage(error.message);
  } finally {
    $('run').disabled = false;
    $('run').textContent = '运行仿真';
  }
});
$('play').addEventListener('click', () => playing ? pause() : play());
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
paint();
