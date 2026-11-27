// Problem reports and anonymous session numbers (the public beta, 15 Nov 2026): the owner has one console, so what two consoles do to each other is learned
// from what players choose to send.
//
//  * POST /v1/report  (signed like every player call: bearer token, X-Msc-App). The game sends it only after the player pressed SEND on the consent
//    screen. The body is one JSON object of at most REPORT_MAX_BYTES (48 KiB; edge.js lets this one route carry that much, every other route 8 KiB).
//    3 a player a UTC day (stored, so a restart keeps the count), kept REPORT_KEEP_DAYS (30), at most REPORT_MAX_COUNT (2,000) reports and
//    REPORT_MAX_TOTAL_BYTES (64 MiB) in all, the oldest dropped first. Refused cleanly (a JSON error with a sentence) when the daily budget is nearly
//    used (budget.js class 'report': from the browse level on), the player used their three, or the switch `report` is on.
//  * What is stored: the JSON the game sent (after a second scrub of addresses, MAC addresses and tokens: the game already removes them, this is the
//    service not trusting it), the profile id (so Delete My Online Account can remove it), the build and the time. The sender's network address is never stored.
//  * Session numbers: the leave request of an online session may carry a header `X-Msc-Stats: d=312;dl=2;rs=0;dr=1;sv=0;e=0` (duration seconds, input delay
//    ticks, resends, drops, starved steps, ended with an error). No name, id or address. They go into a ring of SESSION_RING rows and a daily aggregate
//    (session_daily) that is kept 90 days. A malformed header is ignored: it never fails the leave.
//  * Admin: GET /v1/admin/reports (newest first, metadata), GET /v1/admin/reports/:id (one report), GET /v1/admin/diag (daily aggregates and the session ring).

import { ApiError, isInt, strict, VERSION_RE } from './util.js';
import { bumpDaily, getDaily, dayIndex, dailyCapError } from './limits.js';

export const REPORT_MAX_BYTES = 48 * 1024;
export const REPORT_MAX_COUNT = 2000;
export const REPORT_MAX_TOTAL_BYTES = 64 * 1024 * 1024;
export const REPORT_KEEP_DAYS = 30;
export const REPORTS_PER_PLAYER_DAY = 3;
export const SESSION_RING = 500;
export const DAILY_KEEP_DAYS = 90;
const DAY_MS = 86400000;

const FIELDS = ['v', 'app', 'build', 'name', 'log', 'events', 'conn', 'session', 'note'];
const TEXT_MAX = { app: 24, build: 64, name: 40, log: 24000, events: 16000, conn: 1200, note: 400 };
const BUILD_HEX_RE = /^[0-9a-fA-F]{8,64}$/; // the game sends its 8-hex build fingerprint
const SESSION_KEYS = { d: [0, 86400], dl: [0, 30], rs: [0, 1e6], dr: [0, 1e6], sv: [0, 1e7], e: [0, 1] };

/**
 * Masks a text before it is stored: the game scrubs its own report, this is the second line. Wave 14 (gxnet14): the rules of the Galaxy service's maskText
 * (round e / 34 reviews) replace the older word-boundary rules, which let an address glued to a letter or underscore through (ip_1.2.3.4, 1.2.3.4b), knew no
 * e-mail address, no dotted MAC (aabb.ccdd.eeff), no "profile 123456" and took a time such as 12:34:56 for an IPv6 address.
 * Control characters out (except \n and \t); `Bearer <anything>`; e-mail addresses; MAC addresses (colon, dash, dotted); token-shaped runs (40+ of [A-Za-z0-9_-]) and long hex
 * runs; 4+ digits after profile / pid / account / uid / player; IPv6 (validated: a `::`, or eight groups of at most four digits) and IPv4 addresses, with an optional port.
 * Every rule is linear on a 48 KB log. Idempotent. The placeholders are the older ones (<address>, <secret>) plus <email> and <id>.
 */
