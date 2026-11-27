// Friends and unranked quick match (wave 10): requests by code, mutual friends, presence, played-with, blocks, invites, the game's status string,
// a held friends poll that rides one request, and the quick queue that never counts on the boards.
import { describe, it, expect } from 'vitest';
import { makeServer, makeVirtual, signUp, pairInRoom, startMatch } from '../helpers.js';

const code8 = (p) => p.code; // register returns the XXXX-XXXX public code

async function three(srv = makeServer()) {
  return { srv, a: await signUp(srv, 'Alice1'), b: await signUp(srv, 'Bobby2'), c: await signUp(srv, 'Carol3') };
}
const list = async (srv, p, q = '') => (await srv.call('GET', `/v1/friends${q}`, undefined, p.token)).data;
const row = (l, pid) => l.friends.find((f) => f.pid === pid);

describe('friend requests', () => {
  it('a request by code is outgoing for the sender and incoming for the other; adding back makes them friends', async () => {
    const { srv, a, b } = await three();
    const r = await srv.call('POST', '/v1/friends/add', { code: code8(b) }, a.token);
    expect(r.status).toBe(200);
    expect(r.data.state).toBe('outgoing');
    expect(row(await list(srv, a), b.pid).state).toBe('outgoing');
    const lb = await list(srv, b);
    expect(row(lb, a.pid)).toMatchObject({ state: 'incoming', name: 'Alice1' });
    expect(row(lb, a.pid).presence).toBeUndefined(); // no presence for a stranger
    const back = await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    expect(back.data.state).toBe('friend');
    expect(row(await list(srv, a), b.pid)).toMatchObject({ state: 'friend', presence: 'menu' });
  });

  it('refuses yourself, an unknown code, a bad code, a full list, and a blocked player; decline and remove work', async () => {
    const { srv, a, b, c } = await three(makeServer({ FRIENDS_MAX: '2' }));
    expect((await srv.call('POST', '/v1/friends/add', { pid: a.pid }, a.token)).data.error.code).toBe('bad_friend');
    expect((await srv.call('POST', '/v1/friends/add', { code: 'AAAAAAAA' }, a.token)).status).toBe(404);
    expect((await srv.call('POST', '/v1/friends/add', { code: 'zz' }, a.token)).data.error.code).toBe('bad_code');
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    await srv.call('POST', '/v1/friends/add', { pid: c.pid }, a.token);
    const d = await signUp(srv, 'Dave44');
    expect((await srv.call('POST', '/v1/friends/add', { pid: d.pid }, a.token)).data.error.code).toBe('friends_full');
    // decline: Carol drops Alice's request; Alice's entry is gone
    await srv.call('POST', '/v1/friends/decline', { pid: a.pid }, c.token);
    expect(row(await list(srv, c), a.pid)).toBeUndefined();
    expect(row(await list(srv, a), c.pid)).toBeUndefined();
    // remove removes both sides
    await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    expect(row(await list(srv, a), b.pid).state).toBe('friend');
    await srv.call('POST', '/v1/friends/remove', { pid: b.pid }, a.token);
    expect(row(await list(srv, a), b.pid)).toBeUndefined();
    expect(row(await list(srv, b), a.pid)).toBeUndefined();
  });

  it('is limited per day and requests from strangers never pile up beyond the cap', async () => {
    const srv = makeServer({ FRIEND_ADDS_PER_DAY: '3', FRIENDS_INCOMING_MAX: '2', RATE_LIMIT_SCALE: '1' });
    const a = await signUp(srv, 'Spammer1');
    const targets = [];
    for (let i = 0; i < 5; i++) targets.push(await signUp(srv, `Target${i}x`));
    let ok = 0;
    for (const t of targets) { const r = await srv.call('POST', '/v1/friends/add', { pid: t.pid }, a.token); if (r.status === 200) ok++; else expect(r.status).toBe(429); }
    expect(ok).toBe(3);
    // incoming cap: many senders, the target sees at most 2
    const srv2 = makeServer({ FRIENDS_INCOMING_MAX: '2' });
    const v = await signUp(srv2, 'Victim1');
    for (let i = 0; i < 6; i++) { const s = await signUp(srv2, `Sender${i}x`); await srv2.call('POST', '/v1/friends/add', { pid: v.pid }, s.token); srv2.advance(10); }
    expect((await list(srv2, v)).friends.filter((f) => f.state === 'incoming').length).toBeLessThanOrEqual(2);
  });
});

describe('presence rides on what exists', () => {
  it('friends show menu, queue, room, match and offline derived from the tables and the last request', async () => {
    const { srv, a, b } = await three(makeServer({ RATE_LIMIT_SCALE: '1000' }));
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    expect(row(await list(srv, a), b.pid).presence).toBe('menu');
    await srv.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect(row(await list(srv, a), b.pid).presence).toBe('queue');
    await srv.call('DELETE', '/v1/ranked/queue', undefined, b.token);
    const room = await srv.call('POST', '/v1/rooms', undefined, b.token);
    expect(row(await list(srv, a), b.pid)).toMatchObject({ presence: 'room', roomCode: room.data.code }); // "join their game"
    const g = await srv.call('POST', '/v1/rooms/join', { code: room.data.code }, a.token);
    await startMatch(srv, a, b, room.data.code);
    void g;
    expect(row(await list(srv, a), b.pid).presence).toBe('match');
    srv.advance(5 * 60_000);
    await srv.call('POST', `/v1/rooms/${room.data.code}/leave`, undefined, b.token);
    srv.advance(5 * 60_000);
    expect(row(await list(srv, a), b.pid).presence).toBe('offline'); // not heard from for 90 s
  });
});

