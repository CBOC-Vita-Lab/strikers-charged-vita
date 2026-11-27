#!/usr/bin/env node
// Request budget per player-hour, by state of the online flow. Companion of estimate-writes.mjs (rows written per match).
//
//   node scripts/estimate-requests.mjs [--client old|new|paced|both|all] [--json] [--minutes N]
//
// What it does: simulated consoles run the CLIENT's schedule against the REAL application (core/app.js over node:sqlite) in VIRTUAL time and
// every request that reaches the application is counted (one request = one Worker request + one Durable Object request on the free plan).
// Nothing here touches a network: the clock, the timers of held polls and the clients are all in this process.
//
// The client's constants are READ FROM ITS SOURCE (poll intervals, republish interval, time refresh), so a change there changes the numbers
// (the unit test test/core/estimate-requests.test.js fails when a constant cannot be found). "old" is the 1.1 / 1.3 schedule (plain polls);
// "new" is the paced schedule (held polls, `wait=` on room and queue polls, the service's pollScale obeyed). "paced" (wave 13) is "new" plus a client that
// also obeys the service's pacing hints (`nextWaitSec`, `nextPollMs`, PROTOCOL.md "Pacing hints"), clamped to its own safe range; "both" = old + new, "all" = all three.
//
// A player-hour is a mix, not one state: MIX below is the assumed evening hour (stated, not measured). Everything printed is an ESTIMATE from
// this model; the real number is in the Cloudflare dashboard after the first busy evening.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { makeServer, signUp, makeBlob, T0 } from '../test/helpers.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const NATIVE = path.resolve(HERE, '../../native');

// ----------------------------------------------------------------------------------------------- client constants, read from its source
function grab(file, re, what) {
  const text = fs.readFileSync(path.join(NATIVE, file), 'utf8');
  const m = re.exec(text);
  if (!m) throw new Error(`estimate-requests: cannot find ${what} in ${file}`);
  return Number(m[1]);
}
export function clientConstants() {
  const sig = 'platform/sdk/net/online/np_signal_http.h';
  const k = {
    roomPollMs: grab(sig, /pollMs\((\d+)\)/, 'room poll interval (pollMs)'),
    startedPollMs: grab(sig, /pollStartedMs\((\d+)\)/, 'started-room poll interval (pollStartedMs)'),
    queuePollMs: grab('platform/sdk/dwc/np_dwc_match.cpp', /kPollMs = (\d+)/, 'queue poll interval (kPollMs)'),
    republishMs: grab('platform/sdk/net/online/np_online.h', /refreshMs\((\d+)\)/, 'endpoint republish interval (refreshMs)'),
    statusPollMs: grab('platform/sdk/dwc/np_online_queue.cpp', /kStatusPollMs = (\d+)/, 'match status poll interval (kStatusPollMs)'),
    timeRefreshMin: 30, // np_dwc_login.cpp header comment: "a refresh every 30 minutes"
  };
  try { // the friends poll of np_dwc_friend.cpp (new clients only)
    k.friendsHoldSec = grab('platform/sdk/dwc/np_dwc_friend.cpp', /kHoldSec = (\d+)/, 'friends hold (kHoldSec)');
    k.friendsHeldGapMs = grab('platform/sdk/dwc/np_dwc_friend.cpp', /kHeldGapMs = (\d+)/, 'friends gap after a held answer (kHeldGapMs)');
    k.friendsPlainGapMs = grab('platform/sdk/dwc/np_dwc_friend.cpp', /kPlainGapMs = (\d+)/, 'friends gap without a hold (kPlainGapMs)');
  } catch { /* old tree */ }
  // the paced schedule (absent in an older tree: the estimator then reports only the old column)
  try {
    k.holdSec = grab(sig, /holdSec\((\d+)\)/, 'room hold (holdSec)');
    k.holdStartedSec = grab(sig, /holdStartedSec\((\d+)\)/, 'started-room hold (holdStartedSec)');
    k.queueHoldSec = grab('platform/sdk/dwc/np_dwc_match.cpp', /kHoldSec = (\d+)/, 'queue hold (kHoldSec)');
    k.heldGapMs = grab(sig, /heldGapMs\((\d+)\)/, 'gap after a held answer (heldGapMs)');
  } catch { /* old tree */ }
  return k;
}

