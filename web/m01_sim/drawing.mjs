const colors = {
  ink: '#b8c9dc', dim: '#7990a8', cyan: '#4fe4f5', drive: '#263548',
  belt: '#182739', border: '#38536b', amber: '#f7ad62',
};

const sprite = new Image();
sprite.src = '/assets/materials.png';
const spriteRects = { PET: [219, 33, 1341, 400], AL: [478, 496, 773, 366] };

export function onMaterialsReady(paint) {
  sprite.addEventListener('load', paint, { once: true });
}

function drawSprite(ctx, material, left, top, width, height) {
  const bounds = spriteRects[material];
  if (!bounds || !sprite.complete || !sprite.naturalWidth) return false;
  ctx.drawImage(sprite, ...bounds, left, top, width, height);
  return true;
}

export function drawMaterialCard(canvas, item) {
  const ctx = canvas.getContext('2d');
  const scale = 1.4;
  const width = item.length * scale, height = item.width * scale;
  ctx.fillStyle = '#111e2c';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  drawSprite(ctx, item.material, (canvas.width - width) / 2, (canvas.height - height) / 2, width, height);
}

function box(ctx, x, y, width, height, radius, fill, border) {
  ctx.beginPath();
  ctx.roundRect(x, y, width, height, radius);
  if (fill) {
    ctx.fillStyle = fill;
    ctx.fill();
  }
  if (border) {
    ctx.strokeStyle = border;
    ctx.stroke();
  }
}

function line(ctx, x, y, x2, y2, color, width = 1) {
  ctx.beginPath();
  ctx.moveTo(x, y);
  ctx.lineTo(x2, y2);
  ctx.lineWidth = width;
  ctx.strokeStyle = color;
  ctx.stroke();
  ctx.lineWidth = 1;
}

function text(ctx, value, x, y, color = colors.ink, size = 14) {
  ctx.font = size + 'px system-ui, sans-serif';
  ctx.fillStyle = color;
  ctx.fillText(value, x, y);
}

export function distanceAt(meta, now) {
  let distance = 0;
  for (const part of meta?.motion ?? []) {
    if (now < part.startMs) break;
    if (now < part.endMs) {
      const progress = (now - part.startMs) / (part.endMs - part.startMs);
      return part.begin + (part.end - part.begin) * progress;
    }
    distance = part.end;
  }
  return distance;
}

function drawFootprint(ctx, item, offset, x, y, stroke, fill) {
  if (!item.footprint?.length) return;
  ctx.beginPath();
  for (const [side, stations] of [[-1, item.footprint], [1, [...item.footprint].reverse()]]) {
    for (const [along, width] of stations) {
      ctx.lineTo(x(item.x + offset + (along - 0.5) * item.length), y(item.y + side * width * item.width / 2));
    }
  }
  ctx.closePath();
  if (fill) {
    ctx.fillStyle = fill;
    ctx.fill();
  }
  ctx.strokeStyle = stroke;
  ctx.stroke();
}

function drawMaterial(ctx, item, offset, x, y) {
  const pet = item.material === 'PET';
  drawFootprint(ctx, item, offset, x, y, pet ? '#81e2fa' : '#ffc78d', pet ? '#217fa0' : '#a96836');
  text(ctx, item.id + '  ' + item.material, x(item.x + offset - item.length / 2),
    y(item.y - item.width / 2) - 11, '#dae9f6', 13);
}

function drawCandidates(ctx, evidence, x, y, offset, prefix = '') {
  for (const candidate of evidence) {
    if (!candidate.bounds) continue;
    const [x0, y0, x1, y1] = candidate.bounds;
    const label = candidate.label === 'object' ? '轮廓' : candidate.label === 'AL' ? 'Al' : candidate.label;
    const left = Math.min(x(x0 + offset), x(x1 + offset)) - 3;
    const top = y(y0) - 3;
    const color = candidate.label === 'AL' ? '#ffe499' : '#9ef0ad';
    ctx.lineWidth = 2;
    box(ctx, left, top, Math.abs(x(x1 + offset) - x(x0 + offset)) + 6, y(y1) - y(y0) + 6,
      2, null, color);
    ctx.lineWidth = 1;
    text(ctx, prefix + label, left, top - 4, color, 16);
  }
}

