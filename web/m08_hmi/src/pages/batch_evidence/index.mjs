import { api } from '../../api/client.mjs';
import { esc, pageTitle, pill, table } from '../../ui.mjs';

export function batchEvidence(view, context) {
  const heading = pageTitle('批次证据', '按归档清单查看计划箱与独立计量确认的实际箱。');
  if (!view.canReadEvidence) {
    return heading + '<section class="panel"><p>当前角色无权查看该批次的归档证据。</p></section>';
  }
  const page = context.evidencePage;
  const job = context.job;
  return heading + '<section class="panel"><div class="panel-title"><h2>证据清单</h2>' +
    pill(page?.manifest ?? context.manifest) + '</div>' +
    table(['目标', '计划箱', '实际箱', '独立证据'], (page?.rows ?? []).map((row) => [
      esc(row.target), esc(row.planned), esc(row.actual || '待核'), esc(row.evidence),
    ])) +
    (page?.gaps.length ? '<p class="warning">' + esc(page.gaps.join('；')) + '</p>' : '') +
    '<div class="controls"><button type="button" data-local="evidence-first">首页</button>' +
    '<button type="button" data-local="evidence-next"' + (page?.next ? '' : ' disabled') + '>下一页</button></div>' +
    '</section><section class="panel"><div class="panel-title"><h2>证据导出</h2></div>' +
    (view.canExport ? '<form id="export" class="controls"><label>导出用途<input name="purpose" required maxlength="40" ' +
      'placeholder="例如批次复核" value="' + esc(context.purpose) + '"></label><button type="submit"' +
      (context.online ? '' : ' disabled') + '>导出 T041–T042</button></form>' :
      '<p class="subtle">当前角色可以查看清单。导出需要计量审计权限。</p>') +
    (job ? '<div class="job">' + pill(job.stage === 'ready' ? '可下载' : job.stage === 'queued' ? '排队中' : '失败',
      job.stage === 'ready' ? 'good' : 'alert') + ' <span class="subtle">' + esc(job.range || job.id) + '</span>' +
      (job.checksum ? '<p class="subtle wrap">' + esc(job.checksum) + '</p>' : '') +
      (job.stage === 'ready' ? '<a href="' + esc(api.download(job.id)) + '">下载 CSV</a>' : '') + '</div>' : '') +
    '</section>';
}