// ----------------------------------------------------------------------------------------------- virtual time
function makeWorld(envOverrides = {}) {
  const timers = [];
  let seq = 0;
  let inflight = 0;
  const hooks = { park: () => { inflight--; }, resume: () => { inflight++; } };
  const srv = makeServer({ RATE_LIMIT_SCALE: '1000', MIN_MATCH_SEC: '0', QUEUE_BASE_WINDOW: '5', ...envOverrides }, {
    deps: {
      timers: {
        setTimeout: (fn, ms) => { const id = ++seq; timers.push({ at: srv.clock.now + Math.max(0, ms), id, fn }); return id; },
        clearTimeout: (id) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
      },
      onHoldPark: hooks.park,
      onHoldResume: hooks.resume,
    },
  });
  const counts = { total: 0, byRoute: {} };
  const world = {
    srv, counts,
    now: () => srv.clock.now,
    sleep: (ms) => new Promise((res) => { timers.push({ at: srv.clock.now + ms, id: ++seq, fn: res }); }),
    async req(method, p, body, token) {
      counts.total++;
      const key = `${method} ${p.replace(/[A-Z2-9]{6}/g, ':code').replace(/\?.*/, '')}`;
      counts.byRoute[key] = (counts.byRoute[key] || 0) + 1;
      inflight++;
      try { return await srv.call(method, p, body, token, { ip: '127.0.0.1' }); } finally { inflight--; }
    },
    async settle() {
      await new Promise((r) => setImmediate(r));
      while (inflight > 0) await new Promise((r) => setImmediate(r));
    },
    /** run virtual time forward `ms` while client tasks are running */
    async run(ms) {
      const until = srv.clock.now + ms;
      await world.settle();
      for (;;) {
        let best = -1;
        for (let i = 0; i < timers.length; i++) if (best < 0 || timers[i].at < timers[best].at || (timers[i].at === timers[best].at && timers[i].id < timers[best].id)) best = i;
        if (best < 0 || timers[best].at > until) break;
        const t = timers.splice(best, 1)[0];
        srv.clock.now = Math.max(srv.clock.now, t.at);
        t.fn();
        await world.settle();
      }
      srv.clock.now = until;
    },
    resetCounts() { counts.total = 0; counts.byRoute = {}; },
  };
  return world;
}

// ----------------------------------------------------------------------------------------------- clients
const EP = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAECAwQFBgc=', nat: 'cone' });

/** The pacing a client applies to one poll. old: fixed gaps. new: ask the service to hold, obey `heldMs` / `pollScale` in the answer. */
function pacer(K, mode) {
  let scale = 1;
  let hint = {}; // paced: what the last answer dictated
  const hinted = mode === 'paced';
  const clampWait = (sec, dflt) => (hinted && Number.isFinite(hint.waitSec) ? Math.min(55, Math.max(5, hint.waitSec)) : Math.round(dflt * scale));
  return {
    /** query suffix for a room poll in `state`, and the gap that follows the answer */
    roomAsk(started) {
      if (mode === 'old') return '';
      return `&wait=${hinted && Number.isFinite(hint.waitSec) ? clampWait(0, 0) : Math.round((started ? K.holdStartedSec : K.holdSec) * scale)}`;
    },
    after(answer, base) {
      if (mode === 'old') return base;
      const s = Number(answer && answer.pollScale);
      scale = s > 1 && s <= 1.5 ? s : 1;
      hint = { waitSec: Number(answer && answer.nextWaitSec), pollMs: Number(answer && answer.nextPollMs) };
      const held = answer && answer.heldMs !== undefined;
      if (held) return K.heldGapMs;
      return hinted && Number.isFinite(hint.pollMs) ? Math.min(60000, Math.max(250, hint.pollMs)) : Math.round(base * scale);
    },
    queueAsk() { return mode === 'old' ? '' : `?wait=${hinted && Number.isFinite(hint.waitSec) ? clampWait(0, 0) : Math.round(K.queueHoldSec * scale)}`; },
    statusAsk() { return hinted ? '?hints=1' : ''; },
    statusGap(answer, base) { const g = Number(answer && answer.nextPollMs); return hinted && Number.isFinite(g) ? Math.min(60000, Math.max(500, g)) : base; },
  };
}

