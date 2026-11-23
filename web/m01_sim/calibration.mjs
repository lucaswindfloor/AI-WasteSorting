const $ = (id) => document.getElementById(id);
const statusLabel = {
  available: '有共同覆盖',
  independent_route: 'RGB 已标定 · HSI 独立采样',
  missing_input: '缺少一路原始数据',
  unusable_input: '来源质量或触发关联待核',
  unverified_time: '曝光时间无法核实',
  exposure_separation: '曝光时差超限',
  unverified_count: '曝光计数无法核实',
  reference_unavailable: '参考时刻或位移超限',
  projection_unavailable: '有效空间投影缺失',
  out_of_coverage: '补偿后坐标仍无共同覆盖',
};
const spectralLabel = {
  reference_conditions_unverified: '曝光时刻或参考条件未核实',
  invalid_reference: '白参考与暗参考之差过小',
  saturated: '观测或参考饱和',
  invalid_sample: '采样无效',
};
const countBasis = { exposure: '曝光锁存', interpolation: '同步计数插值', trigger: '触发锁存' };
const colors = { rgb: '#5ee2ef', hsi: '#c7a4ff', common: '#91e7b1', grid: '#283e52', tick: '#a6b9ce' };

function number(value, digits = 2) {
  if (!Number.isFinite(value)) return '—';
  const rounded = Number(value.toFixed(digits));
  return rounded.toFixed(digits);
}

function units(value, unit, digits = 2) {
  return Number.isFinite(value) ? number(value, digits) + ' ' + unit : '—';
}

function line(ctx, x0, y0, x1, y1, color) {
  ctx.beginPath();
  ctx.moveTo(x0, y0);
  ctx.lineTo(x1, y1);
  ctx.strokeStyle = color;
  ctx.stroke();
}

function dot(ctx, x, y, radius, color) {
  ctx.beginPath();
  ctx.arc(x, y, radius, 0, 2 * Math.PI);
  ctx.fillStyle = color;
  ctx.fill();
}

function plot(canvas, row, meta, mode) {
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;
  const bounds = { l: 66, r: w - 28, t: 28, b: h - 67 };
  const center = meta?.scanX ?? 142;
  const beltWidth = meta?.beltWidth ?? 240;
  const x = (mm) => bounds.l + (mm - (center - 6)) / 12 * (bounds.r - bounds.l);
  const y = (mm) => bounds.b - mm / beltWidth * (bounds.b - bounds.t);
  ctx.fillStyle = '#0b1523';
  ctx.fillRect(0, 0, w, h);
  ctx.lineWidth = 1;
  ctx.font = '15px system-ui, sans-serif';
  ctx.textAlign = 'center';
  for (let offset = -6; offset <= 6; offset += 2) {
    const mm = center + offset;
    line(ctx, x(mm), bounds.t, x(mm), bounds.b, offset ? colors.grid : '#627a92');
    ctx.fillStyle = colors.tick;
    ctx.fillText(String(mm), x(mm), bounds.b + 22);
  }
  ctx.textAlign = 'right';
  for (let mm = 0; mm <= beltWidth; mm += beltWidth > 400 ? 200 : 40) {
    line(ctx, bounds.l, y(mm), bounds.r, y(mm), colors.grid);
    ctx.fillStyle = colors.tick;
    ctx.fillText(String(mm), bounds.l - 10, y(mm) + 5);
  }
  ctx.textAlign = 'center';
  ctx.fillText('沿带 X (mm) · 扫描线附近', (bounds.l + bounds.r) / 2, h - 15);
  ctx.textAlign = 'left';
  ctx.fillText('横向 Y (mm)', bounds.l, 20);
  if (!row) return;

  ctx.save();
  ctx.beginPath();
  ctx.rect(bounds.l, bounds.t, bounds.r - bounds.l, bounds.b - bounds.t);
  ctx.clip();
  if (mode === 'aligned') {
    for (const [rgb, hsi] of row.pairs) {
      line(ctx, x(rgb[0]), y(rgb[1]), x(hsi[0]), y(hsi[1]), colors.common);
      dot(ctx, x((rgb[0] + hsi[0]) / 2), y((rgb[1] + hsi[1]) / 2), 5, colors.common);
    }
  }
  for (const sensor of ['rgb', 'hsi']) {
    for (const [ax, ay, , quality] of row[sensor]?.[mode] ?? []) {
      if (quality !== 'valid') continue;
      // Rings keep the second modality distinguishable when both land on the same point.
      if (sensor === 'hsi') {
        ctx.beginPath();
        ctx.arc(x(ax), y(ay), 3.2, 0, 2 * Math.PI);
        ctx.strokeStyle = colors.hsi;
        ctx.stroke();
      } else {
        dot(ctx, x(ax), y(ay), 2, colors.rgb);
      }
    }
  }
  ctx.restore();
}

