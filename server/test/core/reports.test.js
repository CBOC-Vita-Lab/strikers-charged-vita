// Problem reports, anonymous session numbers and Delete My Online Account (core/reports.js, accounts.deleteAccount; public beta, 15 Nov 2026).
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, T0 } from '../helpers.js';
import { edgeCheck, ROUTE_META } from '../../src/core/edge.js';
import { createGate } from '../../src/core/worker_gate.js';
import { classify, refusedAt } from '../../src/core/budget.js';
import { createApp as oldApp } from '../fixtures/src-b1910c0/core/app.js';
import { HMAC_HEX } from '../helpers.js';
import { openNodeSql } from '../../src/sqlite-node.js';
import { parseStats, scrubText } from '../../src/core/reports.js';

// the limits are written out here on purpose: the tests must not follow a changed constant
const REPORT_MAX_BYTES = 49152; // 48 KiB
const REPORT_MAX_COUNT = 2000;
const REPORT_MAX_TOTAL_BYTES = 67108864; // 64 MiB
const REPORTS_PER_PLAYER_DAY = 3;
const SESSION_RING = 500;

const ADMIN = 'test-admin-token-0123456789';
const DAY = 86400000;
const rep = (extra = {}) => ({ v: 1, app: '1.3.0', build: 'abcdef0123456789', name: 'Ana', log: 'T+1.0s link lost: no packets', events: 'session start: online', conn: 'router: cone', session: { d: 312, dl: 2, rs: 0, dr: 1, sv: 0, e: 1 }, ...extra });
const q = (sql, ...b) => (s) => s.store.sql.exec(sql, ...b).toArray();
const count = (s, table, where = '1=1', ...b) => s.store.sql.exec(`SELECT COUNT(*) AS n FROM ${table} WHERE ${where}`, ...b).toArray()[0].n;

