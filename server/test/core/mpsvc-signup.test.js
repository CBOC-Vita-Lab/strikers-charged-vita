// Sign-up hardening (Galaxy finding 6, re-built here): no COUNT(*) over every profile at each sign-up, a daily cap that shrinks as the table fills,
// sign-ups that never came back removed first (after 3 days, or at 24 hours when the table is full). Players are never removed: idle players stay on the board.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, makeBlob } from '../helpers.js';

const DAY = 86400000;
const countScans = (srv) => {
  const t = { n: 0 };
  const real = srv.store.sql.exec.bind(srv.store.sql);
  srv.store.sql.exec = (q, ...b) => { if (/SELECT COUNT\(\*\) AS n FROM profiles/i.test(q)) t.n++; return real(q, ...b); };
  return t;
};

describe('sign-up cost', () => {
  it('counting the accounts is one scan per object start, not one per sign-up', async () => {
    const srv = makeServer({ REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000' });
    const t = countScans(srv);
    for (let i = 0; i < 25; i++) await signUp(srv, `Nobody${i}`);
    expect(t.n).toBeLessThanOrEqual(1);
  });

  it('the count follows deletions', async () => {
    const srv = makeServer({ MAX_PLAYERS: '3', REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000' });
    const a = await signUp(srv, 'Aaaa11');
    await signUp(srv, 'Bbbb22');
    await signUp(srv, 'Cccc33');
    expect((await srv.call('POST', '/v1/register', { name: 'Dddd44' })).status).toBe(503);
    await srv.call('DELETE', '/v1/me', undefined, a.token);
    expect((await srv.call('POST', '/v1/register', { name: 'Dddd44' })).status).toBe(200);
  });
});

describe('daily sign-up cap shrinks with the table', () => {
  it('a day of sign-ups stops short once the table is mostly full (429 rate_limited, never server_full first)', async () => {
    const srv = makeServer({ RATE_LIMIT_SCALE: '1', MAX_PLAYERS: '10', REGISTER_MAX_PER_DAY: '8', REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000' });
    const firsts = [];
    for (let i = 0; i < 4; i++) firsts.push(await signUp(srv, `First${i}x`));
    srv.advance(3600_000);
    for (const f of firsts) await srv.call('GET', '/v1/me', undefined, f.token); // these came back: they are not silent
    srv.advance(24 * 3600_000);
    let granted = 0;
    let refused = null;
    for (let i = 0; i < 8 && !refused; i++) {
      const r = await srv.call('POST', '/v1/register', { name: `Later${i}x` });
      if (r.status === 200) granted++; else refused = r;
    }
    expect(refused && refused.status).toBe(429); // without the shrinking cap all 6 free places would go in one day
    expect(refused.data.error.code).toBe('rate_limited');
    expect(granted).toBeLessThan(6);
  });

  it('signupCap values', async () => {
    const { signupCap } = await import('../../src/core/accounts.js');
    const mk = (count) => ({ fill: { count: () => count }, cfg: { maxPlayers: 100, register: { maxPerDay: 1000 } } });
    expect(signupCap(mk(10))).toBe(1000);
    expect(signupCap(mk(50))).toBe(500);
    expect(signupCap(mk(79))).toBe(500);
    expect(signupCap(mk(80))).toBe(250);
    expect(signupCap(mk(100))).toBe(250);
  });
});

describe('sign-ups that never came back', () => {
  it('are removed by the sweep three days after their last sight; a player who played or came back is not', async () => {
    const srv = makeServer({ REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000' });
    const ghost = await signUp(srv, 'Ghost11');
    const visitor = await signUp(srv, 'Visit22');
    const player = await signUp(srv, 'Player3');
    const opp = await signUp(srv, 'Oppon44');
    srv.advance(2 * 3600_000);
    await srv.call('GET', '/v1/me', undefined, visitor.token); // seen again after its first five minutes
    // player and opp play a match
    const room = await srv.call('POST', '/v1/rooms', undefined, player.token);
    await srv.call('POST', '/v1/rooms/join', { code: room.data.code }, opp.token);
    const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAAAAAAAAAA=' });
    await srv.call('POST', `/v1/rooms/${room.data.code}/endpoint`, ep(1), player.token, { ip: '127.0.0.1' });
    await srv.call('POST', `/v1/rooms/${room.data.code}/endpoint`, ep(2), opp.token, { ip: '127.0.0.1' });
    await srv.call('POST', `/v1/rooms/${room.data.code}/started`, undefined, player.token);
    await srv.call('POST', `/v1/rooms/${room.data.code}/started`, undefined, opp.token);
    for (let d = 0; d < 6; d++) { srv.advance(DAY / 2); srv.app.sweep(); srv.advance(DAY / 2 - 3600_000); srv.app.sweep(); srv.advance(3600_000); srv.app.sweep(); }
    const alive = async (tok) => (await srv.call('GET', '/v1/me', undefined, tok)).status === 200;
    expect(await alive(ghost.token)).toBe(false);
    expect(await alive(visitor.token)).toBe(true);
    expect(await alive(player.token)).toBe(true);
    expect(await alive(opp.token)).toBe(true);
  });

  it('a full table makes room by removing sign-ups older than 24 hours that never came back, before refusing', async () => {
    const srv = makeServer({ MAX_PLAYERS: '5', REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000', REGISTER_MAX_PER_DAY: '1000' });
    for (let i = 0; i < 5; i++) await signUp(srv, `Silent${i}`);
    expect((await srv.call('POST', '/v1/register', { name: 'Newone1' })).status).toBe(503); // all five are younger than a day
    srv.advance(30 * 3600_000);
    const r = await srv.call('POST', '/v1/register', { name: 'Newone1' });
    expect(r.status).toBe(200);
  });

  it('a player on the board is never removed, however long away (idle players stay)', async () => {
    const srv = makeServer({ MAX_PLAYERS: '3', REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000', RANK_PUT_POLICY: 'open' });
    const a = await signUp(srv, 'Board11');
    await srv.call('POST', '/v1/rank/put', { category: 0, region: 2, score: 5, data: makeBlob('Board11') }, a.token);
    srv.advance(400 * DAY);
    for (let i = 0; i < 10; i++) { srv.advance(3600_000); srv.app.sweep(); }
    expect((await srv.call('GET', '/v1/me', undefined, a.token)).status).toBe(200);
  });
});
