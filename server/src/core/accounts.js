// Sign-in (registration), profile, rename, delete.

import { forgetFriends } from './friends.js';
import { forgetReports } from './reports.js';
import { forget as forgetGuess } from './guess.js';
import { ApiError, b64url, randomBytes, randomInt, sha256hex, isInt, strict } from './util.js';
import { CODE_ALPHABET, normalizeCode } from './rooms.js';
import { validateName } from './names.js';

const REGIONS = [1, 2, 4, 8]; // JP, US, EU, KR (DWC_RNK_REGION_*)

export function checkRegion(v, dflt = 2) {
  if (v === undefined || v === null) return dflt;
  if (!REGIONS.includes(v)) throw new ApiError(400, 'bad_region', 'region must be 1 (JP), 2 (US), 4 (EU) or 8 (KR)');
  return v;
}

/** Short public identity code, shown as XXXX-XXXX. Safe to share (it grants nothing), unlike the token. */
function newPublicCode(c) {
  for (let i = 0; i < 30; i++) {
    let s = '';
    for (let j = 0; j < 8; j++) s += CODE_ALPHABET[randomInt(CODE_ALPHABET.length)];
    if (!c.db.get(`SELECT 1 AS x FROM profiles WHERE code = ?`, s)) return s;
  }
  throw new ApiError(503, 'busy', 'could not allocate a code, try again');
}

export const fmtCode = (s) => `${s.slice(0, 4)}-${s.slice(4)}`;

function checkRelayOnly(v) {
  if (v === undefined) return undefined;
  if (typeof v !== 'boolean') throw new ApiError(400, 'bad_request', 'relayOnly must be true or false', { field: 'relayOnly' });
  return v ? 1 : 0;
}

export function publicProfile(p) {
  return {
    pid: p.pid,
    code: fmtCode(p.code),
    name: p.name,
    region: p.region,
    rating: p.rating,
    wins: p.wins,
    losses: p.losses,
    draws: p.draws,
    abandons: p.abandons,
    createdMs: p.created_ms,
  };
}

/**
 * Everything about a registration request that can be decided without writing: field allowlist, name rules, region,
 * relayOnly, and whether the name is already taken. Runs BEFORE any rate-limit bucket that counts successful
 * registrations is charged, so junk requests cannot use up the global daily budget.
 */
export function checkRegisterBody(c, body) {
  strict(body, ['name', 'region', 'relayOnly', 'pow']);
  const relayOnly = checkRelayOnly(body.relayOnly) || 0;
  const { name, key } = validateName(body.name, { extraBlock: c.cfg.nameBlockExtra });
  const region = checkRegion(body.region);
  if (c.db.get(`SELECT 1 AS x FROM profiles WHERE name_key = ?`, key)) {
    throw new ApiError(409, 'name_taken', 'that name is already in use', { reason: 'taken' });
  }
  return { name, key, region, relayOnly };
}

export async function register(c, body) {
  const { name, key, region, relayOnly } = checkRegisterBody(c, body);
  const token = b64url(randomBytes(32));
  const hash = await sha256hex(token);
  const row = c.tx(() => {
    if (c.fill.count() >= c.cfg.maxPlayers) pruneSilent(c, c.now - 7 * 86400_000, c.now - 24 * 3600_000, 200, true); // full: sign-ups that never came back go first
    if (c.fill.count() >= c.cfg.maxPlayers) throw new ApiError(503, 'server_full', 'registration is closed for now');
    if (c.db.get(`SELECT 1 AS x FROM profiles WHERE name_key = ?`, key)) {
      throw new ApiError(409, 'name_taken', 'that name is already in use', { reason: 'taken' });
    }
    return c.db.get(
      `INSERT INTO profiles (code, name, name_key, token_hash, created_ms, last_seen_ms, name_changed_ms, region, relay_only)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING pid, code`,
      newPublicCode(c), name, key, hash, c.now, c.now, c.now, region, relayOnly,
    );
  });
  c.fill.add(1);
  return { pid: row.pid, code: fmtCode(row.code), token, name, region, relayOnly: !!relayOnly, createdMs: c.now };
}

/** The daily sign-up cap, shrinking as the table fills: half from 50 % of MAX_PLAYERS, a quarter from 80 %. */
export function signupCap(c) {
  const full = c.fill.count() / c.cfg.maxPlayers;
  const base = c.cfg.register.maxPerDay;
  return Math.max(1, Math.floor(full >= 0.8 ? base / 4 : full >= 0.5 ? base / 2 : base));
}