describe('played with', () => {
  it('lists the last opponents with ranked / unranked and whether they are friends already', async () => {
    const { srv, a, b, c } = await three();
    const m = await pairInRoom(srv, a, b);
    await startMatch(srv, a, b, m.code);
    srv.advance(1000);
    const m2 = await pairInRoom(srv, c, a);
    await startMatch(srv, c, a, m2.code);
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    const r = (await srv.call('GET', '/v1/friends/recent', undefined, a.token)).data;
    expect(r.players.map((p) => p.name).sort()).toEqual(['Bobby2', 'Carol3']);
    expect(r.players.find((p) => p.name === 'Bobby2')).toMatchObject({ isFriend: true, wasRanked: false });
    expect(r.players.find((p) => p.name === 'Carol3').isFriend).toBe(false);
  });
});

describe('blocking', () => {
  it('removes the friendship, drops invites, is never told to the blocked player, and keeps the pair out of the queue', async () => {
    const srv = makeServer({ QUEUE_BASE_WINDOW: '1000' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    const c = await signUp(srv, 'Carol3');
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    const blk = await srv.call('POST', '/v1/friends/block', { pid: b.pid }, a.token);
    expect(blk.status).toBe(200);
    expect(row(await list(srv, a), b.pid)).toBeUndefined();
    expect(row(await list(srv, b), a.pid)).toBeUndefined();
    expect((await list(srv, a)).blocked.map((x) => x.name)).toEqual(['Bobby2']);
    // the blocked player can still "add" (the answer looks normal) but the blocker never sees it
    const again = await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    expect(again.status).toBe(200);
    expect(again.data.state).toBe('outgoing');
    expect(row(await list(srv, a), b.pid)).toBeUndefined();
    // the blocker must unblock before adding
    expect((await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token)).data.error.code).toBe('blocked');
    // queue: a and b are never paired; a is paired with c instead
    await srv.call('POST', '/v1/ranked/queue', undefined, a.token);
    await srv.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect((await srv.call('GET', '/v1/ranked/queue', undefined, b.token)).data.status).toBe('queued');
    expect((await srv.call('GET', '/v1/ranked/queue', undefined, a.token)).data.status).toBe('queued');
    await srv.call('POST', '/v1/ranked/queue', undefined, c.token);
    const pa = (await srv.call('GET', '/v1/ranked/queue', undefined, a.token)).data;
    const pb = (await srv.call('GET', '/v1/ranked/queue', undefined, b.token)).data;
    expect([pa.status, pb.status].sort()).toEqual(['matched', 'queued']); // c took one of them, never both
    // unblock
    await srv.call('POST', '/v1/friends/unblock', { pid: b.pid }, a.token);
    expect((await list(srv, a)).blocked).toEqual([]);
  });
});

describe('invites without reading a code', () => {
  it('a friend sees "X invites you" with the room code; only mutual friends, only from your own open room; it clears and expires', async () => {
    const srv = makeServer();
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    const c = await signUp(srv, 'Carol3');
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    const room = (await srv.call('POST', '/v1/rooms', undefined, a.token)).data;
    expect((await srv.call('POST', '/v1/friends/invite', { pid: b.pid, roomCode: room.code }, a.token)).status).toBe(403); // not mutual yet
    await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    expect((await srv.call('POST', '/v1/friends/invite', { pid: c.pid, roomCode: room.code }, a.token)).status).toBe(403);
    expect((await srv.call('POST', '/v1/friends/invite', { pid: b.pid, roomCode: 'ZZZZZZ' }, a.token)).status).toBe(404);
    expect((await srv.call('POST', '/v1/friends/invite', { pid: b.pid, roomCode: room.code }, a.token)).status).toBe(200);
    const seen = row(await list(srv, b), a.pid);
    expect(seen.invite.roomCode).toBe(room.code);
    expect((await srv.call('POST', '/v1/rooms/join', { code: seen.invite.roomCode }, b.token)).status).toBe(200); // one press: join
    await srv.call('POST', '/v1/friends/invite/clear', { pid: a.pid }, b.token);
    expect(row(await list(srv, b), a.pid).invite).toBeUndefined();
    await srv.call('POST', '/v1/friends/invite', { pid: b.pid, roomCode: room.code }, a.token).catch(() => null);
  });

  it('expires after FRIEND_INVITE_SEC', async () => {
    const srv = makeServer({ FRIEND_INVITE_SEC: '60' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    const room = (await srv.call('POST', '/v1/rooms', undefined, a.token)).data;
    await srv.call('POST', '/v1/friends/invite', { pid: b.pid, roomCode: room.code }, a.token);
    srv.advance(61_000);
    expect(row(await list(srv, b), a.pid).invite).toBeUndefined();
  });
});

describe("the game's own status string", () => {
  it('is relayed to mutual friends only, validated, and a change wakes a held friends poll', async () => {
    const srv = makeVirtual();
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    const s = await signUp(srv, 'Stranger');
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    await srv.call('POST', '/v1/friends/add', { pid: a.pid }, b.token);
    expect((await srv.call('POST', '/v1/friends/status', { status: 'not base64!' }, a.token)).status).toBe(400);
    const first = (await srv.call('GET', '/v1/friends', undefined, b.token)).data;
    const h = srv.start('GET', `/v1/friends?v=${first.rev}&wait=45`, undefined, b.token);
    await srv.advance(10_000);
    expect(h.settled).toBe(false); // held: no request in between
    await srv.call('POST', '/v1/friends/status', { status: 'U0MyAQ==' }, a.token);
    await srv.advance(0);
    expect(h.settled).toBe(true);
    expect(h.at - h.startedAt).toBe(10_000);
    expect(h.res.data.friends.find((f) => f.pid === a.pid).status).toBe('U0MyAQ==');
    const sl = (await srv.call('GET', '/v1/friends', undefined, s.token)).data;
    expect(sl.friends).toEqual([]);
    // nothing changes: the hold ends at its time with changed:false and heldMs
    const second = (await srv.call('GET', '/v1/friends', undefined, b.token)).data;
    const h2 = srv.start('GET', `/v1/friends?v=${second.rev}&wait=45`, undefined, b.token);
    await srv.advance(46_000);
    expect(h2.settled).toBe(true);
    expect(h2.res.data.changed).toBe(false);
    expect(h2.res.data.heldMs).toBeGreaterThanOrEqual(44_900);
  });
});

describe('unranked quick match', () => {
  it('pairs anyone regardless of rating, makes an unranked room, never counts on the boards, and never mixes with the ranked queue', async () => {
    const srv = makeServer({ QUEUE_BASE_WINDOW: '1' });
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    const c = await signUp(srv, 'Carol3');
    const j = await srv.call('POST', '/v1/ranked/queue', { mode: 'quick' }, a.token);
    expect(j.status).toBe(200);
    expect(j.data.mode).toBe('quick');
    await srv.call('POST', '/v1/ranked/queue', undefined, c.token); // an old client: no body, ranked
    expect((await srv.call('GET', '/v1/ranked/queue', undefined, c.token)).data.mode).toBe('ranked');
    await srv.call('POST', '/v1/ranked/queue', { mode: 'quick' }, b.token);
    const pa = (await srv.call('GET', '/v1/ranked/queue', undefined, a.token)).data;
    expect(pa.status).toBe('matched');
    expect(pa.room.kind).toBe('unranked');
    expect(pa.room.quick).toBe(true);
    expect((await srv.call('GET', '/v1/ranked/queue', undefined, c.token)).data.status).toBe('queued'); // the ranked player did not get a quick partner
    const m = srv.store.sql.exec(`SELECT kind FROM matches WHERE match_id = ?`, pa.room.matchId).toArray()[0];
    expect(m.kind).toBe('unranked');
    // a quick match never reaches a score row
    await startMatch(srv, a, b, pa.room.code);
    const pb = (await srv.call('GET', '/v1/ranked/queue', undefined, b.token)).data;
    expect(pb.status).toBe('matched');
    const board = await srv.call('GET', '/v1/public/leaderboard?board=season');
    expect(board.data.rows.length).toBe(0);
    // bad mode
    expect((await srv.call('POST', '/v1/ranked/queue', { mode: 'chaos' }, c.token)).status).toBe(400);
  });

  it('a player cannot be in a quick match and open an invite room at once', async () => {
    const srv = makeServer();
    const a = await signUp(srv, 'Alice1');
    const b = await signUp(srv, 'Bobby2');
    await srv.call('POST', '/v1/ranked/queue', { mode: 'quick' }, a.token);
    await srv.call('POST', '/v1/ranked/queue', { mode: 'quick' }, b.token);
    await srv.call('GET', '/v1/ranked/queue', undefined, a.token);
    expect((await srv.call('POST', '/v1/rooms', undefined, a.token)).data.error.code).toBe('in_ranked_match');
  });
});

describe('accounts', () => {
  it('deleting an account removes its friend rows', async () => {
    const { srv, a, b } = await three();
    await srv.call('POST', '/v1/friends/add', { pid: b.pid }, a.token);
    await srv.call('POST', '/v1/friends/block', { pid: a.pid }, b.token);
    await srv.call('DELETE', '/v1/me', undefined, a.token);
    for (const t of ['friend_links', 'friend_blocks', 'friend_status', 'friend_invites']) {
      expect(srv.store.sql.exec(`SELECT COUNT(*) AS n FROM ${t}`).toArray()[0].n, t).toBe(0);
    }
  });
});
