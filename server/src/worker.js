// Edge entry point. Everything real happens in the single Durable Object; the Worker rejects junk before waking it
// (unknown paths, wrong methods, oversized bodies, malformed Authorization / version headers: see core/edge.js) and
// forwards the rest, including the client address header (the object keeps it in memory for rate limits; an endpoint call's source address is also kept
// in its room row, deleted with the room; see PROTOCOL.md Privacy).
// Every request the object sees costs a Durable Object request (and often rows) on the free tier; a request answered here does not.
// Per-address token bucket (edgeLimit; 1.3 wave 9: the 1.3 client now polls a room about once in 8 s, so the buckets are far tighter) and the memo guard (kill switch and rate limit on memo hits) also run here.
//
// Two public web routes are answered here as far as possible: GET /leaderboard is static (never wakes the object), and
// GET /v1/public/leaderboard is held in this isolate's memory for 60 s per (board, limit), so a busy page costs the object
// about one request a minute per isolate. (The Cache API is not used: it only works on custom domains, not on workers.dev.)

export { GameServer } from './do.js';
import { edgeCheck } from './core/edge.js';
import { ApiError, errorResponse } from './core/util.js';
import { pageResponse } from './core/leaderboard_page.js';
import { parseQuery, queryKey, MEMO_MS } from './core/publicboard.js';
import { createMemoGuard } from './core/memo_guard.js';
import { createEdgeLimiter } from './core/edge_bucket.js';
import { createGate } from './core/worker_gate.js';

// Best-effort limiter in front of the object (core/edge_bucket.js): a request answered here costs no Durable Object request.
// A request with a player token is charged to its own (address + token) bucket, 100 burst and 2 a second (a game polls a room every 1.7 s and
// the queue every 2 s), and to the address's aggregate, 300 burst and 15 a second, so a venue of ten consoles behind one NAT is not throttled
// while a flood of invented tokens still is. A request with no token has the plain per-address bucket (200 / 6).
// EDGE_IP_BURST / EDGE_IP_PER_SEC change the first, EDGE_IP_AGG_BURST / EDGE_IP_AGG_PER_SEC the aggregate; EDGE_IP_BURST=0 switches all off.
// The object's own per-account and per-address limits stay behind it.
//
// The gate (core/worker_gate.js) is what the Worker has learned from the object's answers: the daily budget level and the addresses / tokens that are
// over their daily ceiling. A request the gate refuses is answered here and never reaches the object (no Durable Object request).
const gate = createGate();
async function toObject(request, env) {
  const ip = request.headers.get('cf-connecting-ip') || 'unknown';
  const auth = request.headers.get('authorization') || '';
  const now = Date.now();
  const refusal = gate.check(request.method, new URL(request.url).pathname, ip, auth, now);
  if (refusal) return errorResponse(refusal);
  const resp = await env.GAME.get(env.GAME.idFromName('main')).fetch(request);
  gate.learn(resp, now, ip, auth);
  return resp;
}
let edgeLimiter = null;
function edgeLimit(request, env) {
  const num = (v, d) => (v === undefined || !Number.isFinite(Number(v)) ? d : Number(v));
  const burst = num(env.EDGE_IP_BURST, 100);
  const perSec = num(env.EDGE_IP_PER_SEC, 2) > 0 ? num(env.EDGE_IP_PER_SEC, 2) : 2;
  const aggBurst = num(env.EDGE_IP_AGG_BURST, 300);
  const aggPerSec = num(env.EDGE_IP_AGG_PER_SEC, 15) > 0 ? num(env.EDGE_IP_AGG_PER_SEC, 15) : 15;
  const cfg = `${burst}:${perSec}:${aggBurst}:${aggPerSec}`;
  if (!edgeLimiter || edgeLimiter.cfg !== cfg) {
    edgeLimiter = createEdgeLimiter({ burst, perSec, aggBurst, aggPerSec });
    edgeLimiter.cfg = cfg;
  }
  const scale = Number(env.RATE_LIMIT_SCALE) > 0 ? Number(env.RATE_LIMIT_SCALE) : 1;
  const wait = edgeLimiter.take(request.headers.get('cf-connecting-ip') || 'unknown', request.headers.get('authorization') || '', Date.now(), scale);
  if (wait <= 0) return null;
  return errorResponse(new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait) }));
}