/** A console sitting in the online menu: GET /v1/time every 30 minutes. */
async function idleClient(w, K, tok, stop) {
  while (!stop.v) { await w.sleep(K.timeRefreshMin * 60000); if (stop.v) break; await w.req('GET', '/v1/time', undefined, tok); }
}

/** Browsing the boards: one POST /v1/rank/get every `everyMs`. */
async function boardClient(w, K, tok, stop, everyMs) {
  while (!stop.v) {
    await w.req('POST', '/v1/rank/get', { mode: 0, category: 0, region: 2, sort: 0, since: 0, limit: 10 }, tok);
    await w.sleep(everyMs);
  }
}

/**
 * The friends screen / online menu with a friend list (np_dwc_friend.cpp): one held GET /v1/friends?v=<rev>&wait=45 at a time. `churnMs`: a friend
 * changes presence that often (their requests are not counted here: they are another player's own traffic).
 */
async function friendsClient(w, K, tok, stop, churn, mode = 'new') {
  let rev = '';
  let scale = 1;
  let hintWait = NaN;
  while (!stop.v) {
    const wait = rev ? `&wait=${mode === 'paced' && Number.isFinite(hintWait) ? Math.min(55, Math.max(5, hintWait)) : Math.min(55, Math.round(K.friendsHoldSec * scale))}` : '';
    const r = await w.req('GET', `/v1/friends?v=${rev}${wait}`, undefined, tok);
    if (r.status !== 200) { await w.sleep(15000); continue; }
    rev = r.data.rev;
    hintWait = Number(r.data.nextWaitSec);
    const s = Number(r.data.pollScale);
    scale = s > 1 && s <= 1.5 ? s : 1;
    const held = r.data.heldMs !== undefined && r.data.heldMs > 0;
    let gap = held ? Math.max(K.friendsHeldGapMs, 1000 - r.data.heldMs) : K.friendsPlainGapMs;
    if (held) gap = Math.round(gap * scale);
    await w.sleep(gap);
  }
}

/** Waiting in the ranked queue (nobody to pair with). */
async function queueClient(w, K, mode, tok, stop) {
  const p = pacer(K, mode);
  await w.req('POST', '/v1/ranked/queue', undefined, tok);
  let gap = K.queuePollMs;
  while (!stop.v) {
    await w.sleep(gap);
    if (stop.v) break;
    const r = await w.req('GET', `/v1/ranked/queue${p.queueAsk()}`, undefined, tok);
    if (r.status !== 200 || r.data.status !== 'queued') break;
    gap = p.after(r.data, K.queuePollMs);
  }
}