// eslint-disable-next-line no-control-regex
const CONTROL_RE = /[\u0000-\u0008\u000B-\u001F\u007F-\u009F]/g;
const BEARER_RE = /\bBearer\s+\S+/gi;
const EMAIL_RE = /[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9.-]{1,255}\.[A-Za-z]{2,24}/g; // bounded: linear on a 48 KB log
const MAC_RE = /(?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])|(?<![0-9A-Fa-f.])(?:[0-9A-Fa-f]{4}\.){2}[0-9A-Fa-f]{4}(?![0-9A-Fa-f])/g;
const LONG_RE = /[A-Za-z0-9_-]{40,}/g; // the session token is 43 base64url characters; any such run is token-shaped
const HEX_RE = /[0-9A-Fa-f]{32,}/g;
const ID_NUM_RE = /\b(profile|pid|account|uid|player)([ \t]*[=:][ \t]*|[ \t]+)\d{4,}/gi; // `pid4711` (glued) and `player 12` stay
const IPV4_RE = /(?<!\d)(?:\d{1,3}\.){3}\d{1,3}(?!\d)(?::\d{1,5}(?!\d))?/g; // glued to letters or underscores too (ip_1.2.3.4, x192.168.1.1, 1.2.3.4b)
const COLON_RE = /(?<![\w:.])[0-9A-Fa-f:]*:[0-9A-Fa-f:]*(?:\.\d{1,3}){0,3}/g; // checked by isIPv6

const octetsOk = (s) => s.split(/[.:]/).slice(0, 4).every((o) => Number(o) <= 255);

/** A run of hex digits and colons that is an IPv6 address: a `::` somewhere, or eight groups, each of at most four digits (a time such as 12:34:56 is not one). */
function isIPv6(tok) {
  if (tok.split(':').length - 1 < 2) return false;
  let t = tok;
  const dot = t.indexOf('.');
  if (dot >= 0) { // an embedded IPv4 tail counts as two groups
    const lastColon = t.lastIndexOf(':');
    const quad = t.slice(lastColon + 1);
    if (!/^(?:\d{1,3}\.){3}\d{1,3}$/.test(quad) || !octetsOk(quad)) return false;
    t = t.slice(0, lastColon + 1) + '0:0';
  }
  const dc = t.indexOf('::');
  if (dc >= 0 && t.indexOf('::', dc + 1) >= 0) return false; // two compressions
  const groups = t.split(':').filter((g, i, a) => !(g === '' && (i === 0 || i === a.length - 1 || dc >= 0)));
  if (!groups.every((g) => /^[0-9A-Fa-f]{1,4}$/.test(g))) return false;
  return dc >= 0 ? groups.length <= 7 : groups.length === 8;
}

export function scrubText(s) {
  let t = String(s).replace(CONTROL_RE, '');
  t = t.replace(BEARER_RE, 'Bearer <secret>');
  t = t.replace(EMAIL_RE, '<email>');
  t = t.replace(MAC_RE, '<address>');
  t = t.replace(LONG_RE, '<secret>');
  t = t.replace(HEX_RE, '<secret>');
  t = t.replace(ID_NUM_RE, '$1$2<id>');
  t = t.replace(COLON_RE, (m) => (isIPv6(m) ? '<address>' : m));
  t = t.replace(IPV4_RE, (m) => (octetsOk(m) ? '<address>' : m));
  return t;
}

function text(body, key) {
  const v = body[key];
  if (v === undefined) return '';
  if (typeof v !== 'string') throw new ApiError(400, 'bad_request', `${key} must be text`, { field: key });
  if (v.length > TEXT_MAX[key]) throw new ApiError(400, 'bad_request', `${key} is too long`, { field: key });
  return v;
}