describe('POST /v1/report', () => {
  it('logs the size only: no client-written text reaches the log line (Workers Logs)', async () => {
    const lines = [];
    const s = makeServer({}, { deps: { log: (l) => lines.push(String(l)) } });
    const a = await signUp(s, 'Ana');
    lines.length = 0;
    const secret = 'PRIVATE-note-zz9';
    const r = await s.call('POST', '/v1/report', rep({ app: '1.3.0', build: 'abcdef01', name: 'Ana', note: secret, log: `${secret} log` }), a.token);
    expect(r.status).toBe(200);
    const rl = lines.filter((l) => l.includes('[report]'));
    expect(rl).toHaveLength(1);
    expect(rl[0]).toMatch(/^\[report\] stored: \d+ bytes$/);
    for (const l of lines) { expect(l).not.toContain(secret); expect(l).not.toContain('Ana'); expect(l).not.toContain('1.3.0'); expect(l).not.toContain('abcdef01'); }
  });

  it('accepts only a version in app and a hex hash in build (they are stored unscrubbed)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    for (const bad of [{ app: 'my name here' }, { app: '1.3.0 x' }, { app: '192.168.1.2' + 'x' }, { build: 'not-hex-at-all' }, { build: 'abc' }, { build: 'g'.repeat(16) }]) {
      const r = await s.call('POST', '/v1/report', rep(bad), a.token);
      expect(r.status).toBe(400);
    }
    expect(count(s, 'reports')).toBe(0);
    expect((await s.call('POST', '/v1/report', rep({ app: '1.3.0', build: 'abcdef01' }), a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/report', rep({ app: '', build: '' }), a.token)).status).toBe(200);
  });

  it('stores a report for a signed player and tells how many are left today', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const r = await s.call('POST', '/v1/report', rep(), a.token);
    expect(r.status).toBe(200);
    expect(r.data).toMatchObject({ ok: true, keptDays: 30, left: REPORTS_PER_PLAYER_DAY - 1 });
    expect(count(s, 'reports')).toBe(1);
    expect(count(s, 'report_daily', 'reports = 1')).toBe(1);
  });

  it('needs the player token (401), and the version header like every player call when the service requires it', async () => {
    const s = makeServer();
    const b = await signUp(s, 'Ben');
    expect((await s.call('POST', '/v1/report', rep())).status).toBe(401);
    expect((await s.call('POST', '/v1/report', rep(), 'x'.repeat(43))).status).toBe(401);
    expect(count(s, 'reports')).toBe(0);
    expect((await s.call('POST', '/v1/report', rep(), b.token)).status).toBe(200);
    const strictServer = makeServer({ REQUIRE_APP_VERSION: '1' });
    const c = await signUp(strictServer, 'Cat12').catch(() => null);
    const r = await strictServer.call('POST', '/v1/register', { name: 'Cat12' });
    expect(r.status).toBe(400); // no X-Msc-App: refused before anything is stored
    expect(c).toBeNull();
  });

  it('refuses a bad body with a 400 and stores nothing', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const bad = [
      undefined, '{', '[]', '7', { v: 2, log: 'x' }, { log: 5 }, { log: 'x', extra: 1 }, {}, { v: 1 }, { log: 'x'.repeat(24001) }, { conn: 'x'.repeat(1201) },
      { log: 'x', session: { d: -1 } }, { log: 'x', session: { d: 1.5 } }, { log: 'x', session: { zz: 1 } }, { log: 'x', session: [] }, { __proto__: null, log: 'x', session: 'x' },
      JSON.parse('{"log":"x","__proto__":{"a":1}}'),
    ];
    for (const b of bad) {
      const r = await s.call('POST', '/v1/report', b, a.token);
      expect(r.status, JSON.stringify(b)?.slice(0, 60)).toBeGreaterThanOrEqual(400);
      expect(r.status).toBeLessThan(500);
    }
    expect(count(s, 'reports')).toBe(0);
    expect(count(s, 'daily', 'reports > 0')).toBe(0); // a refused body does not use up one of the three
  });

  it('takes a body past the 8 KB of every other route, up to 48 KB, and refuses more (413 too_large)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const big = rep({ log: 'L'.repeat(20000), events: 'E'.repeat(15000) }); // about 35 KB
    const ok = await s.call('POST', '/v1/report', big, a.token);
    expect(ok.status).toBe(200);
    // 3-byte characters: 20,000 of them are 60 KB on the wire although the text limit allows them
    const heavy = JSON.stringify(rep({ log: '€'.repeat(20000) }));
    expect(new TextEncoder().encode(heavy).length).toBeGreaterThan(REPORT_MAX_BYTES);
    const r = await s.call('POST', '/v1/report', heavy, a.token);
    expect(r.status).toBe(413);
    expect(r.data.error.code).toBe('too_large');
    // a body that is only just over the cap on a report, and the same size on another route
    const pad = JSON.stringify({ v: 1, log: 'x', note: 'n'.repeat(300), build: 'a'.repeat(60) });
    expect(pad.length).toBeLessThan(REPORT_MAX_BYTES);
    const lie = await s.call('POST', '/v1/report', 'x'.repeat(REPORT_MAX_BYTES + 1), a.token);
    expect(lie.status).toBe(413);
    const other = await s.call('POST', '/v1/friends/status', { status: 'x'.repeat(9000) }, a.token);
    expect(other.status).toBe(413); // every other route keeps 8 KB
    expect(count(s, 'reports')).toBe(1);
  });

  it('the Worker lets exactly 48 KiB through for a report and 8 KiB for the rest', () => {
    const req = (path, len) => new Request(`https://t.invalid${path}`, { method: 'POST', headers: { 'content-length': String(len), authorization: `Bearer ${'a'.repeat(43)}` }, body: 'x'.repeat(Math.min(len, 10)) });
    expect(edgeCheck(req('/v1/report', REPORT_MAX_BYTES), { REQUIRE_APP_VERSION: '0' })).toBeNull();
    expect(edgeCheck(req('/v1/report', REPORT_MAX_BYTES + 1), { REQUIRE_APP_VERSION: '0' })).toMatchObject({ status: 413, code: 'too_large' });
    expect(edgeCheck(req('/v1/friends/status', 8193), { REQUIRE_APP_VERSION: '0' })).toMatchObject({ status: 413 });
    expect(ROUTE_META.some((r) => r[0] === 'POST' && r[1] === '/v1/report' && r[2] === 'player')).toBe(true);
  });

  it('allows 3 a player a UTC day, counts the next day again, and counts per player', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const b = await signUp(s, 'Ben');
    for (let i = 0; i < 3; i++) expect((await s.call('POST', '/v1/report', rep({ note: `n${i}` }), a.token)).status).toBe(200);
    const fourth = await s.call('POST', '/v1/report', rep(), a.token);
    expect(fourth.status).toBe(429);
    expect(fourth.data.error).toMatchObject({ code: 'rate_limited', reason: 'daily_cap', scope: 'reports' });
    expect(fourth.data.error.retryAfterSec).toBeGreaterThan(0);
    expect(count(s, 'reports', 'pid = ?', a.pid)).toBe(3);
    expect((await s.call('POST', '/v1/report', rep(), b.token)).status).toBe(200); // somebody else is unaffected
    s.advance(DAY);
    expect((await s.call('POST', '/v1/report', rep(), a.token)).status).toBe(200);
  });

  it('keeps a report 30 days: stored ones past it go at the next sweep and at the next report', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    await s.call('POST', '/v1/report', rep(), a.token);
    s.advance(29 * DAY);
    s.app.sweep();
    expect(count(s, 'reports')).toBe(1);
    s.advance(2 * DAY);
    s.app.sweep();
    expect(count(s, 'reports')).toBe(0);
    // and a new report removes an old one even before the hourly sweep ran
    const b = await signUp(s, 'Ben');
    s.store.sql.exec(`INSERT INTO reports (pid, created_ms, app, build, bytes, body) VALUES (?, ?, '', '', 10, '{}')`, b.pid, s.clock.now - 31 * DAY);
    await s.call('POST', '/v1/report', rep(), b.token);
    expect(count(s, 'reports')).toBe(1);
  });

  it('keeps at most 2,000 reports, the oldest dropped first', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    s.store.tx(() => {
      for (let i = 0; i < REPORT_MAX_COUNT; i++) s.store.sql.exec(`INSERT INTO reports (pid, created_ms, app, build, bytes, body) VALUES (999, ?, '', '', 10, '{"n":${i}}')`, s.clock.now - 1000 + i);
    });
    expect(count(s, 'reports')).toBe(REPORT_MAX_COUNT);
    const first = q('SELECT MIN(id) AS id FROM reports')(s)[0].id;
    expect((await s.call('POST', '/v1/report', rep(), a.token)).status).toBe(200);
    expect(count(s, 'reports')).toBe(REPORT_MAX_COUNT);
    expect(count(s, 'reports', 'id = ?', first)).toBe(0); // the oldest went
    expect(count(s, 'reports', 'pid = ?', a.pid)).toBe(1); // the new one stayed
  });

  it('keeps at most 64 MiB, the oldest dropped first', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const each = Math.floor(REPORT_MAX_TOTAL_BYTES / 3) + 1000; // three of them are just under the cap
    for (let i = 0; i < 3; i++) s.store.sql.exec(`INSERT INTO reports (pid, created_ms, app, build, bytes, body) VALUES (999, ?, '', '', ?, '{}')`, s.clock.now - 100 + i, each);
    const ids = q('SELECT id FROM reports ORDER BY id')(s).map((r) => r.id);
    expect((await s.call('POST', '/v1/report', rep(), a.token)).status).toBe(200);
    const left = q('SELECT id, bytes FROM reports ORDER BY id')(s);
    expect(left.reduce((n, r) => n + r.bytes, 0)).toBeLessThanOrEqual(REPORT_MAX_TOTAL_BYTES);
    expect(left.map((r) => r.id)).not.toContain(ids[0]);
    expect(left.map((r) => r.id)).toContain(ids[2]);
    expect(left.length).toBe(3); // the oldest one went, the other two and the new one stayed
  });

  it('removes addresses, MAC addresses and tokens even if the game left them in, and stores no network address', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const token = 'q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc';
    const dirty = rep({
      log: `peer 192.168.1.23:41100 and 10.0.0.5 and fe80::1c2d:3eff:fe4f:5a6b and 2001:db8::1 and aa:bb:cc:dd:ee:ff and ${token} and Authorization: Bearer ${token} and ${'0123456789abcdef'.repeat(4)}`,
      conn: 'public 203.0.113.9:3478',
    });
    const r = await s.call('POST', '/v1/report', dirty, a.token, { ip: '198.51.100.77' });
    expect(r.status).toBe(200);
    const body = q('SELECT body FROM reports')(s)[0].body;
    for (const secret of ['192.168.1.23', '10.0.0.5', 'fe80', '2001:db8', 'aa:bb:cc', token, '0123456789abcdef', '203.0.113.9', '198.51.100.77']) expect(body).not.toContain(secret);
    expect(body).toContain('<address>');
    const cols = q(`PRAGMA table_info(reports)`)(s).map((c) => c.name);
    expect(cols.join(',')).not.toMatch(/ip|addr|token|src/);
    expect(scrubText('a 1.2.3.4:5 b')).toBe('a <address> b');
  });

  it('is refused cleanly when the daily budget is running short (a JSON error), before the live calls are', async () => {
    const s = makeServer({ DAILY_REQUEST_BUDGET: '200' });
    const a = await signUp(s, 'Ana');
    expect((await s.call('POST', '/v1/report', rep(), a.token)).status).toBe(200);
    for (let i = 0; i < 160; i++) await s.call('GET', '/v1/info', undefined, undefined, { ip: `203.0.113.${1 + (i % 200)}` }); // past 75 %
    const r = await s.call('POST', '/v1/report', rep({ note: 'again' }), a.token);
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('over_budget');
    expect(typeof r.data.error.message).toBe('string');
    expect(r.data.error.message).toMatch(/report/);
    expect(r.headers.get('x-msc-budget')).toBeTruthy();
    expect(count(s, 'reports')).toBe(1);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200); // the live calls still work
  });

  it('is a class of its own: refused from the browse level on, and by the Worker gate too', () => {
    expect(classify('POST', '/v1/report')).toBe('report');
    expect(refusedAt('', 'report')).toBe(false);
    for (const lv of ['browse-over', 'new-over', 'over']) expect(refusedAt(lv, 'report')).toBe(true);
    const gate = createGate();
    gate.learn(new Response('{}', { status: 503, headers: { 'x-msc-budget': 'browse-over' } }), T0, '203.0.113.1', '');
    expect(gate.check('POST', '/v1/report', '203.0.113.1', '', T0 + 1000)).toMatchObject({ status: 503, code: 'over_budget' });
    expect(gate.check('GET', '/v1/rooms/ABCDEF', '203.0.113.1', '', T0 + 1000)).toBeNull();
  });

  it('is counted in the request budget and the rows budget', async () => {
    const s = makeServer({ DAILY_REQUEST_BUDGET: '5000', DAILY_ROWS_BUDGET: '5000' });
    const a = await signUp(s, 'Ana');
    const before = s.app.budget.summary(s.clock.now);
    await s.call('POST', '/v1/report', rep(), a.token);
    const after = s.app.budget.summary(s.clock.now);
    expect(after.used - before.used).toBe(1);
    expect(after.rows - before.rows).toBeGreaterThanOrEqual(3); // the report row, its two index rows, the daily rows
  });

  it('the kill switch `report` turns it off (503 service_disabled) and nothing else', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    expect((await s.call('POST', '/v1/admin/switches', { kill: ['report'] }, ADMIN)).status).toBe(200);
    const r = await s.call('POST', '/v1/report', rep(), a.token);
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('service_disabled');
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200);
  });
});

