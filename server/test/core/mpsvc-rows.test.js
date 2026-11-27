// Rows written (the Workers Free plan's binding limit: 100,000 a day for the account, shared with the Galaxy service): the budget counts them per
// write path from the platform's `rowsWritten`, degrades like the requests (pollScale, browsing, new sessions, everything), logs the tiers, and
// persists the count. Also: the object's own request ceiling stays at its 45,000 share.
import { describe, it, expect, vi } from 'vitest';
import { makeServer, signUp, T0 } from '../helpers.js';
import { loadConfig } from '../../src/core/config.js';
import { createBudget, ROWS_FLUSH_EVERY } from '../../src/core/budget.js';
import { createApp } from '../../src/core/app.js';
import { openNodeSql } from '../../src/sqlite-node.js';

const rowsOf = (srv) => srv.app.budget.state.rows;
const stats = (srv) => srv.app.budget.summary(srv.clock.now);

describe('rows written are counted per write path', () => {
  it('the node adapter applies the billing rule: row + one per index touched, UPDATE only for indexes in SET', () => {
    const st = openNodeSql();
    st.sql.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, c INTEGER)');
    st.sql.exec('CREATE INDEX t_a ON t(a)');
    st.sql.exec('CREATE INDEX t_ab ON t(a, b)');
    expect(st.sql.exec('INSERT INTO t (a, b, c) VALUES (1, 2, 3)').rowsWritten).toBe(3); // row + 2 indexes
    expect(st.sql.exec('UPDATE t SET c = 9 WHERE id = 1').rowsWritten).toBe(1); // no index touched
    expect(st.sql.exec('UPDATE t SET b = 9 WHERE id = 1').rowsWritten).toBe(2); // t_ab only
    expect(st.sql.exec('UPDATE t SET a = 9, b = 9 WHERE id = 1').rowsWritten).toBe(3);
    expect(st.sql.exec('SELECT * FROM t').rowsWritten).toBe(0);
    expect(st.sql.exec('DELETE FROM t WHERE id = 1').rowsWritten).toBe(3);
    st.close();
  });

  it('a registration, a room, a ranked pairing and a result each add rows; reads add none', async () => {
    const srv = makeServer();
    const a = await signUp(srv, 'Alice1');
    const afterReg = rowsOf(srv);
    expect(afterReg).toBeGreaterThanOrEqual(4); // README: a registration is about 4 rows (profile + its unique indexes, + the all-time index)
    expect(afterReg).toBeLessThanOrEqual(12);
    for (let i = 0; i < 5; i++) await srv.call('GET', '/v1/info');
    await srv.call('GET', '/v1/time');
    expect(rowsOf(srv)).toBe(afterReg); // open routes read nothing and write nothing
    const room = await srv.call('POST', '/v1/rooms', undefined, a.token);
    expect(room.status).toBe(200);
    const afterRoom = rowsOf(srv);
    expect(afterRoom).toBeGreaterThan(afterReg);
    const b = await signUp(srv, 'Bobby22');
    const j = await srv.call('POST', '/v1/rooms/join', { code: room.data.code }, b.token);
    expect(j.status).toBe(200);
    expect(rowsOf(srv)).toBeGreaterThan(afterRoom + 4); // second profile + the join
    expect(stats(srv).rows).toBe(rowsOf(srv));
    expect(stats(srv).rowsBudget).toBe(50000);
  });

  it('every row the platform reports is counted: the sum of rowsWritten over all statements equals the budget count', async () => {
    const srv = makeServer();
    let sum = 0;
    const real = srv.store.sql.exec.bind(srv.store.sql);
    srv.store.sql.exec = (q, ...b) => { const cur = real(q, ...b); sum += cur.rowsWritten || 0; return cur; };
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby22');
    const q = await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    await srv.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect(q.status).toBe(200);
    srv.advance(3000);
    await srv.call('GET', '/v1/ranked/queue', undefined, a.token);
    expect(sum).toBeGreaterThan(20);
    expect(rowsOf(srv)).toBe(sum); // nothing written outside the counted paths (the migration ran before the wrapper was installed)
  });

  it('the object counts the row of setAlarm() through budget.rows, and ignores zero and nonsense', () => {
    const srv = makeServer();
    const before = rowsOf(srv);
    srv.app.budget.rows(srv.clock.now, 1);
    srv.app.budget.rows(srv.clock.now, 0);
    srv.app.budget.rows(srv.clock.now, -5);
    srv.app.budget.rows(srv.clock.now, NaN);
    expect(rowsOf(srv)).toBe(before + 1);
  });
});

