// Friends (wave 10): friend requests by code, a friends list with presence, "played with", blocks, invites into a room, and the game's own
// friend status string.
//
// The ORIGINAL game has friend screens (Online Central roster, friend key entry, "host inviting player"); the client's DWC layer backs them with
// these routes. DWC semantics are kept: a friend is mutual (both sides added the other), the friend key is the 12-digit DWC key of the profile id
// (decoded in the client; the service takes `pid` or the public `code`), and the game's status string is relayed verbatim (base64, opaque here).
//
// Storage: friend_links(pid, other) = "pid added other" (both rows = friends, one row = a request), friend_blocks, friend_status, friend_invites
// (migration 1002). Presence is derived, never stored: online = an authenticated request within `onlineMs` (in memory, `c.seen`), then in a
// room / in a match / in the queue from the rooms and queue tables. No new polling loop: one held poll (GET /v1/friends?v=&wait=) used by the
// online menu, answered at once when a request, an invite or a status changes (friendTouch), otherwise at the end of its hold.
//
// Rules: a blocked player is never told (their request looks pending and never arrives; their invite is dropped); a block removes the friendship,
// the invites and keeps the pair out of the queue (ranked.js). Quick and invite matches stay unranked; nothing here touches the board.

import { ApiError, strict, isInt } from './util.js';
import { fmtCode } from './accounts.js';
import { normalizeCode } from './rooms.js';

const STATUS_RE = /^[A-Za-z0-9+/=.*-]{0,480}$/; // the game's own base64 (DWC's alphabet ends in '.-' and pads with '*') or the standard one
const CODE8 = /^[A-HJKMNP-Z2-9]{8}$/;

export function forgetFriends(c, pid) {
  for (const [t, cols] of [['friend_links', ['pid', 'other']], ['friend_blocks', ['pid', 'other']], ['friend_invites', ['to_pid', 'from_pid']]]) {
    c.db.run(`DELETE FROM ${t} WHERE ${cols[0]} = ? OR ${cols[1]} = ?`, pid, pid);
  }
  c.db.run(`DELETE FROM friend_status WHERE pid = ?`, pid);
}

function person(c, body) {
  if (!body || typeof body !== 'object') throw new ApiError(400, 'bad_request', 'a friend is given by pid or code');
  let row = null;
  if (body.pid !== undefined) {
    if (!isInt(body.pid, 1, 2147483647)) throw new ApiError(400, 'bad_request', 'bad pid', { field: 'pid' });
    row = c.db.get(`SELECT pid, code, name, banned FROM profiles WHERE pid = ?`, body.pid);
  } else if (typeof body.code === 'string') {
    const code = body.code.toUpperCase().replace(/[\s-]/g, '');
    if (!CODE8.test(code)) throw new ApiError(400, 'bad_code', 'friend codes are 8 characters (XXXX-XXXX)');
    row = c.db.get(`SELECT pid, code, name, banned FROM profiles WHERE code = ?`, code);
  } else throw new ApiError(400, 'bad_request', 'a friend is given by pid or code');
  if (!row || row.banned || !row.name) throw new ApiError(404, 'player_not_found', 'no such player');
  return row;
}

const isBlockedEither = (c, a, b) => !!c.db.get(`SELECT 1 AS x FROM friend_blocks WHERE (pid = ? AND other = ?) OR (pid = ? AND other = ?)`, a, b, b, a);

function pidList(rows) {
  return rows.length ? `(${rows.map((r) => Number(r)).join(',')})` : '(0)';
}

/** presence of these pids: offline | menu | queue | room | match */
function presenceOf(c, pids) {
  const out = new Map();
  if (!pids.length) return out;
  const L = pidList(pids);
  const seen = (pid) => (c.seen.get(pid) || 0) > c.now - c.cfg.friends.onlineMs;
  for (const pid of pids) if (seen(pid)) out.set(pid, 'menu');
  for (const r of c.db.all(`SELECT pid FROM queue WHERE pid IN ${L}`)) out.set(r.pid, 'queue');
  const open = new Map(); // pid -> the code of their open invite room: a friend can join it ("Join their game")
  for (const r of c.db.all(`SELECT code, kind, quick, host_pid, join_pid, state FROM rooms WHERE state IN ('open','joined','ready','started') AND (host_pid IN ${L} OR join_pid IN ${L})`)) {
    const v = r.state === 'started' ? 'match' : 'room';
    for (const pid of [r.host_pid, r.join_pid]) if (pid != null && pids.includes(pid)) out.set(pid, v);
    if (r.state === 'open' && r.kind === 'unranked' && !r.quick) open.set(r.host_pid, r.code);
  }
  out.open = open;
  // a console in a room or match stays visible for the hold of its room poll even though it sends no friends poll
  return out;
}