describe('Delete My Online Account: DELETE /v1/account', () => {
  const refs = (s, pid) => {
    const out = [];
    for (const { name } of q(`SELECT name FROM sqlite_master WHERE type = 'table'`)(s)) {
      if (['rooms', 'matches', 'results', 'counters', 'sqlite_sequence', 'pair_stats', 'pair_daily'].includes(name) || name.startsWith('_')) continue;
      for (const col of q(`PRAGMA table_info(${name})`)(s).map((c) => c.name).filter((n) => ['pid', 'other', 'to_pid', 'from_pid', 'lo_pid', 'hi_pid'].includes(n))) {
        const n = count(s, name, `${col} = ?`, pid);
        if (n) out.push(`${name}.${col}=${n}`);
      }
    }
    return out;
  };

  async function populated() {
    const s = makeServer();
    const a = await signUp(s, 'Leaver');
    const pal = await signUp(s, 'Pal1x');
    const other = await signUp(s, 'Other');
    await s.call('POST', '/v1/friends/add', { pid: pal.pid }, a.token);
    await s.call('POST', '/v1/friends/add', { pid: a.pid }, pal.token);
    await s.call('POST', '/v1/friends/add', { pid: other.pid }, a.token); // a pending request
    await s.call('POST', '/v1/friends/block', { pid: other.pid }, a.token);
    await s.call('POST', '/v1/friends/status', { status: 'U0My' }, a.token);
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    await s.call('POST', '/v1/report', rep({ note: 'one' }), a.token);
    await s.call('POST', '/v1/report', rep({ note: 'two' }), a.token);
    await s.call('POST', '/v1/report', rep({ note: 'pal' }), pal.token);
    return { s, a, pal, other };
  }

  it('removes the profile, friends, invites, queue entry and every report of the player, and nobody else\'s', async () => {
    const { s, a, pal, other } = await populated();
    expect(refs(s, a.pid).length).toBeGreaterThan(2);
    expect(count(s, 'reports', 'pid = ?', a.pid)).toBe(2);
    const del = await s.call('DELETE', '/v1/account', undefined, a.token);
    expect(del.status).toBe(200);
    expect(del.data).toEqual({ ok: true });
    expect(refs(s, a.pid)).toEqual([]);
    expect(count(s, 'profiles', 'pid = ?', a.pid)).toBe(0);
    expect(count(s, 'reports', 'pid = ?', a.pid)).toBe(0);
    expect(count(s, 'queue', 'pid = ?', a.pid)).toBe(0);
    expect(count(s, 'friend_links')).toBe(0);
    // the others keep theirs
    expect(count(s, 'reports', 'pid = ?', pal.pid)).toBe(1);
    expect((await s.call('GET', '/v1/me', undefined, pal.token)).status).toBe(200);
    expect((await s.call('GET', '/v1/me', undefined, other.token)).status).toBe(200);
    expect(JSON.stringify((await s.call('GET', '/v1/friends', undefined, pal.token)).data)).not.toContain('Leaver');
  });

  it('is idempotent: the second call has no token left and is 401 (the game reads that as already gone); the old route does the same job', async () => {
    const { s, a } = await populated();
    expect((await s.call('DELETE', '/v1/me', undefined, a.token)).status).toBe(200); // the route older builds call
    expect(count(s, 'reports', 'pid = ?', a.pid)).toBe(0);
    const again = await s.call('DELETE', '/v1/account', undefined, a.token);
    expect(again.status).toBe(401);
    expect(again.data.error.code).toBe('unauthorized');
  });

  it('POST /v1/account does the same (the game\'s HTTPS client speaks GET and POST only): an empty body or {}, idempotent the same way', async () => {
    const { s, a, pal } = await populated();
    expect((await s.call('POST', '/v1/account', { x: 1 }, a.token)).status).toBe(400);
    expect((await s.call('POST', '/v1/account')).status).toBe(401);
    expect(count(s, 'profiles', 'pid = ?', a.pid)).toBe(1);
    const del = await s.call('POST', '/v1/account', undefined, a.token);
    expect(del.status).toBe(200);
    expect(del.data).toEqual({ ok: true });
    expect(refs(s, a.pid)).toEqual([]);
    expect(count(s, 'reports', 'pid = ?', a.pid)).toBe(0);
    expect(count(s, 'reports', 'pid = ?', pal.pid)).toBe(1);
    expect((await s.call('POST', '/v1/account', '{}', a.token)).status).toBe(401); // already gone
  });

  it('takes no body, and needs the token', async () => {
    const { s, a } = await populated();
    expect((await s.call('DELETE', '/v1/account', { x: 1 }, a.token)).status).toBe(400);
    expect((await s.call('DELETE', '/v1/account')).status).toBe(401);
    expect(count(s, 'profiles', 'pid = ?', a.pid)).toBe(1);
  });

  it('the name is free again and the same person can sign up again, with no reports and a fresh day count', async () => {
    const { s, a } = await populated();
    await s.call('DELETE', '/v1/account', undefined, a.token);
    const b = await signUp(s, 'Leaver');
    expect(b.pid).not.toBe(a.pid);
    expect(count(s, 'reports', 'pid = ?', b.pid)).toBe(0);
    expect((await s.call('POST', '/v1/report', rep(), b.token)).data.left).toBe(2);
  });
});

