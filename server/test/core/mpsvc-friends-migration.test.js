// Rollback safety of migration 1002 (friends, quick match): the code deployed today (a verbatim copy at b1910c0, test/fixtures/src-b1910c0) must run on a
// database the new code has migrated and filled, so `wrangler rollback` after the second deploy never needs a database restore.
import { describe, it, expect } from 'vitest';
import { openNodeSql } from '../../src/sqlite-node.js';
import { createApp as newApp } from '../../src/core/app.js';
import { createApp as oldApp } from '../fixtures/src-b1910c0/core/app.js';
import { HMAC_HEX, T0 } from '../helpers.js';

const env = { RANK_HMAC_KEY_HEX: HMAC_HEX, ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '0', RATE_LIMIT_SCALE: '1000', MIN_MATCH_SEC: '0' };

function caller(app, clock) {
  let ip = 0;
  return async (method, path, body, token) => {
    const h = { 'cf-connecting-ip': `198.51.100.${(ip++ % 250) + 1}` };
    if (token) h.authorization = `Bearer ${token}`;
    const init = { method, headers: h };
    if (body !== undefined) { init.body = JSON.stringify(body); h['content-type'] = 'application/json'; }
    const r = await app.handle(new Request(`https://t.invalid${path}`, init));
    const t = await r.text();
    let data; try { data = JSON.parse(t); } catch { data = t; }
    return { status: r.status, data };
  };
}

describe('migration 1002 and the previous code', () => {
  it('the old code serves a database the new code migrated and used (friends, blocks, quick rooms, queue modes)', async () => {
    const store = openNodeSql();
    const clock = { now: T0 };
    const n = caller(newApp({ sql: store.sql, tx: store.tx, env, now: () => clock.now }), clock);
    const a = (await n('POST', '/v1/register', { name: 'Alice1' })).data;
    const b = (await n('POST', '/v1/register', { name: 'Bobby2' })).data;
    const c = (await n('POST', '/v1/register', { name: 'Carol3' })).data;
    await n('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    await n('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    await n('POST', '/v1/friends/block', { pid: c.pid }, a.token);
    await n('POST', '/v1/friends/status', { status: 'U0My' }, a.token);
    await n('POST', '/v1/ranked/queue', { mode: 'quick' }, a.token);
    await n('POST', '/v1/ranked/queue', { mode: 'quick' }, b.token);
    const quick = (await n('GET', '/v1/ranked/queue', undefined, a.token)).data;
    expect(quick.room.quick).toBe(true);
    // now the PREVIOUS code on the same database
    const o = caller(oldApp({ sql: store.sql, tx: store.tx, env, now: () => clock.now }), clock);
    expect((await o('GET', '/v1/me', undefined, a.token)).status).toBe(200);
    expect((await o('GET', `/v1/rooms/${quick.room.code}`, undefined, a.token)).status).toBe(200); // the quick room is an ordinary room to it
    expect((await o('POST', '/v1/rank/get', { mode: 0, category: 0, region: 2, sort: 0, since: 0, limit: 5 }, a.token)).status).toBe(200);
    const d = (await o('POST', '/v1/register', { name: 'Dave44' })).data;
    expect(d.pid).toBeGreaterThan(0);
    await o('POST', `/v1/rooms/${quick.room.code}/leave`, undefined, a.token);
    const r2 = await o('POST', '/v1/rooms', undefined, d.token);
    expect(r2.status).toBe(200);
    expect((await o('POST', '/v1/ranked/queue', undefined, c.token)).status).toBe(200);
    expect((await o('GET', '/v1/ranked/queue', undefined, c.token)).status).toBe(200);
    // the new code again on what the old code did
    expect((await n('GET', '/v1/friends', undefined, b.token)).status).toBe(200);
    expect((await n('POST', '/v1/rooms/join', { code: r2.data.code }, b.token)).status).toBe(200);
  });
});