export function drawBelt(canvas, meta, now, pulse, packet, diagnostic, evidence = [], view = 'line', picking = {}) {
  if (meta?.layout === 'production') {
    drawProductionBelt(canvas, meta, now, pulse, evidence, view, picking);
    return;
  }
  const ctx = canvas.getContext('2d');
  const shift = distanceAt(meta, now);
  const scanX = meta?.scanX ?? 142;
  const scale = 194 / (meta?.beltWidth ?? 240);
  const x = (value) => 468 + (value - scanX) * scale;
  const y = (value) => 101 + value * scale;
  const scanner = x(scanX);
  const pulseActive = pulse && now - pulse.t < 0.7;
  const faultActive = diagnostic && now - diagnostic.t < 7;
  const scanColor = faultActive ? colors.amber : colors.cyan;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  box(ctx, 0, 0, 1000, 380, 14, '#0b1523');
  for (let gx = 24; gx < 990; gx += 32) {
    for (let gy = 20; gy < 367; gy += 28) box(ctx, gx, gy, 1.5, 1.5, 0, '#233449');
  }
  box(ctx, 35, 87, 930, 225, 16, colors.drive, colors.border);
  box(ctx, 48, 101, 904, 194, 12, colors.belt, '#3b5165');
  ctx.save();
  ctx.beginPath();
  ctx.roundRect(48, 101, 904, 194, 12);
  ctx.clip();
  for (let travel = scanX - 580 + (shift % 29); x(travel) < 952; travel += 29) {
    line(ctx, x(travel), 101, x(travel), 295, '#27394c');
  }
  ctx.setLineDash([7, 12]);
  line(ctx, 55, y(120), 946, y(120), '#4c637a');
  ctx.setLineDash([]);
  for (const item of meta?.objects ?? []) drawMaterial(ctx, item, shift, x, y);
  drawCandidates(ctx, evidence, x, y, shift, 'M03 ');
  if (pulseActive || faultActive) box(ctx, scanner - 10, 101, 20, 194, 0, faultActive ? '#f7ad6230' : '#4fe4f536');
  ctx.setLineDash([8, 5]);
  line(ctx, scanner, 103, scanner, 293, scanColor, pulseActive ? 4 : 2);
  ctx.setLineDash([]);
  ctx.restore();
  line(ctx, scanner, 86, scanner, 62, scanColor, 2);
  box(ctx, scanner - 99, 22, 198, 41, 8, '#18354a', scanColor);
  const hsi = meta?.hsiScanX ?? scanX;
  const label = hsi === scanX ? 'RGB + HSI 共同扫描线' : 'RGB / HSI 扫描线';
  text(ctx, label, scanner - 83, 48, '#b4ecf6', 15);
  if (hsi !== scanX) {
    ctx.setLineDash([3, 6]);
    line(ctx, x(hsi), 104, x(hsi), 292, '#c8a5ff');
    ctx.setLineDash([]);
    text(ctx, 'HSI 下游 ' + (hsi - scanX).toFixed(2) + ' mm', scanner + 112, 48, '#c8a5ff', 13);
  }
  line(ctx, 63, 334, 180, 334, colors.dim, 2);
  line(ctx, 173, 327, 180, 334, colors.dim, 2);
  line(ctx, 173, 341, 180, 334, colors.dim, 2);
  text(ctx, '皮带方向', 62, 357, colors.dim, 13);
  if (packet) {
    box(ctx, 719, 322, 222, 33, 7, '#162737', '#365267');
    text(ctx, packet.source + '  收件 ' + packet.bytes.toLocaleString('zh-CN') + ' B', 730, 344, colors.ink, 14);
  }
}