/** The room loop of np_signal_http.cpp: poll with ?v=, republish the endpoint while nobody is connected, report / start as told. */
async function roomLoop(w, K, mode, tok, code, ctl) {
  const p = pacer(K, mode);
  let version = 0;
  let nextPublish = w.now() + (ctl.firstPublishMs ?? 1500);
  let startedSent = false;
  const view = async (r) => { if (r.status === 200 && r.data.version !== undefined) version = r.data.version; return r; };
  while (!ctl.stop.v) {
    const started = ctl.started.v;
    if (ctl.publish && w.now() >= nextPublish) {
      await view(await w.req('POST', `/v1/rooms/${code}/endpoint`, EP(ctl.n), tok));
      nextPublish = w.now() + (ctl.connected.v ? 3600e3 : K.republishMs);
      await w.sleep(300);
      continue;
    }
    if (ctl.report && ctl.connected.v && !ctl.punched.v) { ctl.punched.v = true; await view(await w.req('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', ms: 900 }, tok)); }
    if (ctl.report && ctl.connected.v && !startedSent) { startedSent = true; const sr = await view(await w.req('POST', `/v1/rooms/${code}/started`, undefined, tok)); if (sr.status === 409) { startedSent = false; await w.sleep(1000); continue; } /* the partner's endpoint is not in yet: ask again in a second, not after a long hold */ ctl.started.v = true; }
    const r = await w.req('GET', `/v1/rooms/${code}${version ? `?v=${version}${p.roomAsk(started)}` : ''}`, undefined, tok);
    if (r.status !== 200) break;
    if (r.data.changed !== false) version = r.data.version;
    if (r.data.state === 'closed' || r.data.state === 'expired') break;
    ctl.view = r.data;
    await w.sleep(p.after(r.data, started ? K.startedPollMs : K.roomPollMs));
  }
}

// ----------------------------------------------------------------------------------------------- scenarios
async function scenarioRate(label, K, mode, build, minutes) {
  const w = makeWorld();
  const stop = { v: false };
  const tasks = await build(w, K, mode, stop);
  await w.run(1000);
  w.resetCounts();
  const t0 = w.now();
  await w.run(minutes * 60000);
  const elapsedH = (w.now() - t0) / 3600e3;
  stop.v = true;
  const n = w.counts.total;
  void tasks;
  return { label, requests: n, perPlayerHour: Math.round(n / elapsedH), byRoute: { ...w.counts.byRoute } };
}

async function oneStartedMatch(K, mode, minutes) {
  // a ranked pairing, both consoles go through link-up and play: requests per player-hour in the match, and requests to get there
  const w = makeWorld();
  const a = await signUp(w.srv, 'Sim1');
  const b = await signUp(w.srv, 'Sim2');
  await w.req('POST', '/v1/ranked/queue', undefined, a.token);
  w.srv.advance(1000);
  const q = await w.req('POST', '/v1/ranked/queue', undefined, b.token);
  const code = q.data.roomCode;
  const matchId = q.data.room.matchId;
  w.resetCounts();
  const stop = { v: false };
  const connected = { v: false };
  const mk = (n) => ({ n, stop, started: { v: false }, connected, punched: { v: false }, publish: true, report: true, firstPublishMs: 1200 + 400 * n });
  const ca = mk(1);
  const cb = mk(2);
  roomLoop(w, K, mode, a.token, code, ca);
  roomLoop(w, K, mode, b.token, code, cb);
  await w.run(1000);
  // both endpoints are in after ~2 s; punching succeeds ~3 s after that
  await w.run(5000);
  connected.v = true;
  await w.run(4000);
  const toStart = w.counts.total;
  w.resetCounts();
  await w.run(minutes * 60000);
  const inMatch = w.counts.total;
  stop.v = true;
  return { toStart: toStart + 1, perPlayerHourInMatch: Math.round(inMatch / 2 / (minutes / 60)), matchId, a, b, code };
}

