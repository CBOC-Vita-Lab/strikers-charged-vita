// Wave 15 (net15): the relay caps ported from Galaxy's svcfix3 S5 (per room per hour, per address per day, the unneeded-credential slice) and the
// revoke of the passes still valid when the measured hard stop trips. Stub provider and a mocked Cloudflare API; no network.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, pairInRoom, startMatch } from '../helpers.js';
import { loadConfig } from '../../src/core/config.js';

const ADMIN = 'test-admin-token-0123456789';
const GB = 1e9;
const mint = (s, p, code, ip) => s.call('POST', '/v1/turn', { room: code }, p.token, ip ? { ip } : {});
let n = 0;
async function room(s, started = false) {
  const a = await signUp(s, `Hst${++n}`);
  const b = await signUp(s, `Gst${n}`);
  const { code } = await pairInRoom(s, a, b);
  if (started) await startMatch(s, a, b, code); // a started room outlives the waiting room's short life, so a test can move the clock a day
  return { a, b, code };
}
const NO_SLICE = { TURN_UNNEEDED_SHARE_PCT: '100' };
const counterKeys = (s) => s.store.sql.exec(`SELECT key FROM counters`).toArray().map((r) => r.key);

describe('defaults', () => {
  it('6 a room an hour, 24 an address a day, half of the day for credentials without a visible reason; 0 turns a cap off', () => {
    const t = loadConfig({}).turn;
    expect([t.maxPerRoomHour, t.maxPerAddrDay, t.unneededSharePct]).toEqual([6, 24, 50]);
    const off = loadConfig({ TURN_MAX_CREDS_PER_ROOM_HOUR: '0', TURN_MAX_CREDS_PER_ADDR_DAY: '0', TURN_UNNEEDED_SHARE_PCT: '100' }).turn;
    expect([off.maxPerRoomHour, off.maxPerAddrDay, off.unneededSharePct]).toEqual([0, 0, 100]);
  });
});

describe('per room per clock hour', () => {
  it('the room is refused after its cap in one hour with 429 turn_room_limit, and the next clock hour is open again', async () => {
    const s = makeServer({ ...NO_SLICE, TURN_MAX_CREDS_PER_ROOM_HOUR: '3' });
    const r = await room(s, true);
    for (let i = 0; i < 3; i++) expect((await mint(s, i % 2 ? r.b : r.a, r.code)).status).toBe(200);
    const m = await mint(s, r.a, r.code);
    expect(m.status).toBe(429);
    expect(m.data.error.code).toBe('turn_room_limit');
    s.advance(3600_000); // T0 is 12:00:00 UTC: one hour on is the next clock hour
    expect((await mint(s, r.b, r.code)).status).toBe(200);
  });
  it('another room is not affected, and a refused request moves no counter', async () => {
    const s = makeServer({ ...NO_SLICE, TURN_MAX_CREDS_PER_ROOM_HOUR: '1' });
    const r1 = await room(s);
    const r2 = await room(s);
    expect((await mint(s, r1.a, r1.code)).status).toBe(200);
    const before = JSON.stringify(s.store.sql.exec(`SELECT key, value FROM counters ORDER BY key`).toArray());
    expect((await mint(s, r1.b, r1.code)).status).toBe(429);
    expect(JSON.stringify(s.store.sql.exec(`SELECT key, value FROM counters ORDER BY key`).toArray())).toBe(before);
    expect((await mint(s, r2.a, r2.code)).status).toBe(200);
  });
  it('0 turns the cap off', async () => {
    const s = makeServer({ ...NO_SLICE, TURN_MAX_CREDS_PER_ROOM_HOUR: '0', TURN_MAX_CREDS_PER_PLAYER_DAY: '50' });
    const r = await room(s);
    for (let i = 0; i < 12; i++) expect((await mint(s, r.a, r.code)).status).toBe(200);
  });
});

