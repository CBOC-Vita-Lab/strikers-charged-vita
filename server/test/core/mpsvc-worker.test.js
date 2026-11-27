// The Worker in front of the object (src/worker.js) in node: a fake object namespace runs the real application. What the Worker learns from
// the object's answers (budget level, blocks) must stop later requests there, with no object request; the memo looks at the object once a minute.
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { makeServer, signUp } from '../helpers.js';

async function setup(envOverrides = {}) {
  vi.resetModules();
  const srv = makeServer(envOverrides);
  const calls = { n: 0, paths: [] };
  const env = {
    ...srv.env,
    RATE_LIMIT_SCALE: envOverrides.EDGE_SCALE || '1000',
    GAME: {
      idFromName: () => 'main',
      get: () => ({ fetch: (req) => { calls.n++; calls.paths.push(new URL(req.url).pathname); return srv.app.handle(req); } }),
    },
  };
  const worker = (await import('../../src/worker.js')).default;
  const go = async (method, path, body, token, ip = '203.0.113.7') => {
    const init = { method, headers: { 'cf-connecting-ip': ip, ...(token ? { authorization: `Bearer ${token}` } : {}), ...(body ? { 'content-type': 'application/json' } : {}) } };
    if (body !== undefined) init.body = JSON.stringify(body);
    const r = await worker.fetch(new Request(`https://svc.invalid${path}`, init), env);
    const text = await r.text();
    let data; try { data = JSON.parse(text); } catch { data = text; }
    return { status: r.status, data, headers: r.headers };
  };
  return { srv, calls, go, env };
}

describe('Worker gate', () => {
  it('once the object says the budget is over for new sessions, the Worker refuses them itself: no more object requests', async () => {
    const { srv, calls, go } = await setup({ DAILY_REQUEST_BUDGET: '50', BUDGET_BROWSE_PCT: '60' });
    const a = await signUp(srv, 'Alice1');
    for (let i = 0; i < 45; i++) await go('GET', '/v1/time', undefined, undefined, `203.0.113.${1 + (i % 100)}`);
    // the first refused request reaches the object, which answers with the header; the Worker remembers
    const first = await go('POST', '/v1/rooms', undefined, a.token);
    expect(first.status).toBe(503);
    expect(first.data.error.code).toBe('over_budget');
    const before = calls.n;
    for (let i = 0; i < 20; i++) {
      const r = await go('POST', '/v1/rooms', undefined, a.token);
      expect(r.status).toBe(503);
      expect(r.headers.get('retry-after')).toBeTruthy();
    }
    expect(calls.n).toBe(before); // 20 refusals, no object request
    // matches in progress still reach the object
    const live = await go('GET', '/v1/ranked/queue', undefined, a.token);
    expect(live.status).toBe(200);
    expect(calls.n).toBe(before + 1);
  });

  it('an address over its daily ceiling is answered by the Worker until midnight; others are not affected', async () => {
    const { calls, go } = await setup({ ADDR_OPEN_DAY: '5' });
    for (let i = 0; i < 5; i++) expect((await go('GET', '/v1/time', undefined, undefined, '203.0.113.80')).status).toBe(200);
    expect((await go('GET', '/v1/time', undefined, undefined, '203.0.113.80')).status).toBe(429); // from the object, with x-msc-block
    const n = calls.n;
    for (let i = 0; i < 10; i++) {
      const r = await go('GET', '/v1/time', undefined, undefined, '203.0.113.80');
      expect(r.status).toBe(429);
      expect(r.data.error.reason).toBe('daily_cap');
    }
    expect(calls.n).toBe(n);
    expect((await go('GET', '/v1/time', undefined, undefined, '203.0.113.81')).status).toBe(200);
  });

  it('an account over its daily ceiling is answered by the Worker; other accounts go through', async () => {
    const { srv, calls, go } = await setup({ ACCOUNT_DAY_REQUESTS: '10', ADDR_DAY_REQUESTS: '0' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    for (let i = 0; i < 10; i++) expect((await go('GET', '/v1/me', undefined, a.token)).status).toBe(200);
    expect((await go('GET', '/v1/me', undefined, a.token)).status).toBe(429);
    const n = calls.n;
    for (let i = 0; i < 5; i++) expect((await go('GET', '/v1/me', undefined, a.token, '198.51.100.9')).status).toBe(429);
    expect(calls.n).toBe(n);
    expect((await go('GET', '/v1/me', undefined, b.token)).status).toBe(200);
  });

  it('the edge buckets are tighter than 1.3: a flood from one address is stopped at 100 requests, no object request after that', async () => {
    const { calls, go } = await setup({ ADDR_OPEN_DAY: '0', EDGE_SCALE: '1' });
    let limited = 0;
    for (let i = 0; i < 160; i++) if ((await go('GET', '/v1/info', undefined, undefined, '203.0.113.99')).status === 429) limited++;
    expect(limited).toBeGreaterThan(40);
    expect(calls.n).toBeLessThanOrEqual(110);
  });
});

describe('public board memo recheck', () => {
  it('a busy page costs the object about one info look a minute, not twelve', async () => {
    const { calls, go } = await setup({});
    const realNow = Date.now;
    let t = realNow();
    Date.now = () => t;
    try {
      for (let i = 0; i < 600; i++) {
        await go('GET', '/v1/public/leaderboard?board=season', undefined, undefined, `203.0.113.${1 + (i % 200)}`);
        t += 1000;
      }
    } finally { Date.now = realNow; }
    const infos = calls.paths.filter((p) => p === '/v1/info').length;
    const boards = calls.paths.filter((p) => p === '/v1/public/leaderboard').length;
    expect(infos).toBeLessThanOrEqual(11);
    expect(boards).toBeLessThanOrEqual(11);
    expect(calls.n).toBeLessThanOrEqual(22);
  });
});