describe('rows degrade like the requests', () => {
  async function drive(srv, target) {
    // spend rows with cheap writes (the budget's own rows API stands in for any write path: the counting above proves the real ones)
    srv.app.budget.rows(srv.clock.now, target - rowsOf(srv));
  }

  it('50 % of the rows: pollScale above 1; 75 %: browsing refused; 80 %: no new sessions; matches in progress go on; 100 %: everything but admin', async () => {
    const srv = makeServer({ DAILY_ROWS_BUDGET: '1000', DAILY_REQUEST_BUDGET: '100000000' });
    const a = await signUp(srv, 'Alice1');
    const room = await srv.call('POST', '/v1/rooms', undefined, a.token);
    const poll = () => srv.call('GET', `/v1/rooms/${room.data.code}`, undefined, a.token);
    await drive(srv, 400);
    expect((await poll()).data.pollScale).toBeUndefined();
    await drive(srv, 600);
    const mid = (await poll()).data.pollScale;
    expect(mid).toBeGreaterThan(1);
    expect(mid).toBeLessThan(1.15);
    await drive(srv, 740);
    expect((await srv.call('POST', '/v1/rank/get', { mode: 2, category: 0, region: 255, sort: 1, since: 60, limit: 5 }, a.token)).status).toBe(200);
    await drive(srv, 760);
    const browse = await srv.call('POST', '/v1/rank/get', { mode: 2, category: 0, region: 255, sort: 1, since: 60, limit: 5 }, a.token);
    expect(browse.status).toBe(503);
    expect(browse.headers.get('x-msc-budget')).toBe('browse-over');
    await drive(srv, 810);
    const reg = await srv.call('POST', '/v1/register', { name: 'Newbie1' });
    expect(reg.status).toBe(503);
    expect(reg.headers.get('x-msc-budget')).toBe('new-over');
    expect((await poll()).status).toBe(200); // the reserve: a room in progress keeps working
    await drive(srv, 1000);
    expect((await poll()).status).toBe(503);
    expect((await srv.call('GET', '/v1/admin/stats', undefined, srv.env.ADMIN_TOKEN)).status).toBe(200);
  });

  it('the larger of the two shares decides: a quiet request count does not hide a full rows count, and the reverse', async () => {
    const srv = makeServer({ DAILY_ROWS_BUDGET: '1000', DAILY_REQUEST_BUDGET: '1000' });
    await drive(srv, 820);
    expect(srv.app.budget.level(srv.clock.now)).toBe('new-over');
    const srv2 = makeServer({ DAILY_ROWS_BUDGET: '100000', DAILY_REQUEST_BUDGET: '100' });
    for (let i = 0; i < 85; i++) await srv2.call('GET', '/v1/info');
    expect(srv2.app.budget.level(srv2.clock.now)).toBe('new-over');
  });

  it('DAILY_ROWS_BUDGET=0 turns the rows guard off (rows still counted, never refused)', async () => {
    const srv = makeServer({ DAILY_ROWS_BUDGET: '0' });
    await drive(srv, 10_000_000);
    expect(srv.app.budget.level(srv.clock.now)).toBe('');
    expect(stats(srv).rowsSharePct).toBeNull();
  });

  it('the default share is 50,000 rows and the request budget stays 45,000', () => {
    const cfg = loadConfig({});
    expect(cfg.budget.rowsDaily).toBe(50000);
    expect(cfg.budget.daily).toBe(45000);
  });

  it('a new UTC day starts the rows at zero', async () => {
    const srv = makeServer({ DAILY_ROWS_BUDGET: '1000' });
    await drive(srv, 900);
    expect(srv.app.budget.level(srv.clock.now)).toBe('new-over');
    srv.advance(24 * 3600_000);
    expect(srv.app.budget.level(srv.clock.now)).toBe('');
    expect(rowsOf(srv)).toBe(0);
  });
});

describe('the log line', () => {
  it('goes out once for each of 50 / 75 / 80 / 100 % of the rows, with no ids in it', async () => {
    const lines = [];
    const srv = makeServer({ DAILY_ROWS_BUDGET: '1000', DAILY_REQUEST_BUDGET: '100000000' }, { deps: { log: (l) => lines.push(l) } });
    const a = await signUp(srv, 'Alice1');
    expect(lines.length).toBe(0);
    for (const target of [510, 520, 760, 790, 810, 820, 1000, 1100]) {
      srv.app.budget.rows(srv.clock.now, target - rowsOf(srv));
      await srv.call('GET', '/v1/me', undefined, a.token).catch(() => {}); // a request runs the check
    }
    expect(lines.length).toBe(4);
    expect(lines[0]).toMatch(/\[budget\].*rows written 5\d\d\/1000 \(5\d %\)/);
    expect(lines[3]).toMatch(/rows written 1000\/1000 \(100 %\)/);
    for (const l of lines) expect(l).not.toMatch(/Alice|token|pid|\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}/);
  });
});

