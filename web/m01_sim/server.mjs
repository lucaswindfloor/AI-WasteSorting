import { execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { mkdir, readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const here = fileURLToPath(new URL('.', import.meta.url));
const repo = fileURLToPath(new URL('../../', import.meta.url));
const binary = resolve(process.env.M01_SIM_BINARY || join(repo, 'build/m01_sim'));
const output = resolve(process.env.M01_SIM_RUNS || join(repo, 'build/m01-sim-runs'));
const port = Number(process.env.M01_SIM_PORT || '8081');
const runFile = /^\/runs\/([0-9a-f-]{36})\/(report\.json|ledger\.csv|bcaf\/(?:bcaf-report\.json|\d{4}-\d{4}-(?:overlay|mask)\.png))$/u;
const bcafRepo = process.env.M01_SIM_BCAF_REPO;
const bcafCheckpoint = process.env.M01_SIM_BCAF_CHECKPOINT;
const bcafStats = process.env.M01_SIM_BCAF_HSI_STATS;
const scenarios = new Set(['normal', 'missing', 'late', 'mixed', 'slow-archive', 'reconnect',
  'transport-delay', 'exposure-delay', 'compensated', 'exposure-limit', 'clock-missing',
  'count-interpolation', 'invalid-white', 'inference-overload', 'model-switch']);
const devices = new Map([
  ['demo', ['96', '80', '16']],
  ['wide', ['256', '128', '32']],
]);
const assets = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/styles.css', ['styles.css', 'text/css; charset=utf-8']],
  ['/app.mjs', ['app.mjs', 'text/javascript; charset=utf-8']],
  ['/drawing.mjs', ['drawing.mjs', 'text/javascript; charset=utf-8']],
  ['/calibration.mjs', ['calibration.mjs', 'text/javascript; charset=utf-8']],
  ['/inference.mjs', ['inference.mjs', 'text/javascript; charset=utf-8']],
  ['/assets/materials.png', ['assets/materials.png', 'image/png']],
]);
const execute = promisify(execFile);
const security = {
  'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff',
  'Content-Security-Policy': "default-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
};
let busy = false;

function send(res, code, body, type, extra = {}) {
  res.writeHead(code, { ...security, 'Content-Type': type, ...extra });
  res.end(body);
}

function json(res, code, data) {
  send(res, code, JSON.stringify(data), 'application/json; charset=utf-8');
}

async function requestBody(req) {
  if (req.headers['content-type']?.split(';')[0] !== 'application/json') throw new Error('Invalid body');
  const declared = Number(req.headers['content-length'] ?? 0);
  if (declared > 4096) throw new Error('Request body too large');
  const chunks = [];
  let bytes = 0;
  // Stop accepting an oversized body without buffering its remainder.
  for await (const chunk of req.iterator({ destroyOnReturn: false })) {
    bytes += chunk.length;
    if (bytes > 4096) {
      req.resume();
      throw new Error('Request body too large');
    }
    chunks.push(chunk);
  }
  const value = JSON.parse(Buffer.concat(chunks, bytes).toString('utf8'));
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid body');
  return value;
}