const dayOf = (ms) => Math.floor(ms / 86_400_000);

function hash(str) {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
  return h.toString(36);
}

/** GET /v1/friends[?v=]: the whole picture for the friends screens. `rev` changes whenever any part of it does (presence included). */
export function list(c, p, sinceRev) {
  const f = c.cfg.friends;
  const mine = c.db.all(`SELECT other, created_ms FROM friend_links WHERE pid = ?`, p.pid);
  const theirs = c.db.all(`SELECT pid, created_ms FROM friend_links WHERE other = ? ORDER BY created_ms DESC LIMIT ?`, p.pid, f.incomingMax * 2);
  const blocks = c.db.all(`SELECT other FROM friend_blocks WHERE pid = ?`, p.pid).map((r) => r.other);
  const blockedMe = new Set(c.db.all(`SELECT pid FROM friend_blocks WHERE other = ?`, p.pid).map((r) => r.pid));
  const mineSet = new Set(mine.map((r) => r.other));
  const theirSet = new Set(theirs.map((r) => r.pid));
  const ids = [...new Set([...mine.map((r) => r.other), ...theirs.map((r) => r.pid)])];
  const incoming = theirs.filter((r) => !mineSet.has(r.pid) && !blockedMe.has(r.pid) && !blocks.includes(r.pid)).slice(0, f.incomingMax).map((r) => r.pid);
  const shown = ids.filter((id) => mineSet.has(id) || incoming.includes(id));
  const info = new Map();
  if (shown.length) {
    for (const r of c.db.all(`SELECT pid, code, name, last_seen_ms FROM profiles WHERE pid IN ${pidList(shown)}`)) info.set(r.pid, r);
  }
  const buddies = shown.filter((id) => mineSet.has(id) && theirSet.has(id));
  const pres = presenceOf(c, buddies);
  const status = new Map(buddies.length ? c.db.all(`SELECT pid, status FROM friend_status WHERE pid IN ${pidList(buddies)}`).map((r) => [r.pid, r.status]) : []);
  const invFrom = new Map(c.db.all(`SELECT from_pid, code, created_ms FROM friend_invites WHERE to_pid = ? AND created_ms > ?`, p.pid, c.now - f.inviteTtlMs).map((r) => [r.from_pid, r]));
  const rows = [];
  for (const id of shown) {
    const pr = info.get(id);
    if (!pr) continue;
    const buddy = buddies.includes(id);
    const row = {
      pid: id, code: fmtCode(pr.code), name: pr.name, state: buddy ? 'friend' : mineSet.has(id) ? 'outgoing' : 'incoming',
    };
    if (buddy) {
      row.presence = pres.get(id) || 'offline';
      row.lastSeenDay = dayOf(Math.max(pr.last_seen_ms, c.seen.get(id) || 0));
      row.status = status.get(id) || '';
      if (pres.open && pres.open.has(id)) row.roomCode = pres.open.get(id);
      const inv = invFrom.get(id);
      if (inv) row.invite = { roomCode: inv.code, ageMs: c.now - inv.created_ms };
    }
    rows.push(row);
  }
  const body = {
    code: fmtCode(p.code), pid: p.pid, friends: rows,
    blocked: blocks.length ? c.db.all(`SELECT pid, code, name FROM profiles WHERE pid IN ${pidList(blocks)}`).map((r) => ({ pid: r.pid, code: fmtCode(r.code), name: r.name })) : [],
    max: f.max,
  };
  const rev = hash(JSON.stringify({ ...body, friends: body.friends.map((r) => ({ ...r, invite: r.invite && r.invite.roomCode })) }));
  if (sinceRev !== undefined && sinceRev === rev) return { changed: false, rev };
  return { changed: true, rev, ...body };
}

