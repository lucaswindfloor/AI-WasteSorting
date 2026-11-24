// M07 records effects and contact coordinates. The poses between those events
// are drawn for replay. They are not controller telemetry.
export function createPickMotion({ meta, events }) {
  const actions = new Map(events.filter((event) => event.type === 'pick-action')
    .map(({ data, t }) => [data.state, { time: t, point: data.point }]));
  const item = meta.objects[0];
  const home = [meta.pickBegin + 255, 530, 218];
  const bin = { x: meta.pickBegin + 205, y: meta.beltWidth + 159, length: 365, width: 225, rim: 78 };
  return {
    item, home, bin, admitted: actions.get('admitted')?.time, contact: actions.get('contact'),
    holding: actions.get('holding')?.time, released: actions.get('released')?.time,
    ready: actions.get('robot-ready')?.time, pending: actions.get('pending')?.time,
  };
}

function progress(now, start, end) {
  return Math.max(0, Math.min(1, (now - start) / (end - start || 1)));
}

function interpolate(from, to, ratio) {
  const eased = ratio * ratio * (3 - 2 * ratio);
  return from.map((value, index) => value + (to[index] - value) * eased);
}

export function samplePickMotion(motion, now, travel, contactTravel) {
  const { item, home, bin, admitted, contact, holding, released, ready, pending } = motion;
  const bottle = [item.x + travel, item.y, item.height];
  if (pending !== undefined && now >= pending) {
    return { stage: 'pending', tool: home, bottle };
  }
  if (!contact?.point || admitted === undefined || now < admitted) {
    return { stage: 'standby', tool: home, bottle };
  }

  const point = contact.point;
  const hover = [point[0], point[1], home[2]];
  if (now < contact.time) {
    const ratio = progress(now, admitted, contact.time);
    const aboveBottle = [point[0] + travel - contactTravel, point[1], home[2]];
    if (ratio < 0.46) {
      return { stage: 'approach', tool: interpolate(home, aboveBottle, ratio / 0.46), bottle };
    }
    const surface = [aboveBottle[0], point[1], point[2]];
    return { stage: 'descend', tool: interpolate(aboveBottle, surface, (ratio - 0.46) / 0.54), bottle };
  }
  if (holding === undefined || now < holding) {
    return { stage: 'contact', tool: point, bottle };
  }

  const offset = [item.x + contactTravel - point[0], item.y - point[1], item.height - point[2]];
  if (released === undefined || now === holding) {
    return { stage: 'holding', tool: point, bottle: point.map((value, index) => value + offset[index]) };
  }
  const binHover = [bin.x - offset[0], bin.y - offset[1], home[2]];
  const drop = [binHover[0], binHover[1], item.height - offset[2]];
  if (now < released) {
    const ratio = progress(now, holding, released);
    let stage = 'lift';
    let tool = interpolate(point, hover, ratio / 0.27);
    if (ratio >= 0.73) {
      stage = 'lower';
      tool = interpolate(binHover, drop, (ratio - 0.73) / 0.27);
    } else if (ratio >= 0.27) {
      stage = 'transfer';
      tool = interpolate(hover, binHover, (ratio - 0.27) / 0.46);
    }
    return { stage, tool, bottle: tool.map((value, index) => value + offset[index]) };
  }

  const placed = [bin.x, bin.y, item.height];
  if (ready !== undefined && now >= ready) return { stage: 'ready', tool: home, bottle: placed };
  if (ready === undefined || now === released) return { stage: 'released', tool: drop, bottle: placed };
  const ratio = progress(now, released, ready);
  const tool = ratio < 0.45
    ? interpolate(drop, binHover, ratio / 0.45)
    : interpolate(binHover, home, (ratio - 0.45) / 0.55);
  return { stage: 'return', tool, bottle: placed };
}

export const pickMotionLabels = {
  standby: '吸盘待机', approach: '接近抓点', descend: '跟随皮带下降', contact: '接触物料',
  holding: '已接触 · 吸住物料', lift: '抬起物料', transfer: '携瓶移向测试箱',
  lower: '下降到测试箱', released: '释放到测试箱',
  return: '吸盘回位中', ready: '已回位', pending: '发令效果待核 · 动作位置未确认',
};

