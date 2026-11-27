// gxnet14: the proof of work rises per address and is bound at redeem time (Galaxy svcfix2 item 5, ported). Moot while REGISTER_POW_BITS is 0 (the default).
import { describe, it, expect } from 'vitest';
import { makeServer } from '../helpers.js';
import { solve, bitsFor } from '../../src/core/pow.js';

const reg = (s, body, ip) => s.call('POST', '/v1/register', body, undefined, { ip });
const IP = '203.0.113.9';
const mk = (over = {}) => makeServer({ REGISTER_POW_BITS: '6', REGISTER_POW_FREE: '2', REGISTER_POW_MAX_BITS: '9', REGISTER_IP_BURST: '50', REGISTER_IP_PER_HOUR: '50', ...over });
async function signUp(s, name, ip = IP) {
  const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip })).data;
  const nonce = await solve(ch.challenge, ch.bits);
  const r = await reg(s, { name, pow: { challenge: ch.challenge, nonce } }, ip);
  return { ch, r };
}

describe('proof of work difficulty per address', () => {
  it('base bits for powFree registrations, then one more each, up to powMaxBits; another address starts again', async () => {
    const s = mk();
    const seen = [];
    for (let i = 0; i < 7; i++) { const { ch, r } = await signUp(s, `Rise${i}x`); expect(r.status).toBe(200); seen.push(ch.bits); }
    expect(seen).toEqual([6, 6, 7, 8, 9, 9, 9]); // powFree 2: the first two cost the base, the 3rd one bit more, ... capped at powMaxBits
    const other = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.50' })).data;
    expect(other.bits).toBe(6);
  });
  it('a day later the address is back at the base price', async () => {
    const s = mk();
    for (let i = 0; i < 5; i++) await signUp(s, `Day${i}xx`);
    expect((await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: IP })).data.bits).toBeGreaterThan(6);
    s.advance(86400_000 + 1000);
    expect((await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: IP })).data.bits).toBe(6);
  });
  it('never above POW_CLIENT_MAX_BITS (what the game will grind)', async () => {
    const s = mk({ REGISTER_POW_MAX_BITS: '12', POW_CLIENT_MAX_BITS: '8' });
    const seen = [];
    for (let i = 0; i < 6; i++) { const { ch, r } = await signUp(s, `Cap${i}xxx`); expect(r.status).toBe(200); seen.push(ch.bits); }
    expect(Math.max(...seen)).toBe(8);
  });
  it('bound at redeem time: a stockpiled cheap challenge is refused with a harder one and is not spent', async () => {
    const s = mk();
    const stock = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: IP })).data; // asked at 6 bits
    expect(stock.bits).toBe(6);
    for (let i = 0; i < 4; i++) await signUp(s, `Stk${i}xxx`); // the address now pays more
    const nonce = await solve(stock.challenge, stock.bits);
    const r = await reg(s, { name: 'Stkold1', pow: { challenge: stock.challenge, nonce } }, IP);
    expect(r.status).toBe(428);
    expect(r.data.error).toMatchObject({ code: 'pow_required' });
    expect(r.data.error.bits).toBeGreaterThan(6);
    // not spent: on a quiet address the same challenge still works
    const s2 = mk();
    const fresh = (await s2.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.60' })).data;
    const n2 = await solve(fresh.challenge, fresh.bits);
    expect((await reg(s2, { name: 'Fresh1x', pow: { challenge: fresh.challenge, nonce: n2 } }, '203.0.113.60')).status).toBe(200);
  });
  it('the difficulty is inside the MAC: a challenge cannot be edited down to fewer bits', async () => {
    const s = mk();
    const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: IP })).data;
    const parts = ch.challenge.split('.');
    parts[2] = '1';
    const forged = parts.join('.');
    const r = await reg(s, { name: 'Forge1x', pow: { challenge: forged, nonce: await solve(forged, 1) } }, IP);
    expect(r.data.error).toMatchObject({ code: 'bad_pow', reason: 'mac' });
  });
  it('off (0 bits): nothing changes', async () => {
    const s = makeServer();
    expect((await s.call('GET', '/v1/register/challenge')).data).toEqual({ bits: 0 });
    expect(bitsFor({ cfg: { register: { powBits: 0 } }, now: 0, regSeen: new Map() }, 'x')).toBe(0);
  });
});