/** After the final whistle: both report, poll the match status (2 s) until final, then 2 puts and 2 gets (the ranked client's own habit). */
async function postMatch(K, mode) {
  const p = pacer(K, mode);
  const w = makeWorld({ MIN_MATCH_SEC: '0' });
  const a = await signUp(w.srv, 'SimA');
  const b = await signUp(w.srv, 'SimB');
  await w.req('POST', '/v1/ranked/queue', undefined, a.token);
  const q = await w.req('POST', '/v1/ranked/queue', undefined, b.token);
  const code = q.data.roomCode;
  const matchId = q.data.room.matchId;
  await w.req('POST', `/v1/rooms/${code}/endpoint`, EP(1), a.token);
  await w.req('POST', `/v1/rooms/${code}/endpoint`, EP(2), b.token);
  await w.req('POST', `/v1/rooms/${code}/started`, undefined, a.token);
  await w.req('POST', `/v1/rooms/${code}/started`, undefined, b.token);
  w.srv.advance(180000);
  w.resetCounts();
  const done = { v: false };
  const one = async (tok, delayMs, score) => {
    await w.sleep(delayMs);
    await w.req('POST', `/v1/matches/${matchId}/result`, { result: 0, score0: score[0], score1: score[1], checksum: 'deadbeef' }, tok);
    for (let i = 0; i < 60 && !done.v; i++) {
      const r = await w.req('GET', `/v1/matches/${matchId}${p.statusAsk()}`, undefined, tok);
      if (r.data && r.data.final) break;
      await w.sleep(p.statusGap(r.data, K.statusPollMs));
    }
    await w.req('POST', '/v1/rank/put', { category: 0, region: 2, score: 10, data: makeBlob('SimA') }, tok);
    await w.req('POST', '/v1/rank/get', { mode: 0, category: 0, region: 2, sort: 0, since: 0, limit: 10 }, tok);
    await w.req('POST', '/v1/rank/get', { mode: 1, category: 0, region: 2, sort: 0, since: 0, limit: 10 }, tok);
  };
  one(a.token, 0, [3, 1]);
  one(b.token, 4000, [3, 1]);
  await w.run(120000);
  return { requestsPerMatchBothPlayers: w.counts.total };
}

// ----------------------------------------------------------------------------------------------- the model of an hour
// ASSUMED evening hour of one player (a stated model, not a measurement): minutes in each state, matches per hour.
export const MIX = {
  idle: 10, board: 6, queue: 8, inviteWait: 4, match: 28, // minutes; the rest of the hour is post-match / link-up (below) and menus
  matches: 6, // matches in the hour (a 3-minute timed match plus lobby and result screens)
  note: 'idle in the online menu 10 min, board browsing 6 min (one page every 15 s), ranked queue 8 min, waiting in an invite room 4 min, 28 min in matches, 6 matches (link-up and result flow counted per match)',
};

function plan(K, mode, r) {
  const perHour = (m, rate) => (m / 60) * rate;
  const total = perHour(MIX.idle, r.idle.perPlayerHour) + perHour(MIX.board, r.board.perPlayerHour) + perHour(MIX.queue, r.queue.perPlayerHour) +
    perHour(MIX.inviteWait, r.invite.perPlayerHour) + perHour(MIX.match, r.match.perPlayerHourInMatch) +
    MIX.matches * (r.match.toStart / 2 + r.post.requestsPerMatchBothPlayers / 2) + 4; // + login (info, me, time, rank get)
  return Math.round(total);
}

export async function estimate({ mode = 'old', minutes = 20 } = {}) {
  const K = clientConstants();
  const out = { mode, K };
  out.idle = await scenarioRate('online menu idle', K, mode, async (w, K2, m, stop) => { const a = await signUp(w.srv, 'SimIdle'); idleClient(w, K2, a.token, stop); }, 4 * 60);
  out.board = await scenarioRate('leaderboard browsing', K, mode, async (w, K2, m, stop) => { const a = await signUp(w.srv, 'SimBoard'); boardClient(w, K2, a.token, stop, 15000); }, 10);
  out.queue = await scenarioRate('ranked queue waiting', K, mode, async (w, K2, m, stop) => { const a = await signUp(w.srv, 'SimQueue'); queueClient(w, K2, m, a.token, stop); }, 4.5);
  out.invite = await scenarioRate('invite room waiting (host)', K, mode, async (w, K2, m, stop) => {
    const a = await signUp(w.srv, 'SimHost');
    const created = await w.req('POST', '/v1/rooms', undefined, a.token);
    roomLoop(w, K2, m, a.token, created.data.code, { n: 1, stop, started: { v: false }, connected: { v: false }, punched: { v: false }, publish: true, report: false });
  }, 9);
  if (mode !== 'old' && K.friendsHoldSec) {
    for (const [key, churnMs] of [['friends', 0], ['friendsBusy', 120000]]) {
      out[key] = await scenarioRate(`friends screen open (${churnMs ? 'a friend changes every 2 min' : 'quiet list'})`, K, mode, async (w, K2, m, stop) => {
        const a = await signUp(w.srv, 'SimFr');
        const b = await signUp(w.srv, 'SimFrB');
        await w.srv.call('POST', '/v1/friends/add', { pid: b.pid ?? b.profileId }, a.token, { ip: '127.0.0.1' });
        await w.srv.call('POST', '/v1/friends/add', { pid: a.pid ?? a.profileId }, b.token, { ip: '127.0.0.1' });
        friendsClient(w, K2, a.token, stop, 0, m);
        if (churnMs) (async () => { let i = 0; while (!stop.v) { await w.sleep(churnMs); if (stop.v) break; await w.srv.call('POST', '/v1/friends/status', { status: `S${i++}` }, b.token, { ip: '127.0.0.1' }); } })();
      }, 30);
    }
  }
  out.match = await oneStartedMatch(K, mode, minutes);
  out.post = await postMatch(K, mode);
  out.perPlayerHour = plan(K, mode, out);
  return out;
}

