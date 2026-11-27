// Wrong room / friend code guesses (core/guess.js, wave 13 O2): limits per address and per account, kept in storage.
import { describe, it, expect } from 'vitest';
import { createApp } from '../../src/core/app.js';
import { makeServer, signUp } from '../helpers.js';

const joinWrong = (s, tok, ip, code = 'ZZZZZZ') => s.call('POST', '/v1/rooms/join', { code }, tok, { ip });
const rows = (s) => s.store.sql.exec(`SELECT * FROM guess_misses`).toArray();

describe('wrong-code limits', () => {
  it('10 misses in 10 minutes per address, then 429 with Retry-After; a good code is refused too while limited; the window ends', async () => {
    const s = makeServer({ GUESS_ADDR_WINDOW: '10', GUESS_ADDR_DAY: '40' });
    const accts = [];
    for (let i = 0; i < 6; i++) accts.push(await signUp(s, `Gu${i}A`));
    const ip = '203.0.113.9';
    const out = [];
    // spread over accounts so only the ADDRESS limit can be the one that trips (each account stays under its own 10)
    for (let i = 0; i < 12; i++) out.push((await joinWrong(s, accts[i % 6].token, ip)));
    expect(out.slice(0, 10).every((r) => r.status === 404)).toBe(true);
    expect(out[10].status).toBe(429);
    expect(out[10].data.error.code).toBe('rate_limited');
    expect(out[10].data.error.reason).toBe('code_guesses');
    expect(out[10].headers.get('retry-after')).toMatch(/^\d+$/);
    // another address is untouched
    expect((await joinWrong(s, accts[0].token, '203.0.113.10')).status).toBe(404);
    // a right code from the limited address is refused as well: the caller learns nothing
    const host = await signUp(s, 'GuHost');
    const code = (await s.call('POST', '/v1/rooms', undefined, host.token)).data.code;
    expect((await joinWrong(s, accts[1].token, ip, code)).status).toBe(429);
    s.advance(10 * 60_000 + 1000);
    const fresh = (await s.call('POST', '/v1/rooms', undefined, host.token)).data.code; // the first room has expired meanwhile
    expect((await joinWrong(s, accts[1].token, ip, fresh)).status).toBe(200);
  });

  it('40 a UTC day per address (the window alone would allow 10 every 10 minutes), reset at midnight', async () => {
    const s = makeServer({ GUESS_ACCT_WINDOW: '0', GUESS_ACCT_DAY: '0', GUESS_ADDR_WINDOW: '10', GUESS_ADDR_DAY: '40' });
    const a = await signUp(s, 'DayGuess');
    const ip = '203.0.113.20';
    let ok = 0;
    let first429 = -1;
    for (let i = 0; i < 60 && first429 < 0; i++) {
      const r = await joinWrong(s, a.token, ip);
      if (r.status === 429) first429 = i; else ok++;
      if ((i + 1) % 10 === 0) s.advance(10 * 60_000 + 1000); // a new window every ten misses
    }
    expect(ok).toBe(40);
    expect(first429).toBe(40);
    const blocked = await joinWrong(s, a.token, ip);
    expect(blocked.status).toBe(429);
    expect(Number(blocked.headers.get('retry-after'))).toBeGreaterThan(60);
    s.advance(24 * 3600_000);
    expect((await joinWrong(s, a.token, ip)).status).toBe(404);
  });

  it('per account: 10 in 10 minutes from changing addresses', async () => {
    const s = makeServer({ GUESS_ADDR_WINDOW: '0', GUESS_ADDR_DAY: '0' });
    const a = await signUp(s, 'Roamer');
    const b = await signUp(s, 'Stay');
    for (let i = 0; i < 10; i++) expect((await joinWrong(s, a.token, `203.0.113.${50 + i}`)).status).toBe(404);
    expect((await joinWrong(s, a.token, '203.0.113.99')).status).toBe(429);
    expect((await joinWrong(s, b.token, '203.0.113.98')).status).toBe(404); // another account is untouched
  });

  it('counters survive rebuilding the object (the storage is the memory)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Evicted');
    const ip = '203.0.113.30';
    let app = s.app;
    const via = async (tok) => {
      const r = await app.handle(new Request('https://x.invalid/v1/rooms/join', { method: 'POST', body: JSON.stringify({ code: 'ZZZZZZ' }), headers: { authorization: `Bearer ${tok}`, 'cf-connecting-ip': ip, 'content-type': 'application/json' } }));
      return r.status;
    };
    const seen = [];
    for (let i = 0; i < 12; i++) {
      if (i % 3 === 0) app = createApp({ sql: s.store.sql, tx: s.store.tx, env: s.env, now: () => s.clock.now }); // the object left memory and came back
      seen.push(await via(a.token));
    }
    expect(seen.slice(0, 10).every((x) => x === 404)).toBe(true);
    expect(seen.slice(10)).toEqual([429, 429]);
  });

  it('privacy: no address in storage, a salted fingerprint that changes with the UTC day, old rows deleted', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Private');
    const ip = '203.0.113.31';
    await joinWrong(s, a.token, ip);
    const day1 = rows(s);
    expect(day1.length).toBe(2); // address row + account row
    const dump = JSON.stringify([day1, s.store.sql.exec(`SELECT * FROM settings`).toArray()]);
    expect(dump).not.toContain('203.0.113.31');
    expect(dump).not.toContain('203.0.113');
    const addrKey = day1.find((r) => r.k.startsWith('a:')).k;
    expect(addrKey).toMatch(/^a:[0-9a-f]{16}$/);
    s.advance(24 * 3600_000);
    await joinWrong(s, a.token, ip);
    const day2 = rows(s);
    expect(day2.find((r) => r.k.startsWith('a:')).k).not.toBe(addrKey); // yesterday's fingerprint cannot be matched
    expect(day2.length).toBe(2); // yesterday's rows were deleted at the first use of the new day
  });

  it('friend codes and player lookups count too; hits do not; expired / full rooms are misses, your own room is not', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Seeker');
    const ip = '203.0.113.40';
    for (let i = 0; i < 10; i++) expect((await s.call('GET', '/v1/players/ZZZZZZZZ', undefined, a.token, { ip })).status).toBe(404);
    expect((await s.call('GET', '/v1/players/ZZZZZZZZ', undefined, a.token, { ip })).status).toBe(429);
    const hits = makeServer();
    const x = await signUp(hits, 'X1');
    const y = await signUp(hits, 'Y1');
    for (let i = 0; i < 15; i++) {
      const created = await hits.call('POST', '/v1/rooms', undefined, x.token);
      const r = await hits.call('POST', '/v1/rooms/join', { code: created.data.code }, y.token, { ip: '203.0.113.41' });
      expect(r.status).toBe(200); // successful joins are never counted
      await hits.call('POST', `/v1/rooms/${created.data.code}/leave`, undefined, y.token);
      await hits.call('POST', `/v1/rooms/${created.data.code}/leave`, undefined, x.token);
      hits.advance(1000);
    }
    expect(rows(hits).length).toBe(0);
    const own = (await hits.call('POST', '/v1/rooms', undefined, x.token)).data.code;
    expect((await hits.call('POST', '/v1/rooms/join', { code: own }, x.token)).status).toBe(409); // own_room is no guess
    expect(rows(hits).length).toBe(0);
  });

  it('GUESS_* = 0 turns the limits off; the rows are bounded by the limits (no write once limited)', async () => {
    const off = makeServer({ GUESS_ADDR_WINDOW: '0', GUESS_ADDR_DAY: '0', GUESS_ACCT_WINDOW: '0', GUESS_ACCT_DAY: '0' });
    const a = await signUp(off, 'Free');
    for (let i = 0; i < 30; i++) expect((await joinWrong(off, a.token, '203.0.113.60')).status).toBe(404);
    expect(rows(off).length).toBe(0);
    const on = makeServer();
    const b = await signUp(on, 'Limited');
    for (let i = 0; i < 25; i++) await joinWrong(on, b.token, '203.0.113.61');
    const r = rows(on);
    expect(Math.max(...r.map((x) => x.n_win))).toBe(10);
  });

  it('deleting the account removes its counter', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Leaver');
    await joinWrong(s, a.token, '203.0.113.70');
    expect(rows(s).some((r) => r.k.startsWith('p:'))).toBe(true);
    await s.call('DELETE', '/v1/account', undefined, a.token);
    expect(rows(s).some((r) => r.k.startsWith('p:'))).toBe(false);
  });

  it('defaults: an address shared by many players (a meet-up) is allowed 30 misses in 10 minutes and 200 a day', async () => {
    const s = makeServer();
    const ip = '203.0.113.88';
    const accts = [];
    for (let i = 0; i < 8; i++) accts.push(await signUp(s, `Meet${i}`));
    const st = [];
    for (let i = 0; i < 32; i++) st.push((await joinWrong(s, accts[i % 8].token, ip)).status);
    expect(st.slice(0, 30).every((x) => x === 404)).toBe(true);
    expect(st.slice(30)).toEqual([429, 429]);
  });
});