/** Validates a report body (already parsed JSON) and returns the object to store. Throws ApiError 400. */
export function checkReport(body) {
  strict(body, FIELDS);
  if (body.v !== undefined && body.v !== 1) throw new ApiError(400, 'bad_request', 'unknown report version', { field: 'v' });
  const out = { v: 1 };
  for (const k of ['app', 'build', 'name', 'log', 'events', 'conn', 'note']) out[k] = text(body, k);
  // app and build are stored as columns and are not scrubbed: only a version (1.3.0) and a hex build hash are accepted (Codex review, 15 Nov)
  if (out.app && !VERSION_RE.test(out.app)) throw new ApiError(400, 'bad_request', 'app must look like 1.3.0', { field: 'app' });
  if (out.build && !BUILD_HEX_RE.test(out.build)) throw new ApiError(400, 'bad_request', 'build must be a hex build hash', { field: 'build' });
  if (!out.log && !out.events && !out.conn && !out.note && body.session === undefined) throw new ApiError(400, 'bad_request', 'the report is empty');
  if (body.session !== undefined) {
    const s = strict(body.session, Object.keys(SESSION_KEYS));
    out.session = {};
    for (const [k, [lo, hi]] of Object.entries(SESSION_KEYS)) {
      if (s[k] === undefined) continue;
      if (!isInt(s[k], lo, hi)) throw new ApiError(400, 'bad_request', `session.${k} out of range`, { field: `session.${k}` });
      out.session[k] = s[k];
    }
  }
  for (const k of ['log', 'events', 'conn', 'note', 'name']) out[k] = scrubText(out[k]);
  return out;
}

export function refusedReports() {
  return new ApiError(503, 'service_disabled', 'problem reports are switched off for now', { retryAfterSec: 3600 });
}

/** POST /v1/report. `raw` is the request text (its byte length is the size that counts). */
export function submitReport(c, p, body, rawBytes) {
  if (rawBytes > REPORT_MAX_BYTES) throw new ApiError(413, 'too_large', 'the report is over 48 KB');
  if (body === undefined) throw new ApiError(400, 'bad_request', 'a report body is required');
  const rep = checkReport(body);
  if (getDaily(c, p.pid).reports >= REPORTS_PER_PLAYER_DAY) throw dailyCapError(c, 'reports');
  const stored = JSON.stringify(rep);
  const bytes = new TextEncoder().encode(stored).length;
  c.tx(() => {
    c.db.run(`INSERT INTO reports (pid, created_ms, app, build, bytes, body, kind) VALUES (?, ?, ?, ?, ?, ?, ?)`, p.pid, c.now, rep.app.slice(0, 24), rep.build.slice(0, 64), bytes, stored, kindOf(rep));
    bumpDaily(c, p.pid, 'reports');
    bumpReportDay(c, bytes);
    trimReports(c);
  });
  if (c.log) c.log(`[report] stored: ${bytes} bytes`); // the size only: no field the client wrote reaches the log (Workers Logs)
  return { ok: true, keptDays: REPORT_KEEP_DAYS, left: Math.max(0, REPORTS_PER_PLAYER_DAY - getDaily(c, p.pid).reports) };
}

function bumpReportDay(c, bytes) {
  const day = dayIndex(c.now);
  c.db.run(
    `INSERT INTO report_daily (day, reports, bytes) VALUES (?, 1, ?)
     ON CONFLICT(day) DO UPDATE SET reports = reports + 1, bytes = bytes + excluded.bytes`,
    day, bytes,
  );
}

/** Drops reports past their 30 days, then the oldest while the count or the stored bytes are over the caps. */
export function trimReports(c) {
  c.db.run(`DELETE FROM reports WHERE created_ms < ?`, c.now - REPORT_KEEP_DAYS * DAY_MS);
  for (let guard = 0; guard < 2100; guard++) {
    const t = c.db.get(`SELECT COUNT(*) AS n, COALESCE(SUM(bytes), 0) AS b FROM reports`);
    if (t.n <= REPORT_MAX_COUNT && t.b <= REPORT_MAX_TOTAL_BYTES) return;
    const over = Math.max(t.n - REPORT_MAX_COUNT, 1);
    c.db.run(`DELETE FROM reports WHERE id IN (SELECT id FROM reports ORDER BY id LIMIT ?)`, over);
  }
}

