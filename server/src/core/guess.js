// Wrong-code guessing limits (wave 13, O2). A room code (six characters) or a friend code (eight) that does not lead anywhere is a "miss". Misses are counted
// per network address and per account, in storage, so an object that leaves memory (about ten seconds when idle) does not forget them: the in-memory token
// buckets of the rest of the service reset on every eviction, which is exactly what a slow guesser waits for (VitAdhoc review M1).
//
//   per address: 30 misses in 10 minutes and 200 a UTC day (a meet-up or a carrier NAT shares one address);  per account: 10 and 40 (Galaxy's numbers).   Over a limit the route answers 429
//   `rate_limited` (reason `code_guesses`, retryAfterSec, Retry-After) BEFORE it looks at the code, so a limited caller learns nothing about any code.
//
// Privacy: the address is never written. Its key is a fingerprint, the first 16 hex digits of SHA-256(daily salt + address key); the salt is 16 random bytes in
// `settings` (`guess_salt`, "<day>:<hex>"), renewed at the first use of each UTC day, and the rows of earlier days are deleted then. So yesterday's
// fingerprints cannot be matched to today's. The salt is in the same database, so this is not secrecy against someone who holds it: the address is simply not stored, and the salt goes with the day's rows. The account key is the profile id (it goes with the account).
// Cost: one read of two rows per guessable request; on a miss two row writes (none once the caller is limited). The sweep drops rows of earlier days.

import { ApiError, hex, randomBytes, sha256hex } from './util.js';
import { dayIndex, secondsToMidnight } from './limits.js';

/** Error codes of the routes that mean "that code leads nowhere" (a hit on an existing but unusable room counts too: it confirms the code exists). */
export const MISS_CODES = new Set(['room_not_found', 'room_expired', 'room_closed', 'room_full', 'player_not_found']);

export function isMiss(e) {
  return e instanceof ApiError && MISS_CODES.has(e.code);
}

/** Today's salt (created at the first call of a UTC day; the rows of earlier days go with the old salt). */
function saltOf(c) {
  const day = dayIndex(c.now);
  const row = c.db.get(`SELECT value FROM settings WHERE key = 'guess_salt'`);
  if (row) {
    const [d, h] = String(row.value).split(':');
    if (Number(d) === day && /^[0-9a-f]{32}$/.test(h || '')) return h;
  }
  const fresh = hex(randomBytes(16));
  c.tx(() => {
    c.db.run(`INSERT INTO settings (key, value) VALUES ('guess_salt', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, `${day}:${fresh}`);
    c.db.run(`DELETE FROM guess_misses WHERE day <> ?`, day);
  });
  return fresh;
}

/** The address key's fingerprint for today. Async only because SHA-256 is. */
export async function fingerprint(c, ipk) {
  return (await sha256hex(`${saltOf(c)}|${ipk}`)).slice(0, 16);
}

function row(c, k) {
  return c.db.get(`SELECT day, win_ms, n_win, n_day FROM guess_misses WHERE k = ?`, k);
}

/** Seconds to wait when the counter at `k` is at a limit, else 0. */
function waitFor(c, k, winLimit, dayLimit) {
  const r = row(c, k);
  if (!r || r.day !== dayIndex(c.now)) return 0;
  let wait = 0;
  if (dayLimit && r.n_day >= dayLimit) wait = secondsToMidnight(c.now);
  const g = c.cfg.guess;
  if (winLimit && r.n_win >= winLimit && c.now - r.win_ms < g.windowMs) wait = Math.max(wait, Math.max(1, Math.ceil((r.win_ms + g.windowMs - c.now) / 1000)));
  return wait;
}

/** Throws 429 when the address or the account is over its limit. Call before the code is looked at. */
export function check(c, fp, pid) {
  const g = c.cfg.guess;
  const wait = Math.max(
    fp ? waitFor(c, `a:${fp}`, g.addrWindow, g.addrDay) : 0,
    pid != null ? waitFor(c, `p:${pid}`, g.acctWindow, g.acctDay) : 0,
  );
  if (wait > 0) {
    throw new ApiError(429, 'rate_limited', 'too many wrong codes, wait a little', { retryAfterSec: wait, reason: 'code_guesses' }, { 'retry-after': String(wait) });
  }
}

function bump(c, k) {
  const day = dayIndex(c.now);
  const windowMs = c.cfg.guess.windowMs;
  c.db.run(
    `INSERT INTO guess_misses (k, day, win_ms, n_win, n_day) VALUES (?1, ?2, ?3, 1, 1)
     ON CONFLICT(k) DO UPDATE SET
       n_day = CASE WHEN day = ?2 THEN n_day + 1 ELSE 1 END,
       n_win = CASE WHEN day = ?2 AND ?3 - win_ms < ?4 THEN n_win + 1 ELSE 1 END,
       win_ms = CASE WHEN day = ?2 AND ?3 - win_ms < ?4 THEN win_ms ELSE ?3 END,
       day = ?2`,
    k, day, c.now, windowMs,
  );
}

/** One wrong code: counted against the address and the account (each only when its limits are on). */
export function miss(c, fp, pid) {
  const g = c.cfg.guess;
  c.tx(() => {
    if (fp && (g.addrWindow || g.addrDay)) bump(c, `a:${fp}`);
    if (pid != null && (g.acctWindow || g.acctDay)) bump(c, `p:${pid}`);
  });
}

/** The housekeeping: counters of earlier days (an account's row of yesterday is worth nothing today). */
export function prune(c) {
  c.db.run(`DELETE FROM guess_misses WHERE day < ?`, dayIndex(c.now));
}

/** The account's counter goes with the account. */
export function forget(c, pid) {
  c.db.run(`DELETE FROM guess_misses WHERE k = ?`, `p:${pid}`);
}
