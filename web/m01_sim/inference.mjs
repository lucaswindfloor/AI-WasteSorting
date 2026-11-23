import { drawInferenceWindow } from './drawing.mjs';

const $ = (id) => document.getElementById(id);
const states = {
  completed: '有候选', no_detection: '无候选', input_invalid: '输入无效',
  expired: '超期', queue_full: '队列满', retired: '窗口退役', failed: '运行失败', rejected: '已拒绝',
};
const reasons = {
  no_valid_candidates: '窗口里没有通过阈值的候选',
  too_many_missing_rows: '缺行或无共同覆盖的行数超出模型上限',
  insufficient_valid_samples: '有效样本比例不足',
  expired_waiting_for_window: '等待组窗时已经超期',
  expired_before_enqueue: '入队前已经超期',
  expired_in_queue: '排队时超期',
  expired_during_execution: '执行时超期',
  expired_before_delivery: '回映结束前超期',
  expired_at_report_delivery: '交付时已超期',
  inference_queue_capacity: '等待数量或内存达到上限',
  unfinished_window_at_boundary: '采集世代结束，尾部窗口未组满',
  configuration_retired: '旧世代已退役',
};
const stages = { warmed: '预热通过', rejected: '候选配置被拒绝', activated: '已启用', draining: '已退役，已提交任务可排空' };
const paths = { 'sim-joint': '联合', 'sim-hsi': 'HSI', 'sim-rgb': 'RGB' };
const classes = { object: '物料轮廓', PET: 'PET', AL: 'Al' };

export function windowKey(row) {
  return [row.generation, row.first, row.model.package, row.model.version].join('/');
}

function describeModel(model) {
  return (paths[model.package] ?? model.package) +
    (model.version === 'candidate' ? '候选版' : ' v' + model.version);
}

function millis(value) {
  return Number.isFinite(value) ? value.toFixed(2) + ' ms' : '—';
}

function timeline(row) {
  const body = $('inference-times');
  body.replaceChildren();
  if (!row) {
    const cell = body.insertRow().insertCell();
    cell.colSpan = 5;
    cell.textContent = '等待窗口';
    return;
  }
  const line = body.insertRow();
  for (const value of [row.firstReceivedMs, row.readyMs, row.startedMs, row.finishedMs, row.deadlineMs]) {
    line.insertCell().textContent = millis(value);
  }
  if (row.preparedMs != null || row.backendMs != null) {
    const detail = body.insertRow().insertCell();
    detail.colSpan = 5;
    detail.textContent = '预处理完成 ' + millis(row.preparedMs) + ' · 后端完成 ' + millis(row.backendMs);
  }
}

function displayQuality(row) {
  if (!row) return '';
  const entries = [reasons[row.reason] ?? row.reason];
  const quality = row.quality;
  if (quality?.missingTriggers.length) {
    const intervals = [];
    for (const id of quality.missingTriggers) {
      const last = intervals.at(-1);
      if (last && last[1] + 1 === id) last[1] = id;
      else intervals.push([id, id]);
    }
    entries.push('无效行 ' + intervals.map(([first, last]) => first === last ? '#' + first :
      '#' + first + '–' + last).join('、'));
  }
  for (const [label, key] of [['RGB', 'rgb'], ['HSI', 'hsi']]) {
    const count = quality?.[key];
    if (count) entries.push(label + ' 有效样本 ' + count.valid + ' / ' + count.total);
  }
  return entries.filter(Boolean).join(' · ');
}

function displayEvidence(row, onSource) {
  const container = $('inference-evidence');
  container.replaceChildren();
  for (const item of row?.evidence ?? []) {
    const card = document.createElement('div');
    const title = document.createElement('h3');
    title.textContent = (classes[item.label] ?? item.label) + ' · ' + item.points + ' 个回映源点';
    card.append(title);
    for (const [label, source] of [['RGB', item.rgb], ['HSI', item.hsi]]) {
      if (!source) continue;
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'row-button';
      button.textContent = label + ' #' + source.trigger + ' · 帧 ' + source.frame + ' / 行 ' + source.row + ' / 列 ' + source.column;
      button.title = '查看触发的 M01 账本与 M02 配准，采集段 ' + source.segment;
      button.addEventListener('click', () => onSource(source.trigger));
      card.append(button);
    }
    container.append(card);
  }
}

export class InferenceView {
  constructor(onSource) {
    this.onSource = onSource;
    this.reset();
  }

  reset() {
    this.selected = '';
    this.windows = new Map();
    this.latest = null;
    this.health = null;
    this.models = [];
    this.lastRow = undefined;
    this.lastMeta = undefined;
    this.lastHealth = undefined;
    this.lastModelCount = -1;
    this.lastWindowCount = -1;
  }