/** Housekeeping (the hourly sweep): expired reports, old daily rows. */
export function pruneReports(c) {
  c.db.run(`DELETE FROM reports WHERE created_ms < ?`, c.now - REPORT_KEEP_DAYS * DAY_MS);
  const keep = dayIndex(c.now) - DAILY_KEEP_DAYS;
  c.db.run(`DELETE FROM report_daily WHERE day < ?`, keep);
  c.db.run(`DELETE FROM session_daily WHERE day < ?`, keep);
  c.db.run(`DELETE FROM report_summary WHERE day < ?`, keep);
}

/** The reports of one player go with their account. */
export function forgetReports(c, pid) {
  c.db.run(`DELETE FROM reports WHERE pid = ?`, pid);
}

// ---------------------------------------------------------------------------------------------------------------- session numbers
/** Parses X-Msc-Stats. Returns the numbers or null (anything malformed, unknown, out of range or too long). */
export function parseStats(header) {
  if (typeof header !== 'string' || header.length === 0 || header.length > 120) return null;
  const out = {};
  for (const part of header.split(';')) {
    const m = /^([a-z]{1,2})=(\d{1,8})$/.exec(part);
    if (!m || !Object.hasOwn(SESSION_KEYS, m[1]) || Object.hasOwn(out, m[1])) return null;
    const v = Number(m[2]);
    const [lo, hi] = SESSION_KEYS[m[1]];
    if (v < lo || v > hi) return null;
    out[m[1]] = v;
  }
  return Object.keys(out).length ? out : null;
}

