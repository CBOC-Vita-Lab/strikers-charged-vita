// Galaxy's finding (13 Nov): "one valid account at one request a second reached the object 86,000 times a day unseen by the budget".
// A simulated UTC day (86,400 requests, one a second, ONE valid account, one address) through the real Worker and the real object, over a mix of
// routes that includes every kind of failure answer. Two things must hold: what reaches the object is counted by the budget (nothing unseen),
// and a single account cannot spend more than its daily ceiling plus a few probes.
import { describe, it, expect, vi, afterEach } from 'vitest';
import { makeServer, T0 } from '../helpers.js';

async function rig(envOverrides = {}) {
  vi.resetModules();
  const srv = makeServer({ REQUIRE_APP_VERSION: '1', RATE_LIMIT_SCALE: '1', ...envOverrides });
  const calls = { n: 0, by: new Map() };
  const env = {
    ...srv.env,
    GAME: { idFromName: () => 'main', get: () => ({ fetch: (req) => { calls.n++; const k = `${req.method} ${new URL(req.url).pathname}`; calls.by.set(k, (calls.by.get(k) || 0) + 1); return srv.app.handle(req); } }) },
  };
  const worker = (await import('../../src/worker.js')).default;
  const go = async (method, path, { body, token, ip = '203.0.113.9', app = '1.3.0', raw } = {}) => {
    const headers = { 'cf-connecting-ip': ip, 'x-msc-app': app, ...(token ? { authorization: `Bearer ${token}` } : {}) };
    const init = { method, headers };
    if (raw !== undefined) { init.body = raw; headers['content-type'] = 'application/json'; headers['content-length'] = String(raw.length); }
    else if (body !== undefined) { init.body = JSON.stringify(body); headers['content-type'] = 'application/json'; headers['content-length'] = String(init.body.length); }
    const r = await worker.fetch(new Request(`https://svc.invalid${path}`, init), env);
    await r.text();
    return r.status;
  };
  return { srv, calls, go, env };
}

afterEach(() => { vi.useRealTimers(); });

describe('one valid account, one request a second, for a day (Galaxy finding: class mismatch / verdict keys)', () => {
  it('every request that reaches the object is seen by the budget, and the account stops at its ceiling', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(T0);
    const { srv, calls, go } = await rig();
    const a = (await srv.call('POST', '/v1/register', { name: 'Steady1' }, undefined, { 'x-msc-app': '1.3.0' })).data;
    const base = calls.n;
    const day0 = Math.floor(T0 / 86400000) * 86400000;
    const mix = [
      () => go('GET', '/v1/me', { token: a.token }),
      () => go('GET', '/v1/ranked/queue', { token: a.token }),
      () => go('GET', '/v1/rooms/ABCDEF', { token: a.token }), // not a member: 404
      () => go('GET', '/v1/rooms/AB', { token: a.token }), // bad code
      () => go('POST', '/v1/rooms/join', { token: a.token, body: { code: 'ZZZZZZ' } }),
      () => go('POST', '/v1/rooms/join', { token: a.token, raw: '{not json' }),
      () => go('GET', '/v1/time'),
      () => go('GET', '/v1/info'),
      () => go('GET', '/v1/players/AAAAAAAA', { token: a.token }),
      () => go('POST', '/v1/rank/get', { token: a.token, body: { category: 'x' } }),
      () => go('GET', '/v1/matches/nope', { token: a.token }),
      () => go('GET', '/v1/me', { token: 'A'.repeat(43) }), // wrong token
      () => go('PUT', '/v1/me', { token: a.token, body: { name: 'x' } }), // bad name
      () => go('GET', '/v1/me', { token: a.token, app: 'garbage' }), // bad version header
      () => go('DELETE', '/v1/ranked/queue', { token: a.token }),
      () => go('GET', '/v1/public/leaderboard?board=season', {}),
      () => go('GET', '/leaderboard', {}),
      () => go('GET', '/v1/servers', {}),
    ];
    const status = new Map();
    let last = null;
    for (let i = 0; i < 86400; i++) {
      vi.setSystemTime(T0 + i * 1000);
      srv.clock.now = T0 + i * 1000;
      if (T0 + i * 1000 >= day0 + 86400000) break; // the simulated day ends at midnight UTC (T0 is noon: 43,200 s)
      last = srv.app.budget.summary(srv.clock.now);
      const s = await mix[i % mix.length]();
      status.set(s, (status.get(s) || 0) + 1);
    }
    const sum = last;
    const reached = calls.n - base;
        console.log('day test: sent', [...status.values()].reduce((x, y) => x + y, 0), 'reached the object', reached, 'budget used', sum.used, 'refused', sum.refused, 'ceilingRefused', sum.ceilingRefused, 'statuses', JSON.stringify([...status]));
    expect(Math.abs(reached - sum.used)).toBeLessThanOrEqual(2); // nothing reaches the object unseen by the budget (+ the sign-up)
    // one account, one address: 9,000 (account) + 1,000 (open routes) + 600 (failed authentication) at most, plus the probes that find each ceiling
    // and the Worker's own board refreshes (the public board memo asks the object once a minute: not the address's requests)
    const memo = calls.by.get('GET /v1/public/leaderboard') || 0;
    expect(memo).toBeLessThanOrEqual(300);
    expect(reached - memo).toBeLessThanOrEqual(9000 + 1000 + 600 + 10);
  }, 300000);
});

describe('the reserve (Galaxy: "rejoin allowed inside the reserve")', () => {
  it('a member rejoining their own room still works when new sessions are refused; a new guest does not', async () => {
    const { makeServer: mk, signUp: su, pairInRoom } = await import('../helpers.js');
    const srv = mk({ DAILY_REQUEST_BUDGET: '100', LIVE_RESERVE_PCT: '20' });
    const a = await su(srv, 'Host1');
    const b = await su(srv, 'Guest1');
    const c3 = await su(srv, 'Late1');
    const { code } = await pairInRoom(srv, a, b);
    srv.app.budget.state.used = 85; // 85 %: new sessions are refused, matches in progress are not
    expect((await srv.call('POST', '/v1/rooms/join', { code }, b.token)).status).toBe(200); // the guest's link dropped and comes back: idempotent
    const late = await srv.call('POST', '/v1/rooms', undefined, c3.token);
    expect(late.status).toBe(503);
    const open = await srv.call('POST', '/v1/rooms', undefined, a.token).catch(() => null);
    srv.app.budget.state.used = 50;
    const room2 = await srv.call('POST', '/v1/rooms', undefined, c3.token);
    expect(room2.status).toBe(200);
    srv.app.budget.state.used = 85;
    const g = await srv.call('POST', '/v1/rooms/join', { code: room2.data.code }, a.token);
    expect(g.status).toBe(503);
    expect(g.data.error.code).toBe('over_budget');
    void open;
  });
});
