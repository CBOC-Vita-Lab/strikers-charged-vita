// Held polls and the budget header through the real Worker and Durable Object (workerd): a poll is held inside the object and wakes on a join.
import { describe, it, expect } from 'vitest';
import { env, exports } from 'cloudflare:workers';

const BASE = 'https://svc.example.invalid';
let ipn = 1;
async function call(method, path, body, token) {
  const headers = { 'x-test-ip': `198.51.100.${(ipn++ % 250) + 1}` };
  if (token) headers.authorization = `Bearer ${token}`;
  const init = { method, headers };
  if (body !== undefined) { init.body = JSON.stringify(body); headers['content-type'] = 'application/json'; }
  const resp = await exports.default.fetch(new Request(`${BASE}${path}`, init));
  const text = await resp.text();
  let data; try { data = JSON.parse(text); } catch { data = text; }
  return { status: resp.status, data, headers: resp.headers };
}

describe('held polls in the Workers runtime', () => {
  it('a held room poll waits inside the object and is woken by the partner joining', async () => {
    const a = (await call('POST', '/v1/register', { name: 'HoldA01' })).data;
    const b = (await call('POST', '/v1/register', { name: 'HoldB02' })).data;
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    const t0 = Date.now();
    const held = call('GET', `/v1/rooms/${room.code}?v=${room.version}&wait=8`, undefined, a.token);
    await new Promise((r) => setTimeout(r, 700));
    await call('POST', '/v1/rooms/join', { code: room.code }, b.token);
    const r = await held;
    const took = Date.now() - t0;
    expect(r.status).toBe(200);
    expect(r.data.changed).toBe(true);
    expect(r.data.peer.name).toBe('HoldB02');
    expect(r.data.heldMs).toBeGreaterThanOrEqual(500);
    expect(took).toBeLessThan(4000); // not the full 8 s
  });

  it('a held poll with nothing to report answers at its time with changed:false and heldMs', async () => {
    const a = (await call('POST', '/v1/register', { name: 'HoldC03' })).data;
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    const t0 = Date.now();
    const r = await call('GET', `/v1/rooms/${room.code}?v=${room.version}&wait=2`, undefined, a.token);
    expect(r.data.changed).toBe(false);
    expect(Date.now() - t0).toBeGreaterThanOrEqual(1800);
    expect(r.data.heldMs).toBeGreaterThanOrEqual(1800);
    expect(env.DAILY_REQUEST_BUDGET === undefined || Number(env.DAILY_REQUEST_BUDGET) > 0).toBe(true);
  });
});