  ingest({ type, data, t }) {
    if (type === 'm03-window') {
      this.windows.set(windowKey(data), data);
      if (data.state !== 'retired' || !this.latest) this.latest = data;
    } else if (type === 'm03-health') {
      this.health = data;
    } else if (type === 'm03-model') {
      this.models.push({ ...data, t });
    }
  }

  current() {
    return this.windows.get(this.selected) ?? this.latest;
  }

  paint(meta, rows) {
    const enabled = meta?.m03Mode && meta.m03Mode !== 'off';
    const row = this.current();
    const select = $('inference-window');
    if (this.lastWindowCount !== this.windows.size) {
      this.lastWindowCount = this.windows.size;
      select.replaceChildren(new Option('跟随回放', ''));
      for (const [id, window] of this.windows) {
        select.add(new Option('#' + window.first + '–' + window.last + ' · ' +
          describeModel(window.model) + ' · ' + (states[window.state] ?? window.state), id));
      }
      select.disabled = !this.windows.size;
    }
    select.value = this.windows.has(this.selected) ? this.selected : '';

    if (this.lastModelCount !== this.models.length) {
      this.lastModelCount = this.models.length;
      const events = this.models.filter((event) => event.action !== 'warmed' ||
        !this.models.some((other) => other.action === 'rejected' && other.generation === event.generation &&
          other.models[0]?.version === event.models[0]?.version));
      const list = $('inference-models');
      list.replaceChildren();
      for (const event of events) {
        const item = document.createElement('li');
        const version = event.models.map(describeModel).join('、');
        item.textContent = millis(event.t) + ' · 世代 ' + event.generation + ' · ' + (stages[event.action] ?? event.action) +
          ' · ' + version;
        if (event.action === 'rejected') {
          item.textContent += ' · 裁切、格式或波段顺序不符 · 仍启用 ' +
            (event.active.map(describeModel).join('、') || '—');
          item.className = 'model-rejected';
        }
        list.append(item);
      }
      if (!events.length) {
        const item = document.createElement('li');
        item.textContent = meta?.m03Mode === 'off' ? 'M03 已关闭' : '等待模型预热';
        list.append(item);
      }
    }

    if (this.lastHealth !== this.health) {
      this.lastHealth = this.health;
      const h = this.health;
      $('inference-queue').textContent = h ? '等待 ' + h.waiting + ' · 执行 ' + h.running +
        ' · 最久等待 ' + millis(h.oldestMs) + ' · 张量与映射 ' + h.bytes.toLocaleString('zh-CN') + ' B' : '';
    }
    if (this.lastRow === row && this.lastMeta === meta) return;
    this.lastRow = row;
    this.lastMeta = meta;
    if (enabled) {
      const masks = meta.m03Mask === 'allow' ? '允许部分缺测' : '要求完整输入';
      $('inference-config').textContent = '模拟规则 · ' +
        (meta.layout === 'production' ? '4 帧 · ' : '') + meta.m03Rows + ' 行窗口 / ' + meta.m03Step +
        ' 行步长 · ' + masks + ' · 处理预算 ' + meta.m03BudgetMs + ' ms';
    } else {
      $('inference-config').textContent = meta?.m03Mode === 'off' ? '本次关闭 M03 推理。' :
        '模拟规则读取实际扫描张量，输出窗口级轮廓或材质候选。';
    }
    const tag = $('inference-status');
    tag.textContent = row ? states[row.state] ?? row.state : enabled ? '等待组窗' :
      meta?.m03Mode === 'off' ? '已关闭' : '待运行';
    tag.className = 'tag' + (row ? row.state === 'completed' ? ' ok' :
      row.state === 'no_detection' || row.state === 'retired' ? '' : ' warn' : '');
    $('inference-version').textContent = row ? '#' + row.first + '–' + row.last + ' · 世代 ' +
      row.generation + ' · ' + describeModel(row.model) + ' · ' + (row.reference ?? '—') : '';
    $('inference-quality').textContent = displayQuality(row);
    $('inference-caption').textContent = row ? '局部扫描预览 · ' +
      (row.model.package === 'sim-hsi' ? 'HSI 选定波段' : 'RGB') + ' · 按沿带坐标放大窗口' : '';
    const segment = row ? [...rows.values()].filter((source) =>
      source.trigger >= row.first && source.trigger <= row.last) : [];
    drawInferenceWindow($('inference-map'), segment, meta, row);
    timeline(row);
    displayEvidence(row, this.onSource);
  }
}
