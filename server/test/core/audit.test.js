// Route-by-route audit (ported from the Galaxy review, adapted to this route table): every route has a rate limit and, where it is a feature,
// a kill switch; junk bodies, path segments and queries never produce a 5xx or a change of state; every limit really answers 429 + Retry-After.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp } from '../helpers.js';

const ADMIN = 'test-admin-token-0123456789';
const BODIES = ['{', '[]', 'null', '7', '"x"', 'true', '{"__proto__":{"x":1}}', '{"constructor":1}', '{"code":{"a":1}}', '{"code":["A"]}', '{"code":null}',
  '{"x":' + '['.repeat(200) + ']'.repeat(200) + '}', '{"pid":1.5}', '{"pid":"1"}', '{"table":"profiles","rows":[{"__proto__":1}]}', '{"kill":["nope"]}',
  '{"kill":"rooms"}', '{"name":"\\u0000"}', '{"candidates":[1]}', '{"result":"x","score0":-1,"score1":1e999}', '{"category":0,"region":2,"score":"13","data":1}',
  JSON.stringify({ code: 'A'.repeat(5000) })];
const SEGMENTS = ['..', '%00', '%2e%2e', 'A'.repeat(64), 'ZZZZ-ZZZZ', 'é', 'null', '0', '-1', '1e999', 'constructor', '__proto__', "'%20OR%201=1"];
const QUERIES = ['', '?v=abc', '?v=-1', '?v=1e999', '?v=', '?limit=-1', '?limit=999999', '?limit=1e3', '?board=nope', '?board=archive&season=1', '?a=1&a=2', '?board=season&board=daily'];
const fill = (pattern, seg) => pattern.replace(/:\w+/g, seg);
const skipOne = (method, pattern, o) => o.register || (pattern === '/v1/account' || (method === 'DELETE' && pattern === '/v1/me'));

describe('route audit', () => {
  it('has a rate limit (or is registration / admin, which have their own), and every feature route has a kill switch', () => {
    const s = makeServer();
    const KILLABLE = ['register', 'rooms', 'ranked', 'ranking', 'turn', 'friends', 'report'];
    for (const [method, pattern, o] of s.app.routeOpts) {
      const what = `${method} ${pattern}`;
      if (o.admin) { expect(o.rl, what).toBeUndefined(); continue; }
      if (o.register) continue;
      expect(Array.isArray(o.rl), `${what} needs rl`).toBe(true);
      expect(o.rl[1] > 0 && o.rl[2] > 0, what).toBe(true);
      expect(o.auth ? o.rl[3] === 'pid' : o.rl[3] === 'ip', `${what}: scope follows auth`).toBe(true);
    }
    // costly authenticated routes are switchable; reads of what you already have, leaving and result reports stay reachable
    for (const [method, pattern, o] of s.app.routeOpts) {
      if (!o.auth || method === 'GET' || method === 'DELETE') continue;
      if (/^\/v1\/(me|account|rooms\/:code|matches)/.test(pattern)) continue;
      expect(KILLABLE, `${method} ${pattern} kill`).toContain(o.kill);
    }
    expect(s.app.routeOpts.find((r) => r[1] === '/v1/public/leaderboard')[2]).toMatchObject({ kill: 'ranking', web: true });
  });

  it('junk bodies on every route are 4xx, never 5xx and never a change of state', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const b = await signUp(s, 'Ben');
    const before = JSON.stringify([a, b]);
    for (const [method, pattern, o] of s.app.routeOpts) {
      if (method === 'GET' || skipOne(method, pattern, o)) continue;
      const path = fill(pattern, 'ABCDEFGH');
      for (const body of BODIES) {
        const r = await s.call(method, path, body, o.admin ? ADMIN : o.auth ? a.token : undefined);
        expect(r.status, `${method} ${path} ${body.slice(0, 40)}`).toBeLessThan(500);
        if (!o.admin) expect(r.status, `${method} ${path} ${body.slice(0, 40)}`).toBeGreaterThanOrEqual(400);
      }
    }
    expect(JSON.stringify([a, b])).toBe(before);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).data.name).toBe('Ana');
  });

  it('junk path segments and queries on every route are never 5xx', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    for (const [method, pattern, o] of s.app.routeOpts) {
      if (skipOne(method, pattern, o)) continue;
      for (const seg of pattern.includes(':') ? SEGMENTS : ['x']) {
        const path = fill(pattern, seg.startsWith('%') ? seg : encodeURIComponent(seg));
        for (const q of method === 'GET' ? QUERIES : ['']) {
          const r = await s.call(method, path + q, undefined, o.admin ? ADMIN : o.auth ? a.token : undefined);
          expect(r.status, `${method} ${path}${q}`).toBeLessThan(500);
        }
      }
    }
  });

  it('every rate limit really answers 429 with Retry-After once its bucket is empty', async () => {
    for (const [method, pattern, o] of makeServer().app.routeOpts) {
      if (o.admin || o.register || !o.rl || (pattern === '/v1/account' || (method === 'DELETE' && pattern === '/v1/me'))) continue;
      const s = makeServer({ RATE_LIMIT_SCALE: '1' });
      const a = await signUp(s, 'Ana');
      const path = fill(pattern, 'ABCDEFGH');
      let hit = null;
      for (let i = 0; i < o.rl[1] + 1 && !hit; i++) {
        const r = await s.call(method, path, method === 'GET' ? undefined : '{}', o.auth ? a.token : undefined, { ip: '203.0.113.7' });
        if (r.status === 429) hit = r;
      }
      expect(hit, `${method} ${pattern} (${o.rl[1]} per ${o.rl[2]} s)`).not.toBeNull();
      expect(hit.headers.get('retry-after'), `${method} ${pattern}`).toMatch(/^\d+$/);
    }
  });

  it('every switchable route answers 503 service_disabled while its switch is on (and "all" stops them all)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    for (const sw of ['rooms', 'ranked', 'turn', 'ranking', 'report', 'all']) {
      expect((await s.call('POST', '/v1/admin/switches', { kill: [sw] }, ADMIN)).status).toBe(200);
      for (const [method, pattern, o] of s.app.routeOpts) {
        if (!o.auth || !o.kill || (sw !== 'all' && o.kill !== sw)) continue;
        const r = await s.call(method, fill(pattern, 'ABCDEFGH'), method === 'GET' ? undefined : '{}', a.token);
        expect(r.status, `${method} ${pattern} while ${sw} is off`).toBe(503);
        expect(r.data.error.code).toBe('service_disabled');
      }
      expect((await s.call('GET', '/v1/public/leaderboard?board=season')).status).toBe(sw === 'ranking' || sw === 'all' ? 503 : 200);
    }
    expect((await s.call('POST', '/v1/admin/switches', { kill: [] }, ADMIN)).status).toBe(200);
  });
});
