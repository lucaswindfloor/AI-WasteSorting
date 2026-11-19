import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, resolve, sep } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('.', import.meta.url));
const binary = process.env.M08_DEMO_BINARY ?? fileURLToPath(new URL('../../build/m08_hmi_demo', import.meta.url));
const port = Number(process.env.M08_DEMO_PORT ?? 8088);
const tick = Number(process.env.M08_DEMO_TICK_MS ?? 1000);
const child = spawn(binary, [], { stdio: ['pipe', 'pipe', 'inherit'] });
const waiting = [];
const profiles = new Set(['operator', 'engineer', 'reviewer', 'auditor', 'viewer']);
let childError;

function unavailable(error) {
  childError = error;
  for (const request of waiting.splice(0)) request.reject(error);
}

child.on('error', unavailable);
child.stdin.on('error', unavailable);
child.on('exit', () => unavailable(new Error('M08 demo bridge exited')));
createInterface({ input: child.stdout }).on('line', (line) => {
  const next = waiting.shift();
  if (!next) return;
  try {
    next.resolve(JSON.parse(line));
  } catch (error) {
    next.reject(error);
  }
});

function call(...args) {
  if (childError) return Promise.reject(childError);
  if (waiting.length >= 64) return Promise.reject(new Error('M08 demo bridge is busy'));
  for (const value of args) {
    if (typeof value !== 'string' || Buffer.byteLength(value) > 256 || /[\u0000-\u001f\u007f]/u.test(value)) {
      return Promise.resolve({ issue: 'invalid', data: null });
    }
  }
  return new Promise((resolve, reject) => {
    waiting.push({ resolve, reject });
    child.stdin.write(args.map((value) => JSON.stringify(value)).join(' ') + '\n');
  });
}

const timer = setInterval(() => {
  void call('tick').catch(() => {});
}, tick);

const types = new Map([['.html', 'text/html; charset=utf-8'], ['.mjs', 'text/javascript; charset=utf-8'],
  ['.css', 'text/css; charset=utf-8']]);
const statuses = { none: 200, unauthorized: 403, invalid: 400, staleView: 409, conflict: 409,
  conditions: 422, notFound: 404, notReady: 503, persistence: 503, capacity: 429 };
const headers = { 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' };

function json(res, result, status = statuses[result.issue] ?? 422, extra = {}) {
  res.writeHead(status, { ...headers, 'Content-Type': 'application/json; charset=utf-8', ...extra });
  res.end(JSON.stringify(result));
}

async function body(req) {
  if (req.headers['content-type']?.split(';')[0] !== 'application/json') throw new Error('Expected JSON');
  const chunks = [];
  let size = 0;
  for await (const chunk of req) {
    size += chunk.length;
    if (size > 4096) throw new Error('Request too large');
    chunks.push(chunk);
  }
  const data = JSON.parse(Buffer.concat(chunks, size).toString('utf8'));
  if (!data || typeof data !== 'object' || Array.isArray(data)) throw new Error('Expected object');
  return data;
}

function session(req) {
  const cookies = req.headers.cookie?.split(';') ?? [];
  const role = cookies.map((value) => value.trim()).find((value) => value.startsWith('m08_demo='))
    ?.slice('m08_demo='.length);
  return profiles.has(role) ? role : 'operator';
}

async function api(req, res, url) {
  const { pathname, searchParams } = url;
  const actor = session(req);
  if (req.method === 'POST') {
    if (req.headers.origin && req.headers.origin !== 'http://' + req.headers.host) {
      return json(res, { issue: 'unauthorized' });
    }
    let data;
    try {
      data = await body(req);
    } catch {
      return json(res, { issue: 'invalid' });
    }
    if (pathname === '/api/session' && profiles.has(data.profile)) {
      return json(res, { issue: 'none', data: { profile: data.profile } }, 200, {
        'Set-Cookie': 'm08_demo=' + data.profile + '; Path=/; HttpOnly; SameSite=Strict',
      });
    }
    if (pathname === '/api/operations') {
      return json(res, await call('submit', actor, data.id, data.action, data.object ?? '', data.value ?? '',
        data.revision, data.epoch));
    }
    if (pathname === '/api/exports') {
      return json(res, await call('export', actor, data.id, data.manifest, data.first, data.last,
        data.purpose, data.limit));
    }
    return json(res, { issue: 'invalid' });
  }
  if (req.method !== 'GET') return json(res, { issue: 'invalid' }, 405);
  if (pathname === '/api/workbench') return json(res, await call('view', actor));
  if (pathname === '/api/evidence') {
    return json(res, await call('page', actor, searchParams.get('manifest') ?? '',
      searchParams.get('cursor') ?? '', searchParams.get('limit') ?? '20'));
  }
  const operation = /^\/api\/operations\/([^/]+)$/.exec(pathname);
  if (operation) return json(res, await call('query', actor, decodeURIComponent(operation[1])));
  const job = /^\/api\/exports\/([^/]+)(\/download)?$/.exec(pathname);
  if (job) {
    const result = await call(job[2] ? 'download' : 'job', actor, decodeURIComponent(job[1]));
    if (!job[2] || result.issue !== 'none') return json(res, result);
    res.writeHead(200, { ...headers, 'Content-Type': 'text/csv; charset=utf-8',
      'Content-Disposition': 'attachment; filename="m08-evidence.csv"' });
    return res.end(result.data);
  }
  return json(res, { issue: 'notFound' });
}

const server = createServer(async (req, res) => {
  try {
    if (!/^((localhost|127\.0\.0\.1)(:\d+)?)$/u.test(req.headers.host ?? '')) {
      return json(res, { issue: 'unauthorized' });
    }
    if (!req.url?.startsWith('/') || req.url.startsWith('//')) return json(res, { issue: 'invalid' });
    const url = new URL(req.url, 'http://localhost');
    if (url.pathname.startsWith('/api/')) return await api(req, res, url);
    if (req.method !== 'GET') return json(res, { issue: 'invalid' }, 405);
    const pathname = decodeURIComponent(url.pathname);
    const file = resolve(root, pathname === '/' ? 'index.html' : '.' + pathname);
    const source = resolve(root, 'src') + sep;
    if (file !== resolve(root, 'index.html') && !file.startsWith(source)) {
      return json(res, { issue: 'notFound' });
    }
    const type = types.get(extname(file));
    if (!type) return json(res, { issue: 'notFound' });
    try {
      const data = await readFile(file);
      res.writeHead(200, { ...headers, 'Content-Type': type,
        'Content-Security-Policy': "default-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'" });
      res.end(data);
    } catch {
      return json(res, { issue: 'notFound' });
    }
  } catch (error) {
    if (!res.headersSent) json(res, { issue: error instanceof URIError ? 'invalid' : 'notReady' });
    else res.end();
  }
});

server.listen(port, '127.0.0.1', () => console.log('M08 demo http://127.0.0.1:' + server.address().port));
server.on('close', () => {
  clearInterval(timer);
  child.kill();
});
for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => server.close());