function drawProductionBelt(canvas, meta, now, pulse, evidence, view, picking) {
  const ctx = canvas.getContext('2d');
  const span = view === 'infeed' ? 1850 : meta.beltLength;
  const scale = Math.min((canvas.width - 64) / span, (canvas.height - 145) / meta.beltWidth);
  const start = (canvas.width - span * scale) / 2;
  const y0 = 95, y1 = y0 + meta.beltWidth * scale;
  const x = (mm) => start + mm * scale;
  const y = (mm) => y0 + mm * scale;
  const shift = distanceAt(meta, now);
  const viewEnd = x(span);
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  box(ctx, 0, 0, canvas.width, canvas.height, 14, '#0b1523');
  box(ctx, start - 5, y0 - 7, viewEnd - start + 10, y1 - y0 + 14, 8, '#33465a');
  box(ctx, start, y0, viewEnd - start, y1 - y0, 2, '#1b2a36');
  ctx.save();
  ctx.beginPath();
  ctx.rect(start, y0, viewEnd - start, y1 - y0);
  ctx.clip();
  for (let mm = shift % 150; mm < span; mm += 150) line(ctx, x(mm), y0, x(mm), y1, '#273944');
  line(ctx, start, y(meta.beltWidth / 2), viewEnd, y(meta.beltWidth / 2), '#374c56');
  if (meta.pickBegin < span) {
    box(ctx, x(meta.pickBegin), y0, x(meta.pickEnd) - x(meta.pickBegin), y1 - y0, 0, '#25574569', '#80d3a0');
    text(ctx, '抓取区', x(meta.pickBegin) + 8, y0 + 23, '#a4e7b9', 14);
  }
  for (const item of meta.objects) {
    if (picking.removed) continue;
    const center = item.x + shift;
    const left = x(center - item.length / 2);
    const top = y(item.y - item.width / 2);
    if (!drawSprite(ctx, item.material, left, top, item.length * scale, item.width * scale)) {
      drawFootprint(ctx, item, shift, x, y, '#bdd7e2', '#7ca4b5');
    }
    text(ctx, item.id, left, top - 5, '#e3edf3', 12);
  }
  if (!picking.removed) drawCandidates(ctx, evidence, x, y, shift, '');
  if (picking.trail?.length > 1) {
    ctx.beginPath();
    for (const [px, py] of picking.trail) ctx.lineTo(x(px), y(py));
    ctx.strokeStyle = '#77f5ff75';
    ctx.lineWidth = 2;
    ctx.stroke();
  }
  if (picking.position?.valid) {
    const [px, py] = picking.position.point;
    const at = x(px), across = y(py);
    line(ctx, at - 6, across, at + 6, across, '#77f5ff', 2);
    line(ctx, at, across - 6, at, across + 6, '#77f5ff', 2);
    text(ctx, '中心 #' + picking.position.id, at + 9, across - 9, '#77f5ff', 13);
    if (picking.task) {
      const [gx, gy] = picking.task.point;
      const pickX = x(gx + (picking.position.count - picking.task.referenceCount) * picking.position.mmPerCount);
      box(ctx, pickX - 3, y(gy) - 3, 6, 6, 0, '#ffd97e');
      text(ctx, '抓点', pickX + 7, y(gy) + 15, '#ffd97e', 13);
    }
  }
  const sharedScanLine = meta.scanX === meta.hsiScanX && meta.scanX === meta.depthScanX;
  const stations = sharedScanLine ? [['RGB / HSI / 深度', meta.scanX, colors.cyan]] : [
    ['RGB', meta.scanX, colors.cyan],
    ['HSI', meta.hsiScanX, '#c5aafa'],
    ['深度', meta.depthScanX, '#8ee4ba'],
  ];
  for (const [, at, color] of stations) {
    line(ctx, x(at), y0, x(at), y1, color, at === meta.scanX && pulse && now - pulse.t < 2 ? 4 : 1.5);
  }
  ctx.restore();
  for (const [label, at, color] of stations) {
    line(ctx, x(at), y0, x(at), y0 - 16, color);
    ctx.textAlign = 'center';
    text(ctx, label, x(at), y0 - 25, color, 15);
  }
  ctx.textAlign = 'left';
  text(ctx, '入料端', start, y0 - 44, '#c4d7e5', 15);
  const step = span > 2000 ? 1000 : 500;
  for (let mm = 0; mm <= span; mm += step) {
    line(ctx, x(mm), y1 + 8, x(mm), y1 + 15, colors.dim);
    text(ctx, (mm / 1000).toFixed(mm % 1000 ? 1 : 0) + ' m', x(mm), y1 + 35, colors.ink, 14);
  }
  if (span === meta.beltLength) {
    ctx.textAlign = 'right';
    text(ctx, (span / 1000).toFixed(1) + ' m', viewEnd, y1 + 35, colors.ink, 14);
    ctx.textAlign = 'left';
  }
}

function pseudo(value) {
  if (value < 2500) return [22, 24, 54];
  const strength = Math.min(value / 21000, 1);
  return [Math.round(55 + 150 * strength), Math.round(39 + 115 * strength), Math.round(148 + 74 * strength)];
}

