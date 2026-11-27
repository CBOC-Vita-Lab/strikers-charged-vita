// The measured TURN hard stop (wave 13, O1). The estimate guard in turn.js counts credentials and the bytes clients report, and an abuser reports nothing
// (VitAdhoc review H2): the only number that cannot be lied to is Cloudflare's own. This module reads the egress of the TURN key for the current UTC month
// from the Cloudflare GraphQL analytics API (dataset `callsTurnUsageAdaptiveGroups`, field `egressBytes`; the token needs "Account Analytics: Read" and
// nothing else), caches it (one read every TURN_USAGE_CACHE_SEC, 300 by default), and `check()` says whether new relay credentials must be refused because the month's
// egress is at or above TURN_REAL_BUDGET_GB. The estimate guard stays in force as the second line.
//
// The query shape was written from Cloudflare's documentation and has NOT been tried against the live API (no network from the builders), so every reply is
// read defensively: an HTTP error, a GraphQL `errors` list, a body without the expected path or a non-numeric sum is a FAILED read, never "zero".
// Fail-safe rules:
//  * a failed read never lifts a block: the last good reading of the month is kept, so an over-budget month stays closed until the next good reading says otherwise;
//  * a failed read never opens anything: with no reading the estimate guard alone decides (TURN_USAGE_REQUIRED=1 makes "no good reading within
//    TURN_USAGE_MAX_AGE_SEC" a refusal instead);
//  * after a failed read the next try waits 60 s (not the whole cache time) and at most one read is in flight.
// Nothing sensitive is logged: no token, no account tag, no key id, no response body. The log lines carry the percentage and a short failure code.

import { ApiError } from './util.js';

const GRAPHQL_URL = 'https://api.cloudflare.com/client/v4/graphql';
const FAIL_RETRY_MS = 60_000;
const REVOKE_BATCH = 25; // passes revoked per sweep
const REVOKE_EVERY_MS = 60_000; // one sweep a minute per object
const GB = 1e9; // Cloudflare's gigabyte; counting 1e9 bytes to the GB makes the guard trip a little early, never late
const ACCOUNT_RE = /^[0-9a-f]{32}$/i;
const KEY_RE = /^[A-Za-z0-9_-]{6,64}$/;

const monthOf = (ms) => new Date(ms).toISOString().slice(0, 7);
const dateOf = (ms) => new Date(ms).toISOString().slice(0, 10);

/** The GraphQL document. Inputs are validated (hex account tag, plain key id) before they are put in the text: nothing else ever reaches it. */
export function buildQuery({ accountId, keyId, scope }, now) {
  if (!ACCOUNT_RE.test(accountId)) return null;
  if (scope === 'key' && !KEY_RE.test(keyId)) return null;
  const from = `${monthOf(now)}-01`;
  const filter = `{date_geq: "${from}", date_leq: "${dateOf(now)}"${scope === 'key' ? `, keyId: "${keyId}"` : ''}}`;
  return `query { viewer { accounts(filter: {accountTag: "${accountId}"}) { callsTurnUsageAdaptiveGroups(limit: 10000, filter: ${filter}) { sum { egressBytes } } } } }`;
}

/** Sums `egressBytes` over the groups of a GraphQL answer; returns {bytes} or {error: <short code>}. */
export function parseUsage(data) {
  if (!data || typeof data !== 'object') return { error: 'bad_shape' };
  if (Array.isArray(data.errors) && data.errors.length) return { error: 'graphql_error' };
  const accounts = data.data && data.data.viewer && data.data.viewer.accounts;
  if (!Array.isArray(accounts) || accounts.length !== 1) return { error: 'bad_shape' };
  const groups = accounts[0] && accounts[0].callsTurnUsageAdaptiveGroups;
  if (!Array.isArray(groups)) return { error: 'bad_shape' };
  let bytes = 0;
  const n0 = groups.length;
  for (const g of groups) {
    const v = g && g.sum && g.sum.egressBytes;
    const n = typeof v === 'string' && /^\d{1,18}$/.test(v) ? Number(v) : v;
    if (typeof n !== 'number' || !Number.isFinite(n) || n < 0) return { error: 'bad_value' };
    bytes += n;
  }
  return { bytes, groups: n0 };
}

/**
 * @param {{turn: object, db: {get: Function, run: Function}, clock: () => number, fetchImpl?: typeof fetch, log?: (line: string) => void}} deps
 *   turn: cfg.turn (provider, keyId, usage)
 */
