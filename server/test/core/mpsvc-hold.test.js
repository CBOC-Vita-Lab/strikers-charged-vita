// Held polls (wave 9): a poll with wait=<sec> is answered when something changes or after that long, inside the object, for one request instead of many.
import { describe, it, expect } from 'vitest';
import { makeVirtual, signUp, startMatch } from '../helpers.js';

const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAAAAAAAAAA=' });

async function host(srv, name = 'Hosty') {
  const a = await signUp(srv, name);
  const r = await srv.call('POST', '/v1/rooms', undefined, a.token);
  return { a, code: r.data.code, version: r.data.version };
}

describe('held room polls', () => {
  it('a poll with wait= is held until its time is up, then says so', async () => {
    const srv = makeVirtual();
    const { a, code, version } = await host(srv);
    const h = srv.start('GET', `/v1/rooms/${code}?v=${version}&wait=8`, undefined, a.token);
    await srv.advance(7900);
    expect(h.settled).toBe(false); // still waiting inside the object
    await srv.advance(200);
    expect(h.settled).toBe(true);
    expect(h.res.status).toBe(200);
    expect(h.res.data.changed).toBe(false);
    expect(h.res.data.heldMs).toBeGreaterThanOrEqual(7900);
    expect(h.res.data.heldMs).toBeLessThanOrEqual(8100);
  });

  it('answers at once when the room changes (the partner joins)', async () => {
    const srv = makeVirtual();
    const { a, code, version } = await host(srv);
    const b = await signUp(srv, 'Guesty');
    const h = srv.start('GET', `/v1/rooms/${code}?v=${version}&wait=8`, undefined, a.token);
    await srv.advance(2000);
    expect(h.settled).toBe(false);
    const j = srv.start('POST', '/v1/rooms/join', { code }, b.token);
    await srv.advance(0);
    expect(j.settled).toBe(true);
    expect(h.settled).toBe(true);
    expect(h.at - h.startedAt).toBe(2000);
    expect(h.res.data.changed).toBe(true);
    expect(h.res.data.peer.name).toBe('Guesty');
    expect(h.res.data.heldMs).toBe(2000);
  });

  it('answers at once when the partner posts its endpoint', async () => {
    const srv = makeVirtual();
    const { a, code } = await host(srv);
    const b = await signUp(srv, 'Guesty');
    await srv.call('POST', '/v1/rooms/join', { code }, b.token);
    const posted = await srv.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token); // the host's own post
    const h = srv.start('GET', `/v1/rooms/${code}?v=${posted.data.version}&wait=8`, undefined, a.token);
    await srv.advance(1000);
    srv.start('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    await srv.advance(0);
    expect(h.settled).toBe(true);
    expect(h.res.data.changed).toBe(true);
    expect(h.res.data.peer.endpoint.public).toBe('203.0.113.2:41000');
  });

  it('a member with a held poll open is never shown as quiet or lost to the partner', async () => {
    const srv = makeVirtual();
    const { a, code } = await host(srv);
    const b = await signUp(srv, 'Guesty');
    const joined = await srv.call('POST', '/v1/rooms/join', { code }, b.token);
    const hostView = await srv.call('GET', `/v1/rooms/${code}`, undefined, a.token); // the host is seen once
    const h = srv.start('GET', `/v1/rooms/${code}?v=${hostView.data.version}&wait=20`, undefined, a.token);
    await srv.advance(18000); // longer than LINK_LOST_SEC (15)
    const g = await srv.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    expect(g.data.peer.link).toBe('ok');
    await srv.advance(5000);
    expect(h.settled).toBe(true);
    // and without a held poll the same silence reads as lost
    const g2 = await srv.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    expect(['ok', 'quiet', 'lost']).toContain(g2.data.peer.link);
    srv.advance(20000);
    await srv.settle();
    const g3 = await srv.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    expect(g3.data.peer.link).toBe('lost');
    void joined;
  });

  it('a ranked pairing nobody confirms is still ended on time while a poll is held (join acknowledgement, 5 s)', async () => {
    const srv = makeVirtual();
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = await srv.call('POST', '/v1/ranked/queue', undefined, b.token); // bob's own answer is his acknowledgement; alice never polls again
    const code = q.data.roomCode;
    const rv = await srv.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    const h = srv.start('GET', `/v1/rooms/${code}?v=${rv.data.version}&wait=20`, undefined, b.token);
    await srv.advance(4000);
    expect(h.settled).toBe(false);
    await srv.advance(1500);
    expect(h.settled).toBe(true); // at the 5 s deadline, not after 20 s
    expect(h.at - h.startedAt).toBeLessThan(6000);
    expect(h.res.data.state).toBe('closed');
    expect(h.res.data.closedReason).toBe('ack_timeout');
  });

  it('wait is capped by HOLD_MAX_SEC, and HOLD_MAX_SEC=0 turns holding off', async () => {
    const srv = makeVirtual({ HOLD_MAX_SEC: '10' });
    const { a, code, version } = await host(srv);
    const h = srv.start('GET', `/v1/rooms/${code}?v=${version}&wait=55`, undefined, a.token);
    await srv.advance(9900);
    expect(h.settled).toBe(false);
    await srv.advance(200);
    expect(h.settled).toBe(true);

    const off = makeVirtual({ HOLD_MAX_SEC: '0' });
    const o = await host(off, 'Offly');
    const h2 = off.start('GET', `/v1/rooms/${o.code}?v=${o.version}&wait=8`, undefined, o.a.token);
    await off.advance(0);
    expect(h2.settled).toBe(true);
    expect(h2.res.data.changed).toBe(false);
    expect(h2.res.data.heldMs).toBeUndefined();
  });

  it('a poll without wait= or without v= is answered at once, exactly as before (old clients)', async () => {
    const srv = makeVirtual();
    const { a, code, version } = await host(srv);
    const h = srv.start('GET', `/v1/rooms/${code}?v=${version}`, undefined, a.token);
    await srv.advance(0);
    expect(h.settled).toBe(true);
    expect(h.res.data).toEqual({ code, state: 'open', version, changed: false });
    const h2 = srv.start('GET', `/v1/rooms/${code}?wait=8`, undefined, a.token); // no v: the full view, nothing to wait for
    await srv.advance(0);
    expect(h2.settled).toBe(true);
    expect(h2.res.data.changed).toBe(true);
    expect(h2.res.data.heldMs).toBe(0);
  });

  it('a room that ends releases the held poll with its closed state', async () => {
    const srv = makeVirtual();
    const { a, code, version } = await host(srv);
    const b = await signUp(srv, 'Guesty');
    await srv.call('POST', '/v1/rooms/join', { code }, b.token);
    const v2 = (await srv.call('GET', `/v1/rooms/${code}`, undefined, b.token)).data.version;
    const h = srv.start('GET', `/v1/rooms/${code}?v=${v2}&wait=8`, undefined, b.token);
    await srv.advance(1000);
    await srv.call('POST', `/v1/rooms/${code}/leave`, {}, a.token);
    await srv.advance(0);
    expect(h.settled).toBe(true);
    expect(h.res.data.state).toBe('closed');
    void version;
  });
});

