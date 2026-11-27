// The whole service as one function: Request -> Response, over an abstract SQL handle.
// Used unchanged by the Durable Object (production), by the vitest suites and by the Node fake server.

import { ApiError, json, errorResponse, sha256hex, safeEqual, envInt, noBody, cmpVersion, ipKey, readBodyCapped, te } from './util.js';
import { loadConfig, PROTO, KILL_NAMES } from './config.js';
import { migrate } from './schema.js';
import { RateLimiter } from './ratelimit.js';
import { dayIndex } from './limits.js';
import { versionHeaderError, compilePattern, bodyLimit } from './edge.js';
import * as pow from './pow.js';
import { dateParts, dayStart, season } from './calendar.js';
import { NAME_MIN, NAME_MAX } from './names.js';
import * as accounts from './accounts.js';
import * as rooms from './rooms.js';
import * as matches from './matches.js';
import * as ranked from './ranked.js';
import * as ranking from './ranking.js';
import * as friends from './friends.js';
import * as publicboard from './publicboard.js';
import { pageResponse } from './leaderboard_page.js';
import * as turn from './turn.js';
import { createUsageReader } from './turn_usage.js';
import * as guess from './guess.js';
import * as admin from './admin.js';
import * as reports from './reports.js';
import { archiveSeasons } from './archive.js';
import { createBudget, classify, refusedAt, overBudget } from './budget.js';
import { createHolds } from './hold.js';
import { createFill } from './fill.js';

const MAX_BODY = 8192;

/**
 * @param {object} deps
 * @param {{exec: (q: string, ...b: any[]) => {toArray(): any[]}}} deps.sql
 * @param {(fn: () => any) => any} deps.tx       synchronous transaction wrapper
 * @param {Record<string, any>} [deps.env]       wrangler vars + secrets
 * @param {() => number} [deps.now]              clock in ms (tests inject their own)
 * @param {typeof fetch} [deps.fetchImpl]        for the TURN provider
 * @param {import('./turn.js').TurnProvider|null} [deps.turnProvider]
 */
