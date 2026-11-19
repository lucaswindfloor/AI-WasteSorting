export const phaseText = {
  accepted: '已受理', waitingDevice: '等待现场反馈', waitingClear: '等待清空', canChange: '确认可换箱',
  waitingMapping: '等待映射生效', waitingActivation: '等待配置生效', applied: '已生效',
  failed: '失败', needsReview: '待确认',
};

export const modeText = { idle: '待机', running: '运行中', paused: '已暂停', draining: '排空中', ended: '已结束' };
export const actionText = {
  start: '启动', pause: '暂停', drain: '停料排空', changeBin: '申请换箱', approve: '批准',
  publish: '发布', rollback: '回退', acknowledge: '确认告警',
};

export const issueText = {
  none: '', notReady: '服务暂不可用', unauthorized: '当前角色无权限', invalid: '请检查输入',
  staleView: '状态已更新，请核对后重新提交', conflict: '存在冲突，请检查相关操作',
  conditions: '现场或配置条件未满足', persistence: '记录未确认，请用原标识查询',
  deviceRejected: '现场拒绝该操作', effectUnknown: '现场结果待确认，请保留原操作标识',
  loadFailed: '配置部分加载失败', capacity: '请求超出限额', notFound: '记录暂不可查',
};

const escapeMap = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
export function esc(value = '') {
  return String(value ?? '').replace(/[&<>"']/gu, (character) => escapeMap[character]);
}

export function pill(label, kind = '') {
  return '<span class="pill ' + esc(kind) + '">' + esc(label) + '</span>';
}

export function stamp(value) {
  if (!value) return '<span class="subtle">来源待确认</span>';
  const seconds = BigInt(value.observed) / 1000000000n;
  return '<span class="subtle">' + esc(value.source) + ' · T+' + seconds + 's</span>' +
    (value.fresh ? '' : ' ' + pill('已过期', 'alert'));
}

export function action(snapshot, online, name, object = '', label = actionText[name]) {
  const eligibility = snapshot.actions.find((entry) => entry.action === name && entry.object === object);
  const issue = online ? eligibility?.issue ?? 'notReady' : 'notReady';
  const reason = issueText[issue] ?? '现场条件未满足';
  return '<button type="button" class="' + (name === 'start' || name === 'publish' ? 'primary' : '') +
    '" data-action="' + esc(name) + '" data-object="' + esc(object) + '"' +
    (issue === 'none' ? '' : ' disabled title="' + esc(reason) + '" aria-label="' + esc(label + '，' + reason) + '"') +
    '>' + esc(label) + '</button>';
}

export function pageTitle(title, detail = '') {
  return '<div class="page-title"><h1>' + esc(title) + '</h1>' +
    (detail ? '<p>' + esc(detail) + '</p>' : '') + '</div>';
}

export function table(headings, rows) {
  return '<div class="table-scroll"><table><thead><tr>' +
    headings.map((label) => '<th scope="col">' + esc(label) + '</th>').join('') +
    '</tr></thead><tbody>' + (rows.length ? rows.map((columns) => '<tr>' +
      columns.map((value) => '<td>' + value + '</td>').join('') + '</tr>').join('') :
      '<tr><td colspan="' + headings.length + '" class="subtle">暂无记录</td></tr>') +
    '</tbody></table></div>';
}

export function operationTable(operations) {
  const sorted = [...operations].sort((left, right) => {
    const delta = BigInt(right.updated) - BigInt(left.updated);
    return delta < 0n ? -1 : delta > 0n ? 1 : 0;
  });
  return table(['操作', '对象', '阶段', '操作标识'], sorted.map((op) => [
    esc(actionText[op.action]), esc(op.object || '工位'),
    pill(phaseText[op.phase], op.phase === 'applied' ? 'good' :
      ['failed', 'needsReview'].includes(op.phase) ? 'alert' : '') +
      (op.issue !== 'none' ? '<div class="subtle">' + esc(issueText[op.issue]) + '</div>' : ''),
    '<button type="button" class="quiet mono" data-operation="' + esc(op.id) + '">' + esc(op.id) + '</button>',
  ]));
}
