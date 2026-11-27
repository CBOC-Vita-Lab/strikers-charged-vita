// Server configuration: wrangler vars / secrets (env) with safe defaults, plus runtime overrides
// stored in the settings table (admin kill switches).

import { envInt } from './util.js';

export const PROTO = 1;
export const KILL_NAMES = ['all', 'register', 'rooms', 'ranked', 'ranking', 'turn'];

function hexToBytes(h) {
  if (typeof h !== 'string' || h.length % 2 || !/^[0-9a-fA-F]*$/.test(h) || !h.length) return null;
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
  return out;
}

/**
 * Static part of the configuration (from env). Cheap; build once per object start.
 * @param {Record<string, any>} env
 */
function parseServerList(raw) {
  try {
    const a = JSON.parse(raw || '[]');
    return Array.isArray(a)
      ? a.filter((e) => e && typeof e.url === 'string' && /^https:\/\/[A-Za-z0-9.-]+(:\d+)?\/?$/.test(e.url))
        .slice(0, 8).map((e, i) => ({ url: e.url.replace(/\/$/, ''), priority: Number.isInteger(e.priority) ? e.priority : i }))
      : [];
  } catch {
    return [];
  }
}

export function loadConfig(env) {
  env = env || {};
  const list = (v) => String(v || '').split(',').map((s) => s.trim()).filter(Boolean);
  const policy = String(env.RANK_PUT_POLICY || 'credit');
  return {
    minProto: envInt(env, 'MIN_CLIENT_PROTO', 1, 1, 1000),
    minApp: String(env.MIN_APP_VERSION || ''),
    latestApp: String(env.LATEST_APP_VERSION || ''),
    requireApp: env.REQUIRE_APP_VERSION === '1',
    serverList: parseServerList(env.SERVER_LIST_JSON),
    killEnv: list(env.KILL_SWITCH),
    maintenanceMessage: String(env.MAINTENANCE_MESSAGE || '').slice(0, 200),
    nameBlockExtra: list(env.NAME_BLOCKLIST_EXTRA),
    allowTestClock: env.ALLOW_TEST_CLOCK === '1',
    rateScale: Number(env.RATE_LIMIT_SCALE) > 0 ? Number(env.RATE_LIMIT_SCALE) : 1,
    maxPlayers: envInt(env, 'MAX_PLAYERS', 200000, 1, 10_000_000), // README.md: free-tier arithmetic
    // accounts that never played (no match record, no score) and were not seen for this long are removed by the sweep; 0 = never
    // best rows kept per archived season (archive.js)
    archiveTop: envInt(env, 'ARCHIVE_TOP', 100, 10, 10000),
    pruneIdleMs: envInt(env, 'PRUNE_IDLE_DAYS', 180, 0, 3650) * 86400_000,
    // sign-ups that never came back (no call after the first 5 minutes) and never played are removed after this many days, and first when the table is full; 0 = never
    pruneSilentMs: envInt(env, 'PRUNE_SILENT_DAYS', 3, 0, 3650) * 86400_000,
    renameCooldownMs: envInt(env, 'RENAME_COOLDOWN_HOURS', 24, 0, 24 * 365) * 3600_000,
    stunServers: list(env.STUN_SERVERS || 'stun.cloudflare.com:3478'),
    rooms: {
      openTtlMs: envInt(env, 'ROOM_OPEN_TTL_SEC', 600, 10, 86400) * 1000,
      joinedTtlMs: envInt(env, 'ROOM_JOINED_TTL_SEC', 1200, 10, 86400) * 1000,
      startedTtlMs: envInt(env, 'ROOM_STARTED_TTL_SEC', 3 * 3600, 60, 86400) * 1000,
      keepClosedMs: envInt(env, 'ROOM_KEEP_CLOSED_SEC', 300, 0, 86400) * 1000,
      ackMs: envInt(env, 'JOIN_ACK_SEC', 5, 1, 120) * 1000,
      natLifeMs: envInt(env, 'NAT_LIFE_SEC', 30, 5, 600) * 1000,
      pairFailLimit: envInt(env, 'PAIR_FAIL_LIMIT', 2, 1, 100),
      // 1 (default): an endpoint call whose `public` address (or srflx / observed candidate) is not the address the request came from puts that
      // member on the relay (rooms.js postEndpoint). 0 = trust the claims (the 1.3 builds' behaviour before this check).
      checkClaims: envInt(env, 'ENDPOINT_CHECK', 1, 0, 1) === 1,
      pairFailKeepMs: envInt(env, 'PAIR_FAIL_KEEP_DAYS', 7, 1, 365) * 86400_000,
      lostMs: envInt(env, 'LINK_LOST_SEC', 15, 5, 300) * 1000,
      quietMs: envInt(env, 'LINK_PING_SEC', 5, 1, 60) * 1000,
      punchDelayMs: envInt(env, 'PUNCH_DELAY_MS', 1000, 0, 10000),
      punch: {
        intervalMs: envInt(env, 'PUNCH_INTERVAL_MS', 700, 50, 5000),
        tries: envInt(env, 'PUNCH_TRIES', 7, 1, 50),
        lingerMs: envInt(env, 'PUNCH_LINGER_MS', 5000, 0, 60000),
      },
    },
    queue: {
      staleMs: envInt(env, 'QUEUE_STALE_SEC', 30, 5, 600) * 1000,
      baseWindow: envInt(env, 'QUEUE_BASE_WINDOW', 100, 0, 5000),
      step: envInt(env, 'QUEUE_WINDOW_STEP', 50, 1, 5000),
      stepMs: envInt(env, 'QUEUE_WINDOW_STEP_SEC', 5, 1, 600) * 1000,
      maxWindow: envInt(env, 'QUEUE_MAX_WINDOW', 1000, 0, 100000),
      regionStrictMs: envInt(env, 'QUEUE_REGION_STRICT_SEC', 20, 0, 600) * 1000,
      maxWaitMs: envInt(env, 'QUEUE_MAX_WAIT_SEC', 300, 10, 3600) * 1000,
    },
    matches: {
      resultWindowMs: envInt(env, 'RESULT_WINDOW_SEC', 120, 5, 3600) * 1000,
      maxMatchMs: envInt(env, 'MAX_MATCH_SEC', 90 * 60, 60, 86400) * 1000,
      startWaitMs: envInt(env, 'MATCH_START_WAIT_SEC', 300, 10, 3600) * 1000,
      keepMs: envInt(env, 'MATCH_KEEP_DAYS', 90, 1, 3650) * 86400_000,
      forfeitPoints: envInt(env, 'FORFEIT_POINTS', 10, 0, 100),
      restoreCredit: envInt(env, 'RESTORE_CREDIT', 5, 0, 100),
      eloK: envInt(env, 'ELO_K', 24, 1, 200),
      cooldownBaseSec: envInt(env, 'ABANDON_COOLDOWN_SEC', 60, 0, 3600),
      cooldownMaxSec: envInt(env, 'ABANDON_COOLDOWN_MAX_SEC', 900, 0, 86400),
      // Shortest plausible ranked match for a normal finish (result 0). The game's shortest timed match is 2 minutes
      // (sGameTimeOptions {120,180,240,300}); a goal-limited match (limit 3..10) can end sooner, so the default is
      // well under 120 s. 0 turns the check off.
      minMatchMs: envInt(env, 'MIN_MATCH_SEC', 60, 0, 3600) * 1000,
      // Rated (verified, ranked) matches per UTC day. 0 = no cap.
      ratedPerPairDay: envInt(env, 'RATED_PER_PAIR_DAY', 4, 0, 1000),
      ratedPerAccountDay: envInt(env, 'RATED_PER_ACCOUNT_DAY', 30, 0, 10000),
      // Win-trading flag: >= oneOppMinWins rated wins in the last oneOppDays and >= oneOppSharePct % of them against one opponent.
      oneOppMinWins: envInt(env, 'ONE_OPP_MIN_WINS', 8, 1, 10000),
      oneOppSharePct: envInt(env, 'ONE_OPP_SHARE_PCT', 70, 1, 100),
      oneOppDays: envInt(env, 'ONE_OPP_DAYS', 30, 1, 365),
      // Dodge rule: over the last dodgeWindow settled ranked matches, a player blamed for >= dodgeRatePct % disputes /
      // no-contests (at least dodgeMinSample matches) is flagged and takes dodgePenaltyPct % of an abandon's rating loss.
      dodgeWindow: envInt(env, 'DODGE_WINDOW', 20, 3, 200),
      dodgeMinSample: envInt(env, 'DODGE_MIN_SAMPLE', 6, 1, 200),
      dodgeRatePct: envInt(env, 'DODGE_RATE_PCT', 35, 1, 100),
      dodgePenaltyPct: envInt(env, 'DODGE_PENALTY_PCT', 75, 0, 100),
    },
    // Per-account daily caps (UTC day, stored); 0 = no cap. Every room and every match record costs rows on the free tier.
    caps: {
      roomsPerAccountDay: envInt(env, 'ROOMS_PER_ACCOUNT_DAY', 60, 0, 100000),
      matchesPerAccountDay: envInt(env, 'MATCHES_PER_ACCOUNT_DAY', 60, 0, 100000),
    },
    rank: {
      hmacKey: hexToBytes(env.RANK_HMAC_KEY_HEX),
      requireHmac: env.RANK_REQUIRE_HMAC !== '0',
      checkName: env.RANK_CHECK_NAME !== '0',
      putPolicy: ['credit', 'delta', 'open'].includes(policy) ? policy : 'credit',
      maxDelta: envInt(env, 'RANK_MAX_DELTA', 25, 0, 999999),
      maxScore: envInt(env, 'RANK_MAX_SCORE', 999999, 1, 2147483647),
      allowanceCap: envInt(env, 'RANK_ALLOWANCE_CAP', 200, 0, 999999),
      regionMode: env.RANK_REGION_MODE === 'mask' ? 'mask' : 'pool',
    },
    turn: {
      provider: String(env.TURN_PROVIDER || ''), // 'cloudflare' | 'stub' | '' (off)
      keyId: String(env.TURN_KEY_ID || ''),
      stubUrls: String(env.TURN_STUB_URLS || ''), // tests: comma separated turn: URLs handed out by the stub provider
      stubSecret: String(env.TURN_STUB_SECRET || ''), // tests: secret that signs the stub credentials (TURN REST scheme)
      apiToken: String(env.TURN_KEY_API_TOKEN || ''), // secret; never returned to clients
      ttlSec: envInt(env, 'TURN_TTL_SEC', 3600, 300, 86400),
      maxPerDay: envInt(env, 'TURN_MAX_CREDS_PER_DAY', 300, 0, 1_000_000),
      maxPerPlayerDay: envInt(env, 'TURN_MAX_CREDS_PER_PLAYER_DAY', 6, 0, 10_000),
      // Accounts younger than newAccountHours share a sub-budget of the daily credentials, so a burst of fresh accounts
      // cannot use up the day for established players. 0 hours = no distinction.
      newAccountHours: envInt(env, 'TURN_NEW_ACCOUNT_HOURS', 24, 0, 24 * 365),
      newAccountDaily: envInt(env, 'TURN_NEW_ACCOUNT_DAILY', 60, 0, 1_000_000),
      // Usage reports: only against a credential minted within its TTL (+ grace for the report after the match),
      // clamped to what that TTL can carry at maxKbps, at most maxReportsPerPlayerDay per player per day.
      maxKbps: envInt(env, 'TURN_MAX_KBPS', 128, 1, 100_000),
      reportGraceMs: envInt(env, 'TURN_REPORT_GRACE_SEC', 300, 0, 3600) * 1000,
      maxReportsPerPlayerDay: envInt(env, 'TURN_MAX_REPORTS_PER_PLAYER_DAY', 12, 0, 10_000),
      monthlyBudgetMb: envInt(env, 'TURN_MONTHLY_BUDGET_MB', 100_000, 0, 100_000_000),
      maxReportMb: envInt(env, 'TURN_MAX_REPORT_MB', 200, 1, 10_000),
    },
    register: {
      // Per client address (IPv6 by /64): a burst of ipBurst registrations (a venue full of Vitas on one NAT), then ipPerHour
      // per hour. Requests that fail validation never touch these buckets or the global one; they are counted in their own
      // (badPerHour) bucket instead.
      ipBurst: Math.max(envInt(env, 'REGISTER_IP_BURST', 10, 1, 1000), envInt(env, 'REGISTER_IP_PER_HOUR', 3, 1, 1000)), // never below the sustained rate
      ipPerHour: envInt(env, 'REGISTER_IP_PER_HOUR', 3, 1, 1000),
      badPerHour: envInt(env, 'REGISTER_BAD_PER_HOUR', 30, 1, 10000),
      maxPerDay: envInt(env, 'REGISTER_MAX_PER_DAY', 1000, 1, 10_000_000),
      // Proof of work (PROTOCOL.md section 3): leading zero bits of SHA-256 the client must find; 0 = off (default).
      powBits: envInt(env, 'REGISTER_POW_BITS', 0, 0, 28),
    },
    // The daily request budget of THIS service (core/budget.js). The free plan's 100,000 Worker and 100,000 Durable Object requests a day are
    // per ACCOUNT and shared with the Galaxy service: this is our share, one number. 0 = no guard.
    budget: {
      daily: envInt(env, 'DAILY_REQUEST_BUDGET', 45000, 0, 100_000_000),
      pacePct: envInt(env, 'BUDGET_PACE_PCT', 50, 1, 99), // from here answers to polls carry pollScale
      browsePct: envInt(env, 'BUDGET_BROWSE_PCT', 75, 1, 100), // from here rankings / public board data are refused
      newPct: 100 - envInt(env, 'LIVE_RESERVE_PCT', 20, 0, 90), // from here no new sessions; the reserve is for matches in progress
      maxScale: Math.min(1.5, Math.max(1, Number(env.POLL_SCALE_MAX) > 0 ? Number(env.POLL_SCALE_MAX) : 1.4)),
      accountDay: envInt(env, 'ACCOUNT_DAY_REQUESTS', 9000, 0, 100_000_000), // requests per account per UTC day; 0 = no ceiling
      addrOpenDay: envInt(env, 'ADDR_OPEN_DAY', 1000, 0, 100_000_000), // per address, routes without a token
      addrDay: envInt(env, 'ADDR_DAY_REQUESTS', 30000, 0, 100_000_000), // per address, everything else (a venue of consoles behind one NAT)
    },
    // Held polls (PROTOCOL.md "Held polls"): a poll with `wait=<sec>` is answered when something changes or after that long (at most this).
    // HOLD_MAX_SEC=0 turns holding off (every poll is answered at once, as before).
    hold: {
      maxMs: envInt(env, 'HOLD_MAX_SEC', 55, 0, 55) * 1000,
      queueTickMs: envInt(env, 'HOLD_QUEUE_TICK_SEC', 2, 1, 10) * 1000, // how often a held queue poll looks for a partner (the old polling rate)
    },
    adminToken: String(env.ADMIN_TOKEN || ''), // secret
    adminEnabled: !!env.ADMIN_TOKEN,
  };
}

