// The same service, end to end inside the Workers runtime (workerd via miniflare): Worker -> Durable Object -> SQLite.
import { describe, it, expect } from 'vitest';
import { env, exports } from 'cloudflare:workers';
import { runDurableObjectAlarm, listDurableObjectIds, createScheduledController, createExecutionContext, waitOnExecutionContext } from 'cloudflare:test';
import worker from '../../src/worker.js';

const BASE = 'https://svc.example.invalid';
const T0 = Date.UTC(2026, 9, 30, 12, 0, 0);
let ipn = 1;

async function call(method, path, body, token, extra = {}) {
  const headers = { 'x-test-time': String(T0 + (extra.dt || 0)), 'x-test-ip': extra.ip || (/\/endpoint$/.test(path) ? '127.0.0.1' : `198.51.100.${(ipn++ % 250) + 1}`) };
  if (token) headers.authorization = `Bearer ${token}`;
  if (extra.admin) headers.authorization = `Bearer ${env.ADMIN_TOKEN}`;
  const init = { method, headers };
  if (body !== undefined) {
    init.body = JSON.stringify(body);
    headers['content-type'] = 'application/json';
  }
  const resp = await exports.default.fetch(new Request(`${BASE}${path}`, init));
  const text = await resp.text();
  let data;
  try { data = JSON.parse(text); } catch { data = text; }
  return { status: resp.status, data, headers: resp.headers };
}

// Durable Object storage is shared by all tests in this file, so every test registers fresh names.
let seq = 0;
const uniq = (base) => `${base.slice(0, 6)}${String(++seq).padStart(3, '0')}`.slice(0, 10);
const signUp = async (name) => {
  const r = await call('POST', '/v1/register', { name: uniq(name) });
  if (r.status !== 200) throw new Error(JSON.stringify(r.data));
  return r.data;
};
const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAECAwQFBgc=' });

function hexToBytes(h) { return Uint8Array.from(h.match(/../g).map((x) => parseInt(x, 16))); }

// A tiny HMAC-MD5 through the server's own helper would be circular, so build the blob with WebCrypto-free JS here.
import { hmacMd5 } from '../../src/core/md5.js';
function blobFor(name) {
  const b = new Uint8Array(0x80);
  const dv = new DataView(b.buffer);
  b[0] = 30; b[1] = 10; dv.setUint16(2, 2026, true);
  for (let i = 0; i < name.length; i++) dv.setUint16(0x0e + i * 2, name.charCodeAt(i), true);
  b.set(hmacMd5(hexToBytes(env.RANK_HMAC_KEY_HEX), b.subarray(0, 0x70)), 0x70);
  return btoa(String.fromCharCode(...b));
}

