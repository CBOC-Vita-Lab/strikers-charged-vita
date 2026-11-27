// Wave 5 hardening ported from the Galaxy review of the shared service code: endpoint claims, request bodies, the edge token bucket, the idle sweep.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, pairInRoom } from '../helpers.js';
import { edgeCheck } from '../../src/core/edge.js';
import { readBodyCapped } from '../../src/core/util.js';
import { createEdgeBucket, createEdgeLimiter } from '../../src/core/edge_bucket.js';
import { MIGRATIONS } from '../../src/core/schema.js';

const ADMIN = 'test-admin-token-0123456789';
const ep = (n, extra = {}) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAAAAAAAAAA=', ...extra });

describe('endpoint claims', () => {
  const post = (s, code, p, body, ip) => s.call('POST', `/v1/rooms/${code}/endpoint`, body, p.token, { ip });
  const view = (s, code, p) => s.call('GET', `/v1/rooms/${code}`, undefined, p.token);
  async function pair(s) {
    const h = await signUp(s, 'Hosty');
    const g = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, h, g);
    return { h, g, code };
  }

  it('a public address that is not the caller\'s own puts the member on the relay: no addresses exchanged either way', async () => {
    const s = makeServer();
    const { h, g, code } = await pair(s);
    await post(s, code, h, ep(1), '203.0.113.1');
    const v = (await post(s, code, g, ep(2, { public: '198.51.100.77:41000' }), '203.0.113.2')).data; // names somebody else's address
    expect(v.session.steer).toEqual({ relay: true, reason: 'endpoint_unverified' });
    expect(v.peer.endpoint.public).toBeUndefined(); // the guest is not given the host's address either
    const hv = (await view(s, code, h)).data;
    expect(hv.session.steer).toEqual({ relay: true, reason: 'endpoint_unverified' });
    expect(hv.peer.endpoint.public).toBeUndefined();
    expect(hv.peer.endpoint.local).toBeUndefined();
    expect(JSON.stringify(hv)).not.toContain('198.51.100.77');
    expect(hv.peer.endpoint.nonce).toBeTruthy(); // the relay path still needs the pair's nonces
    // posting the right address again clears it
    const ok = (await post(s, code, g, ep(2), '203.0.113.2')).data;
    expect(ok.session.steer).toEqual({ relay: false, reason: null });
    expect(ok.peer.endpoint.public).toBe('203.0.113.1:41000');
  });

  it('a public candidate (srflx / observed) that is not the caller\'s address is refused the same way; host candidates are not checked', async () => {
    const s = makeServer();
    const { h, g, code } = await pair(s);
    await post(s, code, h, ep(1), '203.0.113.1');
    const bad = (await post(s, code, g, ep(2, { candidates: [{ type: 'srflx', addr: '192.0.2.9:5000' }] }), '203.0.113.2')).data;
    expect(bad.session.steer.reason).toBe('endpoint_unverified');
    const bad2 = (await post(s, code, g, ep(2, { candidates: [{ type: 'observed', addr: '192.0.2.9:5000' }] }), '203.0.113.2')).data;
    expect(bad2.session.steer.reason).toBe('endpoint_unverified');
    const fine = (await post(s, code, g, ep(2, { candidates: [{ type: 'host', addr: '10.0.0.5:5000' }, { type: 'srflx', addr: '203.0.113.2:5001' }] }), '203.0.113.2')).data;
    expect(fine.session.steer.relay).toBe(false);
  });

  it('an IPv6 caller cannot prove an IPv4 claim: relay', async () => {
    const s = makeServer();
    const { h, g, code } = await pair(s);
    await post(s, code, h, ep(1), '203.0.113.1');
    expect((await post(s, code, g, ep(2), '2001:db8::7')).data.session.steer.reason).toBe('endpoint_unverified');
  });

  it('no address or a loopback address (the local fake service, the tests) is not checked', async () => {
    const s = makeServer();
    const { h, g, code } = await pair(s);
    await post(s, code, h, ep(1), '127.0.0.1');
    const v = (await post(s, code, g, ep(2), '127.0.0.1')).data;
    expect(v.session.steer.relay).toBe(false);
    expect(v.peer.endpoint.public).toBe('203.0.113.1:41000');
    const s2 = makeServer();
    const r2 = await pair(s2);
    const noHeader = await s2.app.handle(new Request(`https://test.invalid/v1/rooms/${r2.code}/endpoint`, { method: 'POST', headers: { authorization: `Bearer ${r2.h.token}`, 'content-type': 'application/json' }, body: JSON.stringify(ep(1)) }));
    expect((await noHeader.json()).session.steer.relay).toBe(false);
  });

  it('a relay-only account is not affected (it sends no direct address)', async () => {
    const s = makeServer();
    const { h, g, code } = await pair(s);
    await s.call('PUT', '/v1/me', { relayOnly: true }, g.token);
    await post(s, code, h, ep(1), '203.0.113.1');
    const v = (await post(s, code, g, { relay: '198.51.100.9:3478', nonce: 'AAAAAAAAAAA=' }, '192.0.2.50')).data;
    expect(v.session.steer.reason).toBe('relay_only');
  });

  it('a relay claim or relay-type candidate must be a public address (no private, LAN, loopback, link-local, CGNAT)', async () => {
    const s = makeServer();
    const { h, code } = await pair(s);
    const bads = ['10.0.0.5:3478', '172.16.0.1:3478', '172.31.255.1:3478', '192.168.1.1:3478', '127.0.0.1:3478', '169.254.1.1:3478', '100.64.0.1:3478', '100.127.255.254:3478', '0.0.0.0:3478', '224.0.0.1:3478'];
    for (const a of bads) {
      const r1 = await post(s, code, h, ep(1, { relay: a }), '203.0.113.1');
      expect([a, r1.status, r1.data.error && r1.data.error.code]).toEqual([a, 400, 'bad_endpoint']);
      const r2 = await post(s, code, h, ep(1, { candidates: [{ type: 'relay', addr: a }] }), '203.0.113.1');
      expect([a, r2.status, r2.data.error && r2.data.error.code]).toEqual([a, 400, 'bad_endpoint']);
    }
    // the edges of the ranges are public: 100.63 / 100.128, 172.15 / 172.32
    for (const a of ['100.63.0.1:3478', '100.128.0.1:3478', '172.15.0.1:3478', '172.32.0.1:3478', '198.51.100.9:3478']) {
      expect((await post(s, code, h, ep(1, { relay: a }), '203.0.113.1')).status).toBe(200);
    }
    // host / srflx candidates keep their LAN addresses (that is what a host candidate is)
    const ok = await post(s, code, h, ep(1, { candidates: [{ type: 'host', addr: '192.168.1.7:41000' }, { type: 'relay', addr: '198.51.100.9:3478' }] }), '203.0.113.1');
    expect(ok.status).toBe(200);
  });

  it('ENDPOINT_CHECK=0 trusts the claims again (the 1.3 builds\' behaviour)', async () => {
    const s = makeServer({ ENDPOINT_CHECK: '0' });
    const { h, g, code } = await pair(s);
    await post(s, code, h, ep(1), '203.0.113.1');
    const v = (await post(s, code, g, ep(2, { public: '198.51.100.77:41000' }), '203.0.113.2')).data;
    expect(v.session.steer.relay).toBe(false);
    expect(v.peer.endpoint.public).toBe('203.0.113.1:41000');
  });

  it('the LAN address goes only to a member behind the same public address', async () => {
    const s = makeServer();
    const { h, g, code } = await pair(s);
    const cand = [{ type: 'host', addr: '192.168.1.1:41000' }, { type: 'srflx', addr: '203.0.113.1:41000' }];
    await post(s, code, h, ep(1, { candidates: cand }), '203.0.113.1');
    await post(s, code, g, ep(2), '203.0.113.2'); // another router
    let gv = (await view(s, code, g)).data.peer.endpoint;
    expect(gv.public).toBe('203.0.113.1:41000');
    expect(gv.local).toBeUndefined();
    expect(gv.candidates).toEqual([{ type: 'srflx', addr: '203.0.113.1:41000' }]);
    // the same public address for both (one household): the LAN is shared
    const s2 = makeServer();
    const p2 = await pair(s2);
    await post(s2, p2.code, p2.h, ep(1, { candidates: cand }), '203.0.113.1');
    await post(s2, p2.code, p2.g, ep(1, { public: '203.0.113.1:41001', local: '192.168.1.2:41000' }), '203.0.113.1');
    gv = (await view(s2, p2.code, p2.g)).data.peer.endpoint;
    expect(gv.local).toBe('192.168.1.1:41000');
    expect(gv.candidates).toEqual(cand);
  });

  it('a guest who is replaced by leaving for another room takes its claim state with it', async () => {
    const s = makeServer();
    const { h, g, code } = await pair(s);
    await post(s, code, h, ep(1), '203.0.113.1');
    expect((await post(s, code, g, ep(2, { public: '198.51.100.77:41000' }), '203.0.113.2')).data.session.steer.relay).toBe(true);
    await s.call('POST', '/v1/rooms', undefined, g.token); // the guest opens a room of its own: it leaves the first one (resetGuest)
    const row = s.store.sql.exec(`SELECT join_src, join_forced, state FROM rooms WHERE code = ?`, code).toArray()[0];
    expect(row).toMatchObject({ join_src: null, join_forced: 0, state: 'open' });
    const h2 = await signUp(s, 'Third');
    await s.call('POST', '/v1/rooms/join', { code }, h2.token);
    await post(s, code, h2, ep(3), '203.0.113.3');
    const v = (await view(s, code, h)).data;
    expect(v.session.steer).toEqual({ relay: false, reason: null }); // the old guest's flag does not stick to the new one
  });
});

