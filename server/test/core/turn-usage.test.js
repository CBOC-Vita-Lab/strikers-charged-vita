// The measured TURN hard stop (core/turn_usage.js, wave 13 O1) against a mocked Cloudflare GraphQL endpoint. No network.
import { describe, it, expect } from 'vitest';
import { createApp } from '../../src/core/app.js';
import { makeServer, signUp, pairInRoom } from '../helpers.js';
import { buildQuery, parseUsage } from '../../src/core/turn_usage.js';

const ACCOUNT = 'a'.repeat(32);
const TOKEN = 'usage-token-do-not-log';
const KEY = 'key-id-123456';
const GB = 1e9;

/** A fetch that answers the GraphQL URL with `script(call)` and the TURN provider URL with stub credentials. */
function mockFetch(script) {
  const calls = [];
  const f = async (url, init) => {
    if (String(url).startsWith('https://api.cloudflare.com/client/v4/graphql')) {
      calls.push({ url: String(url), auth: init.headers.authorization, body: JSON.parse(init.body) });
      return script(calls.length, calls[calls.length - 1]);
    }
    return new Response(JSON.stringify({ iceServers: [{ urls: ['stun:stun.cloudflare.com:3478'] }, { urls: ['turn:turn.example.invalid:3478?transport=udp'], username: 'u', credential: 'c' }] }), { status: 200 });
  };
  f.calls = calls;
  return f;
}
const answer = (...egress) => new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: egress.map((e) => ({ sum: { egressBytes: e } })) }] } }, errors: null }), { status: 200 });

function setup(env = {}, script = () => answer(0)) {
  const fetchImpl = mockFetch(script);
  const logs = [];
  const s = makeServer({
    TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: KEY, TURN_KEY_API_TOKEN: 'rtc-token',
    TURN_USAGE_API_TOKEN: TOKEN, TURN_USAGE_ACCOUNT_ID: ACCOUNT, TURN_REAL_BUDGET_GB: '100',
    ...env,
  }, { deps: { fetchImpl, log: (l) => logs.push(l) } });
  return { s, fetchImpl, logs };
}
async function room(s, tag = '') {
  const a = await signUp(s, `Hosty${tag}`);
  const b = await signUp(s, `Guesty${tag}`);
  const { code } = await pairInRoom(s, a, b);
  return { a, b, code };
}
const mint = (s, r, who = 'a') => s.call('POST', '/v1/turn', { room: r.code }, r[who].token);