describe('per source address per day', () => {
  it('an address that has had its credentials today is refused (429 turn_player_limit) whatever the account, and the next day is open', async () => {
    const s = makeServer({ ...NO_SLICE, TURN_MAX_CREDS_PER_ADDR_DAY: '2', TURN_MAX_CREDS_PER_ROOM_HOUR: '0' });
    const ip = '203.0.113.77';
    const r1 = await room(s, true);
    const r2 = await room(s, true);
    expect((await mint(s, r1.a, r1.code, ip)).status).toBe(200);
    expect((await mint(s, r2.a, r2.code, ip)).status).toBe(200); // a different account and room, the same address
    const m = await mint(s, r1.b, r1.code, ip);
    expect(m.status).toBe(429);
    expect(m.data.error.message).toContain('address');
    expect((await mint(s, r1.b, r1.code, '203.0.113.78')).status).toBe(200); // another address
    s.advance(86400_000);
    const r3 = await room(s); // the old rooms have expired by now
    expect((await mint(s, r3.b, r3.code, ip)).status).toBe(200);
  });
  it('the address is never written: the counter key is a fingerprint', async () => {
    const s = makeServer({ ...NO_SLICE, TURN_MAX_CREDS_PER_ADDR_DAY: '5' });
    const r = await room(s);
    await mint(s, r.a, r.code, '203.0.113.99');
    const keys = counterKeys(s).filter((k) => /:a[0-9a-f]{16}$/.test(k));
    expect(keys).toHaveLength(1);
    expect(JSON.stringify(s.store.sql.exec(`SELECT * FROM counters`).toArray())).not.toContain('203.0.113.99');
    expect(JSON.stringify(s.store.sql.exec(`SELECT * FROM settings`).toArray())).not.toContain('203.0.113.99');
  });
  it('a loopback or missing address is not counted (the Node fake)', async () => {
    const s = makeServer({ ...NO_SLICE, TURN_MAX_CREDS_PER_ADDR_DAY: '1', TURN_MAX_CREDS_PER_ROOM_HOUR: '0' });
    const r = await room(s);
    for (let i = 0; i < 3; i++) expect((await mint(s, r.a, r.code, '127.0.0.1')).status).toBe(200);
    expect(counterKeys(s).filter((k) => /:a[0-9a-f]{16}$/.test(k))).toHaveLength(0);
  });
  it('a failed provider call gives the room, address and slice reservations back', async () => {
    let fail = true;
    const provider = { async mint() { if (fail) throw new Error('down'); return { servers: [{ urls: ['turn:x.invalid:3478'], username: 'u', credential: 'c' }], username: 'u' }; }, async revoke() { return true; } };
    const s = makeServer({ TURN_MAX_CREDS_PER_ADDR_DAY: '1', TURN_MAX_CREDS_PER_ROOM_HOUR: '1' }, { deps: { turnProvider: provider } });
    const r = await room(s);
    expect((await mint(s, r.a, r.code, '203.0.113.5')).status).toBe(502);
    fail = false;
    expect((await mint(s, r.a, r.code, '203.0.113.5')).status).toBe(200); // nothing was left reserved
    const rows = s.store.sql.exec(`SELECT key, value FROM counters WHERE key LIKE 'turn:%'`).toArray();
    expect(rows.every((x) => x.value === 1)).toBe(true);
  });
});

describe('the slice for credentials without a visible reason', () => {
  const punchFail = (s, r) => s.call('POST', `/v1/rooms/${r.code}/punch`, { result: 'fail', path: 'direct', ms: 100 }, r.a.token);
  it('with no reason they share half of the day; the 6th of a 10 a day budget is refused 503 turn_budget', async () => {
    const s = makeServer({ TURN_MAX_CREDS_PER_DAY: '10', TURN_MAX_CREDS_PER_PLAYER_DAY: '50', TURN_MAX_CREDS_PER_ROOM_HOUR: '0', TURN_MAX_CREDS_PER_ADDR_DAY: '0', TURN_NEW_ACCOUNT_HOURS: '0' });
    const r = await room(s);
    for (let i = 0; i < 5; i++) expect((await mint(s, r.a, r.code)).status).toBe(200);
    const m = await mint(s, r.b, r.code);
    expect(m.status).toBe(503);
    expect(m.data.error.code).toBe('turn_budget');
    expect(m.data.error.message).toContain('punch through');
    // a room with a visible reason (a recorded failed punch) still gets credentials, out of the rest of the day
    const rn = await room(s);
    expect((await punchFail(s, rn)).status).toBe(200);
    expect((await mint(s, rn.a, rn.code)).status).toBe(200);
  });
  it('a relay steer (relay-only account) or a published relay address counts as a reason', async () => {
    const s = makeServer({ TURN_MAX_CREDS_PER_DAY: '2', TURN_MAX_CREDS_PER_PLAYER_DAY: '50', TURN_MAX_CREDS_PER_ROOM_HOUR: '0', TURN_MAX_CREDS_PER_ADDR_DAY: '0', TURN_NEW_ACCOUNT_HOURS: '0' });
    const r = await room(s);
    expect((await mint(s, r.a, r.code)).status).toBe(200); // the one unneeded credential (50 % of 2)
    expect((await mint(s, r.a, r.code)).status).toBe(503);
    const rn = await room(s);
    const ep = await s.call('POST', `/v1/rooms/${rn.code}/endpoint`, { public: '203.0.113.9:41000', local: '192.168.1.9:41000', relay: '198.51.100.9:50000', nonce: 'AAAAAAAAAAA=' }, rn.a.token);
    expect(ep.status).toBe(200);
    expect((await mint(s, rn.b, rn.code)).status).toBe(200);
  });
  it('100 turns the slice off', async () => {
    const s = makeServer({ TURN_MAX_CREDS_PER_DAY: '10', TURN_UNNEEDED_SHARE_PCT: '100', TURN_MAX_CREDS_PER_PLAYER_DAY: '50', TURN_MAX_CREDS_PER_ROOM_HOUR: '0', TURN_NEW_ACCOUNT_HOURS: '0' });
    const r = await room(s);
    for (let i = 0; i < 10; i++) expect((await mint(s, r.a, r.code)).status).toBe(200);
  });
});

