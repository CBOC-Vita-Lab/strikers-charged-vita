#!/usr/bin/env node
// Mutation check for test/core/turn-caps.test.js: each mutation breaks one rule of the wave-15 relay caps; the suite must FAIL for every one.
// Usage (from server/): node scripts/mutants-turn-caps.mjs     Exit 0 = every mutant was caught. The sources are restored afterwards.
import { readFileSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';

const T = 'src/core/turn.js', U = 'src/core/turn_usage.js', R = 'src/core/rooms.js';
const MUTANTS = [
  [T, "if (counter(c, rk) >= t.maxPerRoomHour) {", "if (false) {", 'room cap never trips'],
  [T, "const rk = `turn:room:${r.code}:${Math.floor(c.now / 3600_000)}`;", "const rk = `turn:room:${r.code}`;", 'room cap not per clock hour'],
  [T, "if (counter(c, ak) >= t.maxPerAddrDay) {", "if (false) {", 'address cap never trips'],
  [T, "const ak = `turn:creds:${day(c.now)}:a${afp}`;", "const ak = `turn:creds:${day(c.now)}:a${ipk}`;", 'raw address written as the key'],
  [T, "if (t.unneededSharePct < 100 && !turnNeeded(c, r, p.pid)) {", "if (false) {", 'unneeded slice never applies'],
  [T, "if (t.unneededSharePct < 100 && !turnNeeded(c, r, p.pid)) {", "if (t.unneededSharePct < 100) {", 'every credential counts as unneeded'],
  [T, "for (const k of reserved.extra) bumpCounter(c, k, -1);", "", 'reservation not given back'],
  [T, "if (stop && stop.why === 'over') await c.turnUsage.revokeValid(c);", "", 'valid passes never revoked'],
  [T, "if (named) c.db.run(", "if (false) c.db.run(", 'pass username not kept'],
  [U, "if (ok) { c.db.run(`UPDATE turn_mints SET revoked = 1 WHERE id = ?`, r.id); n++; stats.revoked++; }", "if (ok) { n++; stats.revoked++; }", 'revoked passes revoked again and again'],
  [U, "c.now - ttlMs,\n      );", "0,\n      );", 'passes past their life are revoked too'],
  [R, "|| r.host_punch === 'fail' || r.join_punch === 'fail'", "", 'a failed punch is no reason'],
  [R, "!!r.host_relay || !!r.join_relay ||", "", 'a relay claim is no reason'],
];
let missed = 0;
for (const [file, from, to, what] of MUTANTS) {
  const orig = readFileSync(file, 'utf8');
  if (!orig.includes(from)) { console.log(`SKIP (text not found): ${what}`); missed++; continue; }
  writeFileSync(file, orig.replace(from, to));
  let r;
  try { r = spawnSync('node_modules/.bin/vitest', ['run', 'test/core/turn-caps.test.js'], { encoding: 'utf8' }); } finally { writeFileSync(file, orig); }
  const caught = r.status !== 0;
  console.log(`${caught ? 'caught' : 'MISSED'}: ${what}`);
  if (!caught) missed++;
}
process.exit(missed ? 1 : 0);
