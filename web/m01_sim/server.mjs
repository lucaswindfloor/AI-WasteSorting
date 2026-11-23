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
const runFile = /^\/runs\/([0-9a-f-]{36})\/(report\.json|ledger\.csv)$/u;
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
  const { scenario, speed, depth, profile, packetLines, inference = 'joint', masks = 'strict' } = options;
  if (!scenarios.has(scenario) || !Number.isInteger(speed) || speed < 500 || speed > 3000 ||
      !['independent', 'encoder'].includes(depth) || !devices.has(profile) ||
      !Number.isInteger(packetLines) || packetLines < 1 || packetLines > 8 ||
      !['off', 'rgb', 'hsi', 'split', 'joint'].includes(inference) || !['strict', 'allow'].includes(masks)) {
    return json(res, 400, { error: '请检查速度、场景或传感器参数。' });
  }
  if (busy) return json(res, 409, { error: '另一轮仿真正在归档，请稍候。' });
  busy = true;
  const id = randomUUID();
  const [rgb, hsi, bands] = devices.get(profile);
  const path = join(output, id);
  try {
    await mkdir(output, { recursive: true });
    try {
      await execute(binary, ['--output', path, '--scenario', scenario, '--speed-mm-s', String(speed),
        '--depth', depth, '--rgb-width', rgb, '--hsi-width', hsi, '--hsi-bands', bands,
        '--packet-lines', String(packetLines), '--m03', inference, '--m03-mask', masks],
      { timeout: 60000, maxBuffer: 128 * 1024 });
    } catch (error) {
      // Exit 1 includes a completed, failed validation report.
      if (error.code !== 1) throw error;
    }
    const report = JSON.parse(await readFile(join(path, 'report.json'), 'utf8'));
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
    return send(res, 200, body, file.endsWith('.csv') ? 'text/csv; charset=utf-8' : 'application/json',
      { 'Content-Disposition': 'attachment; filename="' + file + '"' });
  } catch (error) {
    if (!res.headersSent) json(res, error.code === 'ENOENT' ? 404 : 500, { error: '无法读取文件。' });
    else res.end();
  }
});

server.listen(port, '127.0.0.1', () => {
  console.log('M01 simulation http://127.0.0.1:' + server.address().port);
});
