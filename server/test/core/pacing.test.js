// Pacing hints (app.js paceHints, wave 13 O3): the service dictates `nextWaitSec` / `nextPollMs` to clients that ask (wait= or hints=1); everyone else gets
// byte for byte the old answers.
import { describe, it, expect } from 'vitest';
import { makeServer, makeVirtual, signUp, pairInRoom } from '../helpers.js';

/** a held poll in virtual time: start it, let `ms` pass, return its answer */
async function held(srv, method, path, token, ms = 1500) {
  const h = srv.start(method, path, undefined, token);
  await srv.advance(ms);
  expect(h.settled).toBe(true);
  return h.res.data;
}

describe('pacing hints', () => {
  it('a room poll with wait= carries the hold to ask for next (20 s before the start, 45 s once started) and the gap after a plain answer', async () => {
    const s = makeVirtual();
    const a = await signUp(s, 'PaceA');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    const first = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data;
    expect(first.nextWaitSec).toBeUndefined(); // no wait=: exactly as before
    const r = await held(s, 'GET', `/v1/rooms/${code}?v=${first.version}&wait=1`, a.token);
    expect(r.changed).toBe(false);
    expect(r.nextWaitSec).toBe(20);
    expect(r.nextPollMs).toBe(1700);
  });

  it('started rooms get 45 s; closed rooms no hint', async () => {
    const s = makeVirtual();
    const a = await signUp(s, 'PaceB');
    const b = await signUp(s, 'PaceC');
    const { code } = await pairInRoom(s, a, b);
    const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAAAAAAAAAA=' });
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    await s.call('POST', `/v1/rooms/${code}/started`, undefined, a.token);
    await s.call('POST', `/v1/rooms/${code}/started`, undefined, b.token);
    const r = await held(s, 'GET', `/v1/rooms/${code}?v=0&wait=1`, a.token);
    expect(r.state).toBe('started');
    expect(r.nextWaitSec).toBe(45);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
    const closed = await held(s, 'GET', `/v1/rooms/${code}?v=0&wait=1`, b.token);
    expect(['closed', 'expired']).toContain(closed.state);
    expect(closed.nextWaitSec).toBeUndefined();
  });

  it('the queue: 30 s while queued, none once paired; the friends list: 45 s; the match status: nextPollMs only with hints=1 and only while not final', async () => {
    const s = makeVirtual();
    const a = await signUp(s, 'PaceD');
    const b = await signUp(s, 'PaceE');
    expect((await s.call('POST', '/v1/ranked/queue', undefined, a.token)).status).toBe(200);
    const q = await held(s, 'GET', '/v1/ranked/queue?wait=1', a.token);
    expect(q.status).toBe('queued');
    expect(q.nextWaitSec).toBe(30);
    expect((await s.call('GET', '/v1/ranked/queue', undefined, a.token)).data.nextWaitSec).toBeUndefined();
    const f0 = (await s.call('GET', '/v1/friends', undefined, a.token)).data;
    const f = await held(s, 'GET', `/v1/friends?v=${f0.rev}&wait=1`, a.token);
    expect(f.nextWaitSec).toBe(45);
    const paired = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    const matchId = paired.room.matchId;
    const m1 = (await s.call('GET', `/v1/matches/${matchId}?hints=1`, undefined, a.token)).data;
    expect(m1.final).toBe(false);
    expect(m1.nextPollMs).toBe(2000);
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token)).data.nextPollMs).toBeUndefined();
    const gone = (await s.call('GET', '/v1/ranked/queue?wait=1', undefined, a.token)).data; // paired: answered at once
    expect(gone.status).not.toBe('queued');
    expect(gone.nextWaitSec).toBeUndefined();
  });

  it('the values are the server\'s to change (PACE_*), are scaled by pollScale on a busy day, and PACE_HINTS=0 stops them', async () => {
    const s = makeVirtual({ PACE_QUEUE_WAIT_SEC: '40', PACE_ROOM_GAP_MS: '3000' });
    const a = await signUp(s, 'PaceF');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect((await held(s, 'GET', '/v1/ranked/queue?wait=1', a.token)).nextWaitSec).toBe(40);
    const off = makeVirtual({ PACE_HINTS: '0' });
    const o = await signUp(off, 'PaceG');
    await off.call('POST', '/v1/ranked/queue', undefined, o.token);
    expect((await held(off, 'GET', '/v1/ranked/queue?wait=1', o.token)).nextWaitSec).toBeUndefined();
    // a busy day: 60 % of a small budget used -> pollScale above 1, the hold stretches (capped at 55 s)
    const busy = makeVirtual({ DAILY_REQUEST_BUDGET: '100', PACE_QUEUE_WAIT_SEC: '30' });
    const u = await signUp(busy, 'PaceH');
    await busy.call('POST', '/v1/ranked/queue', undefined, u.token);
    for (let i = 0; i < 60; i++) await busy.call('GET', '/v1/time');
    const r = await held(busy, 'GET', '/v1/ranked/queue?wait=1', u.token);
    expect(r.pollScale).toBeGreaterThan(1);
    expect(r.nextWaitSec).toBe(Math.min(55, Math.round(30 * r.pollScale)));
  });

  it('no hint is sent on routes that never carry one, and an answer without wait= is unchanged (old clients)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'PaceI');
    const me = (await s.call('GET', '/v1/me?wait=3', undefined, a.token)).data;
    expect(me.nextWaitSec).toBeUndefined();
    expect(JSON.stringify((await s.call('GET', '/v1/friends', undefined, a.token)).data)).not.toContain('nextWaitSec');
  });
});