export function createUsageReader({ turn, db, clock, fetchImpl, log }) {
  const u = turn.usage;
  const doFetch = fetchImpl || ((...a) => fetch(...a));
  const say = log || (() => {});
  const configured = u.guard && u.budgetGb > 0 && u.token.length > 0 && u.accountId.length > 0 && turn.provider === 'cloudflare';
  const queryOk = configured && buildQuery({ accountId: u.accountId, keyId: turn.keyId, scope: u.scope }, clock()) !== null;
  // { month, bytes, okMs (last good read), triedMs (last try), error (short code of the last failure or '') }
  const state = { groups: -1, suspectLogged: false, month: '', bytes: 0, okMs: 0, triedMs: 0, error: '', loaded: false, lastPct: -1 };
  let inflight = null;
  let sweepAt = 0;
  const stats = { revoked: 0, notFound: 0, gone: 0 };

  function load() {
    if (state.loaded) return;
    state.loaded = true;
    try {
      const r = db.get(`SELECT value FROM settings WHERE key = 'turn_usage'`);
      const m = r && /^(\d{4}-\d{2}):(\d{1,13}):(\d{1,18})$/.exec(r.value);
      if (m) { state.month = m[1]; state.okMs = Number(m[2]); state.bytes = Number(m[3]); state.triedMs = state.okMs; }
    } catch { /* no row: no reading yet */ }
  }
  function save() {
    db.run(`INSERT INTO settings (key, value) VALUES ('turn_usage', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`,
      `${state.month}:${state.okMs}:${Math.floor(state.bytes)}`);
  }

  async function read(now) {
    const query = buildQuery({ accountId: u.accountId, keyId: turn.keyId, scope: u.scope }, now);
    if (!query) return { error: 'bad_config' };
    const ctl = typeof AbortController === 'function' ? new AbortController() : null;
    const timer = ctl ? setTimeout(() => ctl.abort(), u.timeoutMs) : null;
    try {
      const resp = await doFetch(GRAPHQL_URL, {
        method: 'POST',
        headers: { authorization: `Bearer ${u.token}`, 'content-type': 'application/json' },
        body: JSON.stringify({ query }),
        ...(ctl ? { signal: ctl.signal } : {}),
      });
      if (!resp || !resp.ok) return { error: `http_${resp ? resp.status : 0}` };
      let data;
      try { data = await resp.json(); } catch { return { error: 'bad_json' }; }
      return parseUsage(data);
    } catch {
      return { error: 'network' };
    } finally {
      if (timer) clearTimeout(timer);
    }
  }

  function note(now) {
    const pct = u.budgetGb > 0 ? Math.floor((state.bytes / (u.budgetGb * GB)) * 100) : 0;
    const step = pct >= 100 ? 100 : pct >= 90 ? 90 : pct >= 75 ? 75 : pct >= 50 ? 50 : 0;
    if (step !== state.lastPct) {
      state.lastPct = step;
      if (step) say(`[turn] measured relay egress ${(state.bytes / GB).toFixed(1)} GB of ${u.budgetGb} GB (${pct} %), month ${state.month}`);
    }
    void now;
  }

  return {
    enabled: configured && queryOk,
    /** Brings the reading up to date when it is older than the cache time (or the month changed). Never throws; a failure is recorded, not raised. */
    async refresh(now = clock(), { force = false } = {}) {
      if (!configured) return;
      load();
      const fresh = state.month === monthOf(now) && now - state.triedMs < u.cacheMs;
      const failing = state.error && now - state.triedMs < FAIL_RETRY_MS;
      if (!force && (fresh || failing)) return;
      if (force && now - state.triedMs < 10_000) return; // a forced read from the admin is still limited: one every 10 s
      if (!inflight) {
        inflight = (async () => {
          const r = await read(now);
          state.triedMs = now;
          if (r.error) {
            if (state.error !== r.error) say(`[turn] usage read failed: ${r.error}`);
            state.error = r.error;
          } else {
            state.error = '';
            state.month = monthOf(now);
            state.bytes = r.bytes;
            state.groups = r.groups;
            state.okMs = now;
            try { save(); } catch { /* the reading still counts in memory */ }
            note(now);
          }
        })().finally(() => { inflight = null; });
      }
      await inflight;
    },
    /** Synchronous verdict from the cached reading: null = go on (the estimate guard decides), or {why} = refuse new credentials. */
    check(now = clock()) {
      if (!configured) return null;
      load();
      const thisMonth = state.month === monthOf(now);
      if (thisMonth && state.bytes >= u.budgetGb * GB) return { why: 'over' };
      if (u.required && (!thisMonth || now - state.okMs > u.maxAgeMs)) return { why: 'no_reading' };
      return null;
    },
    /**
     * Wave 15 (Galaxy's revokeValid): while the stop is `over`, revoke the relay passes that are still valid (named, not yet revoked) so they stop carrying traffic.
     * Never throws; returns how many were revoked. One try per pass and at most REVOKE_BATCH per sweep, and a sweep at most every REVOKE_EVERY_MS per object:
     * a 404 on a pass past its TTL counts as gone, a 404 on a valid one (not known to the revoke endpoint yet) is kept for the next sweep.
     * Nothing about the passes (usernames, credentials) is logged.
     */
    async revokeValid(c) {
      if (!c.turn || typeof c.turn.revoke !== 'function') return 0;
      if (c.now - sweepAt < REVOKE_EVERY_MS && c.now >= sweepAt) return 0;
      sweepAt = c.now;
      const ttlMs = turn.ttlSec * 1000;
      const rows = c.db.all(
        `SELECT id, username, minted_ms FROM turn_mints WHERE username != '' AND revoked = 0 AND minted_ms >= ? ORDER BY id LIMIT ${REVOKE_BATCH}`,
        c.now - ttlMs,
      );
      let n = 0;
      for (const r of rows) {
        let ok = false;
        try { ok = await c.turn.revoke(r.username); } catch (e) {
          if (e && e.notFound && r.minted_ms + ttlMs <= c.now) { c.db.run(`UPDATE turn_mints SET revoked = 1 WHERE id = ?`, r.id); stats.gone++; continue; }
          if (e && e.notFound) stats.notFound++;
          ok = false;
        }
        if (ok) { c.db.run(`UPDATE turn_mints SET revoked = 1 WHERE id = ?`, r.id); n++; stats.revoked++; }
      }
      if (n) say(`[turn] the measured stop is on: ${n} relay pass(es) revoked`);
      return n;
    },
    /** An empty answer for the whole month although relays were used (>= 20 credentials minted, or clients reported relayed bytes): the filter is probably wrong, so zero cannot be trusted. */
    suspectEmpty(now = clock()) {
      if (!configured || state.groups !== 0 || state.month !== monthOf(now)) return false;
      const m = monthOf(now);
      const mints = db.get(`SELECT COALESCE(SUM(value), 0) AS n FROM counters WHERE key GLOB ? AND key NOT GLOB '*:p*' AND key NOT GLOB '*:new'`, `turn:creds:${m}-[0-9][0-9]`).n;
      const mb = db.get(`SELECT COALESCE(SUM(value), 0) AS n FROM counters WHERE key = ?`, `turn:mb:${m}`).n;
      const sus = mints >= 20 || mb > 0;
      if (sus && !state.suspectLogged) { state.suspectLogged = true; say('[turn] measured usage is EMPTY although relay credentials were minted this month: the query filter may be wrong; the estimate guard stays in force'); }
      return sus;
    },
    status(now = clock()) {
      load();
      const thisMonth = state.month === monthOf(now);
      return {
        guard: u.guard,
        configured,
        queryOk,
        scope: u.scope,
        required: u.required,
        budgetGb: u.budgetGb,
        month: thisMonth ? state.month : null,
        usedBytes: thisMonth ? Math.floor(state.bytes) : null,
        usedPct: thisMonth && u.budgetGb > 0 ? Math.round((state.bytes / (u.budgetGb * GB)) * 1000) / 10 : null,
        readMs: state.okMs || null,
        lastError: state.error || null,
        suspectEmpty: this.suspectEmpty(now),
        revokedPasses: stats.revoked,
        revokeNotFound: stats.notFound,
        revokeGone: stats.gone,
      };
    },
  };
}

export function budgetError(why) {
  const msg = why === 'no_reading' ? 'the relay is paused until its usage can be read' : 'the relay budget for this month is used up';
  return new ApiError(503, 'turn_budget', msg, { retryAfterSec: 3600, reason: 'usage' });
}