export function createApp(deps) {
  const env = deps.env || {};
  const cfg = loadConfig(env);
  const clock = deps.now || (() => Date.now());
  const limiter = deps.limiter || new RateLimiter();
  // wrong-token counters live in their own map so no flood of ordinary keys can ever evict them (see ratelimit.js)
  const brake = deps.brake || new RateLimiter(50000, { protect: true });
  const powUsed = new Map();
  const regSeen = deps.regSeen || new Map(); // ipk -> registration times of the last 24 h (pow.js bitsFor), memory only
  const tx = deps.tx;
  // held polls (hold.js): what changed in this request, so the right waiters wake at its end
  const dirty = { rooms: new Set(), roomsAll: false, queue: new Set(), friends: new Set() };
  const seen = deps.seen || new Map(); // pid -> ms of the last authenticated request (friends presence; memory only, a restart shows everyone offline for a minute)
  const holds = createHolds({ timers: deps.timers, onPark: deps.onHoldPark, onResume: deps.onHoldResume });
  const holding = new Map(); // `${room code}:${slot}` -> held polls open: a member with a held poll open is present
  const holdingQueue = new Set(); // pids with a held queue poll open
  const ROOM_SQL = /^\s*(?:UPDATE|INSERT(?: OR \w+)? INTO|DELETE FROM)\s+rooms\b/i;
  const QUEUE_PAIR_SQL = /^\s*UPDATE\s+queue\s+SET\s+status\s*=\s*'matched'/i;
  function noteWrite(q, b) {
    if (ROOM_SQL.test(q)) {
      const code = /^\s*INSERT/i.test(q) ? b[0] : b[b.length - 1];
      if (typeof code === 'string' && code.length === 6) dirty.rooms.add(code); else dirty.roomsAll = true;
    } else if (QUEUE_PAIR_SQL.test(q)) {
      for (const v of b.slice(-2)) if (typeof v === 'number') dirty.queue.add(v);
    }
  }
  function flushDirty() {
    if (dirty.roomsAll) holds.wakePrefix('room:');
    else for (const code of dirty.rooms) holds.wake(`room:${code}`);
    for (const pid of dirty.queue) holds.wake(`queue:${pid}`);
    for (const pid of dirty.friends) holds.wake(`friends:${pid}`);
    dirty.friends.clear();
    dirty.rooms.clear();
    dirty.roomsAll = false;
    dirty.queue.clear();
  }
  // every statement goes through here, so every write path is counted: the cursor's `rowsWritten` (the platform's own figure, index rows
  // included) feeds the budget's rows-written estimate (budget.js). A statement that writes through RETURNING comes in by all / get.
  let rowSink = null;
  const exec = (q, ...b) => {
    const cur = deps.sql.exec(q, ...b);
    const rows = cur.toArray();
    if (rowSink && cur.rowsWritten > 0) rowSink(cur.rowsWritten);
    return rows;
  };
  const db = {
    all: (q, ...b) => exec(q, ...b),
    get: (q, ...b) => exec(q, ...b)[0] ?? null,
    run: (q, ...b) => { exec(q, ...b); noteWrite(q, b); },
  };
  const presence = deps.presence || new Map();
  const PRUNE_EVERY_MS = 3600_000;
  let lastPruneMs = 0;
  const board = publicboard.createBoard();
  const turnProvider = deps.turnProvider !== undefined ? deps.turnProvider : turn.createProvider(cfg.turn, deps.fetchImpl);

  migrate(deps.sql, clock(), deps.tx);
  const budget = createBudget({ cfg, db, clock, ...(deps.log ? { log: deps.log } : {}) });
  rowSink = (n) => budget.rows(clock(), n);
  const fill = createFill(db);
  const turnUsage = createUsageReader({ turn: cfg.turn, db, clock, fetchImpl: deps.fetchImpl, log: deps.log || ((line) => console.log(line)) });
  const state = { lastFullPrune: -Infinity, silentCursor: null };

  let settings = null;
  const loadSettings = () => {
    if (!settings) {
      const rows = db.all(`SELECT key, value FROM settings WHERE key IN ('kill','message')`);
      const m = Object.fromEntries(rows.map((r) => [r.key, r.value]));
      let kill = [];
      try { kill = JSON.parse(m.kill || '[]'); } catch { kill = []; }
      settings = { kill: new Set([...cfg.killEnv, ...kill]), message: m.message || cfg.maintenanceMessage };
    }
    return settings;
  };

  function ctxFor(now) {
    return {
      refuseNew: () => refusedAt(budget.level(now), 'new'),
      overBudget: () => overBudget(budget.level(now), now),
      log: deps.log || ((line) => console.log(line)),
      db, tx, now, cfg, turn: turnProvider, turnUsage, presence, seen, powUsed, regSeen, holding, holdingQueue, fill, pruneState: state,
      limit: (key, cap, sec) => limit(null, key, cap, sec, now),
      friendTouch: (...pids) => { for (const pid of pids) dirty.friends.add(pid); },
      budgetSummary: () => ({ ...budget.summary(now), holdsOpen: holds.parked() }),
      killSwitches: () => loadSettings().kill,
      invalidateSettings: () => { settings = null; },
      clearBoard: () => board.clear(), // a ban or an account deletion must leave the public leaderboard quickly
    };
  }

  // ------------------------------------------------------------ rate limiting
  function tooMany(wait) {
    return new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait) });
  }
  function limit(c, key, cap, windowSec, now, burst) {
    const capScaled = Math.max(1, Math.ceil(cap * cfg.rateScale));
    const burstScaled = burst === undefined ? capScaled : Math.max(capScaled, Math.ceil(burst * cfg.rateScale));
    const wait = limiter.take(key, capScaled, windowSec, now, burstScaled);
    if (wait > 0) throw tooMany(wait);
  }
  /** The brake limiter: wrong-token counters. Never scaled by RATE_LIMIT_SCALE (tests that want it exercise the real numbers). */
  function brakeTake(key, cap, windowSec, now) {
    const wait = brake.take(key, cap, windowSec, now);
    if (wait > 0) throw tooMany(wait);
  }

  // ------------------------------------------------------------ routes
  // [method, pattern, options, handler]. options: auth, kill (switch name), rl: [name, cap, windowSec, scope]
  const R = (method, pattern, opts, handler) => ({ method, pattern, re: compilePattern(pattern), opts, handler });
  const routes = [
    R('GET', '/v1/info', { rl: ['info', 120, 60, 'ip'], open: true }, (c) => info(c)),
    R('GET', '/v1/servers', { rl: ['servers', 60, 60, 'ip'], open: true }, (c) => serverList(c)),
    R('GET', '/v1/time', { rl: ['time', 120, 60, 'ip'], open: true }, (c) => timeInfo(c.now)),

    R('GET', '/v1/register/challenge', { kill: 'register', rl: ['regchallenge', 60, 60, 'ip'] }, (c, r) => pow.issueChallenge(c, r.ipk)),
    // registration: the body is validated first and only a request that can succeed is charged to the per-address and global buckets (see prepareRegister)
    R('POST', '/v1/register', { kill: 'register', register: true }, async (c, r) => {
      const out = await accounts.register(c, r.body);
      pow.noteRegistered(c, r.ipk);
      return out;
    }),
    R('GET', '/v1/me', { auth: true, rl: ['me', 60, 60, 'pid'] }, (c, r) => accounts.me(c, r.player)),
    R('PUT', '/v1/me', { auth: true, rl: ['rename', 6, 3600, 'pid'] }, (c, r) => accounts.rename(c, r.player, r.body)),
    R('DELETE', '/v1/me', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return accounts.deleteAccount(c, r.player); }),
    // the game's "Delete My Online Account" (the same function as DELETE /v1/me, which older builds call); idempotent: the second call has no token left (401 = already gone)
    R('DELETE', '/v1/account', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return accounts.deleteAccount(c, r.player); }),
    // the same, as a POST: the game's HTTPS client (np_https.cpp) speaks GET and POST only. An empty body or {}.
    R('POST', '/v1/account', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return accounts.deleteAccount(c, r.player); }),
    // a problem report the player chose to send (core/reports.js): up to 48 KiB, 3 a day; refused first when the daily budget runs short (budget.js class 'report')
    R('POST', '/v1/report', { auth: true, kill: 'report', rl: ['report', 6, 3600, 'pid'], maxBody: true }, (c, r) => reports.submitReport(c, r.player, r.body, r.rawBytes)),
    R('GET', '/v1/me/matches', { auth: true, rl: ['mymatches', 30, 60, 'pid'] }, (c, r) => ({
      matches: matches.listMyMatches(c, r.player, Math.min(50, Math.max(1, envInt({ n: r.query.get('limit') }, 'n', 10)))),
    })),

    R('GET', '/v1/players/:code', { auth: true, guess: true, rl: ['bycode', 30, 60, 'pid'] }, (c, r) => accounts.byCode(c, r.params.code)),

    R('POST', '/v1/rooms', { auth: true, kill: 'rooms', rl: ['roomnew', 10, 600, 'pid'] }, (c, r) => { noBody(r.body); return rooms.createRoom(c, r.player); }),
    R('POST', '/v1/rooms/join', { auth: true, kill: 'rooms', guess: true, rl: ['roomjoin', 20, 600, 'pid'] }, (c, r) => rooms.joinRoom(c, r.player, r.body)),
    R('GET', '/v1/rooms/:code', { auth: true, rl: ['roomget', 150, 60, 'pid'], pace: true, hold: 'room' }, (c, r) => {
      const v = r.query.get('v');
      return rooms.getRoom(c, r.player, r.params.code, v === null ? undefined : Number(v));
    }),
    R('POST', '/v1/rooms/:code/endpoint', { auth: true, rl: ['roomep', 20, 60, 'pid'] }, (c, r) => rooms.postEndpoint(c, r.player, r.params.code, r.body, r.ipk)),
    R('POST', '/v1/rooms/:code/punch', { auth: true, rl: ['roompunch', 20, 60, 'pid'] }, (c, r) => rooms.reportPunch(c, r.player, r.params.code, r.body)),
    R('POST', '/v1/rooms/:code/started', { auth: true, rl: ['roomstart', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); return rooms.markStarted(c, r.player, r.params.code); }),
    // leaving may carry the session's anonymous numbers in the header X-Msc-Stats (reports.js); a bad header is ignored, the leave never depends on it
    R('POST', '/v1/rooms/:code/leave', { auth: true, rl: ['roomleave', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); const out = rooms.leaveRoom(c, r.player, r.params.code); reports.recordSession(c, r.stats); return out; }),
    R('DELETE', '/v1/rooms/:code', { auth: true, rl: ['roomleave', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); const out = rooms.leaveRoom(c, r.player, r.params.code); reports.recordSession(c, r.stats); return out; }),

    R('POST', '/v1/ranked/queue', { auth: true, kill: 'ranked', rl: ['qjoin', 10, 600, 'pid'], pace: true }, (c, r) => ranked.joinQueue(c, r.player, r.body, r.build)),
    R('POST', '/v1/ranked/queue/resume', { auth: true, kill: 'ranked', rl: ['qresume', 20, 600, 'pid'], pace: true }, (c, r) => ranked.resumeQueue(c, r.player, r.body)),
    R('GET', '/v1/ranked/queue', { auth: true, rl: ['qpoll', 120, 60, 'pid'], pace: true, hold: 'queue' }, (c, r) => ranked.pollQueue(c, r.player)),
    R('DELETE', '/v1/ranked/queue', { auth: true, rl: ['qleave', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); return ranked.leaveQueue(c, r.player); }),

    // friends (core/friends.js): the list is a held poll for the online menu; everything else is one small request
    R('GET', '/v1/friends', { auth: true, kill: 'friends', rl: ['friends', 120, 60, 'pid'], pace: true, hold: 'friends' }, (c, r) => {
      const v = r.query.get('v');
      return friends.list(c, r.player, v === null ? undefined : v);
    }),
    R('GET', '/v1/friends/recent', { auth: true, kill: 'friends', rl: ['frecent', 30, 60, 'pid'] }, (c, r) => friends.recent(c, r.player)),
    R('POST', '/v1/friends/add', { auth: true, kill: 'friends', guess: true, rl: ['fadd', 20, 600, 'pid'] }, (c, r) => friends.add(c, r.player, r.body)),
    R('POST', '/v1/friends/decline', { auth: true, kill: 'friends', rl: ['fdecl', 30, 600, 'pid'] }, (c, r) => friends.decline(c, r.player, r.body)),
    R('POST', '/v1/friends/remove', { auth: true, kill: 'friends', rl: ['frem', 30, 600, 'pid'] }, (c, r) => friends.remove(c, r.player, r.body)),
    R('POST', '/v1/friends/block', { auth: true, kill: 'friends', guess: true, rl: ['fblock', 20, 600, 'pid'] }, (c, r) => friends.block(c, r.player, r.body)),
    R('POST', '/v1/friends/unblock', { auth: true, kill: 'friends', rl: ['funblock', 20, 600, 'pid'] }, (c, r) => friends.unblock(c, r.player, r.body)),
    R('POST', '/v1/friends/invite', { auth: true, kill: 'friends', rl: ['finvite', 20, 600, 'pid'] }, (c, r) => friends.invite(c, r.player, r.body)),
    R('POST', '/v1/friends/invite/clear', { auth: true, kill: 'friends', rl: ['finviteclr', 30, 600, 'pid'] }, (c, r) => friends.clearInvite(c, r.player, r.body)),
    R('POST', '/v1/friends/status', { auth: true, kill: 'friends', rl: ['fstatus', 60, 60, 'pid'] }, (c, r) => friends.setStatus(c, r.player, r.body)),

    R('POST', '/v1/matches/:id/result', { auth: true, rl: ['mresult', 20, 600, 'pid'] }, (c, r) => matches.submitResult(c, r.player, r.params.id, r.body)),
    R('GET', '/v1/matches/:id', { auth: true, hint: 'match', rl: ['mget', 60, 60, 'pid'] }, (c, r) => matches.getMatch(c, r.player, r.params.id)),

    R('POST', '/v1/turn', { auth: true, kill: 'turn', rl: ['turn', 10, 600, 'pid'], async: true }, (c, r) => turn.mintTurn(c, r.player, r.body, r.ipk)),
    R('POST', '/v1/turn/usage', { auth: true, kill: 'turn', rl: ['turnuse', 30, 600, 'pid'] }, (c, r) => turn.reportTurnUsage(c, r.player, r.body)),

    R('POST', '/v1/rank/put', { auth: true, kill: 'ranking', rl: ['rankput', 12, 60, 'pid'] }, (c, r) => ranking.putScore(c, r.player, r.body)),
    R('POST', '/v1/rank/get', { auth: true, kill: 'ranking', rl: ['rankget', 40, 60, 'pid'] }, (c, r) => ranking.getScores(c, r.player, r.body)),

    // Public, read-only web leaderboard: no token and no version header (a browser sends neither); the `ranking` / `all` kill switch applies.
    R('GET', '/v1/public/leaderboard', { open: true, web: true, kill: 'ranking', rl: ['pubboard', 30, 60, 'ip'] }, (c, r) => {
      const body = board.get(c, publicboard.parseQuery(r.query));
      return json(200, body, { 'cache-control': `public, max-age=${publicboard.MEMO_MS / 1000}` });
    }),
    R('GET', '/leaderboard', { open: true, rl: ['lbpage', 60, 60, 'ip'] }, () => pageResponse()),

    R('GET', '/v1/admin/stats', { admin: true }, (c) => admin.stats(c)),
    R('GET', '/v1/admin/export', { admin: true }, (c, r) => admin.exportTable(c, r.query)),
    R('POST', '/v1/admin/import', { admin: true }, (c, r) => admin.importRows(c, r.body)),
    R('POST', '/v1/admin/switches', { admin: true }, (c, r) => admin.setSwitches(c, r.body)),
    R('POST', '/v1/admin/ban', { admin: true }, (c, r) => admin.ban(c, r.body)),
    R('GET', '/v1/admin/flags', { admin: true }, (c, r) => admin.listFlags(c, r.query)),
    R('POST', '/v1/admin/flags/clear', { admin: true }, (c, r) => admin.clearFlags(c, r.body)),
    R('GET', '/v1/admin/report-summary', { admin: true }, (c, r) => reports.summaryView(c, r.query)),
    R('POST', '/v1/admin/turn-usage', { admin: true }, async (c, r) => { noBody(r.body); await c.turnUsage.refresh(c.now, { force: true }); return { ok: true, turn: turn.turnStatus(c) }; }),
    R('GET', '/v1/admin/reports', { admin: true }, (c, r) => reports.listReports(c, r.query)),
    R('GET', '/v1/admin/reports/:id', { admin: true }, (c, r) => reports.readReport(c, r.params.id)),
    R('GET', '/v1/admin/diag', { admin: true }, (c, r) => reports.diag(c, r.query)),
  ];

  function info(c) {
    const s = loadSettings();
    return {
      proto: PROTO,
      minProto: cfg.minProto,
      serverMs: c.now,
      kill: [...s.kill],
      message: s.message || null,
      features: { ranked: !s.kill.has('ranked') && !s.kill.has('all'), turn: !!turnProvider && !s.kill.has('turn') && !s.kill.has('all'), ranking: !s.kill.has('ranking') && !s.kill.has('all') },
      limits: { nameMin: NAME_MIN, nameMax: NAME_MAX, dataMax: ranking.DATA_MAX, blobSize: ranking.BLOB_SIZE },
      stun: cfg.stunServers,
      punch: cfg.rooms.punch,
      roomCodeAlphabet: rooms.CODE_ALPHABET,
      resultWindowSec: cfg.matches.resultWindowMs / 1000,
      app: { min: cfg.minApp || null, latest: cfg.latestApp || null },
      link: { pingAfterSec: cfg.rooms.quietMs / 1000, lostAfterSec: cfg.rooms.lostMs / 1000, queueDropSec: cfg.queue.staleMs / 1000, joinAckSec: cfg.rooms.ackMs / 1000, natLifeSec: cfg.rooms.natLifeMs / 1000 },
      servers: cfg.serverList,
    };
  }

  /** Where the service lives. Lets the Worker URL move without a client update: the client keeps the last list it saw. */
  function serverList(c) {
    return {
      version: 1,
      ttlSec: 86400,
      servers: cfg.serverList,
      stun: cfg.stunServers,
      app: { min: cfg.minApp || null, latest: cfg.latestApp || null },
      serverMs: c.now,
    };
  }

  function timeInfo(now) {
    const p = dateParts(now);
    const s = season(now);
    return {
      utcMs: now,
      iso: new Date(now).toISOString(),
      year: p.year, month: p.month, month0: p.month - 1, mday: p.mday,
      hour: p.hour, min: p.min, sec: p.sec, wday: p.wday, yday: p.yday,
      dayStartMs: dayStart(now),
      season: { index: s.index, startMs: s.startMs, endMs: s.endMs },
    };
  }

  async function authenticate(c, request) {
    const h = request.headers.get('authorization') || '';
    const m = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(h);
    if (!m) throw new ApiError(401, 'unauthorized', 'missing or malformed token', {}, { 'www-authenticate': 'Bearer' });
    const hash = await sha256hex(m[1]);
    const p = db.get(`SELECT * FROM profiles WHERE token_hash = ?`, hash);
    if (!p) throw new ApiError(401, 'unauthorized', 'unknown token', {}, { 'www-authenticate': 'Bearer' });
    if (p.banned) throw new ApiError(403, 'banned', 'this account is blocked');
    seen.set(p.pid, c.now);
    if (seen.size > 60000) seen.delete(seen.keys().next().value);
    if (c.now - p.last_seen_ms > 300_000) db.run(`UPDATE profiles SET last_seen_ms = ? WHERE pid = ?`, c.now, p.pid);
    return p;
  }

  // The raw body is always consumed first (the platform dislikes unread request streams); it is parsed only
  // after authentication and rate limits, so junk from strangers costs as little as possible.
  async function readRaw(request, max = MAX_BODY) {
    if (request.method === 'GET' || request.method === 'HEAD') return '';
    const len = Number(request.headers.get('content-length') || 0);
    if (len > max) throw new ApiError(413, 'too_large', 'request body too large');
    const text = await readBodyCapped(request, max); // the stream is cut at the cap: a lying Content-Length cannot get a big body past it
    if (text === null) throw new ApiError(413, 'too_large', 'request body too large');
    return text;
  }

  function parseBody(text) {
    if (!text.trim()) return undefined;
    try {
      return JSON.parse(text);
    } catch {
      throw new ApiError(400, 'bad_json', 'body is not valid JSON');
    }
  }

  /**
   * POST /v1/register, in this order: parse and validate the body (no state touched), proof of work if enabled, then the
   * per-address bucket (a burst for a venue behind one NAT, then a slow rate) and the global daily bucket, then the insert.
   * A request that fails validation (or asks for a taken name) is charged only to a separate, cheap "bad request" bucket:
   * garbage can no longer use up the global 500 a day.
   */
  async function prepareRegister(c, rq, raw, ipk, now) {
    const rg = cfg.register;
    try {
      rq.body = parseBody(raw);
      accounts.checkRegisterBody(c, rq.body);
      await pow.verifyPow(c, ipk, rq.body.pow);
    } catch (e) {
      if (e instanceof ApiError && e.status >= 400 && e.status < 500) limit(c, `regbad:${ipk}`, rg.badPerHour, 3600, now);
      throw e;
    }
    limit(c, `register:ip:${ipk}`, rg.ipPerHour, 3600, now, rg.ipBurst);
    limit(c, 'register:all', accounts.signupCap(c), 86400, now); // shrinks as the table fills
  }

  async function handle(request) {
    let now = clock();
    try {
      const url = new URL(request.url);
      if (cfg.allowTestClock && request.headers.get('x-test-time')) now = Number(request.headers.get('x-test-time'));
      const c = ctxFor(now);
      const ip = (cfg.allowTestClock && request.headers.get('x-test-ip')) || request.headers.get('cf-connecting-ip') || 'unknown';
      const ipk = ipKey(ip); // IPv6 by /64

      let route = null;
      let params = {};
      let pathMatched = false;
      for (const r of routes) {
        const m = r.re.exec(url.pathname);
        if (!m) continue;
        pathMatched = true;
        if (r.method === request.method) { route = r; params = m.groups || {}; break; }
      }
      if (!route) {
        if (pathMatched) throw new ApiError(405, 'method_not_allowed', 'method not allowed');
        throw new ApiError(404, 'not_found', 'no such endpoint');
      }
      const o = route.opts;
      const raw = await readRaw(request, o.maxBody ? bodyLimit(url.pathname) : MAX_BODY);
      budget.admit(now, classify(request.method, url.pathname), ipk); // the daily budget and the per-address ceilings (budget.js)

      if (o.admin) {
        if (!cfg.adminEnabled) throw new ApiError(404, 'not_found', 'no such endpoint');
        // gxnet14 (Galaxy secreview32 F1): a refusal here names the ADDRESS (`x-msc-block: adm:<seconds>`): the Worker then answers that address's admin
        // requests itself for that long (worker_gate.js). Before, a loop of wrong bearers reached the object from one address, all day (each one a Durable Object request).
        const admBlock = (e) => { if (e instanceof ApiError && e.status === 429 && e.extra && e.extra.retryAfterSec > 0) e.headers['x-msc-block'] = `adm:${Math.min(3600, e.extra.retryAfterSec)}`; return e; };
        try { limit(c, `adminip:${ipk}`, 30, 60, now); } catch (e) { throw admBlock(e); }
        const h = request.headers.get('authorization') || '';
        const given = h.startsWith('Bearer ') ? h.slice(7) : '';
        if (!(await safeEqual(given, cfg.adminToken))) {
          try { brakeTake(`adminfail:${ipk}`, 5, 600, now); } catch (e) { throw admBlock(e); }
          throw new ApiError(401, 'unauthorized', 'bad admin token');
        }
      } else {
        // Version gating fails closed: a malformed header is an error, never "no header" (edge.js has the format rules).
        // gxnet14 (Galaxy secreview32 F1): a player route refused BEFORE its token is looked at (wrong or old version headers, a switched-off feature) had no
        // per-address ceiling at all (the open routes are charged above, the others only after authentication). Such a refusal now counts against the address
        // like a failed sign-in (ADDR_BAD_DAY, budget.badAuth): past it the address gets the `addr-bad` verdict, which the Worker enforces without waking the
        // object (tokens it has seen succeed still pass: a NAT's honest consoles keep working).
        const preAuth = (err) => {
          if (o.auth) { try { budget.badAuth(now, ipk); } catch (refused) { return refused; } }
          return err;
        };
        const verr = versionHeaderError(request.headers, { requireApp: cfg.requireApp, open: !!o.open });
        if (verr) throw preAuth(verr);
        const protoHeader = request.headers.get('x-msc-proto');
        if ((protoHeader === null ? PROTO : Number(protoHeader)) < cfg.minProto) {
          throw preAuth(new ApiError(426, 'upgrade_required', 'this client is too old for the server', { minProto: cfg.minProto }));
        }
        const app = request.headers.get('x-msc-app');
        if (app !== null && cfg.minApp && cmpVersion(app, cfg.minApp) < 0) {
          const target = cfg.latestApp || cfg.minApp;
          throw preAuth(new ApiError(426, 'update_required', `Different version: please update the game to ${target}.`, { minVersion: cfg.minApp }));
        }
        if (!o.open || o.web) {
          const kill = loadSettings().kill;
          if (kill.has('all') || (o.kill && kill.has(o.kill))) {
            throw preAuth(new ApiError(503, 'service_disabled', loadSettings().message || 'this service is switched off for now', { retryAfterSec: 300 }));
          }
        }
        if (o.rl && o.rl[3] === 'ip') limit(c, `${o.rl[0]}:ip:${ipk}`, o.rl[1], o.rl[2], now);
        if (o.globalRl) limit(c, `${o.globalRl[0]}:all`, o.globalRl[1], o.globalRl[2], now);
      }

      const query = url.searchParams;
      const rq = { body: undefined, query, params, player: null, ip: ipk, ipk, rawBytes: o.maxBody ? te.encode(raw).length : raw.length, stats: request.headers.get('x-msc-stats'), build: request.headers.get('x-msc-build') || '' };
      if (o.auth) {
        try {
          rq.player = await authenticate(c, request);
        } catch (e) {
          if (e instanceof ApiError && e.status === 401) {
            budget.badAuth(now, ipk); // a day's ceiling for requests that fail authentication, held here and by the Worker (x-msc-block: addr-bad)
            brakeTake(`authfail:${ipk}`, 20, 600, now); // token-guessing brake
          }
          throw e;
        }
        budget.acct(now, rq.player.pid);
        if (o.rl && o.rl[3] === 'pid') limit(c, `${o.rl[0]}:pid:${rq.player.pid}`, o.rl[1], o.rl[2], now);
      }

      if (o.register) await prepareRegister(c, rq, raw, ipk, now);
      else rq.body = parseBody(raw);
      let result;
      if (o.guess) {
        // wrong-code limits (guess.js): checked before the code is looked at, counted after a miss, in storage
        const fp = await guess.fingerprint(c, ipk);
        const gpid = rq.player ? rq.player.pid : null;
        guess.check(c, fp, gpid);
        try {
          result = await route.handler(c, rq);
        } catch (e) {
          if (guess.isMiss(e)) guess.miss(c, fp, gpid);
          throw e;
        }
      } else {
        result = await route.handler(c, rq);
      }
      if (o.hold && result && typeof result === 'object' && !(result instanceof Response)) {
        const waitSec = parseWait(query.get('wait'));
        if (waitSec > 0 && cfg.hold.maxMs > 0) result = await holdPoll(o.hold, route, rq, result, Math.min(waitSec * 1000, cfg.hold.maxMs));
      }
      if (o.pace && result && typeof result === 'object' && !(result instanceof Response)) {
        const scale = budget.pollScale(clock());
        if (scale > 1) result.pollScale = scale; // only when above 1: an old server never sends it and an old client ignores it
      }
      if (cfg.pace.hints && (o.hold || o.hint) && (query.has('wait') || query.get('hints') === '1') && result && typeof result === 'object' && !(result instanceof Response)) paceHints(o, result, budget.pollScale(clock()));
      const resp = result instanceof Response ? result : json(200, result);
      resp.headers.set('x-msc-proto', String(PROTO));
      return budgetHeader(resp);
    } catch (e) {
      if (e instanceof ApiError) {
        if (cfg.latestApp) e.extra.latestVersion = cfg.latestApp;
        return budgetHeader(errorResponse(e));
      }
      // Unexpected: never echo internals or request data.
      const internal = new ApiError(500, 'internal', 'internal error');
      if (cfg.latestApp) internal.extra.latestVersion = cfg.latestApp; // like every other error answer (the Worker adds it at the edge)
      return budgetHeader(errorResponse(internal));
    } finally {
      flushDirty();
    }
  }

  /** Every answer carries the budget level once it is not 'ok': the Worker reads it and then refuses that class itself (worker.js). */
  function budgetHeader(resp) {
    const lv = budget.level(clock());
    if (lv && !resp.headers.has('x-msc-budget')) resp.headers.set('x-msc-budget', lv);
    return resp;
  }

  /**
   * Pacing the service dictates (PROTOCOL.md "Pacing hints"; config.js `pace`): `nextWaitSec` is the hold to ask for with the next held poll, `nextPollMs` the pause
   * before the next poll when the answer was not held (and for the match status poll, which is never held). Both are multiplied by `pollScale` when the day is busy. A
   * client that does not know them keeps its own constants; one that does clamps them to its own safe range. Left out when the poll is over (room closed or expired,
   * queue entry matched or gone, match final). Sent only to a client that asks for them: a poll with `wait=` (a held-capable client) or `hints=1`; every other
   * answer is byte for byte what it was.
   */
  function paceHints(o, result, scale) {
    const p = cfg.pace;
    const k = scale > 1 ? scale : 1;
    const wait = (sec) => Math.min(55, Math.max(1, Math.round(sec * k)));
    if (o.hold === 'room') {
      if (result.state === 'closed' || result.state === 'expired') return;
      result.nextWaitSec = wait(result.state === 'started' ? p.startedWaitSec : p.roomWaitSec);
      result.nextPollMs = Math.round(p.roomGapMs * k);
    } else if (o.hold === 'queue') {
      if (result.status !== 'queued') return;
      result.nextWaitSec = wait(p.queueWaitSec);
    } else if (o.hold === 'friends') {
      result.nextWaitSec = wait(p.friendsWaitSec);
    } else if (o.hint === 'match') {
      if (result.final !== false) return;
      result.nextPollMs = Math.round(p.matchPollMs * k);
    }
  }

  const parseWait = (v) => (v !== null && /^\d{1,2}$/.test(v) ? Math.min(60, Number(v)) : 0);

  /**
   * Held polls. `first` is the answer the handler gave at once. A room poll that found nothing new (changed:false) and a queue poll that is still
   * `queued` wait here, inside the object, until something wakes them (a write to that room, a pairing) or `waitMs` is up, then answer with the
   * handler's fresh result. `heldMs` in the answer says the service held (a client that sees it can ask again at once; an old server never sends it).
   */
  async function holdPoll(kind, route, rq, first, waitMs) {
    const started = clock();
    const finish = (res) => { res.heldMs = Math.max(0, clock() - started); return res; };
    if (kind === 'friends') {
      if (first.changed !== false) return finish(first);
      const pid = rq.player.pid;
      const since = rq.query.get('v');
      let res = first;
      for (;;) {
        const t = clock();
        const left = started + waitMs - t;
        if (left <= 0) break;
        await holds.wait(`friends:${pid}`, left);
        res = route.handler(ctxFor(clock()), rq);
        flushDirty();
        if (res.changed !== false) break;
      }
      return finish(res);
    }
    if (kind === 'room') {
      if (first.changed !== false || first.state === 'closed' || first.state === 'expired') return finish(first);
      const code = rooms.normalizeCode(rq.params.code);
      const row0 = db.get(`SELECT * FROM rooms WHERE code = ?`, code);
      const slot = row0 ? rooms.slotOf(row0, rq.player.pid) : -1;
      const key = `${code}:${slot}`;
      holding.set(key, (holding.get(key) || 0) + 1);
      try {
        let res = first;
        for (;;) {
          const t = clock();
          const left = started + waitMs - t;
          if (left <= 0) break;
          await holds.wait(`room:${code}`, Math.min(left, roomChangesIn(code, t)));
          res = route.handler(ctxFor(clock()), rq);
          flushDirty();
          if (res.changed !== false || res.state === 'closed' || res.state === 'expired') break;
        }
        return finish(res);
      } finally {
        const n = (holding.get(key) || 1) - 1;
        if (n > 0) holding.set(key, n); else holding.delete(key);
      }
    }
    // queue
    if (first.status !== 'queued') return finish(first);
    const pid = rq.player.pid;
    holdingQueue.add(pid);
    try {
      let res = first;
      let touched = started;
      for (;;) {
        const t = clock();
        const left = started + waitMs - t;
        if (left <= 0) break;
        await holds.wait(`queue:${pid}`, Math.min(left, cfg.hold.queueTickMs));
        const t2 = clock();
        // the entry is written (last poll time) at most every 8 s while held: the matchmaker counts a held entry as present anyway
        const c2 = ctxFor(t2);
        c2.skipQueueTouch = t2 - touched < 8000;
        if (!c2.skipQueueTouch) touched = t2;
        res = route.handler(c2, rq);
        flushDirty();
        if (res.status !== 'queued') break;
      }
      return finish(res);
    } finally {
      holdingQueue.delete(pid);
    }
  }

  /** ms until a room may change without anybody writing to it: it expires, or a ranked pairing's join acknowledgement is overdue */
  function roomChangesIn(code, t) {
    const r = db.get(`SELECT kind, quick, state, created_ms, expires_ms, host_ack, join_ack FROM rooms WHERE code = ?`, code);
    if (!r) return 50;
    let at = r.expires_ms;
    if ((r.kind === 'ranked' || r.quick === 1) && r.state === 'joined' && !(r.host_ack && r.join_ack)) at = Math.min(at, r.created_ms + cfg.rooms.ackMs);
    return Math.max(50, at - t + 5);
  }

  /** Housekeeping, run from the Durable Object alarm and opportunistically. Returns whether more work is pending. */
  function sweep() {
    const now = clock();
    budget.alarm(now);
    const c = ctxFor(now);
    c.tx(() => {
      const live = db.all(
        `SELECT code FROM rooms WHERE (state IN ('open','joined','ready','started') AND expires_ms < ?1)
            OR ((kind = 'ranked' OR quick = 1) AND state = 'joined' AND (host_ack = 0 OR join_ack = 0) AND created_ms + ?2 < ?1)`,
        now, cfg.rooms.ackMs,
      );
      for (const r of live) rooms.loadRoom(c, r.code);
      db.run(`DELETE FROM rooms WHERE state IN ('closed','expired') AND expires_ms + ? < ?`, cfg.rooms.keepClosedMs, now);
      ranked.sweepQueue(c);
      matches.sweepMatches(c);
      friends.pruneInvites(c);
      db.run(`DELETE FROM counters WHERE updated_ms < ?`, now - 40 * 86400_000);
      db.run(`DELETE FROM pair_stats WHERE updated_ms < ?`, now - cfg.rooms.pairFailKeepMs);
      db.run(`DELETE FROM turn_mints WHERE minted_ms < ?`, now - 2 * 86400_000);
      db.run(`DELETE FROM daily WHERE day < ?`, dayIndex(now) - 7);
      db.run(`DELETE FROM pair_daily WHERE day < ?`, dayIndex(now) - 1);
      archiveSeasons(c);
      // the idle-account scan (profiles.last_seen_ms is indexed) runs at most hourly: the sweep itself runs every minute while a room is live,
      // and a scan per minute over a large profiles table would spend the free plan's row reads (a restarted object scans at its first sweep)
      if (now - lastPruneMs >= PRUNE_EVERY_MS || now < lastPruneMs) {
        lastPruneMs = now;
        accounts.pruneIdle(c);
        reports.pruneReports(c); // reports past their 30 days, old daily rows, summaries past 90 days
        guess.prune(c); // wrong-code counters of earlier days
        if (cfg.pruneSilentMs) { // sign-ups that never came back, over the hour of `last_seen_ms` that has just become old enough
          const hi = now - cfg.pruneSilentMs;
          const lo = state.silentCursor !== null && state.silentCursor <= hi ? state.silentCursor : hi - 24 * 3600_000;
          for (let i = 0; i < 5 && accounts.pruneSilent(c, lo, hi) >= 200; i++);
          state.silentCursor = hi;
        }
      }
      for (const [k, t] of presence) if (now - t > 3600_000) presence.delete(k);
    for (const [k, t] of seen) if (now - t > 3600_000) seen.delete(k);
    });
    flushDirty();
    const pending =
      db.get(`SELECT COUNT(*) AS n FROM rooms`).n + db.get(`SELECT COUNT(*) AS n FROM queue`).n +
      db.get(`SELECT COUNT(*) AS n FROM matches WHERE resolved_ms IS NULL`).n;
    return pending > 0;
  }

  const routeMeta = routes.map((r) => [r.method, r.pattern, r.opts.admin ? 'admin' : r.opts.auth ? 'player' : 'none', !!r.opts.open]);
  // routeOpts: [method, pattern, options] of every route, so the audit tests can check that each one is rate limited and switchable
  const routeOpts = routes.map((r) => [r.method, r.pattern, r.opts]);
  /** The daily cron (worker.js `scheduled`, through the object): writes yesterday's report summary row and logs one line of counts. Safe to run twice. */
  function dailySummary() {
    const c = ctxFor(clock());
    return reports.makeSummary(c);
  }

  return { handle, sweep, dailySummary, cfg, killNames: KILL_NAMES, routeMeta, routeOpts, limiter, brake, budget, holds };
}