describe('measured TURN hard stop', () => {
  it('the query asks for this key and this month, and the token goes only to the Cloudflare API', async () => {
    const { s, fetchImpl } = setup({}, () => answer(5 * GB));
    const r = await room(s);
    expect((await mint(s, r)).status).toBe(200);
    expect(fetchImpl.calls).toHaveLength(1);
    const c = fetchImpl.calls[0];
    expect(c.auth).toBe(`Bearer ${TOKEN}`);
    expect(c.body.query).toContain('callsTurnUsageAdaptiveGroups');
    expect(c.body.query).toContain('egressBytes');
    expect(c.body.query).toContain(`keyId: "${KEY}"`);
    expect(c.body.query).toContain('date_geq: "2026-10-01"');
    expect(c.body.query).toContain('date_leq: "2026-10-30"');
    expect(c.body.query).toContain(`accountTag: "${ACCOUNT}"`);
  });

  it('refuses new credentials once the month is at the budget, and says so with 503 turn_budget', async () => {
    const { s } = setup({}, () => answer(60 * GB, 41 * GB));
    const r = await room(s);
    const m = await mint(s, r);
    expect(m.status).toBe(503);
    expect(m.data.error.code).toBe('turn_budget');
    expect(m.data.error.reason).toBe('usage');
    // nothing was reserved: the estimate counters did not move
    expect((await s.call('GET', '/v1/admin/stats', undefined, 'test-admin-token-0123456789')).data.turn.credsToday).toBe(0);
  });

  it('below the budget the credentials flow, and the reading is cached: one read per cache time, not per mint', async () => {
    const { s, fetchImpl } = setup({}, () => answer(10 * GB));
    const r = await room(s);
    for (let i = 0; i < 4; i++) expect((await mint(s, r, i % 2 ? 'b' : 'a')).status).toBe(200);
    expect(fetchImpl.calls).toHaveLength(1);
    s.advance(5 * 60_000 + 1000);
    expect((await mint(s, r, 'b')).status).toBe(200);
    expect(fetchImpl.calls).toHaveLength(2);
  });

  it('the stop opens again when a later reading is lower or the month changes', async () => {
    let used = 150 * GB;
    const { s } = setup({}, () => answer(used));
    const r = await room(s);
    expect((await mint(s, r)).status).toBe(503);
    s.advance(6 * 60_000);
    used = 50 * GB;
    expect((await mint(s, r)).status).toBe(200);
    used = 150 * GB;
    s.advance(6 * 60_000);
    expect((await mint(s, r, 'b')).status).toBe(503);
    s.advance(40 * 86400_000); // November: the October reading is not this month's, a new one is taken (and now says 10 GB)
    used = 10 * GB;
    const r2 = await room(s, '2');
    expect((await mint(s, r2)).status).toBe(200);
  });

  it('a failed read never lifts a block: the last good reading stays', async () => {
    let mode = 'over';
    const { s, logs } = setup({}, () => (mode === 'over' ? answer(200 * GB) : new Response('nope', { status: 500 })));
    const r = await room(s);
    expect((await mint(s, r)).status).toBe(503);
    mode = 'fail';
    s.advance(6 * 60_000);
    expect((await mint(s, r)).status).toBe(503); // the read failed (500), the old reading still says over
    expect(logs.some((l) => l.includes('usage read failed: http_500'))).toBe(true);
    const st = (await s.call('GET', '/v1/admin/stats', undefined, 'test-admin-token-0123456789')).data.turn.measured;
    expect(st.lastError).toBe('http_500');
    expect(st.usedBytes).toBe(200 * GB);
  });

  it('every failure shape is a failed read, never zero: HTTP error, GraphQL errors, bad shape, bad value, bad JSON, network error, timeout', async () => {
    const shapes = [
      () => new Response('x', { status: 403 }),
      () => new Response(JSON.stringify({ data: null, errors: [{ message: 'denied' }] }), { status: 200 }),
      () => new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [{ sum: { egressBytes: 1 } }] }] } }, errors: [{ message: 'partial' }] }), { status: 200 }), // data AND errors: not trusted
      () => new Response(JSON.stringify({ data: { viewer: { accounts: [] } } }), { status: 200 }),
      () => new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: null }] } } }), { status: 200 }),
      () => new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [{ sum: { egressBytes: 'lots' } }] }] } } }), { status: 200 }),
      () => new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [{ sum: { egressBytes: -5 } }] }] } } }), { status: 200 }),
      () => new Response('<html>', { status: 200 }),
      () => { throw new Error('connection reset'); },
    ];
    for (const sh of shapes) {
      const { s } = setup({ TURN_USAGE_REQUIRED: '1' }, sh);
      const r = await room(s);
      const m = await mint(s, r);
      // strict mode: a read that cannot be trusted means no new credentials (and never "zero used")
      expect(m.status).toBe(503);
      expect(m.data.error.code).toBe('turn_budget');
      const st = (await s.call('GET', '/v1/admin/stats', undefined, 'test-admin-token-0123456789')).data.turn.measured;
      expect(st.lastError).toBeTruthy();
      expect(st.usedBytes).toBeNull();
    }
  });

  it('default (not strict): a failed read leaves the estimate guard in charge, and does not open anything the estimate refuses', async () => {
    const { s } = setup({ TURN_MAX_CREDS_PER_DAY: '2', TURN_UNNEEDED_SHARE_PCT: '100', TURN_MAX_CREDS_PER_ROOM_HOUR: '0' }, () => new Response('x', { status: 502 }));
    const r = await room(s);
    expect((await mint(s, r)).status).toBe(200);
    expect((await mint(s, r, 'b')).status).toBe(200);
    const third = await mint(s, r);
    expect(third.status).toBe(503); // the daily estimate cap still holds
    expect(third.data.error.message).toContain('today');
  });

  it('a failure is retried after 60 s, not on every mint, and one read is in flight at most', async () => {
    const { s, fetchImpl } = setup({}, () => new Response('x', { status: 500 }));
    const r = await room(s);
    await Promise.all([mint(s, r), mint(s, r, 'b')]);
    expect(fetchImpl.calls.length).toBe(1);
    await mint(s, r);
    expect(fetchImpl.calls.length).toBe(1);
    s.advance(61_000);
    await mint(s, r);
    expect(fetchImpl.calls.length).toBe(2);
  });

  it('the reading is stored: a rebuilt object does not read again within the cache time and still refuses', async () => {
    const { s, fetchImpl } = setup({}, () => answer(500 * GB));
    const r = await room(s);
    expect((await mint(s, r)).status).toBe(503);
    const app2 = createApp({ sql: s.store.sql, tx: s.store.tx, env: s.env, now: () => s.clock.now, fetchImpl });
    const resp = await app2.handle(new Request('https://x.invalid/v1/turn', { method: 'POST', body: JSON.stringify({ room: r.code }), headers: { authorization: `Bearer ${r.b.token}`, 'cf-connecting-ip': '203.0.113.5', 'content-type': 'application/json' } }));
    expect(resp.status).toBe(503);
    expect(fetchImpl.calls.length).toBe(1);
  });

  it('kill switch TURN_USAGE_GUARD=0 and budget 0 turn the measured guard off; without the secrets nothing is read', async () => {
    for (const env of [{ TURN_USAGE_GUARD: '0' }, { TURN_REAL_BUDGET_GB: '0' }, { TURN_USAGE_API_TOKEN: '' }, { TURN_USAGE_ACCOUNT_ID: '' }]) {
      const { s, fetchImpl } = setup(env, () => answer(900 * GB));
      const r = await room(s);
      expect((await mint(s, r)).status).toBe(200);
      expect(fetchImpl.calls.length).toBe(0);
    }
  });

  it('a malformed account id or key id is a bad configuration: nothing is sent, nothing is opened or closed', async () => {
    const { s, fetchImpl } = setup({ TURN_USAGE_ACCOUNT_ID: 'x" } ) { evil' }, () => answer(900 * GB));
    const r = await room(s);
    expect((await mint(s, r)).status).toBe(200);
    expect(fetchImpl.calls.length).toBe(0);
    expect(buildQuery({ accountId: ACCOUNT, keyId: 'bad key"', scope: 'key' }, Date.UTC(2026, 10, 5))).toBeNull();
  });

  it('scope=account sums every key of the account (no keyId filter)', async () => {
    const { s, fetchImpl } = setup({ TURN_USAGE_SCOPE: 'account' }, () => answer(1 * GB));
    const r = await room(s);
    await mint(s, r);
    expect(fetchImpl.calls[0].body.query).not.toContain('keyId');
  });

  it('the log carries the percentage once per step and never the token, the account or the key', async () => {
    let used = 60 * GB;
    const { s, logs } = setup({}, () => answer(used));
    const r = await room(s);
    await mint(s, r);
    s.advance(6 * 60_000);
    used = 95 * GB;
    await mint(s, r, 'b');
    const text = logs.join('\n');
    expect(text).toMatch(/measured relay egress 60\.0 GB of 100 GB \(60 %\)/);
    expect(text).toMatch(/\(95 %\)/);
    for (const secret of [TOKEN, ACCOUNT, KEY, 'rtc-token']) expect(text).not.toContain(secret);
  });

  it('admin: POST /v1/admin/turn-usage forces a read (at most one every 10 s) and shows the status; GET stats has it too', async () => {
    const { s, fetchImpl } = setup({}, () => answer(12 * GB));
    const admin = 'test-admin-token-0123456789';
    const r = await s.call('POST', '/v1/admin/turn-usage', undefined, admin);
    expect(r.status).toBe(200);
    expect(r.data.turn.measured).toMatchObject({ configured: true, queryOk: true, usedBytes: 12 * GB, budgetGb: 100, lastError: null });
    await s.call('POST', '/v1/admin/turn-usage', undefined, admin);
    expect(fetchImpl.calls.length).toBe(1);
    s.advance(11_000);
    await s.call('POST', '/v1/admin/turn-usage', undefined, admin);
    expect(fetchImpl.calls.length).toBe(2);
    expect(JSON.stringify(r.data)).not.toContain(TOKEN);
    expect(JSON.stringify(r.data)).not.toContain(ACCOUNT);
  });

  it('parseUsage: string numbers (GraphQL Int64 comes as a string on some stacks) and several groups are summed', () => {
    expect(parseUsage({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [{ sum: { egressBytes: '5' } }, { sum: { egressBytes: 7 } }] }] } } })).toEqual({ bytes: 12, groups: 2 });
    expect(parseUsage({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [] }] } } })).toEqual({ bytes: 0, groups: 0 });
    expect(parseUsage(null).error).toBe('bad_shape');
  });

  it('an empty answer for the month although relays were minted is suspect: flagged, logged, estimate guard stays', async () => {
    const empty = () => new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [] }] } } }), { status: 200 });
    const { s, logs } = setup({ TURN_MAX_CREDS_PER_DAY: '1000', TURN_MAX_CREDS_PER_PLAYER_DAY: '1000', TURN_UNNEEDED_SHARE_PCT: '100', TURN_MAX_CREDS_PER_ROOM_HOUR: '0' }, empty);
    const admin = 'test-admin-token-0123456789';
    const r = await room(s);
    expect((await mint(s, r)).status).toBe(200);
    expect((await s.call('GET', '/v1/admin/stats', undefined, admin)).data.turn.measured.suspectEmpty).toBe(false); // one credential: not enough to say
    for (let i = 0; i < 20; i++) await mint(s, r, i % 2 ? 'b' : 'a');
    const m = (await s.call('GET', '/v1/admin/stats', undefined, admin)).data.turn.measured;
    expect(m.suspectEmpty).toBe(true);
    expect(m.usedBytes).toBe(0);
    expect(logs.some((l) => l.includes('measured usage is EMPTY'))).toBe(true);
  });
});