describe('revoking the valid passes when the measured stop trips', () => {
  const answer = (b) => new Response(JSON.stringify({ data: { viewer: { accounts: [{ callsTurnUsageAdaptiveGroups: [{ sum: { egressBytes: b } }] }] } }, errors: null }), { status: 200 });
  function setup(revokeStatus = () => 204) {
    const st = { used: 10 * GB, revokes: [], seq: 0 };
    const logs = [];
    const fetchImpl = async (url, init) => {
      url = String(url);
      if (url.startsWith('https://api.cloudflare.com/client/v4/graphql')) return answer(st.used);
      if (url.endsWith('/revoke')) { st.revokes.push(decodeURIComponent(url.split('/credentials/')[1].split('/revoke')[0])); return new Response(null, { status: revokeStatus(st.revokes.length) }); }
      return new Response(JSON.stringify({ iceServers: [{ urls: ['stun:stun.cloudflare.com:3478'] }, { urls: ['turn:turn.example.invalid:3478?transport=udp'], username: `user-${++st.seq}`, credential: 'c' }] }), { status: 200 });
    };
    const s = makeServer({
      TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'key-id-123456', TURN_KEY_API_TOKEN: 'rtc-token', TURN_USAGE_API_TOKEN: 'usage-token-x', TURN_USAGE_ACCOUNT_ID: 'a'.repeat(32), TURN_REAL_BUDGET_GB: '100',
      ...NO_SLICE, TURN_MAX_CREDS_PER_ROOM_HOUR: '0',
    }, { deps: { fetchImpl, log: (l) => logs.push(l) } });
    return { s, st, logs };
  }
  const sel = (s) => s.store.sql.exec(`SELECT username, revoked FROM turn_mints ORDER BY id`).toArray();

  it('every pass still valid is revoked once the month is over budget, and not again', async () => {
    const { s, st, logs } = setup();
    const r = await room(s);
    for (let i = 0; i < 3; i++) expect((await mint(s, i % 2 ? r.b : r.a, r.code)).status).toBe(200);
    expect(sel(s).map((x) => x.username)).toEqual(['user-1', 'user-2', 'user-3']);
    expect(st.revokes).toEqual([]);
    st.used = 150 * GB;
    s.advance(6 * 60_000); // the cached reading is old: a new one says over
    const m = await mint(s, r.a, r.code);
    expect(m.status).toBe(503);
    expect(m.data.error.reason).toBe('usage');
    expect(st.revokes.sort()).toEqual(['user-1', 'user-2', 'user-3']);
    expect(sel(s).every((x) => x.revoked === 1)).toBe(true);
    s.advance(61_000);
    expect((await mint(s, r.b, r.code)).status).toBe(503);
    expect(st.revokes).toHaveLength(3); // nothing left to revoke: no second call
    const st2 = (await s.call('GET', '/v1/admin/stats', undefined, ADMIN)).data.turn.measured;
    expect(st2.revokedPasses).toBe(3);
    expect(logs.join('\n')).not.toContain('user-1'); // no pass name in a log line
    expect(logs.join('\n')).not.toContain('usage-token-x');
  });
  it('passes older than their life are left alone, and a 404 on a valid pass is retried by the next sweep', async () => {
    const { s, st } = setup((k) => (k === 1 ? 404 : 204));
    const r = await room(s, true);
    await mint(s, r.a, r.code); // user-1
    s.advance(3600_000 + 1000);
    await mint(s, r.b, r.code); // user-2 (the cached reading is old: read again, still under budget)
    st.used = 150 * GB;
    s.advance(6 * 60_000);
    expect((await mint(s, r.b, r.code)).status).toBe(503);
    expect(st.revokes).toEqual(['user-2']); // user-1 is past its hour
    expect(sel(s).map((x) => x.revoked)).toEqual([0, 0]); // the 404 on the valid user-2: kept
    s.advance(61_000);
    await mint(s, r.b, r.code);
    expect(st.revokes).toEqual(['user-2', 'user-2']);
    expect(sel(s).map((x) => x.revoked)).toEqual([0, 1]);
  });
  it('under budget nothing is revoked; a failed read does not revoke', async () => {
    const { s, st } = setup();
    const r = await room(s);
    for (let i = 0; i < 4; i++) await mint(s, i % 2 ? r.b : r.a, r.code);
    expect(st.revokes).toEqual([]);
  });
});
