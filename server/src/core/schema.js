// Storage schema. Each migration runs once, in order, inside the Durable Object's SQLite database.
// Add new migrations at the END; never edit a shipped one.

export const MIGRATIONS = [
  {
    id: 1,
    name: 'initial',
    sql: [
      `CREATE TABLE profiles (
        pid INTEGER PRIMARY KEY AUTOINCREMENT,
        code TEXT NOT NULL UNIQUE,
        relay_only INTEGER NOT NULL DEFAULT 0,
        name TEXT NOT NULL,
        name_key TEXT NOT NULL UNIQUE,
        token_hash TEXT NOT NULL UNIQUE,
        created_ms INTEGER NOT NULL,
        last_seen_ms INTEGER NOT NULL,
        name_changed_ms INTEGER NOT NULL DEFAULT 0,
        region INTEGER NOT NULL DEFAULT 2,
        rating INTEGER NOT NULL DEFAULT 1000,
        wins INTEGER NOT NULL DEFAULT 0,
        losses INTEGER NOT NULL DEFAULT 0,
        draws INTEGER NOT NULL DEFAULT 0,
        abandons INTEGER NOT NULL DEFAULT 0,
        abandon_streak INTEGER NOT NULL DEFAULT 0,
        last_abandon_ms INTEGER NOT NULL DEFAULT 0,
        cooldown_until_ms INTEGER NOT NULL DEFAULT 0,
        banned INTEGER NOT NULL DEFAULT 0,
        ban_reason TEXT NOT NULL DEFAULT ''
      )`,
      // profile ids start above 100000 (AUTOINCREMENT remembers the largest id ever used)
      `INSERT INTO profiles (pid, code, name, name_key, token_hash, created_ms, last_seen_ms) VALUES (100000, '', '', '', '', 0, 0)`,
      `DELETE FROM profiles WHERE pid = 100000`,
      `CREATE TABLE rooms (
        code TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        state TEXT NOT NULL,
        host_pid INTEGER NOT NULL,
        join_pid INTEGER,
        created_ms INTEGER NOT NULL,
        expires_ms INTEGER NOT NULL,
        version INTEGER NOT NULL DEFAULT 1,
        key_b64 TEXT,
        cookie INTEGER,
        vip_host INTEGER,
        vip_join INTEGER,
        host_public TEXT, host_local TEXT, host_nonce TEXT, host_nat TEXT, host_ep_ms INTEGER, host_relay TEXT, host_cands TEXT,
        join_public TEXT, join_local TEXT, join_nonce TEXT, join_nat TEXT, join_ep_ms INTEGER, join_relay TEXT, join_cands TEXT,
        host_ack INTEGER NOT NULL DEFAULT 0,
        join_ack INTEGER NOT NULL DEFAULT 0,
        host_punch TEXT,
        join_punch TEXT,
        punch_at_ms INTEGER,
        host_started INTEGER NOT NULL DEFAULT 0,
        join_started INTEGER NOT NULL DEFAULT 0,
        match_id TEXT,
        closed_reason TEXT
      )`,
      `CREATE INDEX rooms_host ON rooms(host_pid)`,
      `CREATE INDEX rooms_join ON rooms(join_pid)`,
      `CREATE INDEX rooms_expires ON rooms(expires_ms)`,
      `CREATE TABLE queue (
        pid INTEGER PRIMARY KEY,
        region INTEGER NOT NULL,
        rating INTEGER NOT NULL,
        enqueued_ms INTEGER NOT NULL,
        last_poll_ms INTEGER NOT NULL,
        status TEXT NOT NULL,
        room_code TEXT,
        ticket_id TEXT,
        ticket_hash TEXT
      )`,
      `CREATE TABLE matches (
        match_id TEXT PRIMARY KEY,
        room_code TEXT NOT NULL,
        kind TEXT NOT NULL,
        p0 INTEGER NOT NULL,
        p1 INTEGER NOT NULL,
        created_ms INTEGER NOT NULL,
        started_ms INTEGER,
        first_result_ms INTEGER,
        status TEXT NOT NULL,
        leaver_slot INTEGER,
        score0 INTEGER,
        score1 INTEGER,
        resolved_ms INTEGER
      )`,
      `CREATE INDEX matches_p0 ON matches(p0)`,
      `CREATE INDEX matches_p1 ON matches(p1)`,
      `CREATE INDEX matches_status ON matches(status)`,
      `CREATE TABLE results (
        match_id TEXT NOT NULL,
        slot INTEGER NOT NULL,
        pid INTEGER NOT NULL,
        result INTEGER NOT NULL,
        score0 INTEGER NOT NULL,
        score1 INTEGER NOT NULL,
        checksum TEXT NOT NULL,
        submitted_ms INTEGER NOT NULL,
        attempt INTEGER NOT NULL DEFAULT 1,
        build TEXT NOT NULL DEFAULT '',
        mode TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (match_id, slot)
      )`,
      `CREATE TABLE pair_stats (
        lo_pid INTEGER NOT NULL,
        hi_pid INTEGER NOT NULL,
        fails INTEGER NOT NULL DEFAULT 0,
        oks INTEGER NOT NULL DEFAULT 0,
        updated_ms INTEGER NOT NULL,
        PRIMARY KEY (lo_pid, hi_pid)
      )`,
      `CREATE TABLE scores (
        category INTEGER NOT NULL,
        pid INTEGER NOT NULL,
        region INTEGER NOT NULL,
        score INTEGER NOT NULL,
        data_b64 TEXT NOT NULL,
        updated_ms INTEGER NOT NULL,
        PRIMARY KEY (category, pid)
      )`,
      `CREATE INDEX scores_rank ON scores(category, score DESC, updated_ms, pid)`,
      `CREATE TABLE allowance (
        pid INTEGER NOT NULL,
        category INTEGER NOT NULL,
        amount INTEGER NOT NULL,
        PRIMARY KEY (pid, category)
      )`,
      `CREATE TABLE counters (
        key TEXT PRIMARY KEY,
        value INTEGER NOT NULL,
        updated_ms INTEGER NOT NULL
      )`,
      `CREATE TABLE settings (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL
      )`,
    ],
  },
  {
    // Result checks, caps and flags (security review round 1). Unshipped when written: extend until the first deploy.
    id: 2,
    name: 'abuse_controls',
    sql: [
      // why a settled match was not rated ('daily_cap'), who is blamed for a dispute / no-contest (bit 0 = slot 0, bit 1 = slot 1)
      // and the rating penalty each slot took for it
      `ALTER TABLE matches ADD COLUMN reason TEXT NOT NULL DEFAULT ''`,
      `ALTER TABLE matches ADD COLUMN blame INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE matches ADD COLUMN pen0 INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE matches ADD COLUMN pen1 INTEGER NOT NULL DEFAULT 0`,
      // one row per player, overwritten when the UTC day changes: no growth, one row written per event
      `CREATE TABLE daily (
        pid INTEGER PRIMARY KEY,
        day INTEGER NOT NULL,
        rooms INTEGER NOT NULL DEFAULT 0,
        matches INTEGER NOT NULL DEFAULT 0,
        rated INTEGER NOT NULL DEFAULT 0,
        turn_reports INTEGER NOT NULL DEFAULT 0
      )`,
      `CREATE TABLE pair_daily (
        lo_pid INTEGER NOT NULL,
        hi_pid INTEGER NOT NULL,
        day INTEGER NOT NULL,
        rated INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (lo_pid, hi_pid)
      )`,
      // stored account flags for the admin (no automatic ban): kind = single_opponent | dodger
      `CREATE TABLE flags (
        pid INTEGER NOT NULL,
        kind TEXT NOT NULL,
        since_ms INTEGER NOT NULL,
        updated_ms INTEGER NOT NULL,
        hits INTEGER NOT NULL DEFAULT 1,
        detail TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (pid, kind)
      )`,
      // relay credentials minted per player: usage reports are only accepted against a recent mint
      `CREATE TABLE turn_mints (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        pid INTEGER NOT NULL,
        minted_ms INTEGER NOT NULL,
        used_bytes INTEGER NOT NULL DEFAULT 0
      )`,
      `CREATE INDEX turn_mints_pid ON turn_mints(pid, minted_ms)`,
    ],
  },
  {
    // Status that lasts (1.3): played-at-all counter (pruning never touches anyone who played), last verified ranked match, all-time
    // verified points, and the final standings of past weekly seasons. Additive only: the live data of migrations 1-2 is kept as it is.
    // Every step is safe to run again (addColumn checks the table first; the backfills recompute from matches; CREATE ... IF NOT EXISTS),
    // and the whole migration runs in one transaction (migrate()).
    id: 3,
    name: 'status_that_lasts',
    sql: [
      addColumn('profiles', 'matches_played', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('profiles', 'last_played_ms', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('profiles', 'points_total', 'INTEGER NOT NULL DEFAULT 0'),
      // backfill from the match records still kept (90 days) and from the counters that outlive them
      `UPDATE profiles SET matches_played = (SELECT COUNT(*) FROM matches m WHERE (m.p0 = profiles.pid OR m.p1 = profiles.pid) AND m.started_ms IS NOT NULL)
        WHERE matches_played = 0`,
      `UPDATE profiles SET matches_played = 1 WHERE matches_played = 0
        AND (wins + losses + draws + abandons > 0 OR pid IN (SELECT pid FROM scores))`,
      `UPDATE profiles SET last_played_ms = COALESCE((SELECT MAX(m.resolved_ms) FROM matches m
         WHERE m.kind = 'ranked' AND m.status = 'verified' AND (m.p0 = profiles.pid OR m.p1 = profiles.pid)), 0) WHERE last_played_ms = 0`,
      `UPDATE profiles SET points_total = COALESCE((SELECT SUM(
           CASE WHEN m.p0 = profiles.pid THEN (CASE WHEN m.score0 > m.score1 THEN 10 ELSE 1 END) + MIN(m.score0, 10)
                ELSE (CASE WHEN m.score1 > m.score0 THEN 10 ELSE 1 END) + MIN(m.score1, 10) END)
         FROM matches m WHERE m.kind = 'ranked' AND m.status = 'verified' AND (m.p0 = profiles.pid OR m.p1 = profiles.pid)), 0) WHERE points_total = 0`,
      // the all-time board reads the best rows through this index (a few rows read per page instead of every profile)
      `CREATE INDEX IF NOT EXISTS profiles_points ON profiles(points_total DESC, last_played_ms, pid)`,
      `CREATE TABLE IF NOT EXISTS season_scores (
        season_start_ms INTEGER NOT NULL,
        pid INTEGER NOT NULL,
        score INTEGER NOT NULL,
        data_b64 TEXT NOT NULL,
        updated_ms INTEGER NOT NULL,
        PRIMARY KEY (season_start_ms, pid)
      )`,
    ],
  },
  {
    // Strikers 1.3 hardening (net agent, wave 5). The id is deliberately far from 4, 5 and 6: Galaxy's service (the same code base, branch mp-int)
    // uses those for its own tables (rooms of up to four, friends, star times, its hardening). Migrations are matched by id, so a deployment that
    // had applied a different step under 4 would silently skip Galaxy's. Strikers-only steps use 1001 and up. Additive, every step can run again.
    //  - rooms: where each member's endpoint call came from (`*_src`, the caller's address as the edge saw it: IPv6 by /64) and whether its
    //    claim did not match that address (`*_forced`: the claims are dropped and the pair goes to the relay, see rooms.js postEndpoint);
    //  - profiles_last_seen: the idle-account sweep (accounts.pruneIdle) no longer scans every profile.
    id: 1001,
    name: 'strikers_endpoint_claims',
    sql: [
      addColumn('rooms', 'host_src', 'TEXT'),
      addColumn('rooms', 'join_src', 'TEXT'),
      addColumn('rooms', 'host_forced', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('rooms', 'join_forced', 'INTEGER NOT NULL DEFAULT 0'),
      `CREATE INDEX IF NOT EXISTS profiles_last_seen ON profiles(last_seen_ms)`,
    ],
  },
  {
    // Friends and quick match (wave 10). Additive only, every step can run again, and the previous code runs on the migrated database
    // (it never names these tables or columns; queue.mode and rooms.quick have defaults that mean "ranked" / "not quick").
    //  - friend_links(pid, other): `pid` added `other`. Both directions present = friends; one = a request (outgoing for pid, incoming for other).
    //  - friend_blocks(pid, other): `pid` blocked `other`. The blocked side is never told.
    //  - friend_status(pid): the game's own friend status string (base64), relayed to friends.
    //  - friend_invites(to_pid, from_pid): "from invites to into room `code`", one per pair, expires.
    //  - queue.mode: 'ranked' | 'quick' (unranked "play with anyone"); rooms.quick: 1 for a room the quick queue made (kind 'unranked').
    id: 1002,
    name: 'friends_quick_match',
    sql: [
      `CREATE TABLE IF NOT EXISTS friend_links (pid INTEGER NOT NULL, other INTEGER NOT NULL, created_ms INTEGER NOT NULL, PRIMARY KEY (pid, other))`,
      `CREATE INDEX IF NOT EXISTS friend_links_other ON friend_links(other)`,
      `CREATE TABLE IF NOT EXISTS friend_blocks (pid INTEGER NOT NULL, other INTEGER NOT NULL, created_ms INTEGER NOT NULL, PRIMARY KEY (pid, other))`,
      `CREATE INDEX IF NOT EXISTS friend_blocks_other ON friend_blocks(other)`,
      `CREATE TABLE IF NOT EXISTS friend_status (pid INTEGER PRIMARY KEY, status TEXT NOT NULL, updated_ms INTEGER NOT NULL)`,
      `CREATE TABLE IF NOT EXISTS friend_invites (to_pid INTEGER NOT NULL, from_pid INTEGER NOT NULL, code TEXT NOT NULL, created_ms INTEGER NOT NULL, PRIMARY KEY (to_pid, from_pid))`,
      addColumn('queue', 'mode', `TEXT NOT NULL DEFAULT 'ranked'`),
      addColumn('rooms', 'quick', 'INTEGER NOT NULL DEFAULT 0'),
    ],
  },
  {
    // Problem reports and anonymous session numbers (public beta, 15 Nov 2026). Additive only, every step can run again, and the previous code runs on the
    // migrated database (it never names these tables or the `daily.reports` column, which has a default).
    //  - reports: what a player chose to send (core/reports.js); `pid` is there so that Delete My Online Account removes them; 30 days, 2,000, 64 MiB.
    //  - report_daily: reports stored per UTC day (count, bytes); session_ring: the last 500 session summaries (no profile id, no name, no address);
    //    session_daily: their sums per UTC day. The two daily tables are kept 90 days.
    //  - daily.reports: reports a player sent today (3 a day).
    id: 1003,
    name: 'problem_reports',
    sql: [
      `CREATE TABLE IF NOT EXISTS reports (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        pid INTEGER NOT NULL,
        created_ms INTEGER NOT NULL,
        app TEXT NOT NULL DEFAULT '',
        build TEXT NOT NULL DEFAULT '',
        bytes INTEGER NOT NULL,
        body TEXT NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS reports_pid ON reports(pid)`,
      `CREATE INDEX IF NOT EXISTS reports_created ON reports(created_ms)`,
      `CREATE TABLE IF NOT EXISTS report_daily (day INTEGER PRIMARY KEY, reports INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0)`,
      `CREATE TABLE IF NOT EXISTS session_ring (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        created_ms INTEGER NOT NULL,
        day INTEGER NOT NULL,
        dur INTEGER NOT NULL DEFAULT 0,
        delay INTEGER NOT NULL DEFAULT 0,
        resends INTEGER NOT NULL DEFAULT 0,
        drops INTEGER NOT NULL DEFAULT 0,
        starved INTEGER NOT NULL DEFAULT 0,
        err INTEGER NOT NULL DEFAULT 0
      )`,
      `CREATE TABLE IF NOT EXISTS session_daily (
        day INTEGER PRIMARY KEY,
        sessions INTEGER NOT NULL DEFAULT 0,
        errors INTEGER NOT NULL DEFAULT 0,
        dur_sum INTEGER NOT NULL DEFAULT 0,
        delay_sum INTEGER NOT NULL DEFAULT 0,
        resends_sum INTEGER NOT NULL DEFAULT 0,
        drops_sum INTEGER NOT NULL DEFAULT 0,
        starved_sum INTEGER NOT NULL DEFAULT 0
      )`,
      addColumn('daily', 'reports', 'INTEGER NOT NULL DEFAULT 0'),
    ],
  },
  {
    // (mpbeta27, 18 Nov 2026) the ranked / quick queue pairs only consoles of the same build (config queue.sameBuild, var QUEUE_SAME_BUILD): queue.build holds the
    // ticket's X-Msc-Build ('' = an older client that sent none, which pairs with anyone). Additive only, safe to run again (addColumn checks first), and the previous
    // code runs on the migrated database: it never names the column, and its INSERT leaves the default ''.
    id: 1004,
    name: 'queue_same_build',
    sql: [
      addColumn('queue', 'build', `TEXT NOT NULL DEFAULT ''`),
    ],
  },
  {
    // Wave 13 (22 Nov 2026), additive only, every step can run again, and the previous code runs on the migrated database (it never names these tables or
    // the `reports.kind` column, which has a default):
    //  - guess_misses: wrong room / friend code counters (core/guess.js). `k` is `a:<salted fingerprint of the address>` or `p:<profile id>`; never an address.
    //  - report_summary: one row per UTC day, written by the daily cron (core/reports.js `makeSummary`): counts by kind / build / app, no personal data.
    //  - reports.kind: what the stored report looks like (reports.js `kindOf`), so the summary groups without reading report bodies.
    id: 1005,
    name: 'guess_misses_report_summary',
    sql: [
      `CREATE TABLE IF NOT EXISTS guess_misses (
        k TEXT PRIMARY KEY,
        day INTEGER NOT NULL,
        win_ms INTEGER NOT NULL,
        n_win INTEGER NOT NULL,
        n_day INTEGER NOT NULL
      )`,
      `CREATE TABLE IF NOT EXISTS report_summary (day INTEGER PRIMARY KEY, made_ms INTEGER NOT NULL, body TEXT NOT NULL)`,
      addColumn('reports', 'kind', `TEXT NOT NULL DEFAULT ''`),
    ],
  },
  {
    // Wave 15: the username of every minted relay pass and whether it was revoked, so the passes still valid can be revoked when the measured hard
    // stop trips (core/turn_usage.js revokeValid). Additive; the previous code never names the columns. turn_mints is not exported.
    id: 1006,
    name: 'turn_pass_usernames',
    sql: [
      addColumn('turn_mints', 'username', "TEXT NOT NULL DEFAULT ''"),
      addColumn('turn_mints', 'revoked', 'INTEGER NOT NULL DEFAULT 0'),
    ],
  },
];

/** A migration step that adds a column only when the table does not have it yet (so a rerun cannot fail). */
function addColumn(table, column, decl) {
  return (sql) => {
    const cols = sql.exec(`PRAGMA table_info(${table})`).toArray().map((r) => r.name);
    if (!cols.includes(column)) sql.exec(`ALTER TABLE ${table} ADD COLUMN ${column} ${decl}`);
  };
}

/** Tables the admin export/import knows about (all have rowid). */
export const EXPORT_TABLES = ['profiles', 'rooms', 'queue', 'matches', 'results', 'scores', 'allowance', 'pair_stats', 'counters', 'settings', 'flags',
  'season_scores',
  'friend_links', 'friend_blocks', 'friend_status', 'friend_invites', 'report_daily', 'session_daily'];

/**
 * @param {{exec: Function}} sql SqlStorage-like (exec(query, ...bindings) -> cursor with toArray())
 * @param {number} now
 * @returns {number} schema version afterwards
 */
export function migrate(sql, now, tx = (fn) => fn()) {
  sql.exec(`CREATE TABLE IF NOT EXISTS _migrations (id INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_ms INTEGER NOT NULL)`);
  const done = new Set(sql.exec(`SELECT id FROM _migrations`).toArray().map((r) => r.id));
  let last = 0;
  for (const m of MIGRATIONS) {
    last = m.id;
    if (done.has(m.id)) continue;
    // one transaction per migration: it is applied completely or not at all (and then again at the next start)
    tx(() => {
      for (const stmt of m.sql) {
        if (typeof stmt === 'function') stmt(sql);
        else sql.exec(stmt);
      }
      sql.exec(`INSERT INTO _migrations (id, name, applied_ms) VALUES (?, ?, ?)`, m.id, m.name, now);
    });
  }
  return last;
}
