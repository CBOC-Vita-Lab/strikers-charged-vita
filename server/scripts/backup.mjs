#!/usr/bin/env node
// Full backup through the admin API: node scripts/backup.mjs <base-url> > backup.json
// The admin token is read from the ADMIN_TOKEN environment variable (never from the command line, so it stays out of
// shell history). The file contains token hashes: keep it private.
// Restore: node scripts/restore.mjs <base-url> < backup.json   (into an empty or existing service; rows are replaced by key)
import { adminBase } from './admin-target.mjs';
const base = process.argv[2] ? adminBase(process.argv[2]) : '';
const token = process.env.ADMIN_TOKEN;
if (!base || !token) {
  console.error('usage: ADMIN_TOKEN=... node scripts/backup.mjs https://your-worker.workers.dev > backup.json');
  process.exit(2);
}
const get = async (path) => {
  const r = await fetch(`${base}${path}`, { headers: { authorization: `Bearer ${token}` } });
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return r.json();
};
const list = await get('/v1/admin/export');
const out = { format: 'msc-online-backup-1', exportedAt: new Date().toISOString(), schema: list.schema, tables: {} };
for (const t of list.tables) {
  out.tables[t.table] = [];
  let after = 0;
  for (;;) {
    const page = await get(`/v1/admin/export?table=${t.table}&after=${after}&limit=1000`);
    out.tables[t.table].push(...page.rows);
    if (page.next === null) break;
    after = page.next;
  }
}
process.stdout.write(JSON.stringify(out));