describe('persistence', () => {
  function reopen(store, env, now) {
    return createApp({ sql: store.sql, tx: store.tx, env, now: () => now, log: () => {} });
  }
  it('flush writes day:rows in its own row and leaves day:requests in the old format; a restarted object reloads it and adds the margin', async () => {
    const srv = makeServer({ DAILY_ROWS_BUDGET: '100000' });
    await signUp(srv, 'Alice1');
    srv.app.budget.rows(srv.clock.now, 5000);
    srv.app.budget.flush(srv.clock.now);
    const get = (k) => srv.store.sql.exec(`SELECT value FROM settings WHERE key = ?`, k).toArray()[0].value;
    expect(get('budget')).toMatch(/^\d+:\d+$/); // unchanged, so the previous build can still read it
    const m = /^(\d+):(\d+)$/.exec(get('budget_rows'));
    expect(m).toBeTruthy();
    expect(Number(m[2])).toBeGreaterThanOrEqual(5000);
    const rowsThen = Number(m[2]);
    const app2 = reopen(srv.store, srv.env, srv.clock.now);
    app2.budget.level(srv.clock.now);
    expect(app2.budget.state.rows).toBe(rowsThen + ROWS_FLUSH_EVERY);
  });

  it('the rows persist on their own: ROWS_FLUSH_EVERY rows of writes cause a flush at the next request', async () => {
    const srv = makeServer({ DAILY_ROWS_BUDGET: '100000' });
    await signUp(srv, 'Alice1');
    const before = srv.app.budget.state.rowsFlushed;
    srv.app.budget.rows(srv.clock.now, ROWS_FLUSH_EVERY + 5);
    await srv.call('GET', '/v1/info');
    expect(srv.app.budget.state.rowsFlushed).toBeGreaterThan(before + ROWS_FLUSH_EVERY);
  });

  it('a day with a request counter but no rows counter (deploy in the middle of the day) guesses one row per request and says so', () => {
    const store = openNodeSql();
    const first = createApp({ sql: store.sql, tx: store.tx, env: {}, now: () => T0, log: () => {} });
    const day = Math.floor(T0 / 86400000);
    store.sql.exec(`INSERT INTO settings (key, value) VALUES ('budget', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, `${day}:7000`);
    const app = reopen(store, {}, T0);
    const s = app.budget.summary(T0);
    expect(s.rowsGuessed).toBe(true);
    expect(s.rows).toBe(7000 + ROWS_FLUSH_EVERY);
    expect(first).toBeTruthy();
  });

  it('a counter of another day is ignored', () => {
    const store = openNodeSql();
    createApp({ sql: store.sql, tx: store.tx, env: {}, now: () => T0, log: () => {} });
    store.sql.exec(`INSERT INTO settings (key, value) VALUES ('budget', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, `1:9000`);
    store.sql.exec(`INSERT INTO settings (key, value) VALUES ('budget_rows', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, `1:9000`);
    expect(reopen(store, {}, T0).budget.summary(T0).rows).toBe(0);
  });
});

describe('the object request ceiling stays at its 45,000 share', () => {
  it('level `over` begins at exactly 100 % of the request share (no factor above it), and the default share is 45,000', () => {
    const cfg = loadConfig({});
    expect(cfg.budget.daily).toBe(45000);
    const store = openNodeSql();
    const cfg2 = loadConfig({ DAILY_REQUEST_BUDGET: '1000' });
    const bud = createBudget({ cfg: cfg2, db: { get: () => null, run: () => {}, all: () => [] }, clock: () => T0, log: () => {} });
    for (let i = 0; i < 999; i++) bud.admit(T0, 'live', 'a');
    expect(bud.level(T0)).toBe('new-over');
    bud.admit(T0, 'live', 'a');
    expect(bud.level(T0)).toBe('over');
    expect(() => bud.admit(T0, 'live', 'a')).toThrow();
    store.close();
  });

  it('through the Worker, a flood of any size reaches the object at most budget + a few times, and the account total stays inside the share', async () => {
    vi.resetModules();
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '600', REQUIRE_APP_VERSION: '0' });
    const calls = { n: 0 };
    const env = { ...srv.env, GAME: { idFromName: () => 'main', get: () => ({ fetch: (req) => { calls.n++; return srv.app.handle(req); } }) } };
    const worker = (await import('../../src/worker.js')).default;
    for (let i = 0; i < 2500; i++) {
      const r = await worker.fetch(new Request('https://svc.invalid/v1/time', { headers: { 'cf-connecting-ip': `203.0.${(i >> 8) & 255}.${i & 255}` } }), env);
      await r.text();
    }
    expect(calls.n).toBeGreaterThanOrEqual(600);
    expect(calls.n).toBeLessThanOrEqual(600 + 5);
    expect(stats(srv).billed).toBeLessThanOrEqual(600 + 5);
  });
});
