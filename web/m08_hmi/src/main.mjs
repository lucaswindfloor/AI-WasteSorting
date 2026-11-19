import { api } from './api/client.mjs';
import { SnapshotState } from './state/snapshot.mjs';
import { savedIds } from './state/operations.mjs';
import { workbench } from './pages/workbench/index.mjs';
import { tasksAndBins } from './pages/tasks_and_bins/index.mjs';
import { diagnostics } from './pages/diagnostics/index.mjs';
import { configuration } from './pages/configuration/index.mjs';
import { batchEvidence } from './pages/batch_evidence/index.mjs';
import { actionText, issueText, phaseText } from './ui.mjs';

const state = new SnapshotState();
const routes = { workbench, tasks: tasksAndBins, diagnostics, configuration, evidence: batchEvidence };
const screen = document.querySelector('#screen');
const scope = document.querySelector('#scope');
const profile = document.querySelector('#profile');
const connection = document.querySelector('#connection');
const notice = document.querySelector('#notice');
const context = {
  online: false, recipe: null, packageId: null, newBins: new Map(), purpose: '',
  evidencePage: null, manifest: 'RUN-042-v1', job: null,
};
let roleChange = 0;
let switching = false;
let polling = false;
let submitting = false;
let lastMarkup = '';
let shownOperation = null;

function route() {
  const key = location.hash.slice(1);
  return Object.hasOwn(routes, key) ? key : 'workbench';
}

function feedback(message, warning = false) {
  notice.textContent = message;
  notice.hidden = !message;
  notice.className = warning ? 'notice warning' : 'notice';
}

function errorText(error) {
  return issueText[error.issue] ?? '连接失败，请稍后重试。';
}

function render(force = false) {
  const view = state.snapshot;
  context.online = state.online && !switching && !submitting;
  connection.className = 'pill ' + (state.online && !switching ? 'good' : 'alert');
  connection.textContent = state.online && !switching ? '已连接' : view ? '连接中断 · 显示最后快照' : '连接中';
  scope.textContent = view ? view.station + '  /  ' + view.run : '等待工位数据';
  for (const link of document.querySelectorAll('#navigation a')) {
    if (link.hash === '#' + route()) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  }
  if (!view) {
    lastMarkup = '<p class="empty">正在读取完整快照…</p>';
    screen.innerHTML = lastMarkup;
    return;
  }
  const current = document.activeElement;
  if (!force && current instanceof HTMLElement && screen.contains(current) &&
    current.matches('input, select, textarea')) return;
  const markup = routes[route()](view, context);
  if (markup === lastMarkup) return;
  const focusAttributes = ['data-action', 'data-object', 'data-operation', 'data-local'];
  const restore = current instanceof HTMLButtonElement && screen.contains(current)
    ? focusAttributes.map((name) => current.getAttribute(name)) : null;
  screen.innerHTML = markup;
  lastMarkup = markup;
  if (restore) {
    [...screen.querySelectorAll('button')].find((button) =>
      focusAttributes.every((name, index) => button.getAttribute(name) === restore[index]))?.focus();
  }
}

function announceOperation(op, detail = false) {
  const evidence = detail && op.evidence.length ? ' · 证据 ' + op.evidence.join('，') : '';
  feedback(actionText[op.action] + ' ' + phaseText[op.phase] + ' · ' + op.id +
    (op.issue !== 'none' ? ' · ' + issueText[op.issue] : '') + evidence,
  ['failed', 'needsReview'].includes(op.phase));
}

function restoreOperation(view) {
  const id = savedIds.last(view);
  if (id) void queryOperation(id, true);
}

async function refresh() {
  if (polling || switching) return;
  const startRole = roleChange;
  const previous = state.snapshot;
  const wasOnline = state.online;
  polling = true;
  try {
    const latest = await api.workbench();
    if (startRole !== roleChange) return;
    state.apply(latest);
    const tracked = latest.operations.find((op) => op.id === shownOperation);
    if (tracked) announceOperation(tracked);
    if (!previous || latest.session !== previous.session || !wasOnline) {
      profile.value = latest.session;
      restoreOperation(latest);
      if (route() === 'evidence' && latest.canReadEvidence) void loadEvidence();
      const exportId = savedIds.last(latest, 'export');
      if (exportId && latest.canExport) void pollJob(exportId);
    }
    if (context.job?.stage === 'queued') void pollJob(context.job.id);
  } catch {
    if (startRole === roleChange) state.disconnect();
  } finally {
    polling = false;
    render(!state.online);
  }
}

