import { action, esc, modeText, operationTable, pageTitle, pill, stamp } from '../../ui.mjs';

export function workbench(view, context) {
  const { active, line, safety, packages, bins, tasks, epoch, revision } = view;
  const chosen = context.recipe ?? active?.package;
  const pending = tasks.filter((task) => !task.actual || !task.destinationStamp.fresh).length;
  const available = bins.filter((bin) => bin.stamp.fresh && bin.usable && bin.present && !bin.full).length;
  const activePackage = packages.find((pkg) => pkg.id === active?.package);
  const recipeOptions = packages.filter((pkg) => pkg.approved).map(({ id }) =>
    '<option value="' + esc(id) + '"' + (id === chosen ? ' selected' : '') + '>' + esc(id) + '</option>').join('');
  return pageTitle('工作台', '监视产线状态，提交普通工艺请求。') +
    '<section class="metric-grid" aria-label="运行状态">' +
    '<div class="metric"><span>输送状态</span><strong>' + esc(modeText[line.mode]) + '</strong>' + stamp(line.stamp) + '</div>' +
    '<div class="metric"><span>普通分选资格</span><strong>' + esc(!line.stamp.fresh ? '待确认' :
      line.fence ? '配置隔离中' : line.ready ? '就绪' : '未就绪') + '</strong>' +
    '<span class="subtle">快照 ' + esc(revision) + '</span></div>' +
    '<div class="metric"><span>安全镜像</span><strong>' + esc(!safety.stamp.fresh ? '待确认' :
      safety.permits ? '允许' : '受限') + '</strong>' + stamp(safety.stamp) + '</div>' +
    '<div class="metric"><span>可用箱位</span><strong>' + available + ' / ' + bins.length + '</strong>' +
    '<span class="subtle">独立归宿 ' + pending + ' 件待核</span></div></section>' +
    '<section class="panel"><div class="panel-title"><div><h2>工艺操作</h2><p>实际配置 ' +
    esc(active?.package || '待确认') + ' · 世代 ' + esc(epoch) + '</p></div>' +
    pill(activePackage?.e11 ? 'E11 已生效' : 'E11 未启用', activePackage?.e11 ? 'good' : '') + '</div>' +
    '<div class="controls"><label>已批准配方<select id="recipe">' + recipeOptions + '</select></label>' +
    action(view, context.online, 'start', chosen ?? '') + action(view, context.online, 'pause') +
    action(view, context.online, 'drain') + '</div>' +
    (chosen && chosen !== active?.package ? '<p class="warning">所选配方需要先经发布流程实际生效。</p>' : '') +
    (line.fence ? '<p class="warning">产线处于配置隔离。关联操作 ' + esc(line.fence) + '</p>' : '') +
    '</section><section class="panel"><div class="panel-title"><h2>操作记录</h2></div>' +
    operationTable(view.operations) + '</section>';
}