export function me(c, p) {
  return {
    ...publicProfile(p),
    relayOnly: !!p.relay_only,
    nameChangeAllowedMs: Math.max(c.now, p.name_changed_ms + c.cfg.renameCooldownMs),
    cooldownUntilMs: p.cooldown_until_ms > c.now ? p.cooldown_until_ms : 0,
  };
}

export function rename(c, p, body) {
  strict(body, ['name', 'region', 'relayOnly']);
  const relayOnly = checkRelayOnly(body.relayOnly);
  // an unchanged name is not re-checked: a name taken before a filter rule was added keeps working for region / relay changes
  const keep = body.name === undefined || body.name === p.name;
  const { name, key } = keep ? { name: p.name, key: p.name_key } : validateName(body.name, { extraBlock: c.cfg.nameBlockExtra });
  const region = body.region === undefined ? p.region : checkRegion(body.region);
  return c.tx(() => {
    if (relayOnly !== undefined) c.db.run(`UPDATE profiles SET relay_only = ? WHERE pid = ?`, relayOnly, p.pid);
    if (name !== p.name || region !== p.region) {
      if (name !== p.name && p.name_changed_ms + c.cfg.renameCooldownMs > c.now) {
        throw new ApiError(429, 'rename_too_soon', 'name was changed recently', {
          retryAfterSec: Math.ceil((p.name_changed_ms + c.cfg.renameCooldownMs - c.now) / 1000),
        });
      }
      const other = c.db.get(`SELECT pid FROM profiles WHERE name_key = ? AND pid <> ?`, key, p.pid);
      if (other) throw new ApiError(409, 'name_taken', 'that name is already in use', { reason: 'taken' });
      c.db.run(
        `UPDATE profiles SET name = ?, name_key = ?, region = ?, name_changed_ms = ? WHERE pid = ?`,
        name, key, region, name !== p.name ? c.now : p.name_changed_ms, p.pid,
      );
    }
    return me(c, c.db.get(`SELECT * FROM profiles WHERE pid = ?`, p.pid));
  });
}

/** Look a player up by their public code (friend lists, "play with"). Public fields only. */
export function byCode(c, rawCode) {
  const code = normalizeCode(String(rawCode).replace(/-/g, ''), 8);
  const p = c.db.get(`SELECT pid, code, name, region FROM profiles WHERE code = ?`, code);
  if (!p) throw new ApiError(404, 'player_not_found', 'no such player');
  return { pid: p.pid, code: fmtCode(p.code), name: p.name, region: p.region };
}

/**
 * Delete the account and everything tied to it that is not a bare match record: the profile, its friend links, requests, blocks, status and invites (both
 * directions), its queue entry, scores, daily counters, flags, relay mints and problem reports. Its open rooms are closed. DELETE /v1/account (the game's
 * Delete My Online Account) and DELETE /v1/me (older builds) are this one function. A second call has no token to show: it is 401 `unauthorized`,
 * which the game reads as "already gone".
 */
export function deleteAccount(c, p) {
  c.tx(() => {
    c.db.run(`DELETE FROM scores WHERE pid = ?`, p.pid);
    c.db.run(`DELETE FROM season_scores WHERE pid = ?`, p.pid); // the player asked for everything to go
    c.db.run(`DELETE FROM allowance WHERE pid = ?`, p.pid);
    c.db.run(`DELETE FROM queue WHERE pid = ?`, p.pid);
    c.db.run(`DELETE FROM daily WHERE pid = ?`, p.pid);
    c.db.run(`DELETE FROM flags WHERE pid = ?`, p.pid);
    c.db.run(`DELETE FROM turn_mints WHERE pid = ?`, p.pid);
    forgetFriends(c, p.pid);
    forgetGuess(c, p.pid);
    forgetReports(c, p.pid); // the problem reports the player sent go with the account (Delete My Online Account)
    c.db.run(
      `UPDATE rooms SET state = 'closed', closed_reason = 'account_deleted', version = version + 1
       WHERE (host_pid = ? OR join_pid = ?) AND state NOT IN ('closed','expired')`,
      p.pid, p.pid,
    );
    c.db.run(`DELETE FROM profiles WHERE pid = ?`, p.pid);
  });
  c.fill.reset();
  c.clearBoard();
  return { ok: true };
}

