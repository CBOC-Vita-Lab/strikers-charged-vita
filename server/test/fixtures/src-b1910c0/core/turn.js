// TURN credential minting. The provider call sits behind a tiny interface so tests and local runs use a stub and
// production uses Cloudflare Realtime TURN. The long-lived TURN key API token never leaves the Worker; clients only
// get short-lived username/credential pairs, and only while they are in a live room.

import { ApiError, b64url, randomBytes, isInt, strict } from './util.js';
import { loadRoom, slotOf } from './rooms.js';
import { bumpDaily, dailyCapError, getDaily } from './limits.js';

/**
 * @typedef {{urls: string[], username: string, credential: string}} IceServer
 * @typedef {{mint(ttlSec: number): Promise<{servers: IceServer[]}>}} TurnProvider
 */

/** Cloudflare Realtime TURN: POST .../turn/keys/{id}/credentials/generate-ice-servers. */
export function cloudflareProvider(cfg, fetchImpl = fetch) {
  return {
    async mint(ttlSec) {
      const url = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(cfg.keyId)}/credentials/generate-ice-servers`;
      const resp = await fetchImpl(url, {
        method: 'POST',
        headers: { authorization: `Bearer ${cfg.apiToken}`, 'content-type': 'application/json' },
        body: JSON.stringify({ ttl: ttlSec }),
      });
      if (!resp.ok) throw new ApiError(502, 'turn_upstream', 'the relay service refused the request');
      const data = await resp.json();
      const list = Array.isArray(data.iceServers) ? data.iceServers : data.iceServers ? [data.iceServers] : [];
      return { servers: list };
    },
  };
}

/**
 * Fake credentials for local development and tests. By default random and meaningless (turn.example.invalid). With
 * `TURN_STUB_URLS` (comma separated turn: URLs) and `TURN_STUB_SECRET` they follow the "TURN REST" scheme that coturn's
 * `use-auth-secret` and the client's test TURN server (native/tools/net/turnsim.cpp) check: username `<expiry unix s>:<id>`,
 * credential = base64(HMAC-SHA1(secret, username)). Never used with the real provider.
 */
export function stubProvider(cfg = {}) {
  const urlList = String(cfg.stubUrls || '').split(',').map((u) => u.trim()).filter(Boolean);
  const secret = String(cfg.stubSecret || '');
  return {
    async mint(ttlSec) {
      const exp = Math.floor(Date.now() / 1000) + ttlSec;
      if (urlList.length && secret) {
        const username = `${exp}:stub-${b64url(randomBytes(6))}`;
        const enc = new TextEncoder();
        const key = await crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
        const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, enc.encode(username)));
        let bin = '';
        for (const b of mac) bin += String.fromCharCode(b);
        return { servers: [{ urls: ['stun:stun.cloudflare.com:3478'] }, { urls: urlList, username, credential: btoa(bin) }] };
      }
      return {
        servers: [
          { urls: ['stun:stun.cloudflare.com:3478'] },
          {
            urls: ['turn:turn.example.invalid:3478?transport=udp', 'turn:turn.example.invalid:53?transport=udp', 'turn:turn.example.invalid:3478?transport=tcp'],
            username: `${exp}:stub-${b64url(randomBytes(6))}`,
            credential: b64url(randomBytes(18)),
          },
        ],
      };
    },
  };
}

export function createProvider(cfg, fetchImpl) {
  if (cfg.provider === 'stub') return stubProvider(cfg);
  if (cfg.provider === 'cloudflare' && cfg.keyId && cfg.apiToken) return cloudflareProvider(cfg, fetchImpl);
  return null;
}

/** The Vita has no TCP (SOSocket2 refuses it) and no TLS-to-relay: keep UDP TURN and plain STUN only. */
export function udpOnly(servers) {
  const out = [];
  for (const s of servers) {
    const urls = (Array.isArray(s.urls) ? s.urls : [s.urls]).filter(
      (u) => typeof u === 'string' && /^(stun|turn):/.test(u) && !/transport=tcp/i.test(u),
    );
    if (urls.length) out.push({ urls, ...(s.username ? { username: s.username, credential: s.credential } : {}) });
  }
  return out;
}

const day = (ms) => new Date(ms).toISOString().slice(0, 10);
const month = (ms) => new Date(ms).toISOString().slice(0, 7);

function counter(c, key) {
  const r = c.db.get(`SELECT value FROM counters WHERE key = ?`, key);
  return r ? r.value : 0;
}
function bumpCounter(c, key, by) {
  c.db.run(
    `INSERT INTO counters (key, value, updated_ms) VALUES (?, ?, ?)
     ON CONFLICT(key) DO UPDATE SET value = value + excluded.value, updated_ms = excluded.updated_ms`,
    key, by, c.now,
  );
}

export function turnStatus(c) {
  const t = c.cfg.turn;
  return {
    enabled: !!c.turn,
    credsToday: counter(c, `turn:creds:${day(c.now)}`),
    maxPerDay: t.maxPerDay,
    estimatedMbThisMonth: counter(c, `turn:mb:${month(c.now)}`),
    monthlyBudgetMb: t.monthlyBudgetMb,
  };
}

export async function mintTurn(c, p, body) {
  const t = c.cfg.turn;
  if (!c.turn) throw new ApiError(503, 'turn_unavailable', 'the relay is not configured');
  strict(body, ['room']);
  // Reserve budget and check room membership in one synchronous step, before the network call.
  const reserved = c.tx(() => {
    const r = loadRoom(c, String(body.room || '').toUpperCase());
    if (!r || slotOf(r, p.pid) < 0 || !['joined', 'ready', 'started'].includes(r.state)) {
      throw new ApiError(403, 'turn_not_allowed', 'relay credentials are only issued inside an active room');
    }
    if (counter(c, `turn:creds:${day(c.now)}`) >= t.maxPerDay) {
      throw new ApiError(503, 'turn_budget', 'the relay budget for today is used up', { retryAfterSec: 3600 });
    }
    if (counter(c, `turn:mb:${month(c.now)}`) >= t.monthlyBudgetMb) {
      throw new ApiError(503, 'turn_budget', 'the relay budget for this month is used up', { retryAfterSec: 86400 });
    }
    if (counter(c, `turn:creds:${day(c.now)}:p${p.pid}`) >= t.maxPerPlayerDay) {
      throw new ApiError(429, 'turn_player_limit', 'too many relay credentials requested today', { retryAfterSec: 3600 });
    }
    // fresh accounts have their own slice of the day, so a swarm of new accounts cannot starve established players
    const fresh = t.newAccountHours > 0 && c.now - p.created_ms < t.newAccountHours * 3600_000;
    if (fresh && counter(c, `turn:creds:${day(c.now)}:new`) >= t.newAccountDaily) {
      throw new ApiError(503, 'turn_budget', 'the relay is busy for new accounts today', { retryAfterSec: 3600 });
    }
    bumpCounter(c, `turn:creds:${day(c.now)}`, 1);
    bumpCounter(c, `turn:creds:${day(c.now)}:p${p.pid}`, 1);
    if (fresh) bumpCounter(c, `turn:creds:${day(c.now)}:new`, 1);
    // remember the mint: usage reports are only accepted against a credential that is still valid
    const row = c.db.get(`INSERT INTO turn_mints (pid, minted_ms) VALUES (?, ?) RETURNING id`, p.pid, c.now);
    return { mintId: row.id, fresh };
  });
  let minted;
  try {
    minted = await c.turn.mint(t.ttlSec);
  } catch (e) {
    // give the reservation back
    c.tx(() => {
      bumpCounter(c, `turn:creds:${day(c.now)}`, -1);
      bumpCounter(c, `turn:creds:${day(c.now)}:p${p.pid}`, -1);
      if (reserved.fresh) bumpCounter(c, `turn:creds:${day(c.now)}:new`, -1);
      c.db.run(`DELETE FROM turn_mints WHERE id = ?`, reserved.mintId);
    });
    if (e instanceof ApiError) throw e;
    throw new ApiError(502, 'turn_upstream', 'the relay service is unavailable');
  }
  return { ttlSec: t.ttlSec, expiresMs: c.now + t.ttlSec * 1000, servers: udpOnly(minted.servers) };
}

/** Most bytes one credential can plausibly carry: its TTL at TURN_MAX_KBPS (and never more than TURN_MAX_REPORT_MB). */
export function reportCapBytes(t) {
  return Math.min(t.maxReportMb * 1048576, Math.floor((t.ttlSec * t.maxKbps * 1000) / 8));
}

/**
 * Clients report roughly how much they relayed so the monthly budget can trip a breaker. The numbers are untrusted,
 * so a report only counts when (1) the player minted credentials within the credential TTL (plus a short grace for the
 * report that follows the match), (2) it is clamped to what one TTL can carry at a sane bitrate, and the sum over the
 * credentials still valid cannot exceed their combined capacity, and (3) at most TURN_MAX_REPORTS_PER_PLAYER_DAY
 * reports are accepted per player per UTC day. Without this a few accounts could report huge numbers and trip the
 * monthly breaker for everybody.
 */
export function reportTurnUsage(c, p, body) {
  strict(body, ['bytes']);
  if (!isInt(body.bytes, 0, 2 ** 40)) {
    throw new ApiError(400, 'bad_request', 'bytes must be a non-negative integer');
  }
  const t = c.cfg.turn;
  return c.tx(() => {
    const mints = c.db.all(
      `SELECT id, used_bytes FROM turn_mints WHERE pid = ? AND minted_ms >= ? ORDER BY minted_ms`,
      p.pid, c.now - (t.ttlSec * 1000 + t.reportGraceMs),
    );
    if (!mints.length) throw new ApiError(403, 'turn_not_allowed', 'usage can only be reported while relay credentials you minted are valid');
    if (t.maxReportsPerPlayerDay && getDaily(c, p.pid).turn_reports >= t.maxReportsPerPlayerDay) throw dailyCapError(c, 'turn_reports');
    const cap = reportCapBytes(t);
    let left = Math.min(body.bytes, cap); // clamp each report
    const asked = body.bytes;
    let counted = 0;
    for (const m of mints) {
      if (left <= 0) break;
      const room = Math.max(0, cap - m.used_bytes); // and each credential only ever carries `cap`
      const take = Math.min(left, room);
      if (take > 0) c.db.run(`UPDATE turn_mints SET used_bytes = used_bytes + ? WHERE id = ?`, take, m.id);
      left -= take;
      counted += take;
    }
    bumpDaily(c, p.pid, 'turn_reports');
    const mb = Math.ceil(counted / 1048576);
    if (mb > 0) bumpCounter(c, `turn:mb:${month(c.now)}`, mb);
    return { ok: true, countedBytes: counted, clamped: counted < asked };
  });
}
