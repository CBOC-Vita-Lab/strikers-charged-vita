// SqlStorage-compatible adapter over node:sqlite (Node >= 22.5). Used by the unit tests and the Node fake server;
// the Worker itself uses the Durable Object's built-in SQLite.

import { DatabaseSync } from 'node:sqlite';

export function openNodeSql(path = ':memory:') {
  const db = new DatabaseSync(path);
  const cache = new Map();
  let depth = 0;
  // `rowsWritten` on the returned cursor, the way the Durable Object's SqlStorageCursor has it: every INSERT / UPDATE / DELETE row counts one,
  // plus one for each index the write touches (an UPDATE: the indexes whose column is in SET; a primary key that is not the rowid counts as an
  // index). ON CONFLICT upserts count as inserts (an upper bound). Tests and the estimators only; the Worker uses the platform's own number.
  const idxCache = new Map();
  const all = (q) => db.prepare(q).all();
  function indexCols(table) {
    if (!idxCache.has(table)) {
      const cols = [];
      const info = all(`PRAGMA table_info(${table})`);
      for (const ix of all(`PRAGMA index_list(${table})`)) {
        if (ix.origin === 'pk' && info.filter((c) => c.pk).length === 1 && /INTEGER/i.test(info.find((c) => c.pk).type)) continue; // rowid alias
        cols.push(all(`PRAGMA index_info(${ix.name})`).map((c) => c.name));
      }
      idxCache.set(table, cols);
    }
    return idxCache.get(table);
  }
  function rowsWritten(q) {
    const m = /^\s*(INSERT(?: OR \w+)? INTO|UPDATE|DELETE FROM)\s+(\w+)/i.exec(q);
    if (!m) return 0;
    const n = db.prepare('SELECT changes() AS n').get().n;
    if (!n) return 0;
    const idx = indexCols(m[2]);
    let per = 1;
    if (/^UPDATE/i.test(m[1])) {
      const set = /\bSET\b([\s\S]*?)(\bWHERE\b|$)/i.exec(q)?.[1] || '';
      per += idx.filter((cols) => cols.some((c) => new RegExp(`\\b${c}\\s*=`).test(set))).length;
    } else per += idx.length;
    return n * per;
  }
  const sql = {
    exec(query, ...binds) {
      let st = cache.get(query);
      if (!st) {
        st = db.prepare(query);
        cache.set(query, st);
      }
      const rows = st.all(...binds).map((r) => ({ ...r }));
      if (/^\s*(CREATE|ALTER|DROP)\b/i.test(query)) idxCache.clear();
      const written = rowsWritten(query);
      return { toArray: () => rows, one: () => rows[0], rowsWritten: written };
    },
  };
  const tx = (fn) => {
    if (depth > 0) return fn();
    depth++;
    db.exec('BEGIN');
    try {
      const r = fn();
      db.exec('COMMIT');
      return r;
    } catch (e) {
      db.exec('ROLLBACK');
      throw e;
    } finally {
      depth--;
    }
  };
  return { sql, tx, close: () => db.close(), raw: db };
}