/**
 * Housekeeping (alarm): accounts that never played and were not seen for PRUNE_IDLE_DAYS (180) go. "Never played" is strict: no match ever
 * started (matches_played, which outlives the 90-day match records), no win, loss, draw, abandon, verified point or ranked date, no score row
 * and no archived season row. Anyone who played is never removed, however long they are away; nothing else is touched. Without it every registration stayed
 * forever: MAX_PLAYERS (20,000) fills in about 40 days at the 500-a-day registration cap, after which nobody can sign up (`server_full`),
 * and each such account holds its name. Only accounts with no match record, no score and no ban are touched (a ban must stay), at most
 * `limit` per sweep. A console that still holds the token of a removed account is refused once and registers again with its stored
 * name (np_dwc_login.cpp), so a real player who comes back loses nothing that existed.
 */
export function pruneIdle(c, limit = 200) {
  if (!c.cfg.pruneIdleMs) return 0;
  const rows = c.db.all(
    `SELECT pid FROM profiles p WHERE banned = 0 AND last_seen_ms < ? AND matches_played = 0
       AND wins = 0 AND losses = 0 AND draws = 0 AND abandons = 0 AND points_total = 0 AND last_played_ms = 0
       AND NOT EXISTS (SELECT 1 FROM season_scores a WHERE a.pid = p.pid)
       AND NOT EXISTS (SELECT 1 FROM matches m WHERE m.p0 = p.pid OR m.p1 = p.pid)
       AND NOT EXISTS (SELECT 1 FROM scores s WHERE s.pid = p.pid)
     LIMIT ?`,
    c.now - c.cfg.pruneIdleMs, limit,
  );
  for (const { pid } of rows) {
    for (const t of ['allowance', 'queue', 'daily', 'flags', 'turn_mints']) c.db.run(`DELETE FROM ${t} WHERE pid = ?`, pid);
    forgetFriends(c, pid);
    c.db.run(`DELETE FROM profiles WHERE pid = ?`, pid);
  }
  c.fill.add(-rows.length);
  return rows.length;
}

/**
 * Sign-ups that never came back: the account was not seen again after its first five minutes (`last_seen_ms` is written only when a call comes more
 * than five minutes after the last one), never played, no score, no ban. They are removed `pruneSilentMs` (3 days) after their last sight by the sweep, and,
 * when the table is full, at 24 hours old by a registration. The scan is over a WINDOW of `last_seen_ms` (the index), never the whole table: the sweep
 * passes the hour since its last look, a full table the last week, so the rows read follow the sign-ups of the period, not the number of accounts.
 * A console that still holds the token of a removed account registers again with its stored name (np_dwc_login.cpp).
 */
export function pruneSilent(c, fromMs, toMs, limit = 200, onlyIfDue = false) {
  if (!c.cfg.pruneSilentMs) return 0;
  if (onlyIfDue) { // full-table path: at most once every ten minutes, whatever the number of sign-up attempts
    const last = c.pruneState.lastFullPrune;
    if (c.now - last < 600_000 && c.now >= last) return 0;
    c.pruneState.lastFullPrune = c.now;
  }
  const rows = c.db.all(
    `SELECT pid FROM profiles p WHERE last_seen_ms >= ? AND last_seen_ms < ? AND last_seen_ms < created_ms + 300000 AND banned = 0 AND matches_played = 0
       AND wins = 0 AND losses = 0 AND draws = 0 AND abandons = 0 AND points_total = 0 AND last_played_ms = 0
       AND NOT EXISTS (SELECT 1 FROM season_scores a WHERE a.pid = p.pid)
       AND NOT EXISTS (SELECT 1 FROM matches m WHERE m.p0 = p.pid OR m.p1 = p.pid)
       AND NOT EXISTS (SELECT 1 FROM scores s WHERE s.pid = p.pid)
     LIMIT ?`,
    fromMs, toMs, limit,
  );
  for (const { pid } of rows) {
    for (const t of ['allowance', 'queue', 'daily', 'flags', 'turn_mints']) c.db.run(`DELETE FROM ${t} WHERE pid = ?`, pid);
    forgetFriends(c, pid);
    c.db.run(`DELETE FROM profiles WHERE pid = ?`, pid);
  }
  c.fill.add(-rows.length);
  return rows.length;
}

export function checkPid(v) {
  return isInt(v, 1, 2147483647);
}
