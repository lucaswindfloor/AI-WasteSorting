const colors = {
  ink: '#b8c9dc', dim: '#7990a8', cyan: '#4fe4f5', drive: '#263548',
  belt: '#182739', border: '#38536b', amber: '#f7ad62',
};

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

export function drawBelt(canvas, meta, now, pulse, packet, diagnostic) {
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
    const x0 = Math.round(x(center - pitch / 2));
    const x1 = Math.round(x(center + pitch / 2));
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