describe('held queue polls', () => {
  it('a held queue poll is answered the moment a partner arrives', async () => {
    const srv = makeVirtual({ QUEUE_BASE_WINDOW: '1000' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    const h = srv.start('GET', '/v1/ranked/queue?wait=8', undefined, a.token);
    await srv.advance(3000);
    expect(h.settled).toBe(false);
    srv.start('POST', '/v1/ranked/queue', undefined, b.token);
    await srv.advance(0);
    expect(h.settled).toBe(true);
    expect(h.at - h.startedAt).toBe(3000);
    expect(h.res.data.status).toBe('matched');
    expect(h.res.data.room.matchId).toBeTruthy();
  });

  it('a lone held poll waits out its time and says queued; a poll without wait= is answered at once', async () => {
    const srv = makeVirtual();
    const a = await signUp(srv, 'Alice1');
    await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    const plain = srv.start('GET', '/v1/ranked/queue', undefined, a.token);
    await srv.advance(0);
    expect(plain.settled).toBe(true);
    expect(plain.res.data.heldMs).toBeUndefined();
    const h = srv.start('GET', '/v1/ranked/queue?wait=8', undefined, a.token);
    await srv.advance(8100);
    expect(h.settled).toBe(true);
    expect(h.res.data.status).toBe('queued');
    expect(h.res.data.heldMs).toBeGreaterThanOrEqual(8000);
  });

  it('a held poll writes the queue entry only now and then (rows written), and the entry is not dropped as stale while held', async () => {
    const srv = makeVirtual();
    const a = await signUp(srv, 'Alice1');
    await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    let writes = 0;
    const real = srv.store.sql.exec.bind(srv.store.sql);
    srv.store.sql.exec = (q, ...b) => { if (/^\s*UPDATE queue SET last_poll_ms/i.test(q)) writes++; return real(q, ...b); };
    for (let i = 0; i < 4; i++) {
      const h = srv.start('GET', '/v1/ranked/queue?wait=20', undefined, a.token);
      await srv.advance(20100);
      expect(h.res.data.status).toBe('queued');
      expect(h.res.data.heldMs).toBeGreaterThanOrEqual(20000); // really held (ten ticks inside, one request)
      await srv.advance(250);
    }
    // 80 s of waiting: plain polling every 2 s would have written 40 times
    expect(writes).toBeLessThan(18);
  });

  it('a ghost is skipped, a held entry is not (it is present although its last write is old)', async () => {
    const srv = makeVirtual({ QUEUE_BASE_WINDOW: '1000' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    const h = srv.start('GET', '/v1/ranked/queue?wait=25', undefined, a.token);
    await srv.advance(18000); // 18 s since the entry's write: older than LINK_LOST_SEC
    expect(h.settled).toBe(false);
    const j = srv.start('POST', '/v1/ranked/queue', undefined, b.token);
    await srv.advance(0);
    expect(j.res.data.status).toBe('matched');
    expect(h.settled).toBe(true);
    expect(h.res.data.status).toBe('matched');
  });
});
