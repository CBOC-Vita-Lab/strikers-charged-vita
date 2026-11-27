// gxnet14 (Galaxy secreview32 F1, ported): no request shape may reach the Durable Object without a ceiling. From ONE address without an account:
//   a) /v1/admin/* with wrong bearers          -> every request reached the object (the Worker never consulted a verdict for admin routes)
//   b) a player route with a too-old X-Msc-App  -> 426 from the object before any per-address ceiling
//   c) a player route with X-Msc-Proto: 0       -> the same
//   d) a player route of a switched-off feature -> 503 from the object before any per-address ceiling
// The real src/worker.js runs in node in front of the real application (a fake object namespace counts the requests that wake it).
import { describe, it, expect, vi } from 'vitest';
import { makeServer, signUp } from '../helpers.js';
import { versionGate, loadConfig } from '../../src/core/config.js';

async function setup(over = {}) {
  vi.resetModules();
  const srv = makeServer({ DAILY_REQUEST_BUDGET: '45000', ADDR_BAD_DAY: '600', ...over });
  const calls = { n: 0 };
  const env = { ...srv.env, RATE_LIMIT_SCALE: '1000', GAME: { idFromName: () => 'main', get: () => ({ fetch: (req) => { calls.n++; return srv.app.handle(req); } }) } };
  const worker = (await import('../../src/worker.js')).default;
  const go = async (method, path, { token, ip = '203.0.113.66', headers = {} } = {}) => {
    const r = await worker.fetch(new Request(`https://svc.invalid${path}`, { method, headers: { 'cf-connecting-ip': ip, ...(token ? { authorization: `Bearer ${token}` } : {}), ...headers } }), env);
    const text = await r.text();
    let data; try { data = JSON.parse(text); } catch { data = text; }
    return { status: r.status, data, headers: r.headers };
  };
  return { srv, calls, go };
}
const tok = (i) => `x${String(i).padStart(42, '0')}`;
const ADMIN = 'test-admin-token-0123456789';