describe('request bodies and the edge', () => {
  const base = { ADMIN_TOKEN: ADMIN, REQUIRE_APP_VERSION: '0' };
  const h = { authorization: `Bearer ${'a'.repeat(43)}` };

  it('POST / PUT without a numeric Content-Length is 411 at the edge; no body at all is fine', () => {
    const stream = () => new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode('{}')); c.close(); } });
    const chunked = new Request('https://x.invalid/v1/rooms/join', { method: 'POST', headers: h, body: stream(), duplex: 'half' });
    expect(chunked.headers.get('content-length')).toBeNull();
    expect(edgeCheck(chunked, base)).toMatchObject({ status: 411 });
    const chunkedPut = new Request('https://x.invalid/v1/me', { method: 'PUT', headers: h, body: stream(), duplex: 'half' });
    expect(edgeCheck(chunkedPut, base)).toMatchObject({ status: 411 });
    const bad = new Request('https://x.invalid/v1/rooms/join', { method: 'POST', headers: { ...h, 'content-length': 'abc' }, body: '{}' });
    expect(edgeCheck(bad, base)).toMatchObject({ status: 411 });
    const ok = new Request('https://x.invalid/v1/rooms/join', { method: 'POST', headers: { ...h, 'content-length': '2' }, body: '{}' });
    expect(edgeCheck(ok, base)).toBeNull();
    // what the game sends for a call without a body (np_https.cpp: no Content-Length, no body) stays fine
    expect(edgeCheck(new Request('https://x.invalid/v1/rooms/AAAAAA/started', { method: 'POST', headers: h }), base)).toBeNull();
    expect(edgeCheck(new Request('https://x.invalid/v1/rooms', { method: 'POST', headers: { ...h, 'content-length': '0' } }), base)).toBeNull();
    const big = new Request('https://x.invalid/v1/rooms/join', { method: 'POST', headers: { ...h, 'content-length': '9000' }, body: 'x'.repeat(9000) });
    expect(edgeCheck(big, base)).toMatchObject({ status: 413 });
  });

  it('the object caps the stream itself: a body longer than its Content-Length says is 413 and is not read to the end', async () => {
    let pulled = 0;
    const stream = new ReadableStream({ pull(c) { pulled += 1024; c.enqueue(new Uint8Array(1024).fill(120)); if (pulled > 1_000_000) c.close(); } });
    const req = new Request('https://x.invalid/', { method: 'POST', body: stream, duplex: 'half' });
    expect(await readBodyCapped(req, 8192)).toBeNull();
    expect(pulled).toBeLessThan(30_000);
    expect(await readBodyCapped(new Request('https://x.invalid/', { method: 'POST', body: '{"a":1}' }), 8192)).toBe('{"a":1}');
    expect(await readBodyCapped(new Request('https://x.invalid/'), 8192)).toBe('');
    // through the application: a liar (Content-Length 2, body 9000 bytes) cannot get a big body past it
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const r = await s.call('POST', '/v1/rooms/join', JSON.stringify({ code: 'x'.repeat(9000) }), a.token, { 'content-length': '2' });
    expect(r.status).toBe(413);
  });

  it('multi-byte bodies are capped in bytes, not characters', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ana');
    const r = await s.call('POST', '/v1/rooms/join', JSON.stringify({ code: 'é'.repeat(5000) }), a.token); // 10,000 bytes, 5,000 characters
    expect(r.status).toBe(413);
  });

  it('an unexpected exception is a bare 500 internal that still names the latest version', async () => {
    const s = makeServer({ LATEST_APP_VERSION: '1.3.0' });
    const a = await signUp(s, 'Ana');
    s.store.sql.exec(`DROP TABLE daily`); // makes the next rooms call throw inside the object
    const r = await s.call('POST', '/v1/rooms', undefined, a.token);
    expect(r.status).toBe(500);
    expect(r.data.error).toEqual({ code: 'internal', message: 'internal error', latestVersion: '1.3.0' });
  });

  it('the edge token bucket: burst, refill, per address, bounded memory, off with burst 0', () => {
    const b = createEdgeBucket({ burst: 5, perSec: 1 });
    for (let i = 0; i < 5; i++) expect(b.take('203.0.113.1', 1000)).toBe(0);
    expect(b.take('203.0.113.1', 1000)).toBeGreaterThanOrEqual(1);
    expect(b.take('203.0.113.2', 1000)).toBe(0);
    expect(b.take('203.0.113.1', 3000)).toBe(0);
    expect(b.take('203.0.113.1', 3000)).toBe(0);
    expect(b.take('203.0.113.1', 3000)).toBeGreaterThanOrEqual(1);
    for (let i = 0; i < 5; i++) b.take('2001:db8:1:2::99', 0);
    expect(b.take('2001:db8:1:2:ffff::1', 0)).toBeGreaterThanOrEqual(1); // IPv6 by /64
    for (let i = 0; i < 6000; i++) b.take(`10.${i >> 8}.${i & 255}.1`, 0);
    expect(b.size()).toBeLessThanOrEqual(4000);
    expect(createEdgeBucket({ burst: 0, perSec: 1 }).take('1.2.3.4', 0)).toBe(0);
  });

  it('the edge limiter: ten consoles behind one address are not throttled together, a token flood still is', () => {
    const tok = (i) => `Bearer ${String(i).padStart(43, 'a')}`;
    const lim = createEdgeLimiter({ burst: 200, perSec: 6, aggBurst: 400, aggPerSec: 30 });
    // 10 consoles, each polling about every 1.7 s plus a queue poll every 2 s (about 1.1 a second each, 11 a second together) for two minutes
    let refused = 0;
    for (let t = 0; t < 120000; t += 100) {
      for (let c = 0; c < 10; c++) {
        if ((t + c * 37) % 1700 < 100 && lim.take('198.51.100.7', tok(c), t) > 0) refused++;
        if ((t + c * 53) % 2000 < 100 && lim.take('198.51.100.7', tok(c), t) > 0) refused++;
      }
    }
    expect(refused).toBe(0);
    // one console that hammers is refused on its own bucket and does not use up the others
    const one = createEdgeLimiter({ burst: 5, perSec: 1, aggBurst: 400, aggPerSec: 30 });
    for (let i = 0; i < 5; i++) expect(one.take('203.0.113.9', tok(1), 0)).toBe(0);
    expect(one.take('203.0.113.9', tok(1), 0)).toBeGreaterThanOrEqual(1);
    expect(one.take('203.0.113.9', tok(2), 0)).toBe(0);
    // a flood with invented tokens gets fresh per-token buckets but hits the address's aggregate (400 burst, then 30 a second)
    const flood = createEdgeLimiter({ burst: 200, perSec: 6, aggBurst: 400, aggPerSec: 30 });
    let passed = 0;
    for (let i = 0; i < 5000; i++) if (flood.take('192.0.2.1', tok(i), 0) === 0) passed++;
    expect(passed).toBe(400);
    for (let i = 0; i < 5000; i++) if (flood.take('192.0.2.1', tok(10000 + i), 10000) === 0) passed++;
    expect(passed).toBe(400 + 300); // ten seconds refill 300
    // no token: the plain per-address bucket, 6 a second
    const plain = createEdgeLimiter({ burst: 5, perSec: 1 });
    for (let i = 0; i < 5; i++) expect(plain.take('192.0.2.2', '', 0)).toBe(0);
    expect(plain.take('192.0.2.2', '', 0)).toBeGreaterThanOrEqual(1);
    expect(plain.take('192.0.2.3', '', 0)).toBe(0);
    // burst 0 = off for everyone
    expect(createEdgeLimiter({ burst: 0, perSec: 1 }).take('1.2.3.4', tok(1), 0)).toBe(0);
    // memory stays bounded when many tokens come
    for (let i = 0; i < 9000; i++) lim.take(`10.${i >> 8}.${i & 255}.1`, tok(i), 0);
    expect(lim.size()).toBeLessThanOrEqual(12000);
  });
});