export function add(c, p, body) {
  const f = c.cfg.friends;
  if (!f.max) throw new ApiError(503, 'service_disabled', 'friends are switched off');
  return c.tx(() => {
    const t = person(c, body);
    if (t.pid === p.pid) throw new ApiError(400, 'bad_friend', 'that is your own friend code', { reason: 'self' });
    if (c.db.get(`SELECT 1 AS x FROM friend_blocks WHERE pid = ? AND other = ?`, p.pid, t.pid)) throw new ApiError(409, 'blocked', 'you blocked this player; unblock first');
    const has = c.db.get(`SELECT 1 AS x FROM friend_links WHERE pid = ? AND other = ?`, p.pid, t.pid);
    if (!has) {
      const n = c.db.get(`SELECT COUNT(*) AS n FROM friend_links WHERE pid = ?`, p.pid).n;
      if (n >= f.max) throw new ApiError(409, 'friends_full', `the friend list holds ${f.max}`, { max: f.max });
      c.limit(`fadd:${p.pid}`, f.addsPerDay, 86400);
      c.db.run(`INSERT INTO friend_links (pid, other, created_ms) VALUES (?, ?, ?)`, p.pid, t.pid, c.now);
      // a stranger's pile of requests never grows past incomingMax: the oldest go first
      const over = c.db.get(`SELECT COUNT(*) AS n FROM friend_links WHERE other = ?`, t.pid).n - f.incomingMax * 2;
      if (over > 0) c.db.run(`DELETE FROM friend_links WHERE rowid IN (SELECT rowid FROM friend_links WHERE other = ? ORDER BY created_ms LIMIT ?)`, t.pid, over);
      // a blocked sender is not told, and the receiver never hears (the row is hidden in `list`)
      c.friendTouch(p.pid, t.pid);
    }
    const back = c.db.get(`SELECT 1 AS x FROM friend_links WHERE pid = ? AND other = ?`, t.pid, p.pid);
    const state = back && !isBlockedEither(c, p.pid, t.pid) ? 'friend' : 'outgoing';
    return { state, pid: t.pid, code: fmtCode(t.code), name: t.name };
  });
}

export function decline(c, p, body) {
  strict(body, ['pid', 'code']);
  const t = person(c, body);
  c.db.run(`DELETE FROM friend_links WHERE pid = ? AND other = ?`, t.pid, p.pid);
  c.friendTouch(p.pid);
  return { ok: true };
}

export function remove(c, p, body) {
  strict(body, ['pid', 'code']);
  const t = person(c, body);
  c.db.run(`DELETE FROM friend_links WHERE (pid = ? AND other = ?) OR (pid = ? AND other = ?)`, p.pid, t.pid, t.pid, p.pid);
  c.db.run(`DELETE FROM friend_invites WHERE (to_pid = ? AND from_pid = ?) OR (to_pid = ? AND from_pid = ?)`, p.pid, t.pid, t.pid, p.pid);
  c.friendTouch(p.pid, t.pid);
  return { ok: true };
}

export function block(c, p, body) {
  strict(body, ['pid', 'code']);
  return c.tx(() => {
    const t = person(c, body);
    if (t.pid === p.pid) throw new ApiError(400, 'bad_friend', 'that is your own friend code', { reason: 'self' });
    if (!c.db.get(`SELECT 1 AS x FROM friend_blocks WHERE pid = ? AND other = ?`, p.pid, t.pid)) {
      if (c.db.get(`SELECT COUNT(*) AS n FROM friend_blocks WHERE pid = ?`, p.pid).n >= c.cfg.friends.blocksMax) throw new ApiError(409, 'blocks_full', 'the block list is full');
      c.db.run(`INSERT INTO friend_blocks (pid, other, created_ms) VALUES (?, ?, ?)`, p.pid, t.pid, c.now);
    }
    c.db.run(`DELETE FROM friend_links WHERE (pid = ? AND other = ?) OR (pid = ? AND other = ?)`, p.pid, t.pid, t.pid, p.pid);
    c.db.run(`DELETE FROM friend_invites WHERE (to_pid = ? AND from_pid = ?) OR (to_pid = ? AND from_pid = ?)`, p.pid, t.pid, t.pid, p.pid);
    c.friendTouch(p.pid); // the blocked player is NOT woken: nothing changes for them
    return { ok: true, pid: t.pid, code: fmtCode(t.code), name: t.name };
  });
}

export function unblock(c, p, body) {
  strict(body, ['pid', 'code']);
  const t = person(c, body);
  c.db.run(`DELETE FROM friend_blocks WHERE pid = ? AND other = ?`, p.pid, t.pid);
  c.friendTouch(p.pid);
  return { ok: true };
}