/** Called by the leave routes. Never throws: the leave itself must not depend on this. */
export function recordSession(c, header) {
  try {
    const s = parseStats(header);
    if (!s) return false;
    const day = dayIndex(c.now);
    c.tx(() => {
      c.db.run(
        `INSERT INTO session_ring (created_ms, day, dur, delay, resends, drops, starved, err) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
        c.now, day, s.d ?? 0, s.dl ?? 0, s.rs ?? 0, s.dr ?? 0, s.sv ?? 0, s.e ?? 0,
      );
      c.db.run(
        `INSERT INTO session_daily (day, sessions, errors, dur_sum, delay_sum, resends_sum, drops_sum, starved_sum) VALUES (?, 1, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(day) DO UPDATE SET sessions = sessions + 1, errors = errors + excluded.errors, dur_sum = dur_sum + excluded.dur_sum,
           delay_sum = delay_sum + excluded.delay_sum, resends_sum = resends_sum + excluded.resends_sum, drops_sum = drops_sum + excluded.drops_sum,
           starved_sum = starved_sum + excluded.starved_sum`,
        day, s.e ?? 0, s.d ?? 0, s.dl ?? 0, s.rs ?? 0, s.dr ?? 0, s.sv ?? 0,
      );
      const last = c.db.get(`SELECT MAX(id) AS id FROM session_ring`);
      if (last && last.id > SESSION_RING) c.db.run(`DELETE FROM session_ring WHERE id <= ?`, last.id - SESSION_RING);
    });
    return true;
  } catch {
    return false;
  }
}

// ---------------------------------------------------------------------------------------------------------------- admin
export function listReports(c, params) {
  const limit = Math.min(200, Math.max(1, Number(params.get('limit') || 50)));
  const before = Number(params.get('before') || 0);
  if (!Number.isFinite(limit) || !Number.isFinite(before) || before < 0) throw new ApiError(400, 'bad_request', 'bad paging');
  const rows = c.db.all(
    `SELECT r.id, r.pid, r.created_ms, r.app, r.build, r.bytes, p.name FROM reports r LEFT JOIN profiles p ON p.pid = r.pid
       ${before ? 'WHERE r.id < ?' : ''} ORDER BY r.id DESC LIMIT ?`,
    ...(before ? [before] : []), limit,
  );
  const t = c.db.get(`SELECT COUNT(*) AS n, COALESCE(SUM(bytes), 0) AS b FROM reports`);
  return {
    total: t.n, totalBytes: t.b, maxCount: REPORT_MAX_COUNT, maxBytes: REPORT_MAX_TOTAL_BYTES,
    reports: rows.map((r) => ({ id: r.id, pid: r.pid, name: r.name, createdMs: r.created_ms, app: r.app, build: r.build, bytes: r.bytes })),
    next: rows.length === limit ? rows[rows.length - 1].id : null,
  };
}

export function readReport(c, rawId) {
  const id = Number(rawId);
  if (!Number.isInteger(id) || id < 1) throw new ApiError(404, 'not_found', 'no such report');
  const r = c.db.get(`SELECT * FROM reports WHERE id = ?`, id);
  if (!r) throw new ApiError(404, 'not_found', 'no such report');
  let body = null;
  try { body = JSON.parse(r.body); } catch { /* keep null */ }
  return { id: r.id, createdMs: r.created_ms, app: r.app, build: r.build, bytes: r.bytes, report: body };
}

export function diag(c, params) {
  const days = Math.min(90, Math.max(1, Number(params.get('days') || 14)));
  const from = dayIndex(c.now) - (Number.isFinite(days) ? days : 14);
  return {
    nowMs: c.now,
    reportsDaily: c.db.all(`SELECT day, reports, bytes FROM report_daily WHERE day > ? ORDER BY day DESC`, from),
    sessionsDaily: c.db.all(
      `SELECT day, sessions, errors, dur_sum AS durSum, delay_sum AS delaySum, resends_sum AS resendsSum, drops_sum AS dropsSum, starved_sum AS starvedSum
         FROM session_daily WHERE day > ? ORDER BY day DESC`, from,
    ),
    sessionRing: c.db.all(
      `SELECT id, created_ms AS createdMs, dur, delay, resends, drops, starved, err FROM session_ring ORDER BY id DESC LIMIT ?`,
      Math.min(SESSION_RING, Math.max(1, Number(params.get('ring') || 100))),
    ),
  };
}

// ---------------------------------------------------------------------------------------------------------------- daily summary (wave 13, O7)
// The owner reads the beta's problem reports one by one (GET /v1/admin/reports); this is the one-glance version. A cron trigger (wrangler.toml `[triggers]`, 00:13 UTC) calls
// the object once a day; it writes one `report_summary` row for the UTC day that just ended (counts by kind / build / app, bytes, the session aggregate of that day) and logs one
// line of numbers (Workers Logs are on). GET /v1/admin/report-summary shows the stored rows and a live row for today. Nothing personal: no profile id, no name, no address, no
// report text; builds are the 8-hex fingerprints the game itself sends. Rows are kept 90 days. Storage stays the object's SQLite (the reports table), as before.

/** The kinds, by the first rule that matches (a stored column, so the summary never reads report bodies). Heuristics over the game's own sections, not a promise. */
export const KINDS = ['note', 'session_error', 'desync', 'connection', 'session_trouble', 'other'];
export function kindOf(rep) {
  if (rep.note) return 'note'; // the player wrote something
  if (rep.session && rep.session.e === 1) return 'session_error'; // the session ended with an error (X-Msc-Stats `e`)
  if (/desync|mismatch|diverg/i.test(`${rep.events}\n${rep.log}`)) return 'desync'; // lockstep trouble
  if (/fail|timeout|timed out|refused|unreachable|error/i.test(rep.conn || '')) return 'connection'; // the connection test said no
  if (rep.session && ((rep.session.dr || 0) > 0 || (rep.session.sv || 0) > 0)) return 'session_trouble'; // drops or starved steps
  return 'other';
}

const TOP_BUILDS = 10;
const tally = (rows, key) => {
  const m = new Map();
  for (const r of rows) m.set(r[key] || '(none)', (m.get(r[key] || '(none)') || 0) + r.n);
  return [...m].sort((a, b) => b[1] - a[1] || String(a[0]).localeCompare(String(b[0]))).slice(0, TOP_BUILDS).map(([name, n]) => ({ name, n }));
};

/** Counts of the reports stored in [fromMs, toMs): total, bytes, by kind (all known kinds, zeros included), top builds, top app versions. No personal data. */
export function countReports(c, fromMs, toMs) {
  const t = c.db.get(`SELECT COUNT(*) AS n, COALESCE(SUM(bytes), 0) AS b FROM reports WHERE created_ms >= ? AND created_ms < ?`, fromMs, toMs);
  const kinds = Object.fromEntries(KINDS.map((k) => [k, 0]));
  for (const r of c.db.all(`SELECT kind, COUNT(*) AS n FROM reports WHERE created_ms >= ? AND created_ms < ? GROUP BY kind`, fromMs, toMs)) kinds[r.kind || 'unknown'] = (kinds[r.kind || 'unknown'] || 0) + r.n;
  const builds = c.db.all(`SELECT build, COUNT(*) AS n FROM reports WHERE created_ms >= ? AND created_ms < ? GROUP BY build`, fromMs, toMs);
  const apps = c.db.all(`SELECT app, COUNT(*) AS n FROM reports WHERE created_ms >= ? AND created_ms < ? GROUP BY app`, fromMs, toMs);
  return { reports: t.n, bytes: t.b, byKind: kinds, byBuild: tally(builds, 'build'), byApp: tally(apps, 'app') };
}

function sessionDay(c, day) {
  const s = c.db.get(`SELECT sessions, errors FROM session_daily WHERE day = ?`, day);
  return s ? { sessions: s.sessions, errors: s.errors } : { sessions: 0, errors: 0 };
}

/** The row for UTC day `day` (an index since 1970): what it holds when computed now. */
export function summaryOf(c, day) {
  const from = day * DAY_MS;
  return { day, date: new Date(from).toISOString().slice(0, 10), ...countReports(c, from, from + DAY_MS), ...sessionDay(c, day) };
}

/** The cron: summarize the day before `c.now` (UTC) once; a second run rewrites the same row. Returns the row. */
export function makeSummary(c) {
  const day = dayIndex(c.now) - 1;
  const row = summaryOf(c, day);
  c.db.run(
    `INSERT INTO report_summary (day, made_ms, body) VALUES (?, ?, ?) ON CONFLICT(day) DO UPDATE SET made_ms = excluded.made_ms, body = excluded.body`,
    day, c.now, JSON.stringify(row),
  );
  const top = row.byKind;
  if (c.log) c.log(`[report-summary] ${row.date}: ${row.reports} reports (${KINDS.map((k) => `${k} ${top[k] || 0}`).join(', ')}), ${row.bytes} bytes, ${row.sessions} sessions, ${row.errors} ended in error`);
  return row;
}

/** GET /v1/admin/report-summary?days=N (default 7, at most 90): the stored rows, newest first, and today so far. */
export function summaryView(c, params) {
  const days = Math.min(90, Math.max(1, Number(params.get('days') || 7)));
  const today = dayIndex(c.now);
  const stored = c.db.all(`SELECT day, made_ms, body FROM report_summary WHERE day > ? ORDER BY day DESC`, today - (Number.isFinite(days) ? days : 7) - 1);
  return {
    nowMs: c.now,
    today: summaryOf(c, today),
    days: stored.map((r) => { let body = null; try { body = JSON.parse(r.body); } catch { /* keep null */ } return { day: r.day, madeMs: r.made_ms, ...(body || {}) }; }),
    note: 'counts only: no names, no profile ids, no report text. `kind` is a heuristic over each report (see src/core/reports.js kindOf).',
  };
}
