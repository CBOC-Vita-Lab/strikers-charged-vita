// The daily request budget of this service, and the per-address / per-account daily ceilings (1.3 wave 9; the ideas are Galaxy's service
// findings 1, 2 and 4, re-built on this code).
//
// Cloudflare's free plan gives the whole ACCOUNT 100,000 Worker requests and 100,000 Durable Object requests a day, and the Galaxy service
// shares them with this one. Every client request that reaches the object spends one of each. `DAILY_REQUEST_BUDGET` is THIS service's share
// (one number; the other service has its own). The object counts what reaches it (alarms included), keeps the count in memory and writes it
// to `settings` every FLUSH_EVERY requests (a restarted object adds a margin for what was not written), and answers by class:
//
//   share used     what happens
//   < pace%  (50)  nothing
//   pace..new% (80) answers to polls carry `pollScale` 1..1.4: clients that understand it stretch their holds and gaps (fewer requests)
//   >= browse% (75) browsing is refused (503 over_budget, x-msc-budget: browse-over): rank/get, the public board, /me/matches, /players
//   >= new%  (80)  new sessions are refused too (x-msc-budget: new-over): register, create / join a room, join the ranked queue.
//                  The last 20 % is the reserve for matches in progress: room, queue, result, rank put and relay calls still work
//   >= 100 %       everything but /admin is refused (x-msc-budget: over)
//
// The Worker reads `x-msc-budget` on every object answer and then answers the refused classes itself (no object request) until the end
// of the UTC day or for BLOCK_MAX_MS, whichever is first: a refused request that reaches the Worker still counts for the platform, but it no longer
// wakes the object.
//
// Rows written (1.3 wave 11). The binding limit of the Workers Free plan is not requests but SQLite rows written: 100,000 a day for the account, shared
// with the Galaxy service, and neither service counted them. `DAILY_ROWS_BUDGET` is this service's share (default 50,000, half; the other service
// is asked for the same). The count comes from the platform's own `rowsWritten` on every statement (index rows included; the node test adapter
// applies the same rule), so every write path is counted where it happens, plus the `setAlarm()` row the object counts itself. It uses the same
// levels as the requests: the larger of the two shares decides the level and `pollScale` (fewer polls means fewer queue / room touches), the
// last LIVE_RESERVE_PCT is the reserve for matches in progress, and a log line goes out each time either share passes 50 / 75 / 80 / 100 %.
// It is persisted in its own `settings` row `budget_rows` ("day:rows"; the request count's row keeps its old format, so a rollback to the previous
// code still reads it). With no row of today (a deploy in the middle of a day) the rows are guessed as one per request already counted; a restarted
// object adds ROWS_FLUSH_EVERY as margin.
//
// Ceiling of the object's own requests: the level reaches `over` at 100 % of DAILY_REQUEST_BUDGET (45,000), counting every request that reaches the
// object, alarms included. What can still pass: /admin calls, and what reaches the object before the Worker has learned the level (the Worker then
// answers every class itself for up to BLOCK_MAX_MS), plus FLUSH_EVERY per restart of the object. `billed` in the summary is used + every
// refused request (they reach the object too).
//
// Ceilings (in memory, exact while the object lives; an eviction resets them): per account (ACCOUNT_DAY_REQUESTS, default 9,000), per address for
// the open routes (ADDR_OPEN_DAY, 1,000) and per address for everything (ADDR_DAY_REQUESTS, 30,000: a venue of consoles behind one NAT).
// Over a ceiling the answer is 429 rate_limited with reason `daily_cap` (every client treats that as final for the day) and x-msc-block:
// `addr` or `acct`, which lets the Worker refuse that address / token itself until midnight UTC. Requests refused by a ceiling or by the
// budget are counted apart (`refused`) and do not move the budget level.

import { ApiError } from './util.js';
import { dayIndex, secondsToMidnight } from './limits.js';

