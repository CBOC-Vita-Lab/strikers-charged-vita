// The daily request budget (core/budget.js): pollScale, the levels with the reserve for matches in progress, persistence, and the per-account /
// per-address daily ceilings. DAILY_REQUEST_BUDGET is this service's share of the account's 100,000 a day; the other service has its own.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp } from '../helpers.js';
import { createApp } from '../../src/core/app.js';
import { classify, refusedAt } from '../../src/core/budget.js';

const info = (srv, ip = '203.0.113.9') => srv.call('GET', '/v1/info', undefined, undefined, { ip });
async function use(srv, n) { for (let i = 0; i < n; i++) await info(srv, '203.0.113.' + (1 + (i % 200))); }

describe('classes', () => {
  it('every route has a class, and the levels refuse the right ones', () => {
    expect(classify('POST', '/v1/rooms')).toBe('new');
    expect(classify('GET', '/v1/rooms/ABC234')).toBe('live');
    expect(classify('POST', '/v1/rooms/join')).toBe('live'); // the object refuses a NEW guest itself; a member rejoining goes through (the reserve)
    expect(classify('POST', '/v1/ranked/queue')).toBe('new');
    expect(classify('GET', '/v1/ranked/queue')).toBe('live');
    expect(classify('POST', '/v1/rank/get')).toBe('browse');
    expect(classify('POST', '/v1/rank/put')).toBe('live');
    expect(classify('POST', '/v1/matches/abc/result')).toBe('live');
    expect(classify('GET', '/v1/admin/stats')).toBe('admin');
    expect(refusedAt('browse-over', 'browse')).toBe(true);
    expect(refusedAt('browse-over', 'new')).toBe(false);
    expect(refusedAt('new-over', 'new')).toBe(true);
    expect(refusedAt('new-over', 'live')).toBe(false);
    expect(refusedAt('over', 'live')).toBe(true);
    expect(refusedAt('over', 'admin')).toBe(false);
  });
});

describe('daily request budget', () => {
  it('pollScale appears on poll answers from half the budget, only above 1, and reaches 1.4 at the new-session line', async () => {
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '1000' });
    const a = await signUp(srv, 'Alice1');
    const room = await srv.call('POST', '/v1/rooms', undefined, a.token);
    const poll = () => srv.call('GET', `/v1/rooms/${room.data.code}`, undefined, a.token);
    expect((await poll()).data.pollScale).toBeUndefined();
    await use(srv, 520);
    const mid = (await poll()).data.pollScale;
    expect(mid).toBeGreaterThan(1);
    expect(mid).toBeLessThan(1.1);
    await use(srv, 250); // 77 % used
    const high = (await poll()).data.pollScale;
    expect(high).toBeGreaterThan(1.3);
    expect(high).toBeLessThanOrEqual(1.4);
  });

  it('pollScale is also on the queue answers, and not on other answers', async () => {
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '100' });
    const a = await signUp(srv, 'Alice1');
    await use(srv, 60);
    const q = await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect(q.data.pollScale).toBeGreaterThan(1);
    const me = await srv.call('GET', '/v1/me', undefined, a.token);
    expect(me.data.pollScale).toBeUndefined();
  });

  it('browsing stops at 75 %, new sessions at 80 %, matches in progress go on to 100 %, and then everything but /admin stops', async () => {
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '200' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    const c3 = await signUp(srv, 'Carol3');
    const made = await srv.call('POST', '/v1/rooms', undefined, a.token);
    const code = made.data.code;
    await srv.call('POST', '/v1/rooms/join', { code }, b.token);
    const body = { mode: 0, category: 0, region: 2, sort: 0, since: 0, limit: 10 };
    expect((await srv.call('POST', '/v1/rank/get', body, a.token)).status).toBe(200);
    await use(srv, 150 - srv.app.budget.state.used);
    expect(srv.app.budget.level(srv.clock.now)).toBe('browse-over');
    const br = await srv.call('POST', '/v1/rank/get', body, a.token);
    expect(br.status).toBe(503);
    expect(br.data.error.code).toBe('over_budget');
    expect(br.data.error.retryAfterSec).toBeGreaterThan(0);
    expect(br.headers.get('x-msc-budget')).toBe('browse-over');
    expect(br.headers.get('retry-after')).toBeTruthy();
    const c3room = await srv.call('POST', '/v1/rooms', undefined, c3.token);
    expect(c3room.status).toBe(200); // a new room is still allowed below 80 %
    await use(srv, 160 - srv.app.budget.state.used);
    expect(srv.app.budget.level(srv.clock.now)).toBe('new-over');
    for (const [m, p, bd, t] of [['POST', '/v1/rooms', undefined, a.token], ['POST', '/v1/rooms/join', { code: c3room.data.code }, a.token], ['POST', '/v1/ranked/queue', undefined, a.token], ['POST', '/v1/register', { name: 'Newbie9' }, undefined]]) {
      const r = await srv.call(m, p, bd, t);
      expect(r.status, p).toBe(503);
      expect(r.headers.get('x-msc-budget')).toBe('new-over');
    }
    // the reserve: a match in progress still works, a member who lost the link rejoins their own room
    expect((await srv.call('POST', '/v1/rooms/join', { code }, b.token)).status).toBe(200);
    expect((await srv.call('GET', `/v1/rooms/${code}`, undefined, a.token)).status).toBe(200);
    expect((await srv.call('GET', '/v1/ranked/queue', undefined, a.token)).status).toBe(200);
    expect((await srv.call('POST', `/v1/rooms/${code}/endpoint`, { public: '203.0.113.1:41000', local: '192.168.1.1:41000', nonce: 'AAAAAAAAAAA=' }, a.token)).status).toBe(200);
    await use(srv, 200 - srv.app.budget.state.used);
    expect(srv.app.budget.level(srv.clock.now)).toBe('over');
    const dead = await srv.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    expect(dead.status).toBe(503);
    expect(dead.headers.get('x-msc-budget')).toBe('over');
    const adm = await srv.call('GET', '/v1/admin/stats', undefined, undefined, { authorization: 'Bearer test-admin-token-0123456789' });
    expect(adm.status).toBe(200);
    expect(adm.data.budget.level).toBe('over');
    expect(adm.data.budget.budget).toBe(200);
  });

  it('refused requests do not use up the budget, and a new UTC day starts again', async () => {
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '100' });
    const a = await signUp(srv, 'Alice1');
    await use(srv, 90 - srv.app.budget.state.used);
    const before = srv.app.budget.state.used;
    for (let i = 0; i < 20; i++) await srv.call('POST', '/v1/rooms', undefined, a.token); // refused (new-over)
    expect(srv.app.budget.state.used).toBe(before);
    expect(srv.app.budget.state.refused).toBe(20);
    srv.advance(12 * 3600 * 1000);
    expect((await srv.call('POST', '/v1/rooms', undefined, a.token)).status).toBe(200);
    expect(srv.app.budget.state.used).toBeLessThan(5);
  });

  it('the count survives a restart of the object (written every 200 requests, plus a margin)', async () => {
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '100000' });
    await use(srv, 450);
    const had = srv.app.budget.state.used;
    const app2 = createApp({ sql: srv.store.sql, tx: srv.store.tx, env: srv.env, now: () => srv.clock.now });
    await app2.handle(new Request('https://x.invalid/v1/info', { headers: { 'cf-connecting-ip': '203.0.113.1' } }));
    expect(app2.budget.state.used).toBeGreaterThanOrEqual(400);
    expect(app2.budget.state.used).toBeLessThanOrEqual(had + 201);
  });

  it('the housekeeping alarm counts (it is a Durable Object request)', async () => {
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '1000' });
    await signUp(srv, 'Alice1');
    const u = srv.app.budget.state.used;
    srv.app.sweep();
    srv.app.sweep();
    expect(srv.app.budget.state.used).toBe(u + 2);
  });

  it('DAILY_REQUEST_BUDGET=0 switches the guard off', async () => {
    const srv = makeServer({ DAILY_REQUEST_BUDGET: '0' });
    await use(srv, 120);
    expect(srv.app.budget.level(srv.clock.now)).toBe('');
    expect((await info(srv)).headers.get('x-msc-budget')).toBeNull();
  });
});