function depthRgb(value) {
  if (value < 0) return [199, 134, 65];
  const strength = Math.min(value / 490, 1);
  return [Math.round(21 + 75 * strength), Math.round(35 + 179 * strength), Math.round(58 + 119 * strength)];
}

function previewColor(values, sensor, row) {
  if (!values.length) return row % 6 < 3 ? [160, 86, 44] : [104, 54, 34];
  if (sensor === 'rgb') return values.slice(row * 3, row * 3 + 3);
  if (sensor === 'hsi') return pseudo(values[row]);
  return depthRgb(values[row]);
}

export function drawPreview(canvas, rows, sensor, width = 64) {
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#0b1523';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  if (!rows.length) {
    for (let i = 1; i < 5; i++) line(ctx, canvas.width * i / 5, 0, canvas.width * i / 5, canvas.height, '#1b2b3b');
    return;
  }
  const columns = Math.max(60, Math.min(150, rows.length));
  const visible = rows.slice(-columns);
  const pixels = ctx.createImageData(columns, width);
  for (let col = 0; col < columns; col++) {
    const values = visible[col];
    if (!values) continue;
    for (let row = 0; row < width; row++) {
      const rgba = pixels.data;
      const index = (row * columns + col) * 4;
      const rgb = previewColor(values, sensor, row);
      rgb.forEach((channel, i) => rgba[index + i] = channel);
      rgba[index + 3] = 255;
    }
  }
  const scratch = document.createElement('canvas');
  scratch.width = columns;
  scratch.height = width;
  scratch.getContext('2d').putImageData(pixels, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(scratch, 0, 0, canvas.width, canvas.height);
}

export function drawAlignedPreview(canvas, samples, sensor, meta, outline) {
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#0b1523';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  if (!meta) return;
  const { scanX, pitch, beltWidth, previewWidth } = meta;
  const extent = meta.previewTravel ?? meta.travel;
  const scale = Math.min((canvas.width - 32) / extent, (canvas.height - 32) / beltWidth);
  const left = (canvas.width - extent * scale) / 2;
  const top = 8;
  const origin = scanX - extent;
  const x = (mm) => left + (mm - origin) * scale;
  const y = (mm) => top + mm * scale;
  const step = beltWidth / previewWidth;
  box(ctx, left, top, extent * scale, beltWidth * scale, 0, '#142237');
  ctx.save();
  ctx.beginPath();
  ctx.rect(left, top, extent * scale, beltWidth * scale);
  ctx.clip();
  for (const sample of samples) {
    if (!sample || !Number.isFinite(sample.displacementMm)) continue;
    // The fixed scan line intersects this position in the moving belt frame.
    const center = (sample.scanX ?? scanX) - sample.displacementMm;
    const width = sample.pitch ?? pitch;
    const x0 = Math.round(x(center - width / 2));
    const x1 = Math.round(x(center + width / 2));
    if (x1 <= x0 || x1 <= left || x0 >= left + extent * scale) continue;
    for (let row = 0; row < previewWidth; row++) {
      ctx.fillStyle = `rgb(${previewColor(sample.pixels, sensor, row).join(',')})`;
      const y0 = Math.round(y(row * step));
      const y1 = Math.round(y((row + 1) * step));
      ctx.fillRect(x0, y0, x1 - x0, y1 - y0);
    }
  }
  if (outline) {
    ctx.setLineDash([4, 3]);
    for (const item of meta.objects) drawFootprint(ctx, item, 0, x, y, '#f1f6ff');
  }
  ctx.restore();
  box(ctx, left, top, extent * scale, beltWidth * scale, 0, null, colors.border);
  text(ctx, `${Number(origin.toFixed(2))} mm`, left, y(beltWidth) + 17, colors.ink, 12);
  ctx.textAlign = 'center';
  text(ctx, '皮带方向 →', canvas.width / 2, y(beltWidth) + 17, colors.ink, 12);
  ctx.textAlign = 'right';
  text(ctx, `${scanX} mm`, x(scanX), y(beltWidth) + 17, colors.ink, 12);
  ctx.textAlign = 'left';
}