describe('anonymous session numbers on leave', () => {
  async function inRoom(s, a, b) {
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    await s.call('POST', '/v1/rooms/join', { code }, b.token);
    return code;
  }

  it('parses only well formed numbers', () => {
    expect(parseStats('d=312;dl=2;rs=0;dr=1;sv=5;e=1')).toEqual({ d: 312, dl: 2, rs: 0, dr: 1, sv: 5, e: 1 });
    for (const bad of [null, '', 'x', 'd=-1', 'd=1;d=2', 'name=Ana', 'd=99999999', 'dl=31', 'e=2', 'd=1;;e=0', 'd=1.5', 'd=1 ', 'zz=1', 'x=1', 'ab=5', `d=1;${'e=0;'.repeat(40)}`, '__proto__=1', 'constructor=1']) expect(parseStats(bad), String(bad)).toBeNull();
  });

  it('stores them in a ring and a daily aggregate, without a name, an id or an address', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const b = await signUp(s, 'Ben');
    const code = await inRoom(s, a, b);
    const r = await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token, { 'x-msc-stats': 'd=312;dl=2;rs=4;dr=1;sv=9;e=1', ip: '198.51.100.9' });
    expect(r.status).toBe(200);
    const code2 = await inRoom(s, b, a);
    await s.call('DELETE', `/v1/rooms/${code2}`, undefined, b.token, { 'x-msc-stats': 'd=100;dl=4;rs=0;dr=0;sv=0;e=0' });
    const ring = q('SELECT * FROM session_ring ORDER BY id')(s);
    expect(ring.length).toBe(2);
    expect(ring[0]).toMatchObject({ dur: 312, delay: 2, resends: 4, drops: 1, starved: 9, err: 1 });
    expect(Object.keys(ring[0]).join(',')).not.toMatch(/pid|name|ip|addr|code|token/);
    const day = q('SELECT * FROM session_daily')(s);
    expect(day.length).toBe(1);
    expect(day[0]).toMatchObject({ sessions: 2, errors: 1, dur_sum: 412, delay_sum: 6, resends_sum: 4, drops_sum: 1, starved_sum: 9 });
    expect(JSON.stringify(q('SELECT * FROM session_ring')(s))).not.toContain('198.51.100');
  });

  it('a malformed header never fails the leave and stores nothing; no header stores nothing', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const b = await signUp(s, 'Ben');
    for (const h of ['garbage', 'd=-5', 'name=Ana;d=1', 'd=1;e=7']) {
      const code = await inRoom(s, a, b);
      expect((await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token, { 'x-msc-stats': h })).status).toBe(200);
    }
    const code = await inRoom(s, a, b);
    expect((await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token)).status).toBe(200);
    expect(count(s, 'session_ring')).toBe(0);
    expect(count(s, 'session_daily')).toBe(0);
  });

  it('a failure while storing the numbers never fails the leave', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const b = await signUp(s, 'Ben');
    const code = await inRoom(s, a, b);
    s.store.sql.exec('DROP TABLE session_daily');
    const r = await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token, { 'x-msc-stats': 'd=5' });
    expect(r.status).toBe(200);
    expect(count(s, 'session_ring')).toBe(0); // the half-done write was rolled back
  });

  it('the ring keeps the last 500 and the daily aggregate keeps counting', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const b = await signUp(s, 'Ben');
    s.store.tx(() => {
      for (let i = 0; i < SESSION_RING + 20; i++) s.store.sql.exec(`INSERT INTO session_ring (created_ms, day, dur) VALUES (1, 1, ?)`, i);
    });
    const code = await inRoom(s, a, b);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token, { 'x-msc-stats': 'd=7' });
    expect(count(s, 'session_ring')).toBe(SESSION_RING);
    expect(q('SELECT MIN(dur) AS m FROM session_ring')(s)[0].m).toBeGreaterThan(0); // the oldest were the ones dropped
    expect(count(s, 'session_ring', 'dur = 7')).toBe(1);
  });
});

