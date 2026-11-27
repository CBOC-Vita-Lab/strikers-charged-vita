#!/usr/bin/env node
// Mutation check for the wave-11 changes (same-region pairing, rows-written budget): applies each one-line mutation to a source file, runs the named
// tests, and requires them to FAIL; the file is always put back. `node scripts/mutants.mjs` (about a minute); exit 1 if any mutant survives.
import { readFileSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const PAIR = ['test/core/ranked.test.js'];
const ROWS = ['test/core/mpsvc-rows.test.js'];
const BUDGET = ['test/core/mpsvc-rows.test.js', 'test/core/mpsvc-budget.test.js'];
const REPORT = ['test/core/reports.test.js'];
const BUILD = ['test/core/mpbeta-build.test.js'];
const M = (process.argv[2] ? (a) => a.filter((m) => m[0].includes(process.argv[2])) : (a) => a)([
  // pairing (src/core/ranked.js)
  // (mpbeta27) the same build only (src/core/ranked.js, config.js)
  ['build: filter dropped', 'src/core/ranked.js', "AND (build = ? OR build = '') ORDER BY", "AND (build = ? OR build <> '') ORDER BY", BUILD],
  ['build: not stored', 'src/core/ranked.js', "ticketId, ticketHash, mode, build,", "ticketId, ticketHash, mode, '',", BUILD],
  ['build: case kept', 'src/core/ranked.js', "buildHeader.toLowerCase() : ''", "buildHeader : ''", BUILD],
  ['build: old tickets refused', 'src/core/ranked.js', "AND (build = ? OR build = '') ORDER BY", "AND build = ? ORDER BY", BUILD],
  ['build: switch default off', 'src/core/config.js', "'QUEUE_SAME_BUILD', 1, 0, 1)", "'QUEUE_SAME_BUILD', 0, 0, 1)", BUILD],
  ['pair: quick ignores region', 'src/core/ranked.js', "(same ? 0 : (quick ? 1 : q.regionSlack))", "(same ? 0 : (quick ? 0 : q.regionSlack))", PAIR],
  ['pair: ranked slack ignored', 'src/core/ranked.js', "(same ? 0 : (quick ? 1 : q.regionSlack))", "(same ? 0 : (quick ? 1 : 0))", PAIR],
  ['pair: penalty on the wrong side', 'src/core/ranked.js', "(same ? 0 : (quick ? 1 : q.regionSlack))", "(same ? (quick ? 1 : q.regionSlack) : 0)", PAIR],
  ['pair: ranked ignores the rating gap', 'src/core/ranked.js', "const key = (quick ? 0 : diff) +", "const key = 0 +", PAIR],
  ['pair: strict first seconds dropped', 'src/core/ranked.js', "if (!same && Math.min(myWait, oWait) < q.regionStrictMs) continue;", "", PAIR],
  ['pair: ties go to the newest waiter', 'src/core/ranked.js', "if (key < bestKey) {", "if (key <= bestKey) {", PAIR],
  ['pair: slack default 0', 'src/core/config.js', "'QUEUE_REGION_SLACK', 100,", "'QUEUE_REGION_SLACK', 0,", PAIR],
  ['pair: slack variable ignored', 'src/core/config.js', "regionSlack: envInt(env, 'QUEUE_REGION_SLACK', 100, 0, 100000)", "regionSlack: 100", PAIR],
  // rows (src/core/budget.js, app.js, config.js)
  ['rows: statements not counted', 'src/core/app.js', "if (rowSink && cur.rowsWritten > 0) rowSink(cur.rowsWritten);", "", ROWS],
  ['rows: counted as one per statement', 'src/core/app.js', "rowSink(cur.rowsWritten)", "rowSink(1)", ROWS],
  ['rows: share ignores rows', 'src/core/budget.js', "Math.max(reqShare(now), rowsShare(now))", "reqShare(now)", ROWS],
  ['rows: share ignores requests', 'src/core/budget.js', "Math.max(reqShare(now), rowsShare(now))", "rowsShare(now)", ROWS],
  ['rows: not persisted', 'src/core/budget.js', "KEY_ROWS, `${dayIndex(now)}:${st.rows}`", "KEY_ROWS, `${dayIndex(now)}:0`", ROWS],
  ['rows: request counter format changed', 'src/core/budget.js', "KEY, `${dayIndex(now)}:${st.used}`);", "KEY, `${dayIndex(now)}:${st.used}:${st.rows}`);", ROWS],
  ['rows: no margin after a restart', 'src/core/budget.js', "Number(m[2])) + ROWS_FLUSH_EVERY;", "Number(m[2])) + 0;", ROWS],
  ['rows: legacy counter guessed as zero', 'src/core/budget.js', "(known ? Number(mr[2]) : Number(m[2]))", "(known ? Number(mr[2]) : 0)", ROWS],
  ['rows: rows tier never logged', 'src/core/budget.js', "const qw = b.rowsDaily ? tierOf(rowsShare(now) * 100) : 0;", "const qw = 0;", ROWS],
  ['rows: add() ignores n', 'src/core/budget.js', "st.rows += n;", "st.rows += 1;", ROWS],
  ['rows: not reset at midnight', 'src/core/budget.js', "st.alarms = 0; st.rows = 0;", "st.alarms = 0;", ROWS],
  ['rows: no flush on rows', 'src/core/budget.js', " || st.rows - st.rowsFlushed >= ROWS_FLUSH_EVERY", "", ROWS],
  ['rows: default share 100,000', 'src/core/config.js', "'DAILY_ROWS_BUDGET', 50000,", "'DAILY_ROWS_BUDGET', 100000,", ROWS],
  ['rows: zero does not switch the guard off', 'src/core/budget.js', "return b.rowsDaily ? st.rows / b.rowsDaily : 0;", "return st.rows / (b.rowsDaily || 1);", ROWS],
  ['rows: pollScale ignores rows', 'src/core/budget.js', "const f = share(now) * 100;\n    if (f <= b.pacePct) return 1;", "const f = reqShare(now) * 100;\n    if (f <= b.pacePct) return 1;", ROWS],
  // problem reports, session numbers, Delete My Online Account (src/core/reports.js, accounts.js, edge.js, budget.js)
  ['report: size cap 96 KiB in the object', 'src/core/reports.js', "export const REPORT_MAX_BYTES = 48 * 1024;", "export const REPORT_MAX_BYTES = 96 * 1024;", REPORT],
  ['report: size cap 96 KiB at the edge', 'src/core/edge.js', "{ '/v1/report': 48 * 1024 }", "{ '/v1/report': 96 * 1024 }", REPORT],
  ['report: the big body limit is not applied', 'src/core/app.js', "o.maxBody ? bodyLimit(url.pathname) : MAX_BODY", "MAX_BODY", REPORT],
  ['report: 4 a day', 'src/core/reports.js', "export const REPORTS_PER_PLAYER_DAY = 3;", "export const REPORTS_PER_PLAYER_DAY = 4;", REPORT],
  ['report: the daily count is not checked', 'src/core/reports.js', "if (getDaily(c, p.pid).reports >= REPORTS_PER_PLAYER_DAY) throw dailyCapError(c, 'reports');", "", REPORT],
  ['report: the daily count is not bumped', 'src/core/reports.js', "    bumpDaily(c, p.pid, 'reports');\n", "", REPORT],
  ['report: kept 60 days', 'src/core/reports.js', "export const REPORT_KEEP_DAYS = 30;", "export const REPORT_KEEP_DAYS = 60;", REPORT],
  ['report: the hourly sweep keeps old reports', 'src/core/reports.js', "export function pruneReports(c) {\n  c.db.run(`DELETE FROM reports WHERE created_ms < ?`, c.now - REPORT_KEEP_DAYS * DAY_MS);", "export function pruneReports(c) {", REPORT],
  ['report: 3,000 reports', 'src/core/reports.js', "export const REPORT_MAX_COUNT = 2000;", "export const REPORT_MAX_COUNT = 3000;", REPORT],
  ['report: 128 MiB', 'src/core/reports.js', "export const REPORT_MAX_TOTAL_BYTES = 64 * 1024 * 1024;", "export const REPORT_MAX_TOTAL_BYTES = 128 * 1024 * 1024;", REPORT],
  ['report: the newest are dropped, not the oldest', 'src/core/reports.js', "SELECT id FROM reports ORDER BY id LIMIT ?", "SELECT id FROM reports ORDER BY id DESC LIMIT ?", REPORT],
  ['report: no trim after a report', 'src/core/reports.js', "    trimReports(c);\n  });", "  });", REPORT],
  ['report: addresses are not scrubbed', 'src/core/reports.js', ".replace(/\\b\\d{1,3}(?:\\.\\d{1,3}){3}(?::\\d{1,5})?\\b/g, '<address>')", "", REPORT],
  ['report: tokens are not scrubbed', 'src/core/reports.js', ".replace(/\\b[A-Za-z0-9_-]{40,}\\b/g, '<secret>')", "", REPORT],
  ['report: the scrub is not applied', 'src/core/reports.js', "for (const k of ['log', 'events', 'conn', 'note', 'name']) out[k] = scrubText(out[k]);", "", REPORT],
  ['report: unknown fields accepted', 'src/core/reports.js', "  strict(body, FIELDS);\n", "", REPORT],
  ['report: a session number out of range is accepted', 'src/core/reports.js', "if (!isInt(s[k], lo, hi)) throw", "if (false) throw", REPORT],
  ['report: not a class of its own', 'src/core/budget.js', "if (pathname === '/v1/report') return 'report';", "", REPORT],
  ['report: never refused by the budget', 'src/core/budget.js', "if (klass === 'report') return true; // any level above ok", "", REPORT],
  ['report: the budget message is the generic one', 'src/core/budget.js', "const msg = klass === 'report'", "const msg = false", REPORT],
  ['report: no kill switch', 'src/core/app.js', "{ auth: true, kill: 'report', rl: ['report', 6, 3600, 'pid'], maxBody: true }", "{ auth: true, rl: ['report', 6, 3600, 'pid'], maxBody: true }", REPORT],
  ['delete: reports are not purged', 'src/core/accounts.js', "    forgetReports(c, p.pid); //", "    //", REPORT],
  ['delete: only the first report is purged', 'src/core/reports.js', "DELETE FROM reports WHERE pid = ?`, pid);", "DELETE FROM reports WHERE pid = ? AND id = (SELECT MIN(id) FROM reports WHERE pid = ?)`, pid, pid);", REPORT],
  ['delete: /v1/account is not the same function', 'src/core/app.js', "R('DELETE', '/v1/account', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return accounts.deleteAccount(c, r.player); }),", "R('DELETE', '/v1/account', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return { ok: true }; }),", REPORT],
  ['delete: POST /v1/account is not the same function', 'src/core/app.js', "R('POST', '/v1/account', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return accounts.deleteAccount(c, r.player); }),", "R('POST', '/v1/account', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return { ok: true }; }),", REPORT],
  ['session: numbers not recorded on POST leave', 'src/core/app.js', "const out = rooms.leaveRoom(c, r.player, r.params.code); reports.recordSession(c, r.stats); return out; }),\n    R('DELETE', '/v1/rooms/:code'", "const out = rooms.leaveRoom(c, r.player, r.params.code); return out; }),\n    R('DELETE', '/v1/rooms/:code'", REPORT],
  ['session: numbers not recorded on DELETE leave', 'src/core/app.js', "const out = rooms.leaveRoom(c, r.player, r.params.code); reports.recordSession(c, r.stats); return out; }),\n\n    R('POST', '/v1/ranked/queue',", "const out = rooms.leaveRoom(c, r.player, r.params.code); return out; }),\n\n    R('POST', '/v1/ranked/queue',", REPORT],
  ['session: ring never trimmed', 'src/core/reports.js', "if (last && last.id > SESSION_RING)", "if (false)", REPORT],
  ['session: ring of 600', 'src/core/reports.js', "export const SESSION_RING = 500;", "export const SESSION_RING = 600;", REPORT],
  ['session: out of range numbers accepted', 'src/core/reports.js', "if (v < lo || v > hi) return null;", "", REPORT],
  ['session: daily aggregate not summed', 'src/core/reports.js', "sessions = sessions + 1,", "sessions = 1,", REPORT],
  ['session: unknown keys accepted', 'src/core/reports.js', "!Object.hasOwn(SESSION_KEYS, m[1]) || ", "", REPORT],
  ['session: a failure in the numbers breaks the leave', 'src/core/reports.js', "  } catch {\n    return false;\n  }\n}\n\n// ----", "  } catch (e) {\n    throw e;\n  }\n}\n\n// ----", REPORT],
  // the object's request ceiling
  ['ceiling: over only at 125 %', 'src/core/budget.js', "if (f >= 1) return 'over';", "if (f >= 1.25) return 'over';", BUDGET],
  ['ceiling: default share 56,250', 'src/core/config.js', "'DAILY_REQUEST_BUDGET', 45000,", "'DAILY_REQUEST_BUDGET', 56250,", ROWS],
]);

let survived = 0;
for (const [name, file, from, to, tests] of M) {
  const path = join(root, file);
  const orig = readFileSync(path, 'utf8');
  if (orig.split(from).length !== 2) { console.log(`?? ${name}: pattern not found exactly once`); survived++; continue; }
  writeFileSync(path, orig.replace(from, () => to));
  let r;
  try { r = spawnSync('npx', ['vitest', 'run', ...tests], { cwd: root, encoding: 'utf8' }); } finally { writeFileSync(path, orig); }
  const killed = r.status !== 0;
  console.log(`${killed ? 'killed  ' : 'SURVIVED'} ${name}`);
  if (!killed) survived++;
}
console.log(survived ? `${survived} mutant(s) not killed` : `all ${M.length} mutants killed`);
process.exit(survived ? 1 : 0);