describe('gxnet14: every path to the object has a ceiling', () => {
  it('a) wrong admin bearers from one address: after the first few the Worker answers itself; the owner (another address) is unaffected', async () => {
    const { srv, calls, go } = await setup();
    const by = {};
    for (let i = 0; i < 3000; i++) { const r = await go('GET', '/v1/admin/stats', { token: tok(i) }); by[r.status] = (by[r.status] || 0) + 1; }
    expect(by[200]).toBeUndefined();
    expect(calls.n).toBeLessThan(10); // 5 wrong bearers, then the object's 429 teaches the Worker; before the fix: 3000
    const before = calls.n;
    const blocked = await go('GET', '/v1/admin/stats', { token: ADMIN }); // even the right token, from the blocked address, waits
    expect(blocked.status).toBe(429);
    expect(calls.n).toBe(before);
    expect(Number(blocked.headers.get('retry-after'))).toBeGreaterThan(0);
    expect((await go('GET', '/v1/admin/stats', { token: ADMIN, ip: '198.51.100.20' })).status).toBe(200);
    // an honest player elsewhere is not affected
    const a = await signUp(srv, 'Honest1');
    expect((await go('GET', '/v1/me', { token: a.token, ip: '198.51.100.7' })).status).toBe(200);
  });

  it('a2) the verdict ends: after its time the blocked address is asked again', async () => {
    const { srv, calls, go } = await setup();
    for (let i = 0; i < 20; i++) await go('GET', '/v1/admin/stats', { token: tok(i) });
    expect((await go('GET', '/v1/admin/stats', { token: ADMIN })).status).toBe(429);
    srv.advance(11 * 60_000);
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(Date.now() + 11 * 60_000);
    const r = await go('GET', '/v1/admin/stats', { token: ADMIN });
    vi.useRealTimers();
    expect(r.status).toBe(200);
  });

  it('a3) an honest admin run (a backup paging 25 requests) is not slowed', async () => {
    const { go } = await setup();
    for (let i = 0; i < 25; i++) expect((await go('GET', '/v1/admin/export', { token: ADMIN, ip: '198.51.100.20' })).status).toBe(200);
  });

  it('b) a too-old X-Msc-App: 426 from the Worker, the object is never woken', async () => {
    const { calls, go } = await setup({ MIN_APP_VERSION: '1.3.0' });
    let n426 = 0;
    for (let i = 0; i < 1000; i++) if ((await go('GET', '/v1/me', { token: tok(i), headers: { 'x-msc-app': '1.0.0' } })).status === 426) n426++;
    expect(n426).toBe(1000);
    expect(calls.n).toBe(0);
    const one = await go('GET', '/v1/me', { token: tok(1), ip: '198.51.100.9', headers: { 'x-msc-app': '1.0.0' } });
    expect(one.data.error).toMatchObject({ code: 'update_required', minVersion: '1.3.0' });
    // a current client passes the gate (and reaches the object)
    expect((await go('GET', '/v1/time', { headers: { 'x-msc-app': '1.3.0' } })).status).toBe(200);
    expect(calls.n).toBe(1);
  });

  it('c) X-Msc-Proto below the minimum: 426 from the Worker, the object is never woken (open routes too)', async () => {
    const { calls, go } = await setup({ MIN_CLIENT_PROTO: '2' });
    for (let i = 0; i < 500; i++) expect((await go('GET', '/v1/me', { token: tok(i), headers: { 'x-msc-proto': '1' } })).status).toBe(426);
    const info = await go('GET', '/v1/info', { headers: { 'x-msc-proto': '1' } });
    expect(info.status).toBe(426);
    expect(info.data.error).toMatchObject({ code: 'upgrade_required', minProto: 2 });
    expect(calls.n).toBe(0);
    expect((await go('GET', '/v1/info', { headers: { 'x-msc-proto': '2' } })).status).toBe(200);
  });

  it('d) a switched-off feature: refusals count against the address like failed sign-ins and stop at ADDR_BAD_DAY', async () => {
    const { srv, calls, go } = await setup({ KILL_SWITCH: 'friends', ADDR_BAD_DAY: '100' });
    const by = {};
    for (let i = 0; i < 400; i++) { const r = await go('GET', '/v1/friends', { token: tok(i) }); by[r.status] = (by[r.status] || 0) + 1; }
    expect(by[503]).toBe(100);
    expect(by[429]).toBe(300);
    expect(calls.n).toBeLessThan(110); // 100 refusals + the one that learns the verdict; before the fix: 400
    const a = await signUp(srv, 'Honest2');
    const fr = await go('GET', '/v1/friends', { token: a.token, ip: '198.51.100.7' });
    expect(fr.status).toBe(503);
    expect(fr.data.error.code).toBe('service_disabled');
    expect((await go('GET', '/v1/me', { token: a.token, ip: '198.51.100.7' })).status).toBe(200);
  });

  it('the app keeps its own version gate (defence in depth): same codes as the Worker', async () => {
    const { srv } = await setup({ MIN_APP_VERSION: '1.3.0', MIN_CLIENT_PROTO: '1' });
    const direct = (headers) => srv.app.handle(new Request('https://x.invalid/v1/me', { headers: { 'cf-connecting-ip': '198.51.100.30', authorization: `Bearer ${tok(7)}`, ...headers } }));
    const a = await direct({ 'x-msc-app': '1.0.0' });
    expect(a.status).toBe(426);
    expect((await a.json()).error.code).toBe('update_required');
  });

  it('versionGate (the Worker) reads the same variables as loadConfig (the object)', () => {
    for (const env of [{}, { MIN_APP_VERSION: '1.3.0' }, { MIN_CLIENT_PROTO: '3', MIN_APP_VERSION: '2.0.0' }, { MIN_CLIENT_PROTO: 'junk' }]) {
      const g = versionGate(env), c = loadConfig(env);
      expect(g).toEqual({ minProto: c.minProto, minApp: c.minApp });
    }
  });
});