describe('daily ceilings', () => {
  it('an account has a ceiling of requests a day: over it, 429 daily_cap with x-msc-block: acct; other accounts are not affected; it resets at midnight UTC', async () => {
    const srv = makeServer({ ACCOUNT_DAY_REQUESTS: '30', ADDR_DAY_REQUESTS: '0' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    let last;
    for (let i = 0; i < 30; i++) last = await srv.call('GET', '/v1/me', undefined, a.token, { ip: '203.0.113.5' });
    expect(last.status).toBe(200);
    const over = await srv.call('GET', '/v1/me', undefined, a.token, { ip: '203.0.113.5' });
    expect(over.status).toBe(429);
    expect(over.data.error.reason).toBe('daily_cap');
    expect(over.data.error.retryAfterSec).toBeGreaterThan(0);
    expect(over.headers.get('x-msc-block')).toBe('acct');
    expect((await srv.call('GET', '/v1/me', undefined, b.token, { ip: '203.0.113.6' })).status).toBe(200);
    srv.advance(13 * 3600 * 1000);
    expect((await srv.call('GET', '/v1/me', undefined, a.token, { ip: '203.0.113.5' })).status).toBe(200);
  });

  it('an address has two ceilings: open routes (1,000 by default) and everything (30,000); over one, x-msc-block: addr', async () => {
    const srv = makeServer({ ADDR_OPEN_DAY: '5', ADDR_DAY_REQUESTS: '12', ACCOUNT_DAY_REQUESTS: '0' });
    for (let i = 0; i < 5; i++) expect((await info(srv, '203.0.113.50')).status).toBe(200);
    const over = await info(srv, '203.0.113.50');
    expect(over.status).toBe(429);
    expect(over.headers.get('x-msc-block')).toBe('addr-open');
    expect((await info(srv, '203.0.113.51')).status).toBe(200); // another address is not affected
    const a = await signUp(srv, 'Alice1');
    let n = 0;
    let last;
    do { last = await srv.call('GET', '/v1/me', undefined, a.token, { ip: '203.0.113.60' }); n++; } while (last.status === 200 && n < 40);
    expect(last.status).toBe(429);
    expect(n).toBe(13);
    expect(last.headers.get('x-msc-block')).toBe('addr');
  });

  it('the defaults leave an honest evening alone: a player polling a held room all day stays far below the account ceiling', () => {
    const srv = makeServer({});
    expect(srv.app.cfg.budget.accountDay).toBe(9000);
    expect(srv.app.cfg.budget.addrOpenDay).toBe(1000);
    expect(srv.app.cfg.budget.addrDay).toBe(30000);
    expect(srv.app.cfg.budget.daily).toBe(45000);
  });
});