export const FLUSH_EVERY = 200;
export const ROWS_FLUSH_EVERY = 400; // rows written between two writes of the counters (a restarted object adds this as margin)
export const BLOCK_MAX_MS = 15 * 60 * 1000;
const MAP_CAP = 20000;

/** @returns {'open'|'new'|'browse'|'report'|'admin'|'live'} what a request is, for the budget levels (shared by the object and the Worker) */
export function classify(method, pathname) {
  if (pathname.startsWith('/v1/admin/')) return 'admin';
  if (pathname === '/v1/info' || pathname === '/v1/time' || pathname === '/v1/servers' || pathname === '/leaderboard') return 'open';
  if (pathname === '/v1/register' || pathname === '/v1/register/challenge') return 'new';
  // POST /v1/rooms/join is 'live' on purpose: a member who lost the link rejoins the room they are in even in the reserve; the object itself refuses
  // a NEW guest at new-over (rooms.joinRoom, `c.refuseNew`)
  if (method === 'POST' && (pathname === '/v1/rooms' || pathname === '/v1/ranked/queue')) return 'new';
  if (pathname === '/v1/report') return 'report'; // a problem report can wait for a quiet day: refused from the browse level on
  if (pathname === '/v1/friends' || pathname === '/v1/friends/recent') return 'browse'; // the friends list is shed with the other browsing
  if (pathname === '/v1/friends/invite') return 'new'; // an invite starts a game: it waits for a quiet day with the other new sessions
  if (pathname === '/v1/rank/get' || pathname === '/v1/public/leaderboard' || pathname === '/v1/me/matches' || pathname.startsWith('/v1/players/')) return 'browse';
  return 'live';
}

/** Is a request of class `klass` refused at budget level `level` ('', 'browse-over', 'new-over', 'over')? */
export function refusedAt(level, klass) {
  if (!level || klass === 'admin') return false;
  if (level === 'over') return true;
  if (klass === 'report') return true; // any level above ok
  if (level === 'new-over') return klass === 'new' || klass === 'browse';
  if (level === 'browse-over') return klass === 'browse';
  return false;
}

export function overBudget(level, nowMs, klass) {
  const wait = Math.min(secondsToMidnight(nowMs), 900);
  const msg = klass === 'report'
    ? 'the online service is very busy today: problem reports are not taken until tomorrow (UTC)'
    : level === 'browse-over'
    ? 'the online service is very busy today: the rankings are closed until tomorrow (UTC); matches still work'
    : level === 'new-over'
      ? 'the online service is very busy today: no new games can start until tomorrow (UTC); games in progress are not affected'
      : 'the online service has used up its allowance for today (UTC); it opens again tomorrow';
  return new ApiError(503, 'over_budget', msg, { retryAfterSec: wait, level }, { 'retry-after': String(wait), 'x-msc-budget': level });
}

/** an address that keeps sending requests that fail authentication (wrong, missing or unknown tokens) */
export const BAD_BLOCK = 'addr-bad';

export function dailyCapped(nowMs, scope, block) {
  const wait = secondsToMidnight(nowMs);
  return new ApiError(429, 'rate_limited', 'daily request limit reached, try again tomorrow (UTC)', { retryAfterSec: wait, reason: 'daily_cap', scope }, { 'retry-after': String(wait), 'x-msc-block': block });
}

const TIERS = [50, 75, 80, 100];
const tierOf = (pct) => TIERS.filter((t) => pct >= t).length;