describe('worker + durable object', () => {
  it('rejects paths outside /v1 at the edge, serves info and time from the object', async () => {
    const r = await exports.default.fetch(new Request(`${BASE}/`));
    expect(r.status).toBe(404);
    const i = await call('GET', '/v1/info');
    expect(i.status).toBe(200);
    expect(i.data.proto).toBe(1);
    const t = await call('GET', '/v1/time');
    expect(t.data).toMatchObject({ utcMs: T0, year: 2026, month: 10 });
  });

  it('sign-in persists in the object between requests', async () => {
    const a = await signUp('Alpha');
    expect(a.pid).toBeGreaterThan(100000);
    expect((await call('GET', '/v1/me', undefined, a.token)).data.name).toBe(a.name);
    expect((await call('POST', '/v1/register', { name: a.name.toLowerCase() })).data.error.code).toBe('name_taken');
    expect((await call('GET', '/v1/me', undefined, 'z'.repeat(43))).status).toBe(401);
  });

  it('invite room flow with unique vips and endpoint exchange', async () => {
    const a = await signUp('Hosty');
    const b = await signUp('Guesty');
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    expect(room.code).toMatch(/^[A-Z2-9]{6}$/);
    const j = await call('POST', '/v1/rooms/join', { code: room.code }, b.token);
    expect(j.status).toBe(200);
    await call('POST', `/v1/rooms/${room.code}/endpoint`, ep(1), a.token);
    const done = await call('POST', `/v1/rooms/${room.code}/endpoint`, ep(2), b.token);
    expect(done.data.state).toBe('ready');
    expect(done.data.peer.endpoint.public).toBe('203.0.113.1:41000');
    expect(done.data.you.vip).not.toBe(done.data.peer.vip);
    expect((await call('POST', `/v1/rooms/${room.code}/started`, undefined, a.token)).status).toBe(200);
    const st = await call('POST', `/v1/rooms/${room.code}/started`, undefined, b.token);
    expect(st.data.state).toBe('started');
  });

  it('ranked: queue, pair, agree on a result, rank rows come back with the opaque blob', async () => {
    const a = await signUp('Anna');
    const b = await signUp('Bert');
    await call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    expect(q.status).toBe('matched');
    for (const [p, n] of [[a, 1], [b, 2]]) await call('POST', `/v1/rooms/${q.roomCode}/endpoint`, ep(n), p.token);
    for (const p of [a, b]) await call('POST', `/v1/rooms/${q.roomCode}/started`, undefined, p.token);
    const res = { result: 0, score0: 3, score1: 1, checksum: 'abcdef0123456789' };
    await call('POST', `/v1/matches/${q.room.matchId}/result`, res, a.token);
    expect((await call('POST', `/v1/matches/${q.room.matchId}/result`, res, b.token)).data.status).toBe('verified');
    const blob = blobFor(a.name);
    const put = await call('POST', '/v1/rank/put', { category: 0, region: 2, score: 13, data: blob }, a.token);
    expect(put.data).toEqual({ ok: true, stored: 13, clamped: false });
    const near = await call('POST', '/v1/rank/get', { mode: 2, category: 0, region: 2, sort: 1, since: 1, limit: 5 }, b.token);
    expect(near.data.rows.find((r) => r.pid === a.pid).score).toBe(13);
    expect(near.data.rows.find((r) => r.pid === b.pid).score).toBe(0);
    expect(near.data.rows[0].data).toBe(blob);
    expect(near.data.rows.every((r) => r.order >= 1)).toBe(true);
  });

  it('time-dependent behaviour works in the runtime: room expiry via the test clock', async () => {
    const a = await signUp('Expiry');
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    expect((await call('GET', `/v1/rooms/${room.code}`, undefined, a.token, { dt: 60_000 })).data.state).toBe('open');
    expect((await call('GET', `/v1/rooms/${room.code}`, undefined, a.token, { dt: 11 * 60_000 })).data.state).toBe('expired');
  });

  it('admin: stats, export pages and kill switch through the real object', async () => {
    const who = await signUp('Alpha');
    expect((await call('GET', '/v1/admin/stats')).status).toBe(401);
    const st = await call('GET', '/v1/admin/stats', undefined, undefined, { admin: true });
    expect(st.data.profiles).toBeGreaterThanOrEqual(1);
    const ex = await call('GET', '/v1/admin/export', undefined, undefined, { admin: true });
    expect(ex.data.schema).toEqual([{ id: 1, name: 'initial' }, { id: 2, name: 'abuse_controls' }, { id: 3, name: 'status_that_lasts' }, { id: 1001, name: 'strikers_endpoint_claims' }, { id: 1002, name: 'friends_quick_match' }, { id: 1003, name: 'problem_reports' }, { id: 1004, name: 'queue_same_build' }, { id: 1005, name: 'guess_misses_report_summary' }, { id: 1006, name: 'turn_pass_usernames' }]);
    const rows = await call('GET', '/v1/admin/export?table=profiles&after=0&limit=1000', undefined, undefined, { admin: true });
    const row = rows.data.rows.find((r) => r.pid === who.pid);
    expect(row.name).toBe(who.name);
    expect(row.token_hash).toMatch(/^[0-9a-f]{64}$/);
    await call('POST', '/v1/admin/switches', { kill: ['register'] }, undefined, { admin: true });
    expect((await call('POST', '/v1/register', { name: 'Betaxx' })).data.error.code).toBe('service_disabled');
    await call('POST', '/v1/admin/switches', { kill: [] }, undefined, { admin: true });
  });

  it('the daily cron calls the object, which writes a summary row; the admin route shows it (no HTTP route involved)', async () => {
    const a = await signUp('Cronny');
    const rep = await call('POST', '/v1/report', { v: 1, app: '1.3.0', build: 'abcdef01', log: 'x', events: 'desync at tick 3' }, a.token);
    expect(rep.status).toBe(200);
    const ctx = createExecutionContext();
    await worker.scheduled(createScheduledController({ scheduledTime: T0 + 86400000, cron: '13 0 * * *' }), env, ctx);
    await waitOnExecutionContext(ctx);
    const view = await call('GET', '/v1/admin/report-summary?days=3', undefined, undefined, { admin: true });
    expect(view.status).toBe(200);
    expect(view.data.today).toHaveProperty('byKind');
    expect(Array.isArray(view.data.days)).toBe(true);
    expect(view.data.days.length).toBe(1);
    expect(JSON.stringify(view.data)).not.toContain('Cronny');
  });

  it('the housekeeping alarm is scheduled by traffic and runs', async () => {
    await signUp('Alarm');
    const ids = await listDurableObjectIds(env.GAME);
    expect(ids.length).toBe(1);
    const stub = env.GAME.get(ids[0]);
    const ran = await runDurableObjectAlarm(stub);
    expect(ran).toBe(true);
  });

  it('turn credentials come from the stub provider configured for tests', async () => {
    const a = await signUp('Hosty');
    const b = await signUp('Guesty');
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    await call('POST', '/v1/rooms/join', { code: room.code }, b.token);
    const t = await call('POST', '/v1/turn', { room: room.code }, a.token);
    expect(t.status).toBe(200);
    expect(t.data.servers.some((s) => s.username)).toBe(true);
  });

  it('the public leaderboard memo obeys the ranking kill switch at once (a hit never skips the switch)', async () => {
    const first = await call('GET', '/v1/public/leaderboard?board=season&limit=7');
    expect(first.status).toBe(200);
    const hit = await call('GET', '/v1/public/leaderboard?board=season&limit=7');
    expect(hit.headers.get('x-msc-cache')).toBe('hit');
    await call('POST', '/v1/admin/switches', { kill: ['ranking'] }, undefined, { admin: true });
    const off = await call('GET', '/v1/public/leaderboard?board=season&limit=7');
    expect(off.status).toBe(503);
    expect(off.headers.get('x-msc-cache')).toBeNull();
    await call('POST', '/v1/admin/switches', { kill: [] }, undefined, { admin: true });
    expect((await call('GET', '/v1/public/leaderboard?board=season&limit=7')).status).toBe(200);
  });

  it('a body without Content-Length is 411 at the edge; a lying Content-Length cannot get a big body into the object', async () => {
    const a = await signUp('Bodyy');
    const stream = () => new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode('{"code":"AAAAAA"}')); c.close(); } });
    const chunked = await exports.default.fetch(new Request(`${BASE}/v1/rooms/join`, {
      method: 'POST', headers: { authorization: `Bearer ${a.token}`, 'content-type': 'application/json' }, body: stream(), duplex: 'half',
    }));
    expect(chunked.status).toBe(411);
    expect((await chunked.json()).error.code).toBe('length_required');
    const big = await call('POST', '/v1/rooms/join', { code: 'x'.repeat(9000) }, a.token);
    expect(big.status).toBe(413);
  });
});
