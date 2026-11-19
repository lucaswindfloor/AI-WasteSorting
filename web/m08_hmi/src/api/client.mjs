export class ApiError extends Error {
  constructor(issue) {
    super(issue);
    this.issue = issue;
  }
}

async function request(path, body) {
  const response = await fetch(path, {
    cache: 'no-store',
    signal: AbortSignal.timeout(5000),
    ...(body && { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }),
  });
  const result = await response.json();
  if (!response.ok || result.issue !== 'none') throw new ApiError(result.issue ?? 'notReady');
  return result.data;
}

const pathId = encodeURIComponent;

export const api = {
  workbench: () => request('/api/workbench'),
  changeRole: (profile) => request('/api/session', { profile }),
  submit: (operation) => request('/api/operations', operation),
  operation: (id) => request('/api/operations/' + pathId(id)),
  evidence: (manifest, cursor = '') => request('/api/evidence?' +
    new URLSearchParams({ manifest, cursor, limit: '1' })),
  export: (job) => request('/api/exports', job),
  job: (id) => request('/api/exports/' + pathId(id)),
  download: (id) => '/api/exports/' + pathId(id) + '/download',
};