async function queryOperation(id, onlyIfPending = false) {
  const role = roleChange;
  try {
    const op = await api.operation(id);
    if (role !== roleChange) return;
    if (onlyIfPending && ['applied', 'failed'].includes(op.phase)) return;
    shownOperation = onlyIfPending ? op.id : null;
    announceOperation(op, true);
  } catch (error) {
    if (role === roleChange && !onlyIfPending) feedback(errorText(error) + ' · ' + id, true);
  }
}

async function submit(action, object) {
  if (submitting || switching || !state.online) return;
  const view = state.snapshot;
  const eligibility = view.actions.find((entry) => entry.action === action && entry.object === object);
  if (eligibility?.issue !== 'none') return feedback(issueText[eligibility?.issue] ?? '当前条件不允许操作。', true);
  const value = action === 'changeBin' ? (context.newBins.get(object) ?? '').trim() : '';
  if (action === 'changeBin' && !value) return feedback('请输入新箱的实物标识。', true);

  shownOperation = null;
  const id = crypto.randomUUID();
  savedIds.remember(view, id);
  submitting = true;
  profile.disabled = true;
  render(true);
  try {
    const op = await api.submit({ id, action, object, value, revision: view.revision, epoch: view.epoch });
    shownOperation = op.id;
    announceOperation(op);
    if (action === 'changeBin') context.newBins.delete(object);
    await refresh();
  } catch (error) {
    feedback(errorText(error) + ' · ' + id, true);
    if (!error.issue || error.issue === 'persistence') void queryOperation(id, true);
    await refresh();
  } finally {
    submitting = false;
    profile.disabled = false;
    render(true);
  }
}

async function loadEvidence(cursor = '') {
  const previous = roleChange;
  if (!state.online || !state.snapshot?.canReadEvidence) return;
  try {
    const page = await api.evidence(context.manifest, cursor);
    if (previous !== roleChange) return;
    context.evidencePage = page;
    render(true);
  } catch (error) {
    if (previous === roleChange) feedback(errorText(error), true);
  }
}

async function pollJob(id) {
  const previous = roleChange;
  try {
    const job = await api.job(id);
    if (previous === roleChange) {
      context.job = job;
      render();
    }
  } catch (error) {
    if (previous === roleChange && route() === 'evidence') feedback(errorText(error), true);
  }
}

async function enqueue() {
  const view = state.snapshot;
  if (submitting || switching || !state.online || !view?.canExport || !context.purpose.trim()) return;
  shownOperation = null;
  const id = crypto.randomUUID();
  savedIds.remember(view, id, 'export');
  submitting = true;
  profile.disabled = true;
  render(true);
  try {
    context.job = await api.export({ id, manifest: context.manifest, first: 'T041', last: 'T042',
      purpose: context.purpose.trim(), limit: '2' });
    feedback('导出任务已受理 · ' + id);
  } catch (error) {
    feedback(errorText(error) + ' · ' + id, true);
  } finally {
    submitting = false;
    profile.disabled = false;
    render(true);
  }
}

window.addEventListener('hashchange', () => {
  shownOperation = null;
  feedback('');
  render(true);
  if (route() === 'evidence') void loadEvidence();
});

profile.addEventListener('change', async () => {
  switching = true;
  ++roleChange;
  profile.disabled = true;
  state.clear();
  shownOperation = null;
  context.recipe = null;
  context.packageId = null;
  context.newBins.clear();
  context.purpose = '';
  context.evidencePage = null;
  context.job = null;
  feedback('');
  render(true);
  try {
    await api.changeRole(profile.value);
  } catch (error) {
    feedback(errorText(error), true);
  } finally {
    switching = false;
    profile.disabled = false;
    await refresh();
  }
});

screen.addEventListener('change', (event) => {
  if (event.target.id === 'recipe') context.recipe = event.target.value;
  else if (event.target.id === 'package') context.packageId = event.target.value;
  else return;
  render(true);
});
screen.addEventListener('input', (event) => {
  if (event.target.dataset.newBin) context.newBins.set(event.target.dataset.newBin, event.target.value);
  if (event.target.name === 'purpose') context.purpose = event.target.value;
});
screen.addEventListener('focusout', () => requestAnimationFrame(() => render()));
screen.addEventListener('click', (event) => {
  const button = event.target.closest('button');
  if (!button || button.disabled) return;
  if (button.dataset.action) void submit(button.dataset.action, button.dataset.object);
  if (button.dataset.operation) void queryOperation(button.dataset.operation);
  if (button.dataset.local === 'evidence-first') void loadEvidence();
  if (button.dataset.local === 'evidence-next') void loadEvidence(context.evidencePage?.next);
});
screen.addEventListener('submit', (event) => {
  if (event.target.id === 'export') {
    event.preventDefault();
    void enqueue();
  }
});

void refresh();
setInterval(refresh, 1000);
