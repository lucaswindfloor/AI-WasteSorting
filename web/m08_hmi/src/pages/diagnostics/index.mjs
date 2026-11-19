import { action, actionText, esc, issueText, pageTitle, pill, stamp, table } from '../../ui.mjs';

export function diagnostics(view, context) {
  const modules = view.modules.map((module) => [
    esc(module.module), pill(module.status, module.stamp.fresh ? 'good' : 'alert'),
    esc(module.detail), stamp(module.stamp),
  ]);
  const alarms = view.alarms.map((alarm) => [
    esc(alarm.id), esc(alarm.description), esc(alarm.impact), stamp(alarm.stamp),
    alarm.active ? pill('活动', 'alert') : pill('现场已恢复', 'good'),
    view.acknowledgements[alarm.id] ? '已确认 · ' + esc(view.acknowledgements[alarm.id]) :
      action(view, context.online, 'acknowledge', alarm.id),
  ]);
  return pageTitle('设备诊断') +
    '<section class="panel"><div class="panel-title"><h2>模块与状态源</h2></div>' +
    table(['模块', '最后状态', '实际版本', '来源时间'], modules) + '</section>' +
    '<section class="panel"><div class="panel-title"><h2>告警</h2></div>' +
    table(['告警', '内容', '影响', '来源时间', '现场状态', '确认'], alarms) + '</section>' +
    (view.audits.length ? '<section class="panel"><div class="panel-title"><h2>近期操作审计</h2></div>' +
      table(['操作标识', '操作者', '动作', '对象', '快照 / 世代', '记录时间', '受理结果'],
        [...view.audits].reverse().map((row) => [esc(row.id), esc(row.actor || '会话无效'),
          esc(actionText[row.action]), esc(row.object || '工位'), esc(row.revision + ' / ' + row.epoch),
          'T+' + BigInt(row.at) / 1000000000n + 's',
          row.issue === 'none' ? pill('已受理', 'good') : pill(issueText[row.issue], 'alert')])) + '</section>' : '');
}