describe('sweep and schema', () => {
  it('the idle-account scan runs at most hourly; its index exists', async () => {
    const s = makeServer({ PRUNE_IDLE_DAYS: '1' });
    const idx = s.store.sql.exec(`PRAGMA index_list(profiles)`).toArray().map((r) => r.name);
    expect(idx).toContain('profiles_last_seen');
    const count = (pid) => s.store.sql.exec(`SELECT COUNT(*) AS n FROM profiles WHERE pid = ?`, pid).toArray()[0].n;
    const old = await signUp(s, 'Olly');
    s.advance(2 * 86400_000);
    s.app.sweep(); // the first sweep scans
    expect(count(old.pid)).toBe(0);
    const keep = await signUp(s, 'Kay');
    s.advance(2 * 86400_000);
    s.app.sweep(); // two days later: scans again
    expect(count(keep.pid)).toBe(0);
    const third = await signUp(s, 'Tom');
    s.advance(2 * 86400_000);
    s.app.sweep();
    expect(count(third.pid)).toBe(0);
    const late = await signUp(s, 'Lou');
    s.store.sql.exec(`UPDATE profiles SET last_seen_ms = ? WHERE pid = ?`, s.clock.now - 3 * 86400_000, late.pid);
    s.advance(600_000);
    s.app.sweep(); // 10 minutes after the last scan: no scan, the old account stays
    expect(count(late.pid)).toBe(1);
    s.advance(3600_000);
    s.app.sweep();
    expect(count(late.pid)).toBe(0);
  });

  it('Strikers-only migrations keep clear of the ids Galaxy\'s branch uses (4, 5, 6)', () => {
    const ids = MIGRATIONS.map((m) => m.id);
    expect(ids).toEqual([...new Set(ids)]);
    expect(ids.filter((i) => i >= 4 && i < 1000)).toEqual([]);
    expect(ids).toContain(1001);
  });
});