describe('admin routes', () => {
  it('list and read reports, and the daily aggregates; admin token only', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const b = await signUp(s, 'Ben');
    await s.call('POST', '/v1/report', rep({ note: 'first' }), a.token);
    await s.call('POST', '/v1/report', rep({ note: 'second' }), b.token);
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    await s.call('POST', '/v1/rooms/join', { code }, b.token);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, b.token, { 'x-msc-stats': 'd=60;dl=2;e=0' });
    expect((await s.call('GET', '/v1/admin/reports')).status).toBe(401);
    expect((await s.call('GET', '/v1/admin/reports', undefined, a.token)).status).toBe(401);
    const list = await s.call('GET', '/v1/admin/reports', undefined, ADMIN);
    expect(list.status).toBe(200);
    expect(list.data.total).toBe(2);
    expect(list.data.reports[0].id).toBeGreaterThan(list.data.reports[1].id); // newest first
    expect(list.data.reports[0]).toMatchObject({ name: 'Ben', app: '1.3.0' });
    const one = await s.call('GET', `/v1/admin/reports/${list.data.reports[1].id}`, undefined, ADMIN);
    expect(one.data.report).toMatchObject({ note: 'first', log: 'T+1.0s link lost: no packets' });
    expect((await s.call('GET', '/v1/admin/reports/99999', undefined, ADMIN)).status).toBe(404);
    expect((await s.call('GET', '/v1/admin/reports/abc', undefined, ADMIN)).status).toBe(404);
    const page = await s.call('GET', `/v1/admin/reports?limit=1&before=${list.data.reports[0].id}`, undefined, ADMIN);
    expect(page.data.reports.length).toBe(1);
    expect(page.data.reports[0].id).toBe(list.data.reports[1].id);
    const d = await s.call('GET', '/v1/admin/diag', undefined, ADMIN);
    expect(d.data.reportsDaily[0]).toMatchObject({ reports: 2 });
    expect(d.data.sessionsDaily[0]).toMatchObject({ sessions: 1, durSum: 60 });
    expect(d.data.sessionRing.length).toBe(1);
  });
});

describe('the previous code on the migrated database (rollback)', () => {
  it('serves a database that has migration 1003 and reports in it', async () => {
    const store = openNodeSql();
    const env = { RANK_HMAC_KEY_HEX: HMAC_HEX, ADMIN_TOKEN: ADMIN, TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '0', RATE_LIMIT_SCALE: '1000', MIN_MATCH_SEC: '0' };
    const n = makeServer(env);
    const a = await signUp(n, 'Ana');
    await n.call('POST', '/v1/report', rep(), a.token);
    const clock = { now: T0 };
    const o = oldApp({ sql: n.store.sql, tx: n.store.tx, env, now: () => clock.now });
    const r = await o.handle(new Request('https://t.invalid/v1/me', { headers: { authorization: `Bearer ${a.token}`, 'cf-connecting-ip': '198.51.100.1' } }));
    expect(r.status).toBe(200);
    expect(store).toBeTruthy();
  });
});