export function drawPickMotion(canvas, meta, motion, pose, travel, drawSprite) {
  const ctx = canvas.getContext('2d');
  const { bin, item } = motion;
  const start = meta.pickBegin - 355;
  const end = meta.pickEnd + 260;
  const w = meta.beltWidth;
  const [tx, ty, tz] = pose.tool;
  const columnX = Math.max(start + 25, Math.min(end - 25, tx));
  const railZ = 265;
  const project = ([x, y, z]) => [126 + (x - start) * 0.48 + y * 0.23,
    223 + (x - start) * 0.045 + y * 0.13 - z * 0.7];
  const face = (points, fill, stroke) => {
    ctx.beginPath();
    for (const point of points) ctx.lineTo(...project(point));
    ctx.closePath();
    ctx.fillStyle = fill;
    ctx.fill();
    if (stroke) {
      ctx.strokeStyle = stroke;
      ctx.lineWidth = 1;
      ctx.stroke();
    }
  };
  const beam = (from, to, width, color) => {
    ctx.beginPath();
    ctx.moveTo(...project(from));
    ctx.lineTo(...project(to));
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.stroke();
  };
  const plane = (x0, x1, y0, y1, z) => [[x0, y0, z], [x1, y0, z], [x1, y1, z], [x0, y1, z]];
  const label = (labelText, point, color) => {
    ctx.font = '600 17px system-ui, sans-serif';
    ctx.fillStyle = color;
    ctx.fillText(labelText, ...project(point));
  };

  ctx.fillStyle = '#0c1725';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  face(plane(start - 135, end + 63, -115, w + 305, -38), '#111f2c', '#24374a');
  face([[start, w, 0], [end, w, 0], [end, w, -25], [start, w, -25]], '#243a4e', '#4c657a');
  face(plane(start, end, 0, w, 0), '#202f3a', '#465f74');
  face(plane(meta.pickBegin, meta.pickEnd, 0, w, 0), '#244449', '#557f7b');
  for (let line = start + (travel % 105); line < end; line += 105) {
    beam([line, 0, 0], [line, w, 0], 1, '#54758455');
  }
  beam([start, w, 0], [end, w, 0], 4, '#57768d');
  label('皮带方向 →', [start + 55, w, -59], '#9ab5ca');

  const left = bin.x - bin.length / 2, right = bin.x + bin.length / 2;
  const back = bin.y - bin.width / 2, front = bin.y + bin.width / 2;
  const rim = bin.rim;
  face(plane(left, right, back, front, 0), '#37434a', '#b38d50');
  face([[left, back, 0], [right, back, 0], [right, back, rim], [left, back, rim]], '#5b5344', '#aa8a54');

  const [bx, by, bz] = pose.bottle;
  const cupKnown = pose.stage !== 'pending';
  for (const x of [start + 25, end - 25]) {
    beam([x, -36, -38], [x, -36, railZ], 14, '#526a7e');
    beam([x, -36, -38], [x, -36, railZ], 4, '#8ca8bf');
  }
  beam([start + 25, -36, railZ], [end - 25, -36, railZ], 15, '#4c647a');
  beam([start + 25, -36, railZ + 5], [end - 25, -36, railZ + 5], 4, '#9fb5c6');
  if (cupKnown) {
    beam([columnX, -36, railZ], [columnX, w + 234, railZ], 16, '#71879a');
    beam([columnX, -36, railZ + 5], [columnX, w + 234, railZ + 5], 4, '#acc3d1');
  }

  if (bx > start - item.length / 2 && bx < end + item.length / 2) {
    const [shadowX, shadowY] = project([bx, by, 0]);
    ctx.fillStyle = '#090f1680';
    ctx.beginPath();
    ctx.ellipse(shadowX, shadowY, item.length * 0.23, 7, 0.095, 0, 2 * Math.PI);
    ctx.fill();
    const [px, py] = project([bx, by, bz]);
    const width = item.length * 0.48;
    const height = item.height * 0.7 + item.width * 0.11;
    ctx.save();
    ctx.translate(px, py + height * 0.4);
    ctx.rotate(0.095);
    if (!drawSprite(ctx, item.material, -width / 2, -height / 2, width, height)) {
      ctx.fillStyle = '#9ac9cd';
      ctx.beginPath();
      ctx.roundRect(-width / 2, -height / 2, width, height, 6);
      ctx.fill();
    }
    ctx.restore();
  }

  const corner = [right, front, rim];
  face([[right, back, rim], corner, [right, front, 0], [right, back, 0]], '#5a4830b0', '#aa8a54');
  face([[left, front, rim], corner, [right, front, 0], [left, front, 0]], '#493b2bb0', '#aa8a54');
  label('测试箱', [left + 65, front, -37], '#e6bd80');
  if (cupKnown) {
    beam([columnX, ty, railZ + 5], [columnX, ty, railZ - 24], 28, '#dc9c4f');
    beam([tx, ty, railZ - 14], [tx, ty, tz + 10], 8, '#d5e1ed');
    beam([tx, ty - 30, tz + 4], [tx, ty + 30, tz + 4], 9, '#f0b96c');
    const [cx, cy] = project([tx, ty, tz]);
    ctx.fillStyle = '#fbd091';
    ctx.beginPath();
    ctx.ellipse(cx, cy, 11, 4, 0, 0, 2 * Math.PI);
    ctx.fill();
  }
}