export function drawInferenceWindow(canvas, rows, meta, window) {
  const production = meta?.layout === 'production';
  const canvasWidth = production ? 396 : 640;
  const canvasHeight = production ? 570 : 350;
  if (canvas.width !== canvasWidth) canvas.width = canvasWidth;
  if (canvas.height !== canvasHeight) canvas.height = canvasHeight;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#0b1523';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  if (!meta || !window || !rows.length) {
    text(ctx, '等待窗口结果', 20, 36, colors.dim, 19);
    return;
  }
  const sensor = window.model.package === 'sim-hsi' ? 'hsi' : 'rgb';
  const coordinates = rows.map((row) => sensor === 'hsi'
    ? meta.hsiScanX - row.hsiDisplacementMm
    : meta.scanX - row.displacementMm);
  const { pitch, beltWidth, previewWidth } = meta;
  // The first row is on the positive-X side. Count each row as a strip of one pitch.
  const max = production ? meta.scanX - (window.first - 0.5) * pitch : Math.max(...coordinates) + 3 * pitch;
  const span = production ? meta.m03Rows * pitch : max - (Math.min(...coordinates) - 3 * pitch);
  const frameSpan = meta.packetLines * pitch;
  const min = max - span;
  const top = production ? 42 : 28;
  const scale = Math.min((canvas.width - 100) / span, (canvas.height - top - 53) / beltWidth);
  const width = production ? span * scale : canvas.width - 78;
  const height = production ? beltWidth * scale : canvas.height - top - 50;
  const left = production ? (canvas.width - width) / 2 : 57;
  const x = (mm) => left + (mm - min) / (max - min) * width;
  const y = (mm) => top + mm / beltWidth * height;
  box(ctx, left, top, width, height, 0, '#17273b');
  ctx.save();
  ctx.beginPath();
  ctx.rect(left, top, width, height);
  ctx.clip();
  for (const [i, row] of rows.entries()) {
    const center = coordinates[i];
    const start = Math.round(x(center - pitch / 2));
    const end = Math.round(x(center + pitch / 2));
    if (end <= start) continue;
    for (let col = 0; col < previewWidth; col++) {
      const upper = y(col / previewWidth * beltWidth);
      const lower = y((col + 1) / previewWidth * beltWidth);
      ctx.fillStyle = `rgb(${previewColor(row[sensor + 'Pixels'], sensor, col).join(',')})`;
      ctx.fillRect(start, Math.floor(upper), end - start, Math.ceil(lower) - Math.floor(upper));
    }
  }
  if (production) {
    ctx.setLineDash([4, 4]);
    for (let mm = frameSpan; mm < span; mm += frameSpan) {
      const at = x(min + mm);
      line(ctx, at, top, at, top + height, '#7590a9');
    }
    ctx.setLineDash([]);
  }
  drawCandidates(ctx, window.evidence, x, y, 0);
  ctx.restore();
  if (production) {
    box(ctx, left, top, width, height, 0, null, '#7590a9');
    const frameCount = Math.ceil(span / frameSpan);
    const bottom = y(beltWidth);
    ctx.textAlign = 'center';
    for (let frame = 0; frame <= frameCount; frame++) {
      const offset = Math.min(frame * frameSpan, span);
      const at = x(min + offset);
      line(ctx, at, bottom, at, bottom + 6, colors.dim);
      text(ctx, String(offset), at, bottom + 21, colors.ink, 14);
      if (frame < frameCount) {
        const midpoint = (offset + Math.min(offset + frameSpan, span)) / 2;
        text(ctx, '第' + (frameCount - frame) + '段', x(min + midpoint), top - 11, colors.ink, 13);
      }
    }
    text(ctx, '窗口内沿带 X（mm）', left + width / 2, bottom + 42, colors.ink, 14);
    ctx.textAlign = 'right';
    for (const mm of [0, beltWidth / 2, beltWidth]) text(ctx, String(mm), left - 9, y(mm) + 5, colors.ink, 14);
    ctx.textAlign = 'left';
    text(ctx, '跨带 Y（mm）', 10, 18, colors.ink, 14);
    return;
  }
  for (const mm of [0, beltWidth / 2, beltWidth]) text(ctx, String(mm), 11, y(mm) + 5, colors.ink, 15);
  text(ctx, 'Y mm', 10, 18, colors.ink, 15);
  text(ctx, min.toFixed(1), left, y(beltWidth) + 24, colors.ink, 15);
  ctx.textAlign = 'center';
  text(ctx, '沿带 X（mm）', left + width / 2, y(beltWidth) + 24, colors.ink, 15);
  ctx.textAlign = 'right';
  text(ctx, max.toFixed(1), left + width, y(beltWidth) + 24, colors.ink, 15);
  ctx.textAlign = 'left';
}