async function simulate(req, res) {
  if (req.headers.origin && req.headers.origin !== 'http://' + req.headers.host) {
    return json(res, 403, { error: '只接受本地页面的仿真请求。' });
  }
  let options;
  try {
    options = await requestBody(req);
  } catch {
    return json(res, 400, { error: '请求格式无效。' });
  }
  const { layout = 'demo', scenario, speed, depth, profile, packetLines,
    inference = layout === 'production' ? 'rgb' : 'joint', masks = 'strict' } = options;
  const production = layout === 'production';
  const bcaf = inference === 'bcaf';
  const validLayout = production ?
    ['normal', 'rgb-frame-loss'].includes(scenario) && ['off', 'rgb', 'bcaf'].includes(inference) :
    layout === 'demo' && scenarios.has(scenario) && devices.has(profile) &&
      Number.isInteger(packetLines) && packetLines >= 1 && packetLines <= 8 &&
      ['off', 'rgb', 'hsi', 'split', 'joint'].includes(inference);
  if (!validLayout || !Number.isInteger(speed) || speed < (production ? 1000 : 500) || speed > 3000 ||
      !['independent', 'encoder'].includes(depth) || !['strict', 'allow'].includes(masks) || (bcaf && masks !== 'strict')) {
    return json(res, 400, { error: '请检查速度、场景或传感器参数。' });
  }
  if (bcaf && (!bcafRepo || !bcafCheckpoint)) {
    return json(res, 503, { error: '请按仿真 README 配置 BCAF 仓库和 512×512 权重，再重启服务。' });
  }
  if (busy) return json(res, 409, { error: '另一轮仿真正在归档，请稍候。' });
  busy = true;
  const id = randomUUID();
  const [rgb, hsi, bands] = devices.get(profile) ?? [];
  const deviceArgs = production ? [] :
    ['--rgb-width', rgb, '--hsi-width', hsi, '--hsi-bands', bands, '--packet-lines', String(packetLines)];
  const path = join(output, id);
  try {
    await mkdir(output, { recursive: true });
    try {
      await execute(binary, ['--output', path, '--layout', layout, '--scenario', scenario, '--speed-mm-s', String(speed),
        '--depth', depth, ...deviceArgs, '--m03', bcaf ? 'rgb' : inference, '--m03-mask', masks,
        ...(bcaf ? ['--export-bcaf'] : [])],
      { timeout: 120000, maxBuffer: 128 * 1024 });
    } catch (error) {
      // Exit 1 includes a completed, failed validation report.
      if (error.code !== 1) throw error;
    }
    const report = JSON.parse(await readFile(join(path, 'report.json'), 'utf8'));
    if (bcaf && report.summary.passed) {
      const predictions = join(path, 'bcaf');
      try {
        await execute(process.env.M01_SIM_BCAF_PYTHON || 'python3', [
          join(repo, 'sim/m01/bcaf_infer.py'), '--run', path, '--output', predictions,
          '--bcaf-repo', resolve(bcafRepo), '--checkpoint', resolve(bcafCheckpoint),
          '--device', process.env.M01_SIM_BCAF_DEVICE || 'cpu',
          ...(bcafStats ? ['--hsi-stats', resolve(bcafStats)] : ['--sim-reflectance']),
        ], { timeout: 240000, maxBuffer: 128 * 1024 });
        const segmentation = JSON.parse(await readFile(join(predictions, 'bcaf-report.json'), 'utf8'));
        return json(res, 200, { id, report, bcaf: segmentation });
      } catch (error) {
        console.error('BCAF inference error:', error.stderr || error.message);
        return json(res, 200, { id, report, bcafError: 'BCAF 分割未完成。请检查服务终端的权重、依赖或运行错误。' });
      }
    }
    return json(res, 200, { id, report });
  } catch (error) {
    console.error('M01 simulation error:', error.message);
    return json(res, 503, { error: '仿真程序无法完成。请确认已运行 make m01-sim，并检查终端输出。' });
  } finally {
    busy = false;
  }
}

const server = createServer(async (req, res) => {
  try {
    if (!/^(localhost|127\.0\.0\.1)(:\d+)?$/u.test(req.headers.host || '')) {
      return json(res, 403, { error: '请从本机地址打开仿真台。' });
    }
    if (req.method === 'POST' && req.url === '/api/run') return await simulate(req, res);
    if (req.method !== 'GET') return json(res, 405, { error: '不支持此请求。' });
    const asset = assets.get(req.url);
    if (asset) return send(res, 200, await readFile(join(here, asset[0])), asset[1]);
    const match = runFile.exec(req.url || '');
    if (!match) return json(res, 404, { error: '文件不存在。' });
    const [, id, file] = match;
    if (!/^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/u.test(id)) {
      return json(res, 404, { error: '运行编号无效。' });
    }
    const body = await readFile(join(output, id, file));
    if (file.endsWith('.png')) return send(res, 200, body, 'image/png');
    return send(res, 200, body, file.endsWith('.csv') ? 'text/csv; charset=utf-8' : 'application/json',
      { 'Content-Disposition': 'attachment; filename="' + file.split('/').at(-1) + '"' });
  } catch (error) {
    if (!res.headersSent) json(res, error.code === 'ENOENT' ? 404 : 500, { error: '无法读取文件。' });
    else res.end();
  }
});

server.listen(port, '127.0.0.1', () => {
  console.log('M01 simulation http://127.0.0.1:' + server.address().port);
});