function capacity(perHour, budget) { return Math.floor(budget / perHour); }

function print(out) {
  console.log(`\n== client schedule: ${out.mode} ==`);
  const row = (label, v, note = '') => console.log(`  ${label.padEnd(34)} ${String(v).padStart(7)}  ${note}`);
  row('online menu idle', out.idle.perPlayerHour, 'requests / player-hour (time refresh)');
  row('leaderboard browsing', out.board.perPlayerHour, 'requests / player-hour (a page every 15 s)');
  row('ranked queue waiting', out.queue.perPlayerHour, 'requests / player-hour');
  row('invite room waiting (host)', out.invite.perPlayerHour, 'requests / player-hour (poll + republish)');
  if (out.friends) {
    row('friends screen, quiet list', out.friends.perPlayerHour, 'requests / player-hour (one held poll per 45 s; only while the online menu is open)');
    row('friends screen, friend changes /2 min', out.friendsBusy.perPlayerHour, 'requests / player-hour (each change answers the held poll at once)');
    const mixMin = MIX.idle + MIX.board + MIX.inviteWait; // the menu is open, the poll runs
    console.log(`  added to the MIX hour (poll runs in the ${mixMin} menu minutes): +${Math.round(mixMin / 60 * out.friendsBusy.perPlayerHour)} requests / player-hour (busy list), +${Math.round(mixMin / 60 * out.friends.perPlayerHour)} (quiet)`);
  }
  row('paired / starting', out.match.toStart, 'requests for the two consoles, pairing to started');
  row('in a match', out.match.perPlayerHourInMatch, 'requests / player-hour');
  row('post-match (result, status, puts)', out.post.requestsPerMatchBothPlayers, 'requests for the two consoles');
  row('MIX hour', out.perPlayerHour, 'requests / player-hour');
  const day = 100000 - 1440 - 1000; // minus the 60 s alarm all day and the non-player traffic (page loads, probes): a stated allowance
  console.log(`  free plan, whole account:  ${capacity(out.perPlayerHour, day)} player-hours a day;  half (50/50 with Galaxy): ${capacity(out.perPlayerHour, day / 2)}`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
  const which = arg('--client', 'both');
  const modes = which === 'all' ? ['old', 'new', 'paced'] : which === 'both' ? ['old', 'new'] : [which];
  const results = [];
  console.log(`assumed hour: ${MIX.note}`);
  console.log('(every figure is an estimate from the model; no network, no live service)');
  for (const m of modes) {
    if (m !== 'old' && !clientConstants().holdSec) { console.log('\n(new schedule: not in this tree)'); continue; }
    const r = await estimate({ mode: m });
    results.push(r);
    if (!process.argv.includes('--json')) print(r);
  }
  if (process.argv.includes('--json')) console.log(JSON.stringify(results, null, 1));
}
void T0;