/** POST /v1/friends/invite {pid|code, roomCode}: "X invites you" for a mutual friend; the room must be the caller's own open unranked room. */
export function invite(c, p, body) {
  strict(body, ['pid', 'code', 'roomCode']);
  return c.tx(() => {
    const t = person(c, body);
    const code = normalizeCode(body.roomCode);
    const mutual = c.db.get(`SELECT 1 AS x FROM friend_links a JOIN friend_links b ON a.pid = b.other AND a.other = b.pid WHERE a.pid = ? AND a.other = ?`, p.pid, t.pid);
    if (!mutual) throw new ApiError(403, 'not_friends', 'you can only invite a friend');
    const room = c.db.get(`SELECT kind, state, host_pid, join_pid FROM rooms WHERE code = ?`, code);
    if (!room || room.kind !== 'unranked' || room.state !== 'open' || room.host_pid !== p.pid) throw new ApiError(404, 'room_not_found', 'invite from your own open room');
    if (isBlockedEither(c, p.pid, t.pid)) return { ok: true }; // a blocked inviter is not told
    c.db.run(
      `INSERT INTO friend_invites (to_pid, from_pid, code, created_ms) VALUES (?, ?, ?, ?)
       ON CONFLICT(to_pid, from_pid) DO UPDATE SET code = excluded.code, created_ms = excluded.created_ms`,
      t.pid, p.pid, code, c.now,
    );
    c.friendTouch(t.pid);
    return { ok: true };
  });
}

export function clearInvite(c, p, body) {
  strict(body, ['pid', 'code']);
  const t = person(c, body);
  c.db.run(`DELETE FROM friend_invites WHERE (to_pid = ? AND from_pid = ?) OR (to_pid = ? AND from_pid = ?)`, p.pid, t.pid, t.pid, p.pid);
  c.friendTouch(p.pid, t.pid);
  return { ok: true };
}

/** POST /v1/friends/status {status}: the game's friend status string (DWC_SetOwnStatusData), shown to friends. */
export function setStatus(c, p, body) {
  strict(body, ['status']);
  if (typeof body.status !== 'string' || !STATUS_RE.test(body.status)) throw new ApiError(400, 'bad_request', 'status is base64, at most 480 characters', { field: 'status' });
  const cur = c.db.get(`SELECT status FROM friend_status WHERE pid = ?`, p.pid);
  if (!cur || cur.status !== body.status) {
    c.db.run(`INSERT INTO friend_status (pid, status, updated_ms) VALUES (?, ?, ?) ON CONFLICT(pid) DO UPDATE SET status = excluded.status, updated_ms = excluded.updated_ms`, p.pid, body.status, c.now);
    const friends = c.db.all(`SELECT a.other AS pid FROM friend_links a JOIN friend_links b ON a.pid = b.other AND a.other = b.pid WHERE a.pid = ?`, p.pid).map((r) => r.pid);
    c.friendTouch(...friends);
  }
  return { ok: true };
}

/** GET /v1/friends/recent: the last 20 opponents (ranked, quick and invite matches), newest first. */
export function recent(c, p) {
  const f = c.cfg.friends;
  const rows = c.db.all(
    `SELECT CASE WHEN p0 = ?1 THEN p1 ELSE p0 END AS opp, kind, MAX(created_ms) AS at
       FROM matches WHERE (p0 = ?1 OR p1 = ?1) AND status <> 'void' GROUP BY opp ORDER BY at DESC LIMIT ?2`,
    p.pid, f.recent + 5,
  );
  const ids = rows.map((r) => r.opp);
  const info = new Map(ids.length ? c.db.all(`SELECT pid, code, name, banned FROM profiles WHERE pid IN ${pidList(ids)}`).map((r) => [r.pid, r]) : []);
  const mine = new Set(c.db.all(`SELECT other FROM friend_links WHERE pid = ?`, p.pid).map((r) => r.other));
  const blocked = new Set(c.db.all(`SELECT other FROM friend_blocks WHERE pid = ?`, p.pid).map((r) => r.other));
  const out = [];
  for (const r of rows) {
    const i = info.get(r.opp);
    if (!i || i.banned || !i.name || blocked.has(r.opp)) continue;
    out.push({ pid: r.opp, code: fmtCode(i.code), name: i.name, wasRanked: r.kind === 'ranked', day: dayOf(r.at), isFriend: mine.has(r.opp) });
    if (out.length >= f.recent) break;
  }
  return { players: out };
}

/** pids this player must never be paired with (either direction), for the matchmaker */
export function blockedPids(c, pid) {
  const s = new Set();
  for (const r of c.db.all(`SELECT other AS x FROM friend_blocks WHERE pid = ? UNION SELECT pid AS x FROM friend_blocks WHERE other = ?`, pid, pid)) s.add(r.x);
  return s;
}

export function pruneInvites(c) {
  c.db.run(`DELETE FROM friend_invites WHERE created_ms < ?`, c.now - c.cfg.friends.inviteTtlMs);
}
