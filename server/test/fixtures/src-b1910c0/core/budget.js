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
// Ceilings (in memory, exact while the object lives; an eviction resets them): per account (ACCOUNT_DAY_REQUESTS, default 9,000), per address for
// the open routes (ADDR_OPEN_DAY, 1,000) and per address for everything (ADDR_DAY_REQUESTS, 30,000: a venue of consoles behind one NAT).
// Over a ceiling the answer is 429 rate_limited with reason `daily_cap` (every client treats that as final for the day) and x-msc-block:
// `addr` or `acct`, which lets the Worker refuse that address / token itself until midnight UTC. Requests refused by a ceiling or by the
// budget are counted apart (`refused`) and do not move the budget level.

import { ApiError } from './util.js';
import { dayIndex, secondsToMidnight } from './limits.js';

export const FLUSH_EVERY = 200;
export const BLOCK_MAX_MS = 15 * 60 * 1000;
const MAP_CAP = 20000;

/** @returns {'open'|'new'|'browse'|'admin'|'live'} what a request is, for the budget levels (shared by the object and the Worker) */
export function classify(method, pathname) {
  if (pathname.startsWith('/v1/admin/')) return 'admin';
  if (pathname === '/v1/info' || pathname === '/v1/time' || pathname === '/v1/servers' || pathname === '/leaderboard') return 'open';
  if (pathname === '/v1/register' || pathname === '/v1/register/challenge') return 'new';
  if (method === 'POST' && (pathname === '/v1/rooms' || pathname === '/v1/rooms/join' || pathname === '/v1/ranked/queue')) return 'new';
  if (pathname === '/v1/rank/get' || pathname === '/v1/public/leaderboard' || pathname === '/v1/me/matches' || pathname.startsWith('/v1/players/')) return 'browse';
  return 'live';
}

/** Is a request of class `klass` refused at budget level `level` ('', 'browse-over', 'new-over', 'over')? */
export function refusedAt(level, klass) {
  if (!level || klass === 'admin') return false;
  if (level === 'over') return true;
  if (level === 'new-over') return klass === 'new' || klass === 'browse';
  if (level === 'browse-over') return klass === 'browse';
  return false;
}

export function overBudget(level, nowMs) {
  const wait = Math.min(secondsToMidnight(nowMs), 900);
  const msg = level === 'browse-over'
    ? 'the online service is very busy today: the rankings are closed until tomorrow (UTC); matches still work'
    : level === 'new-over'
      ? 'the online service is very busy today: no new games can start until tomorrow (UTC); games in progress are not affected'
      : 'the online service has used up its allowance for today (UTC); it opens again tomorrow';
  return new ApiError(503, 'over_budget', msg, { retryAfterSec: wait, level }, { 'retry-after': String(wait), 'x-msc-budget': level });
}

export function dailyCapped(nowMs, scope, block) {
  const wait = secondsToMidnight(nowMs);
  return new ApiError(429, 'rate_limited', 'daily request limit reached, try again tomorrow (UTC)', { retryAfterSec: wait, reason: 'daily_cap', scope }, { 'retry-after': String(wait), 'x-msc-block': block });
}

export function createBudget({ cfg, db, clock }) {
  const b = cfg.budget;
  const st = { day: -1, used: 0, flushed: 0, refused: 0, ceilingRefused: 0, alarms: 0 };
  let addrOpen = new Map();
  let addrAll = new Map();
  let accts = new Map();

  const KEY = 'budget';
  function load(now) {
    st.day = dayIndex(now);
    st.used = 0;
    st.flushed = 0;
    try {
      const row = db.get(`SELECT value FROM settings WHERE key = ?`, KEY);
      const m = row && /^(\d+):(\d+)$/.exec(row.value);
      // a restarted object does not know how many requests came after the last write: add the most there could have been
      if (m && Number(m[1]) === st.day) { st.used = Number(m[2]) + FLUSH_EVERY; st.flushed = st.used; }
    } catch { /* the table does not exist yet (first migration still running): start at zero */ }
  }
  function flush(now) {
    try {
      db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, KEY, `${dayIndex(now)}:${st.used}`);
      st.flushed = st.used;
    } catch { /* never let bookkeeping fail a request */ }
  }
  function rollover(now) {
    if (st.day !== dayIndex(now)) {
      const first = st.day === -1;
      if (first) load(now);
      else { st.day = dayIndex(now); st.used = 0; st.flushed = 0; st.refused = 0; st.ceilingRefused = 0; st.alarms = 0; }
      addrOpen = new Map();
      addrAll = new Map();
      accts = new Map();
    }
  }
  const share = (now) => { rollover(now); return b.daily ? st.used / b.daily : 0; };

  function level(now) {
    if (!b.daily) return '';
    const f = share(now);
    if (f >= 1) return 'over';
    if (f >= b.newPct / 100) return 'new-over';
    if (f >= b.browsePct / 100) return 'browse-over';
    return '';
  }

  /** 1 below `pace` % of the budget, rising to `maxScale` at `new` %; polls are stretched by it (clients that understand pollScale) */
  function pollScale(now) {
    if (!b.daily) return 1;
    const f = share(now) * 100;
    if (f <= b.pacePct) return 1;
    const t = Math.min(1, (f - b.pacePct) / Math.max(1, b.newPct - b.pacePct));
    return Math.round((1 + (b.maxScale - 1) * t) * 100) / 100;
  }

  function bump(now, n = 1) {
    rollover(now);
    st.used += n;
    if (st.used - st.flushed >= FLUSH_EVERY) flush(now);
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
      if (refusedAt(lv, klass)) { st.refused++; throw overBudget(lv, now); }
      if (b.addrOpenDay && klass === 'open' && touchMap(addrOpen, ipk) > b.addrOpenDay) { st.ceilingRefused++; throw dailyCapped(now, 'requests', 'addr-open'); }
      if (b.addrDay && klass !== 'open' && touchMap(addrAll, ipk) > b.addrDay) { st.ceilingRefused++; throw dailyCapped(now, 'requests', 'addr'); }
      bump(now);
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
        day: dayIndex(now), used: (rollover(now), st.used), budget: b.daily, sharePct: b.daily ? Math.round(share(now) * 1000) / 10 : null,
        level: level(now) || 'ok', pollScale: pollScale(now), refused: st.refused, ceilingRefused: st.ceilingRefused, alarms: st.alarms,
        addressesTracked: addrAll.size + addrOpen.size, accountsTracked: accts.size,
      };
    },
    flush: (now) => flush(now),
  };
}
