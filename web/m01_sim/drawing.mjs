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

function drawMaterial(ctx, item, offset, x, y) {
  const center = x(item.x + offset);
  const lane = y(item.y);
  const width = x(item.length) - x(0);
  const height = y(item.width) - y(0);
  if (item.material === 'PET') {
    const body = width * 0.76;
    box(ctx, center - 0.5 * width, lane - height / 2, body, height, 11, '#217fa0', '#81e2fa');
    box(ctx, center + 0.24 * width, lane - height * 0.24, width * 0.17, height * 0.48, 3, '#2e8ca6', '#81e2fa');
    box(ctx, center + 0.395 * width, lane - height * 0.29, width * 0.106, height * 0.58, 2, '#74ddf4');
    box(ctx, center - width * 0.21, lane - height * 0.44, width * 0.26, height * 0.88, 3, '#5ec8db');
    line(ctx, center - width * 0.43, lane - height * 0.33, center + width * 0.20,
      lane - height * 0.33, '#8dd7e7');
  } else {
    box(ctx, center - width / 2, lane - height / 2, width, height, 9, '#a96836', '#ffc78d');
    line(ctx, center - width * 0.34, lane - height * 0.34, center + width * 0.32,
      lane - height * 0.34, '#ffd39f', 2);
    box(ctx, center + width * 0.29, lane - height * 0.38, width * 0.12, height * 0.75, 3, '#e4ad74');
    box(ctx, center - width * 0.33, lane + height * 0.17, width * 0.44, height * 0.11, 3, '#d4905c');
  }
  text(ctx, item.id + '  ' + item.material, center - width / 2, lane - height / 2 - 11, '#dae9f6', 13);
}

export function drawBelt(canvas, meta, now, pulse, packet, diagnostic) {
  const ctx = canvas.getContext('2d');
  const shift = distanceAt(meta, now);
  const x = (value) => 48 + (value + 151) * 904 / 631;
  const y = (value) => 101 + value * 194 / (meta?.beltWidth ?? 240);
  const scanner = x(meta?.scanX ?? 142);
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
  for (let travel = -150 + (shift % 29); travel < 490; travel += 29) {
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
  text(ctx, 'RGB + HSI 共同扫描线', scanner - 83, 48, '#b4ecf6', 15);
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
      let rgb;
      if (!values.length) rgb = row % 6 < 3 ? [160, 86, 44] : [104, 54, 34];
      else if (sensor === 'rgb') rgb = values.slice(row * 3, row * 3 + 3);
      else if (sensor === 'hsi') rgb = pseudo(values[row]);
      else rgb = depthRgb(values[row]);
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