export function createBudget({ cfg, db, clock, log = (line) => console.log(line) }) {
  const b = cfg.budget;
  const st = { day: -1, used: 0, flushed: 0, refused: 0, ceilingRefused: 0, alarms: 0, rows: 0, rowsFlushed: 0, rowsGuessed: false, tierReq: 0, tierRows: 0 };
  let addrOpen = new Map();
  let addrAll = new Map();
  let accts = new Map();
  let addrBad = new Map();

  const KEY = 'budget'; // "day:requests", the format of 1.3 wave 9 (an older build still reads it after a rollback)
  const KEY_ROWS = 'budget_rows'; // "day:rows", its own row so that the older format never changes
  function load(now) {
    st.day = dayIndex(now);
    st.used = 0;
    st.flushed = 0;
    st.rows = 0;
    st.rowsFlushed = 0;
    try {
      const row = db.get(`SELECT value FROM settings WHERE key = ?`, KEY);
      const m = row && /^(\d+):(\d+)$/.exec(row.value);
      const rr = db.get(`SELECT value FROM settings WHERE key = ?`, KEY_ROWS);
      const mr = rr && /^(\d+):(\d+)$/.exec(rr.value);
      // a restarted object does not know how many requests came after the last write: add the most there could have been
      if (m && Number(m[1]) === st.day) {
        st.used = Number(m[2]) + FLUSH_EVERY;
        st.flushed = st.used;
        // no rows counter of today (a deploy in the middle of a day): guess one row per request already counted (a mixed hour is about 2 to 3)
        const known = mr && Number(mr[1]) === st.day;
        st.rows = (known ? Number(mr[2]) : Number(m[2])) + ROWS_FLUSH_EVERY;
        st.rowsFlushed = st.rows;
        st.rowsGuessed = !known;
      }
    } catch { /* the table does not exist yet (first migration still running): start at zero */ }
  }
  function flush(now) {
    try {
      db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, KEY, `${dayIndex(now)}:${st.used}`);
      db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, KEY_ROWS, `${dayIndex(now)}:${st.rows}`);
      st.flushed = st.used;
      st.rowsFlushed = st.rows;
    } catch { /* never let bookkeeping fail a request */ }
  }
  function rollover(now) {
    if (st.day !== dayIndex(now)) {
      const first = st.day === -1;
      if (first) load(now);
      else { st.day = dayIndex(now); st.used = 0; st.flushed = 0; st.refused = 0; st.ceilingRefused = 0; st.alarms = 0; st.rows = 0; st.rowsFlushed = 0; st.rowsGuessed = false; st.tierReq = 0; st.tierRows = 0; }
      addrOpen = new Map();
      addrAll = new Map();
      accts = new Map();
      addrBad = new Map();
    }
  }
  const reqShare = (now) => { rollover(now); return b.daily ? st.used / b.daily : 0; };
  const rowsShare = (now) => { rollover(now); return b.rowsDaily ? st.rows / b.rowsDaily : 0; };
  /** the larger of the request share and the rows share decides the level and pollScale */
  const share = (now) => Math.max(reqShare(now), rowsShare(now));
  const guardOn = () => !!(b.daily || b.rowsDaily);

  function level(now) {
    if (!guardOn()) return '';
    const f = share(now);
    if (f >= 1) return 'over';
    if (f >= b.newPct / 100) return 'new-over';
    if (f >= b.browsePct / 100) return 'browse-over';
    return '';
  }

  /** 1 below `pace` % of the budget, rising to `maxScale` at `new` %; polls are stretched by it (clients that understand pollScale) */
  function pollScale(now) {
    if (!guardOn()) return 1;
    const f = share(now) * 100;
    if (f <= b.pacePct) return 1;
    const t = Math.min(1, (f - b.pacePct) / Math.max(1, b.newPct - b.pacePct));
    return Math.round((1 + (b.maxScale - 1) * t) * 100) / 100;
  }

  /** one log line each time the request share or the rows share passes 50 / 75 / 80 / 100 % (no ids; the platform's own logging is off, `wrangler tail` shows it) */
  function logTiers(now) {
    const qr = b.daily ? tierOf(reqShare(now) * 100) : 0;
    const qw = b.rowsDaily ? tierOf(rowsShare(now) * 100) : 0;
    if (qr > st.tierReq || qw > st.tierRows) {
      st.tierReq = Math.max(st.tierReq, qr);
      st.tierRows = Math.max(st.tierRows, qw);
      try {
        log(`[budget] day ${dayIndex(now)}: requests ${st.used}/${b.daily} (${Math.round(reqShare(now) * 100)} %), rows written ${st.rows}/${b.rowsDaily} (${Math.round(rowsShare(now) * 100)} %), level ${level(now) || 'ok'}, pollScale ${pollScale(now)}`);
      } catch { /* logging never fails a request */ }
    }
  }
  function bump(now, n = 1) {
    rollover(now);
    st.used += n;
    if (st.used - st.flushed >= FLUSH_EVERY || st.rows - st.rowsFlushed >= ROWS_FLUSH_EVERY) flush(now);
    logTiers(now);
  }

  function touchMap(map, key, cap) {
    let n = map.get(key);
    if (n === undefined) {
      if (map.size >= MAP_CAP) return 0; // full: an address that is not tracked is not limited (fail open, never blocks an honest one)
      n = 0;
    }
    n += 1;
    map.set(key, n);
    return n;
  }

  return {
    state: st,
    level,
    pollScale,
    /**
     * A request has reached the object. Throws the refusal (503 over_budget / 429 daily_cap) or counts it.
     * `pid` is known only after authentication: call again with it (`acct`) for the per-account ceiling.
     */
    admit(now, klass, ipk) {
      rollover(now);
      if (klass === 'admin') { bump(now); return; }
      const lv = level(now);
      if (refusedAt(lv, klass)) { st.refused++; logTiers(now); throw overBudget(lv, now, klass); }
      if (b.addrOpenDay && klass === 'open' && touchMap(addrOpen, ipk) > b.addrOpenDay) { st.ceilingRefused++; throw dailyCapped(now, 'requests', 'addr-open'); }
      if (b.addrDay && klass !== 'open' && touchMap(addrAll, ipk) > b.addrDay) { st.ceilingRefused++; throw dailyCapped(now, 'requests', 'addr'); }
      bump(now);
    },
    /** A request from `ipk` failed authentication. Over `addrBadDay` a day the address gets 429 daily_cap (x-msc-block: addr-bad) until midnight. */
    badAuth(now, ipk) {
      rollover(now);
      if (b.addrBadDay && touchMap(addrBad, ipk) > b.addrBadDay) { st.ceilingRefused++; throw dailyCapped(now, 'requests', BAD_BLOCK); }
    },
    acct(now, pid) {
      if (!b.accountDay) return;
      if (touchMap(accts, pid) > b.accountDay) { st.ceilingRefused++; throw dailyCapped(now, 'requests', 'acct'); }
    },
    /** the 60 s housekeeping alarm is a Durable Object request too */
    alarm(now) { st.alarms++; bump(now); },
    /** for /v1/admin/stats */
    summary(now) {
      return {
        day: dayIndex(now), used: (rollover(now), st.used), budget: b.daily, sharePct: b.daily ? Math.round(reqShare(now) * 1000) / 10 : null,
        rows: st.rows, rowsBudget: b.rowsDaily, rowsSharePct: b.rowsDaily ? Math.round(rowsShare(now) * 1000) / 10 : null, rowsGuessed: st.rowsGuessed,
        billed: st.used + st.refused + st.ceilingRefused,
        level: level(now) || 'ok', pollScale: pollScale(now), refused: st.refused, ceilingRefused: st.ceilingRefused, alarms: st.alarms,
        addressesTracked: addrAll.size + addrOpen.size, accountsTracked: accts.size,
      };
    },
    /**
     * `n` rows were written (the platform's `rowsWritten` of one statement, or one for `setAlarm()`). Only adds: it runs inside the statement that
     * wrote, so it never writes itself; the counters are persisted by the next request or alarm (`bump`).
     */
    rows(now, n) {
      if (!(n > 0)) return;
      rollover(now);
      st.rows += n;
    },
    flush: (now) => flush(now),
  };
}