function temporalTable(row) {
  const tbody = $('calibration-times');
  tbody.replaceChildren();
  if (!row) {
    const cell = tbody.insertRow().insertCell();
    cell.colSpan = 4;
    cell.textContent = '等待该触发的 M02 结果';
    return;
  }
  for (const sensor of ['rgb', 'hsi']) {
    const measured = row[sensor];
    const line = tbody.insertRow();
    const label = document.createElement('th');
    label.scope = 'row';
    label.textContent = sensor.toUpperCase();
    line.append(label);
    line.insertCell().textContent = units(measured?.exposureMs, 'ms', 3);
    line.insertCell().textContent = units(measured?.receivedMs, 'ms', 3);
    line.insertCell().textContent = units(measured?.shiftMm, 'mm');
  }
}

function evidence(row, depth, meta, hsi) {
  const sample = row?.spectrum;
  $('calibration-spectrum').textContent = sample
    ? [meta?.hsiPreviewBand + ' nm', '列 ' + sample.column, '原始 ' + sample.raw,
        Number.isFinite(sample.corrected) ? '校正 ' + number(sample.corrected, 4) :
          spectralLabel[sample.reason] ?? (sample.reason || '校正待核'),
        sample.reference].join(' · ')
    : hsi ? '独立帧 ' + hsi.frame + ' · 工位 ' + hsi.scanX + ' mm · 计数 ' + hsi.count +
      ' · 原始 ' + hsi.raw + ' · 校正 ' + number(hsi.corrected, 4)
      : meta?.layout === 'production' ? '等待 HSI 独立观测' : '当前触发无 HSI 样本';

  const e05 = depth?.observation;
  const points = e05?.measured.filter((point) => point[3] === 'valid');
  const representative = points?.reduce((best, sample) => !best || sample[2] > best[2] ? sample : best, null);
  $('calibration-depth').textContent = !e05 ? '当前时间暂无 E05 投影' :
    '帧 ' + e05.frame + ' · ' + (depth.trigger ? '参考 #' + depth.trigger : '独立观测') +
    ' · 有效 0 mm 点 ' + depth.zeros + ' · 缺测 ' + depth.masked +
    (representative ? ' · 示例 (X, Y, Z) = (' +
      representative.slice(0, 3).map((value) => number(value)).join(', ') + ') mm' : '');
}

export class CalibrationView {
  constructor() {
    this.reset();
  }

  reset() {
    this.rows = new Map();
    this.depth = new Map();
    this.latest = null;
    this.latestDepth = null;
    this.latestHsi = null;
    this.last = null;
  }

  ingest({ type, data }) {
    if (type === 'm02') {
      this.rows.set(data.trigger, data);
      this.latest = data;
    } else if (type === 'm02-depth') {
      if (data.trigger) this.depth.set(data.trigger, data);
      this.latestDepth = data;
    } else if (type === 'm02-hsi') {
      this.latestHsi = data;
    }
  }

  paint(selected, meta) {
    const row = selected ? this.rows.get(selected) : this.latest;
    const recent = this.latestDepth;
    const depth = this.depth.get(selected) ??
      ((!row || recent?.generation === row.generation) ? recent : null);
    const mode = $('calibration-mode').value;
    if (this.last?.row === row && this.last?.depth === depth && this.last?.selected === selected &&
        this.last?.meta === meta && this.last?.mode === mode && this.last?.hsi === this.latestHsi) return;
    this.last = { row, depth, selected, meta, mode, hsi: this.latestHsi };
    const tag = $('calibration-status');
    const production = meta?.layout === 'production';
    tag.textContent = row ? production && row.status === 'missing_input' ? 'RGB 缺行' :
      statusLabel[row.status] ?? row.status : '等待配准';
    tag.className = 'tag' + (row ? (row.coverage || row.status === 'independent_route') ? ' ok' : ' warn' : '');
    $('calibration-version').textContent = row
      ? '#' + row.trigger + ' · 世代 ' + row.generation + ' · M01 段 ' + row.segment +
        ' · ' + row.geometry + ' · ' + row.clock
      : selected ? '触发 #' + selected + ' 的 M02 结果尚未回放到。' : '运行后，选中触发即可查看坐标和标定版本。';

    const sharedScanLine = meta?.scanX === meta?.hsiScanX && meta?.scanX === meta?.depthScanX;
    $('calibration-coverage').textContent = production ?
      (sharedScanLine ? '三路共用扫描截面，' : '') + 'HSI 与深度保留各自的曝光时刻和位移。' :
      row ? '共同覆盖 ' + row.coverage + ' 个 RGB 列 · 曝光时差 ' +
      (Number.isFinite(row.rgb?.exposureMs) && Number.isFinite(row.hsi?.exposureMs)
        ? units(row.hsi.exposureMs - row.rgb.exposureMs, 'ms', 3) : '待核') +
      ' · 上限 ' + units(row.exposureLimitMs, 'ms') : '';
    const basis = row?.hsi?.countBasis;
    $('calibration-ref').textContent = row ? '参考 ' + units(row.referenceMs, 'ms', 3) +
      ' · 计数 ' + row.referenceCount + (production ? '' : ' · HSI 曝光计数 ' +
      (basis ? number(row.hsi.count) + ' (' + (countBasis[basis] ?? basis) + ')' : '待核')) : '';
    temporalTable(row);
    plot($('calibration-map'), row, meta, mode);
    evidence(row, depth, meta, this.latestHsi);
  }
}