const BOARD_PATH = '/v1/public/leaderboard';
/** @type {Map<string, {at: number, status: number, body: string, headers: [string, string][]}>} */
const boardMemo = new Map();

/** Canonical key (board, limit and archived season only, so extra query noise cannot dodge the memo), or null when the query is invalid (the object answers 400). */
function boardKey(url) {
  try {
    return queryKey(parseQuery(url.searchParams));
  } catch {
    return null;
  }
}

function fromMemo(m) {
  const headers = new Headers(m.headers);
  headers.set('x-msc-cache', 'hit');
  return new Response(m.body, { status: m.status, headers });
}

// A memo hit must not outlive the `ranking` kill switch or the per-address rate limit the object applies to this route (both live in the
// object, and a hit never reaches it). So a hit is served only after (1) the static KILL_SWITCH variable and the isolate's view of the object's
// switches say the route is on and (2) a per-address bucket here (the object's numbers) lets it through (core/memo_guard.js). The object's switches
// are re-read (one cheap GET /v1/info, never switched off) at most every RECHECK_MS (60 s) per isolate and whenever a request misses the memo;
// when the route is off the memo is dropped and the request goes to the object, which answers with its own 503 (message included).
const guard = createMemoGuard();

async function refreshKill(request, env, now) {
  try {
    const url = new URL(request.url);
    url.pathname = '/v1/info';
    url.search = '';
    const resp = await toObject(new Request(url, { method: 'GET', headers: { 'cf-connecting-ip': request.headers.get('cf-connecting-ip') || 'unknown' } }), env);
    if (resp.status === 200) {
      const j = await resp.json();
      guard.seen(Array.isArray(j.kill) ? j.kill : [], now);
      return;
    }
  } catch { /* keep the last known state and try again shortly */ }
  guard.retrySoon(now);
}

async function publicBoard(request, env) {
  const key = boardKey(new URL(request.url));
  const now = Date.now();
  const hit = key && boardMemo.get(key);
  if (hit && now - hit.at < MEMO_MS) {
    if (guard.stale(now)) await refreshKill(request, env, now);
    if (guard.off(env, 'ranking')) {
      boardMemo.clear(); // switched off: the object answers (503 with its message)
    } else {
      const wait = guard.take(env, 'ranking', request.headers.get('cf-connecting-ip'), now);
      if (wait > 0) return errorResponse(new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait) }));
      return fromMemo(hit);
    }
  }
  const resp = await toObject(request, env);
  if (resp.status === 503) guard.forget(); // switched off (or overloaded): look again at the next hit
  if (key && resp.status === 200) {
    const body = await resp.clone().text();
    guard.routeOn(now); // the object just served this route, so it is not switched off
    if (boardMemo.size >= 300) boardMemo.clear();
    boardMemo.set(key, { at: now, status: 200, body, headers: [...resp.headers] });
  }
  return resp;
}

export default {
  /** The daily cron (wrangler.toml [triggers]): one call into the object, which writes yesterday's problem-report summary row. No HTTP route is involved. */
  async scheduled(_event, env, ctx) {
    ctx.waitUntil(env.GAME.get(env.GAME.idFromName('main')).daily());
  },
  /** @param {Request} request @param {{GAME: DurableObjectNamespace}} env */
  async fetch(request, env) {
    const err = edgeCheck(request, env);
    if (err) {
      if (env.LATEST_APP_VERSION) err.extra.latestVersion = String(env.LATEST_APP_VERSION);
      return errorResponse(err);
    }
    const path = new URL(request.url).pathname;
    if (path !== '/leaderboard') {
      const slow = edgeLimit(request, env); // before anything that wakes the object
      if (slow) return slow;
    }
    if (path === '/v1/admin/switches') { // an admin changed the switches: this isolate looks again at the next public hit
      const resp = await toObject(request, env);
      guard.forget();
      boardMemo.clear();
      return resp;
    }
    if (path === '/leaderboard') return pageResponse();
    if (path === BOARD_PATH) return publicBoard(request, env);
    return toObject(request, env);
  },
};
