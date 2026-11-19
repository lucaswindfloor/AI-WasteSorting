import { action, esc, operationTable, pageTitle, pill, stamp, table } from '../../ui.mjs';

export function tasksAndBins(view, context) {
  const tasks = view.tasks.map((task) => [
    esc(task.target), esc(task.planned), esc(task.execution) + '<div>' + stamp(task.executionStamp) + '</div>',
    esc(task.carrier), esc(task.robot),
    (task.actual ? esc(task.actual) : pill('待核', 'alert')) + '<div>' + stamp(task.destinationStamp) + '</div>',
  ]);
  const bins = view.bins.map((bin) => {
    const work = view.operations.find((op) => op.id === bin.fence);
    const label = !bin.stamp.fresh ? '状态陈旧' : bin.fence ?
      (work?.phase === 'canChange' ? '确认可换箱' : bin.clear ? '换箱隔离' : '等待在途任务') :
      !bin.present ? '未在位' : bin.full ? '已满' : bin.usable ? '可用' : '不可用';
    return [esc(bin.slot), esc(bin.physical), pill(label, label === '可用' ? 'good' : 'alert'),
      esc(bin.mapping), stamp(bin.stamp),
      '<div class="inline-field"><input aria-label="' + esc(bin.slot) + ' 新箱标识" type="text" maxlength="48" ' +
      'data-new-bin="' + esc(bin.slot) + '" placeholder="输入新箱标识" value="' +
      esc(context.newBins.get(bin.slot) ?? '') + '">' +
      action(view, context.online, 'changeBin', bin.slot) + '</div>'];
  });
  return pageTitle('任务与箱位', '核对任务计划、机器人状态和独立计量归宿。') +
    '<section class="panel"><div class="panel-title"><h2>任务快照</h2></div>' +
    table(['目标', '计划箱', '末端反馈', '载体', '机器人', '独立归宿'], tasks) + '</section>' +
    '<section class="panel"><div class="panel-title"><h2>箱位与换箱</h2></div>' +
    table(['箱位', '实物标识', '当前状态', '映射世代', '现场来源', '操作'], bins) +
    '<p class="subtle">模拟工位会在显示可换箱后，自动生成新箱到位反馈。</p></section>' +
    '<section class="panel"><div class="panel-title"><h2>换箱记录</h2></div>' +
    operationTable(view.operations.filter((op) => op.action === 'changeBin')) + '</section>';
}
