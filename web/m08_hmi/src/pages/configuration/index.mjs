import { action, esc, operationTable, pageTitle, pill, stamp, table } from '../../ui.mjs';

export function configuration(view, context) {
  const { active, staged } = view;
  const stagedPackage = view.packages.find((pkg) => pkg.id === staged?.package);
  const selected = view.packages.find((pkg) => pkg.id === context.packageId) ?? view.packages[0];
  const choices = view.packages.map((pkg) => '<option value="' + esc(pkg.id) + '"' +
    (pkg.id === selected?.id ? ' selected' : '') + '>' + esc(pkg.id) + '</option>').join('');
  const modules = new Set([...Object.keys(selected?.expected ?? {}), ...Object.keys(active?.actual ?? {}),
    ...Object.keys(staged?.actual ?? {}), ...Object.keys(staged?.problems ?? {})]);
  const rows = [...modules].sort().map((key) => [
    esc(key), esc(selected?.expected[key] || '—'), esc(active?.actual[key] || '待确认'),
    esc(staged?.actual[key] || '—'),
    staged?.problems[key] ? '<span class="warning">' + esc(staged.problems[key]) + '</span>' : '—',
  ]);
  return pageTitle('配置管理', '核对候选包、模块实装和配置世代。') +
    '<section class="panel"><div class="panel-title"><div><h2>实际生效</h2><p>' +
    esc(active?.package || '待确认') + ' · 世代 ' + esc(view.epoch) + '</p></div>' +
    (active?.stamp ? stamp(active.stamp) : '') + '</div>' +
    '<p class="subtle">' + (staged ? '最近加载 ' + esc(stagedPackage?.id ?? staged.package) +
      ' · 世代 ' + esc(staged.epoch) : '暂无加载记录') + '</p></section>' +
    '<section class="panel"><div class="panel-title"><h2>候选配置</h2>' +
    pill(selected?.approved ? '已批准' : '待批准', selected?.approved ? 'good' : 'alert') + '</div>' +
    '<div class="controls"><label>配置包<select id="package">' + choices + '</select></label>' +
    action(view, context.online, 'approve', selected?.id) + action(view, context.online, 'publish', selected?.id) +
    action(view, context.online, 'rollback', selected?.id) + '</div>' +
    '<p class="subtle">' + esc(selected?.digest ?? '') + ' · ' + (selected?.e11 ? '含 E11' : '标准路线') + '</p>' +
    table(['模块', '候选要求', '实际生效', '最近加载', '加载反馈'], rows) + '</section>' +
    '<section class="panel"><div class="panel-title"><h2>配置操作记录</h2></div>' +
    operationTable(view.operations.filter((op) => ['approve', 'publish', 'rollback'].includes(op.action))) + '</section>';
}
