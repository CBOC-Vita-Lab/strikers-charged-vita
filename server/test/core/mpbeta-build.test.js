// mpbeta27 (18 Nov 2026): the ranked / quick queue pairs only consoles of the same build. Two consoles with other builds (or other match rules) can never play
// each other: the game's link refuses every frame of the other one (its fingerprint), so pairing them is a failed match in the players' hands. A ticket
// carries the X-Msc-Build the game sends with every request; an older client that sends none pairs with anyone (as before). QUEUE_SAME_BUILD=0 turns it off.
// Migration 1004 (queue.build) is additive and the code that is live now (r22b, commit 20539dc8) runs on the migrated database.
import { describe, it, expect } from 'vitest';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { makeServer, signUp, HMAC_HEX, T0 } from '../helpers.js';
import { openNodeSql } from '../../src/sqlite-node.js';
import { createApp } from '../../src/core/app.js';

const B1 = '1a2b3c4d';
const B2 = 'cafef00d';
const queue = (s, who, build, body) => s.call('POST', '/v1/ranked/queue', body, who.token, build ? { 'x-msc-build': build } : {});
const poll = (s, who) => s.call('GET', '/v1/ranked/queue', undefined, who.token);

describe('the queue pairs the same build only', () => {
  it('two consoles of one build pair at once', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    expect((await queue(s, a, B1)).data.status).toBe('queued');
    s.advance(1000);
    expect((await queue(s, b, B1)).data.status).toBe('matched');
  });

  it('two builds never pair, however long they wait; each pairs with its own build', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await queue(s, a, B1);
    s.advance(1000);
    expect((await queue(s, b, B2)).data.status).toBe('queued');
    for (let i = 0; i < 6; ++i) {
      s.advance(5000);
      expect((await poll(s, a)).data.status).toBe('queued');
      expect((await poll(s, b)).data.status).toBe('queued');
    }
    const c = await signUp(s, 'Ccc3');
    expect((await queue(s, c, B2)).data.status).toBe('matched');   // the other B2 waiter
    expect((await poll(s, b)).data.status).toBe('matched');
    expect((await poll(s, a)).data.status).toBe('queued');
  });

  it('the build is compared without regard to case', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await queue(s, a, B2.toUpperCase());
    s.advance(1000);
    expect((await queue(s, b, B2)).data.status).toBe('matched');
  });

  it('a ticket without a build (an older client) pairs with anyone, both ways', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await queue(s, a, null);
    s.advance(1000);
    expect((await queue(s, b, B1)).data.status).toBe('matched');
    const c = await signUp(s, 'Ccc3');
    const d = await signUp(s, 'Ddd4');
    await queue(s, c, B2);
    s.advance(1000);
    expect((await queue(s, d, null)).data.status).toBe('matched');
  });

  it('the quick queue follows the same rule', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await queue(s, a, B1, { mode: 'quick' });
    s.advance(1000);
    expect((await queue(s, b, B2, { mode: 'quick' })).data.status).toBe('queued');
    const c = await signUp(s, 'Ccc3');
    expect((await queue(s, c, B1, { mode: 'quick' })).data.status).toBe('matched');
  });

  it('QUEUE_SAME_BUILD=0 pairs any builds (the 1.3.0 rule)', async () => {
    const s = makeServer({ QUEUE_SAME_BUILD: '0' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await queue(s, a, B1);
    s.advance(1000);
    expect((await queue(s, b, B2)).data.status).toBe('matched');
  });

  it('a malformed build header is refused at the edge as before (and never stored)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const r = await queue(s, a, 'not-hex!');
    expect(r.status).toBe(400);
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM queue').toArray()[0].n).toBe(0);
  });

  it('migration 1004 adds queue.build once and can run again', async () => {
    const s = makeServer();
    const cols = s.store.sql.exec('PRAGMA table_info(queue)').toArray().map((r) => r.name);
    expect(cols.filter((c) => c === 'build').length).toBe(1);
    const env = { RANK_HMAC_KEY_HEX: HMAC_HEX, ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '0', RATE_LIMIT_SCALE: '1000' };
    expect(() => createApp({ sql: s.store.sql, tx: s.store.tx, env, now: () => T0 })).not.toThrow();   // a second start on the same database
    const ids = s.store.sql.exec('SELECT id FROM _migrations ORDER BY id').toArray().map((r) => r.id);
    expect(ids).toContain(1004);                           // later steps (1005, 1006) may follow it
  });
});

// The code that is live (r22b): after this deploy a rollback runs it on a database that has migration 1004. Its queue INSERT names no `build`, so the column's
// default '' is stored, and '' pairs with anyone: the old code pairs as it always did, and the new code then treats its tickets as older clients.
const LIVE = '20539dc8';
let have = false;
try { execFileSync('git', ['cat-file', '-e', `${LIVE}^{commit}`], { stdio: 'ignore' }); have = true; } catch { have = false; }

describe.skipIf(!have)('rollback to the live code after migration 1004', () => {
  it('the live code queues and pairs on the migrated database, and the new code reads its tickets', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'msc-rollback-1004-'));
    try {
      const tar = execFileSync('git', ['-C', '..', 'archive', LIVE, 'server/src']);
      execFileSync('tar', ['-x', '-C', dir], { input: tar });
      const { createApp: liveApp } = await import(pathToFileURL(join(dir, 'server/src/core/app.js')).href);
      const env = { RANK_HMAC_KEY_HEX: HMAC_HEX, ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '0', RATE_LIMIT_SCALE: '1000', MIN_MATCH_SEC: '0' };
      const n = makeServer(env);                       // the new code: migrates to 1004
      const a = await signUp(n, 'Aaa1');
      const b = await signUp(n, 'Bbb2');
      const clock = { now: T0 };
      const o = liveApp({ sql: n.store.sql, tx: n.store.tx, env, now: () => clock.now });   // the rollback
      let ip = 0;
      const call = async (app, method, path, token, headers = {}) => {
        const h = { 'cf-connecting-ip': `198.51.100.${(ip++ % 250) + 1}`, authorization: `Bearer ${token}`, ...headers };
        const r = await app.handle(new Request(`https://t.invalid${path}`, { method, headers: h }));
        return { status: r.status, data: JSON.parse(await r.text()) };
      };
      const qa = await call(o, 'POST', '/v1/ranked/queue', a.token, { 'x-msc-build': B1 });
      expect(qa.status).toBe(200);
      expect(qa.data.status).toBe('queued');
      expect(n.store.sql.exec('SELECT build FROM queue').toArray()[0].build).toBe('');   // the old INSERT leaves the default
      clock.now += 1000;
      const qb = await call(o, 'POST', '/v1/ranked/queue', b.token, { 'x-msc-build': B2 });
      expect(qb.data.status).toBe('matched');           // the live rule: any build
      expect(() => o.sweep()).not.toThrow();
      // and the new code again (roll forward) on the same database: the tickets the old code wrote pair with anyone
      const c = await signUp(n, 'Ccc3');
      const d = await signUp(n, 'Ddd4');
      await call(o, 'POST', '/v1/ranked/queue', c.token, { 'x-msc-build': B1 });
      n.advance(1000);
      const qd = await n.call('POST', '/v1/ranked/queue', undefined, d.token, { 'x-msc-build': B2 });
      expect(qd.data.status).toBe('matched');
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
