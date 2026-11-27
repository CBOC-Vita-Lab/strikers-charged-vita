# Strikers Charged Vita: online service

One Cloudflare Worker plus one Durable Object (SQLite). Sign-in, invite rooms / signaling, ranked queue, match records and
result checks, DWC-style leaderboard, TURN credential minting, admin backup. The wire format is in `PROTOCOL.md`.
No account ids, no secrets and nothing personal are in this folder; `wrangler.toml` carries a placeholder name.

```
src/worker.js        edge entry: answers unknown paths, wrong methods, oversized bodies and malformed Authorization / version headers itself (core/edge.js), forwards the rest to the single object
src/do.js            Durable Object (SQLite, alarm housekeeping)
src/core/publicboard.js, leaderboard_page.js   the public web leaderboard: `GET /v1/public/leaderboard` (JSON, no private fields, 60 s memo) and `GET /leaderboard` (one self-contained page)
src/core/*.js        the whole service as Request -> Response over an abstract SQL handle (pure JS, no platform imports)
src/sqlite-node.js   node:sqlite adapter used by tests and the fake
fake/server.mjs      local fake: same code, HTTP + HTTPS with a throw-away root certificate, clock / fault controls
scripts/             extract-hmac-key.mjs, backup.mjs, restore.mjs, estimate-writes.mjs (free-tier budget per match)
test/core            unit and integration tests on node:sqlite with an injected clock
test/worker          the same service inside workerd (miniflare) through the real Worker and Durable Object
```

## Develop and test (no account, no network service)

```sh
# Node >= 22.5 is required (node:sqlite for the tests and the fake)
cd server && npm install
npm test                              # both suites; `npm run test:core` is the fast one (about 5 s)
npm run fake                          # http://127.0.0.1:8787 and https://localhost:8788; the web leaderboard is http://127.0.0.1:8787/leaderboard
```
The fake writes its root to `fake/certs/root.pem` / `root.der` on first start (needs `openssl`); trust only that root in the client's host test.
`cp .dev.vars.example .dev.vars` and `npm run dev` runs the real Worker under `wrangler dev` (workerd) if you want the production runtime locally.

## Configuration

Plain variables (`[vars]` in `wrangler.toml`, or the environment of the fake). All optional.

| name | default | meaning |
|---|---|---|
| `KILL_SWITCH` | empty | comma list of `all register rooms ranked ranking turn friends report`; the admin API can also change it at runtime. `ranking` and `all` also switch off the public leaderboard data (the page stays up and says so) |
| `MAINTENANCE_MESSAGE` | empty | shown to players while something is switched off |
| `MIN_CLIENT_PROTO` | 1 | older wire protocol gets `426 upgrade_required` |
| `MIN_APP_VERSION` `LATEST_APP_VERSION` `REQUIRE_APP_VERSION` | empty / empty / off in code, **`1` in `wrangler.toml`** | game versions: below the minimum gets `426 update_required`; the latest is echoed in every error; `1` makes `X-Msc-App` mandatory (production sets it; the tests set `0` explicitly). A malformed `X-Msc-Proto` is always `400` |
| `SERVER_LIST_JSON` | `[]` | `[{"url":"https://...","priority":0}]` served by `/v1/servers` (https only) |
| `JOIN_ACK_SEC` `LINK_PING_SEC` `LINK_LOST_SEC` `NAT_LIFE_SEC` | 5 / 5 / 15 / 30 | session liveness timings (also published in `/v1/info`) |
| `PAIR_FAIL_LIMIT` `PAIR_FAIL_KEEP_DAYS` | 2 / 7 | failed punches per pair before steering to the relay |
| `ENDPOINT_CHECK` | 1 | 1: an endpoint call whose `public` / srflx / observed address is not the caller's address goes to the relay (`endpoint_unverified`); 0 = trust the claims (PROTOCOL.md, "Endpoint claims") |
| `EDGE_IP_BURST` `EDGE_IP_PER_SEC` `EDGE_IP_AGG_BURST` `EDGE_IP_AGG_PER_SEC` | 100 / 2, 300 / 15 | the Worker's best-effort token buckets in front of the object (a request with a player token: its own address+token bucket 100 / 2 and the address's aggregate 300 / 15, so ten consoles behind one NAT are not throttled together; no token: the per-address bucket 100 / 2; was 200 / 6 and 400 / 30 before 1.3 wave 9, which let one address spend a quarter of the day in minutes) (`src/worker.js`, `core/edge_bucket.js`: one isolate's memory, so a flood over many isolates gets through in part; IPv6 by /64; `EDGE_IP_BURST=0` = off) |
| `DAILY_REQUEST_BUDGET` | 45000 | **this service's share** of the account's 100,000 requests a day (the Galaxy service has its own number; keep the two summing to at most about 90,000). 0 = guard off. See "Free-tier budget" |
| `DAILY_ROWS_BUDGET` | 50000 | **this service's share of the account's 100,000 SQLite rows written a day** (the limit that binds first, see "Rows written" below; Galaxy gets the other 50,000). Counted per statement from the platform's `rowsWritten`; the larger of the request share and the rows share sets the level and `pollScale` (same `BUDGET_*` / `LIVE_RESERVE_PCT` percentages). 0 = rows not guarded |
| `BUDGET_PACE_PCT` `BUDGET_BROWSE_PCT` `LIVE_RESERVE_PCT` `POLL_SCALE_MAX` | 50 / 75 / 20 / 1.4 | share of the budget at which polls are stretched (`pollScale` 1 to the max), browsing is refused, and new sessions are refused (100 minus the reserve; the reserve keeps matches in progress working) |
| `FRIENDS_MAX` `FRIENDS_INCOMING_MAX` `FRIENDS_BLOCKS_MAX` `FRIEND_ADDS_PER_DAY` `FRIEND_INVITE_SEC` `FRIEND_ONLINE_SEC` | 64 / 50 / 100 / 40 / 600 / 90 | friends + requests sent per account (64 = the game's list; 0 turns the friend routes off), requests received kept, blocked players, new requests per account per day, seconds an invite lives, seconds without a request before a friend shows offline |
| `ACCOUNT_DAY_REQUESTS` `ADDR_OPEN_DAY` `ADDR_DAY_REQUESTS` `ADDR_BAD_DAY` | 9000 / 1000 / 30000 / 600 | requests per account, per address on the open routes, per address in all, and per address that fail authentication (wrong or missing token), per UTC day; over it: 429 `daily_cap` (0 = off). The Worker then answers that address (and, for `ADDR_BAD_DAY`, only tokens it has not seen succeed) itself |
| `HOLD_MAX_SEC` `HOLD_QUEUE_TICK_SEC` | 55 / 2 | held polls (PROTOCOL.md "Held polls"): the longest a poll may wait inside the object, and how often a held queue poll looks for a partner. `HOLD_MAX_SEC=0` turns holding off (the rollback lever: every poll is answered at once, as before) |
| `PRUNE_SILENT_DAYS` | 3 | sign-ups that never did anything (last seen within 5 minutes of creating the account, never played) are removed after this many days, and are the first to go when the table is full. Players who ever played are never removed; 0 = never |
| `RANK_PUT_POLICY` | `credit` | `credit` = a score only rises by what verified matches earned; `delta` = at most +`RANK_MAX_DELTA` (25) per put; `open` = anything in range |
| `RANK_REGION_MODE` | `pool` | `pool` = one board for all regions; `mask` = honour the client's region mask |
| `RANK_REQUIRE_HMAC` / `RANK_CHECK_NAME` | on | set `0` to relax the digest / blob-name checks (debugging only) |
| `ROOM_OPEN_TTL_SEC` `ROOM_JOINED_TTL_SEC` `ROOM_STARTED_TTL_SEC` | 600 / 1200 / 10800 | room lifetimes |
| `PUNCH_INTERVAL_MS` `PUNCH_TRIES` `PUNCH_LINGER_MS` `PUNCH_DELAY_MS` | 700 / 7 / 5000 / 1000 | hints sent to clients (GameSpy natneg values) |
| `QUEUE_BASE_WINDOW` `QUEUE_WINDOW_STEP` `QUEUE_WINDOW_STEP_SEC` `QUEUE_MAX_WINDOW` `QUEUE_REGION_STRICT_SEC` | 100 / 50 / 5 / 1000 / 20 | skill window; another region pairs only after the strict seconds |
| `QUEUE_SAME_BUILD` | 1 | (mpbeta27) the ranked and quick queues pair only tickets of the same `X-Msc-Build` (the game's link fingerprint); a ticket without one pairs with anyone. 0 = any builds (the 1.3.0 rule). Column `queue.build`, migration 1004 |
| `QUEUE_REGION_SLACK` | 100 | after the strict seconds, same region wins a tie: a quick match takes the oldest waiter of the caller's region before an older one from another; a ranked match counts a rating gap in another region this many points larger (0 = region ignored, the old rule). Uses only the profile's `region`: no coordinates, nothing from `request.cf` |
| `RESULT_WINDOW_SEC` | 120 | how long the second player has to report |
| `FORFEIT_POINTS` `RESTORE_CREDIT` `ELO_K` `ABANDON_COOLDOWN_SEC` `ABANDON_COOLDOWN_MAX_SEC` | 10 / 5 / 24 / 60 / 900 | disconnect penalty rule |
| `MIN_MATCH_SEC` | 60 | a normal finish (result 0) sooner than this after both sides started is refused (429 `too_early`). The game's shortest timed match is 120 s (`sGameTimeOptions` 120/180/240/300, default 180); only a 3-goal limit can end sooner, so the default is half the shortest timed match. 0 = off |
| `RATED_PER_PAIR_DAY` `RATED_PER_ACCOUNT_DAY` | 4 / 30 | rated (verified or forfeit-win) ranked matches per UTC day; 0 = no cap |
| `ONE_OPP_MIN_WINS` `ONE_OPP_SHARE_PCT` `ONE_OPP_DAYS` | 8 / 70 / 30 | win-trading flag: that many rated wins with that share against one opponent in that many days (flag only, no ban) |
| `DODGE_WINDOW` `DODGE_MIN_SAMPLE` `DODGE_RATE_PCT` `DODGE_PENALTY_PCT` | 20 / 6 / 35 / 75 | dodge rule (PROTOCOL.md section 6): blamed disputes / no-contests over the window, and the share of an abandon's rating loss charged |
| `TURN_PROVIDER` | `cloudflare` | `cloudflare`, `stub` (fake credentials) or empty (off) |
| `TURN_TTL_SEC` `TURN_MAX_CREDS_PER_DAY` `TURN_MAX_CREDS_PER_PLAYER_DAY` `TURN_MONTHLY_BUDGET_MB` | 3600 / 300 / 6 / 100000 | TURN budget and breaker (the estimate guard: credentials and client-reported megabytes) |
| `TURN_MAX_CREDS_PER_ROOM_HOUR` `TURN_MAX_CREDS_PER_ADDR_DAY` `TURN_UNNEEDED_SHARE_PCT` | 6 / 24 / 50 | Relay credentials per room per clock hour, per source address per UTC day (counted under a daily salted fingerprint, never the address), and the share of the day (percent of `TURN_MAX_CREDS_PER_DAY`) for credentials without a visible reason (no relay steer, relay claim or failed punch). 0 turns a cap off, 100 turns the share off. While the measured hard stop is over budget the passes still valid are also revoked (at most 25 a minute) |
| `TURN_REAL_BUDGET_GB` `TURN_USAGE_GUARD` `TURN_USAGE_SCOPE` `TURN_USAGE_CACHE_SEC` `TURN_USAGE_MAX_AGE_SEC` `TURN_USAGE_REQUIRED` `TURN_USAGE_TIMEOUT_MS` | 300 / 1 / key / 300 / 21600 / 0 / 5000 | the measured TURN hard stop ("TURN hard stop" below): this service's share in GB of the account's monthly egress, its kill switch (0), key or account scope, seconds a reading is cached, the age after which a reading counts as missing, strict mode (1: no good reading = no new credentials), the read's timeout |
| `GUESS_WINDOW_SEC` `GUESS_ADDR_WINDOW` `GUESS_ADDR_DAY` `GUESS_ACCT_WINDOW` `GUESS_ACCT_DAY` | 600 / 30 / 200 / 10 / 40 | wrong room / friend code limits ("Wrong codes" below); 0 turns one off |
| `PACE_HINTS` `PACE_ROOM_WAIT_SEC` `PACE_STARTED_WAIT_SEC` `PACE_QUEUE_WAIT_SEC` `PACE_FRIENDS_WAIT_SEC` `PACE_ROOM_GAP_MS` `PACE_MATCH_POLL_MS` | 1 / 20 / 45 / 30 / 45 / 1700 / 2000 | the polling rhythm the service dictates to clients that ask (PROTOCOL.md "Pacing hints") |
| `TURN_NEW_ACCOUNT_HOURS` `TURN_NEW_ACCOUNT_DAILY` | 24 / 60 | accounts younger than this share at most that many of the daily credentials |
| `TURN_MAX_KBPS` `TURN_REPORT_GRACE_SEC` `TURN_MAX_REPORTS_PER_PLAYER_DAY` `TURN_MAX_REPORT_MB` | 128 / 300 / 12 / 200 | usage reports: clamp per credential (TTL x kbit/s), grace after expiry, reports per player per day, hard per-report ceiling |
| `ADMIN_TOKEN_MIN` | 0 | the shortest `ADMIN_TOKEN` that turns the admin routes on; a shorter one leaves them off (404, as if absent). 0 = no minimum, as before. The Galaxy service uses 24; set it with the deploy when the token is long enough |
| `MAX_PLAYERS` `RENAME_COOLDOWN_HOURS` `NAME_BLOCKLIST_EXTRA` `STUN_SERVERS` | 200000 / 24 / empty / stun.cloudflare.com:3478 | `MAX_PLAYERS`: see "Free-tier capacity" below |
| `PRUNE_IDLE_DAYS` | 180 | the alarm removes accounts that **never played**: no match ever started (unranked included), no win, loss, draw, abandon, verified point, score row or archived season, no ban, and not seen for this many days (at most 200 a sweep); 0 = never. Anyone who played is never removed, however long they are away |
| `ARCHIVE_TOP` | 100 | best rows kept per past season in the archive (`board=archive`) |
| `REGISTER_IP_BURST` `REGISTER_IP_PER_HOUR` `REGISTER_MAX_PER_DAY` `REGISTER_BAD_PER_HOUR` | 10 / 3 / 1000 / 30 | registration limits: per address (IPv6 per /64) a burst, then a slow rate; the whole service per day; bad requests per address per hour (they never touch the other two) |
| `REGISTER_POW_BITS` | 0 (off) | proof of work on register: leading zero bits of SHA-256 (PROTOCOL.md section 3); 16 is about 65k hashes. Turn on only after the client can solve it |
| `ROOMS_PER_ACCOUNT_DAY` `MATCHES_PER_ACCOUNT_DAY` | 60 / 60 | rooms created and match records per account per UTC day (stored); 0 = no cap |

Secrets (never in files; `wrangler secret put`): `RANK_HMAC_KEY_HEX` (the ranking digest key, 64 hex), `ADMIN_TOKEN`, `TURN_KEY_ID`, `TURN_KEY_API_TOKEN`.
The ranking key is the game's `sRankingHmacKey`; `node scripts/extract-hmac-key.mjs` prints it from the decomp source. It is not logged anywhere by the service
and never appears in a response. (It also ships inside every copy of the game, so the 0x70 HMAC is an integrity check and **not authentication**: it only keeps honest clients honest and anybody can forge a valid digest. The real protections are the
credit rule, both-sides result agreement, the minimum match length, daily caps, flags and rate limits.)

## Free-tier capacity (1.3; the arithmetic behind `MAX_PLAYERS` and `REGISTER_MAX_PER_DAY`)

Cloudflare's documented Workers Free limits for Durable Objects (developers.cloudflare.com/durable-objects/platform/pricing and .../limits, read
6 Nov 2026): 100,000 requests a day, 100,000 SQLite rows written a day, 5,000,000 rows read a day, 5 GB stored per account, and 1 GB per object on
the Free plan (one place on the limits page says 10 GB; take the smaller). Every index entry a write touches counts as one more row written.
* **Storage per player** (measured with this schema in node:sqlite, 20,000 synthetic players): a profile with its unique indexes about 265 bytes;
  an active player's three score rows and five credit rows about 885 bytes more: **about 1.15 KB per active player**. 200,000 players is about
  230 MB; with 90 days of match records at the write-bound rate below (about 1,000 ranked matches a day, a few hundred bytes each) and the season
  archive (100 rows a week) it stays under 1 GB with more than 3x headroom. So **`MAX_PLAYERS` = 200,000** (was 20,000).
* **Writes.** `node scripts/estimate-writes.mjs`: a ranked match costs about 40 requests and 98 rows written (1.3 adds about 5: the played counters,
  points / last played on a verified match and its index row), an invite match 15 / 33 (+2), a registration 4 rows (+1 for the all-time index).
  **`REGISTER_MAX_PER_DAY` = 1,000** costs at most 5,000 rows a day, 5 % of the write budget, which leaves room for about 950 ranked matches a day;
  at that rate the never-played pruning (180 days) keeps at most about 180,000 dormant sign-ups, under `MAX_PLAYERS`.
* **Reads are the real ceiling, not players.** A NEAR / ORDER ranking request counts the rows above the player on the current board (`rankOf`),
  so it reads about as many rows as there are players on that board; at 5,000,000 rows read a day, a board of N active players allows roughly
  5,000,000 / (3 N) such requests a day (N = 2,000: about 800). Registered players who do not play cost no reads. Watch "rows read" in the
  dashboard first; a paid plan or a cached rank is the fix if a weekly board ever holds thousands of players.

## Deploying (what the owner has to do; nothing here has been run)

1. **Cloudflare account.** Free plan is enough (Durable Objects with SQLite and Workers are on the free plan). Use a neutral account name.
   In the dashboard open *Workers & Pages* once so the account gets its `workers.dev` subdomain (choose a neutral one).
2. **Tooling.** `cd server && npm ci` (Node >= 22.5).
3. **Log in.** `npx wrangler login` (browser) or, headless, create an API token with the *Edit Cloudflare Workers* template and
   `export CLOUDFLARE_API_TOKEN=... CLOUDFLARE_ACCOUNT_ID=...`. Do not commit the ids.
4. **Name.** Edit `name = "msc-online-example"` in `wrangler.toml` to the final neutral worker name. The URL will be `https://<name>.<subdomain>.workers.dev`.
5. **TURN key.** Dashboard -> *Realtime* -> *TURN* -> create a key. Keep the *Key ID* and *API token*. (Skip to run without relay: set `TURN_PROVIDER = ""`.)
6. **First deploy.** `npx wrangler deploy`. This creates the Worker and applies the Durable Object migration `v1` (`new_sqlite_classes`).
7. **Secrets.**
   ```sh
   node scripts/extract-hmac-key.mjs | npx wrangler secret put RANK_HMAC_KEY_HEX
   openssl rand -base64 32 | tr '+/' '-_' | tr -d '=\n' | tee /dev/stderr | npx wrangler secret put ADMIN_TOKEN   # note the printed value in a password manager
   npx wrangler secret put TURN_KEY_ID
   npx wrangler secret put TURN_KEY_API_TOKEN
   ```
   Secrets are live immediately (a secret change makes a new deployment of the same code).
8. **Smoke test.**
   ```sh
   curl -s https://<url>/v1/info
   curl -s https://<url>/v1/time
   ADMIN_TOKEN=... curl -s -H "Authorization: Bearer $ADMIN_TOKEN" https://<url>/v1/admin/stats
   ```
9. **TLS roots for the Vita.** `openssl s_client -connect <name>.<subdomain>.workers.dev:443 -showcerts </dev/null` and note which root signs the chain (Cloudflare's
   `workers.dev` certificates are issued by Google Trust Services or Let's Encrypt and the issuer can change over time). Bundle ISRG Root X1/X2 and GTS Root R1-R4 (plus the
   cross-signing GlobalSign root if the chain shows it) in the client's own root store, and check the tool's TLS test against the live host before shipping.
10. **Give the client worker the URL** (build-time constant, e.g. `MSC_ONLINE_HOST`).
11. **Backups.** `ADMIN_TOKEN=... node scripts/backup.mjs https://<url> > backup-$(date +%F).json` (weekly is plenty; the file holds token hashes, keep it private).
    Restore: `ADMIN_TOKEN=... node scripts/restore.mjs https://<url> < backup.json`.
12. **Emergency stop** without a redeploy: `curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -d '{"kill":["ranked","turn"],"message":"Back soon"}' https://<url>/v1/admin/switches`
    (`["all"]` stops everything but `/info` and `/time`; `[]` clears). Roll back code with `npx wrangler rollback`. Never edit a shipped migration in `src/core/schema.js`; append a new one.
13. **Cost guards.** Platform logging is off (`[observability]`). The free-plan quotas and what a match costs are in "Free-tier budget" below. The Worker answers junk itself (unknown paths,
    wrong methods, oversized bodies, malformed `Authorization` / version headers) so it never spends a Durable Object request; per-account daily caps bound rooms and match records.
    TURN is guarded by `TURN_MAX_CREDS_*` and `TURN_MONTHLY_BUDGET_MB`; also set a billing alert / usage notification in the dashboard.

## The shared zone psvita.dev (custom domain strikers.psvita.dev)

The custom domain `strikers.psvita.dev` is attached to this Worker by hand in the dashboard (Settings -> Domains & Routes); `wrangler.toml` deliberately declares no `routes` (see the comment there), so `wrangler deploy` never changes it. The Worker answers on both names (with `workers_dev = true`) with the same data (an account made on one works on the other).
The zone `psvita.dev` is shared with the other online service, so some settings are zone-wide and are not ours to change:

- **Bot Fight Mode must stay OFF** (zone-wide; it challenges the games' API calls). Read it, never change it.
- **Browser Integrity Check** applies on a zone (it never did on workers.dev). The game's User-Agent is `StrikersVita`; check that a harmless GET with it (`/v1/time`) answers 200.
- **ONE rate-limiting rule per zone** (Free plan), shared: created by the other service's zone script as `psvita-shared-ratelimit` on `/v1/*` and `/times` (100 requests per 10 s per colo + address, 10 s block).
  We add **no** rate-limiting rule. Every path this Worker serves is under `/v1/`, except one: `GET /leaderboard` (the web page). Give `/leaderboard` to that script's `--ratelimit-extra-paths` if the page should be covered too
  (the Worker already limits it by address and serves it from a memo; it is optional). Moving it is not worth it: the page is linked from the changelog and the game never calls it.
- **WAF custom rules** (5 per zone, the other service uses 1, scoped to its own host): we may add one, scoped to our host and never touching a rule we do not own: block `http.host eq "strikers.psvita.dev"` for every path that is neither under `/v1/` nor `/leaderboard`.
- **Budget:** `DAILY_REQUEST_BUDGET = "45000"` stays; the account's 100,000 requests a day are shared with the other service (45,000 + 45,000). Tell the owner before raising it.
- Keep the workers.dev address on until the owner's hardware test passes. The game (strikers.psvita.dev first, workers.dev as the fallback) is in docs/ONLINE-TLS.md.

**TURN exhaustion, worked through.** Three things can be abused: the daily credential count, the monthly byte counter and the player's patience.
*Credential count:* the old 10 per player and 300 per day meant 30 accounts (about 10 hours of registrations from one address at 3/h, minutes for a few addresses) used up the whole day and the relay was
gone for everyone. Now a player can mint 6 a day (a credential lasts an hour and one is enough for a room), fresh accounts (< 24 h) share a sub-budget of 60, so 10 fresh accounts can
at most use their own slice and established players keep the other 240; exhausting those needs 40 accounts that each waited a day, and the admin sees them. The kill switch (`turn`) and a ban
remain the emergency tools. *Byte counter:* usage reports used to be accepted from anybody, up to 200 MB per report, 30 reports per 10 minutes, so one account could trip the 100 GB monthly breaker in under
three hours. Now a report needs a live credential of the reporter, is clamped to TTL x 128 kbit/s (57.6 MB), a credential carries that total only once, and 12 reports a day: the worst case per account and day is
6 x 57.6 MB, i.e. 100 accounts could add 35 GB a day at most, and the number can never exceed what the credentials the server minted could physically carry. The real relay usage is in the Cloudflare dashboard; set a usage alert there.

## Hardening ported from the Galaxy review (wave 5)

The same code base serves Galaxy (branch mp-int, `GAME=smg`), whose two reviews found these; they apply to this service as well and are in `server/` now.
- **Request bodies.** A `POST` / `PUT` without a numeric `Content-Length` (a chunked upload) is answered **411** `length_required` at the edge; the announced length is capped (8 KiB); the object reads the body stream with the same hard cap
  in **bytes** (`readBodyCapped`: a lying `Content-Length` cannot get a big body past it, and 5,000 two-byte characters no longer count as 5,000 bytes). The game's HTTPS client always sends `Content-Length` with a body and none without one, so it is unaffected.
- **Edge token bucket.** `src/worker.js` runs a per-address limiter (a request with a player token: its own address+token bucket, 200 burst and 6 a second, plus the address's aggregate, 400 and 30 a second; no token: 200 and 6 for the address; `EDGE_IP_BURST` / `EDGE_IP_PER_SEC` / `EDGE_IP_AGG_BURST` / `EDGE_IP_AGG_PER_SEC`) before it wakes the object, so one address hammering the service costs Worker requests but not Durable Object requests.
  **Best effort**: the table is in the isolate's memory and a flood spread over isolates gets through in part; the object's own limits stay behind it.
- **Public leaderboard memo.** A memo hit in the Worker is served only if the `ranking` / `all` switch is not on (the isolate re-reads the object's switches at most every 60 s and at once after an admin change through this Worker) and a per-address bucket (30 a minute, the object's number) lets it through.
  Before, a memoised board kept being served for up to a minute after the switch was thrown, and the per-address limit did not apply to hits.
- **Endpoint claims** (`ENDPOINT_CHECK`, PROTOCOL.md "Endpoint claims"): a member cannot hand its partner somebody else's address any more; LAN addresses go only to a partner behind the same public address.
- **Idle-account scan.** At most hourly, on an index of `profiles.last_seen_ms` (migration 1001), instead of a scan of every profile at each 60 s sweep: this is the one query that grows with the number of players, and at 200,000 accounts a scan per minute would have used the free plan's row reads.
- **500 answers** carry `latestVersion` like every other error. **`scripts/restore.mjs`** restores every table the backup holds (it used to skip `flags`, `season_scores` and `pair_stats`).
- **Taken in wave 10:** friends (requests, presence, played with, blocks, invites, the game's own status string) and the unranked quick match (PROTOCOL.md sections 5 and 13). **Not taken from Galaxy:** star times, rooms of four, the Galaxy route table and limits (`GAME=smg`); the slower started-room poll (this client already polls a started room every 4 s and the budget is bound by rows written, not requests: 8 s would need `LINK_PING_SEC` 12 and `LINK_LOST_SEC` 20, both already variables).
- **Migration ids.** Strikers-only steps start at **1001**. Galaxy's branch uses 4, 5 and 6 for its own tables; migrations are matched by id, so never add a Strikers step as 4 to 6.

## Public web leaderboard

`https://<url>/leaderboard` shows the game's SEASON LEADERS / DAILY LEADERS tables, ALL-TIME LEADERS and PAST SEASONS to anybody with a browser; it reads `GET /v1/public/leaderboard?board=season|daily|alltime|archive&limit=50`
(PROTOCOL.md section 8). Display name, win / loss record and points only: no player code, id, token, region or address. Banned accounts are left out. No new variable,
secret or migration: it reads the existing `scores` and `profiles` tables. `GET /leaderboard` is answered by the Worker without waking the object; the data is memoised for 60 s in the
object and in the Worker, so a page left open in many browsers costs about one object request a minute per isolate (each open page refreshes once a minute while visible), and a
cache miss is one indexed query (`LIMIT` 50) plus a join of those rows to `profiles`. Rate limit: 30 data requests a minute per address. After a ban or an account deletion the board
changes within a minute (the object's memo is cleared at once; the Worker's copy expires). Fan-project footer only; nothing identifies the owner.

## Free-tier budget

Cloudflare's numbers as read on 30 Oct 2026 (they change: re-check before relying on them):

| Free plan | Limit | Source |
|---|---|---|
| Workers requests | 100,000 per day, resets 00:00 UTC (over it: error 1027) | <https://developers.cloudflare.com/workers/platform/limits/> |
| Workers CPU time | 10 ms per HTTP request; 50 subrequests per request | same page |
| Durable Objects (SQLite) requests | 100,000 per day; HTTP requests, RPC sessions, alarm invocations and WebSocket messages (20:1) all count | <https://developers.cloudflare.com/durable-objects/platform/pricing/> |
| Durable Objects duration | 13,000 GB-s per day (one object active all day at 128 MB is about 11,000) | same page |
| SQLite rows read | 5 million per day | same page |
| SQLite rows written | **100,000 per day**; deletes count, `setAlarm()` is one row | same page |
| SQLite storage | 5 GB per account | same page; per-object limits and the 1,000 requests/second soft limit: <https://developers.cloudflare.com/durable-objects/platform/limits/> |
| How rows are counted | an INSERT / UPDATE / DELETE counts the row, plus one more row for each index the write touches; failed statements are not documented | <https://developers.cloudflare.com/d1/platform/pricing/> (the Durable Objects page says it matches D1) |

Every client request that reaches the service uses one Worker request **and** one Durable Object request, so the two daily quotas of 100,000 are spent together; a request the Worker rejects
at the edge uses only the Worker's. When a quota is used up the platform fails further operations of that kind until 00:00 UTC.

**Write budget per match** (`node scripts/estimate-writes.mjs` replays a typical match through the real code and applies the row rules above; an upper bound, because upserts are counted as inserts):

| | requests | rows written |
|---|---|---|
| ranked match (queue 10 s, 3 min play, polls at the documented rates, results, 2 x put, 2 x get per player) | ~34 + ~6 alarm | ~92 + ~6 `setAlarm` = **~98** |
| invite match (create, join, endpoints, punch, started, leave) | ~15 | ~33 |
| registration | 1 | 4 |

So the binding limit is **rows written: about 1,000 ranked matches a day** (about 2,500 by requests), or roughly 3,000 invite matches. The caps keep one account from using a
disproportionate share: 60 rooms and 60 match records a day, 30 rated matches, 12 TURN reports. Rows read (5 million) is not a constraint at this scale; the one query that grows with the crowd is
the matchmaker's scan of up to 200 queue rows per poll, so a very long queue is the first thing to watch in the dashboard.

### Rows written (1.3 wave 11): the limit that binds

Rows written, not requests, bind first (about 1,000 ranked matches a day against about 2,500 by requests), and neither this service nor Galaxy counted them. Split of the account's 100,000 rows a day:
**50,000 for this service (`DAILY_ROWS_BUDGET`), 50,000 for Galaxy**; the two request shares stay 45,000 + 45,000 (about 90,000, the rest is the platform's own bookkeeping). Change the numbers together with Galaxy's.
* **Counted per write path.** Every statement of the application goes through one wrapper (`src/core/app.js`, `db`), which adds the cursor's `rowsWritten` (the platform's figure, index rows included; the node test adapter applies the same rule as `scripts/estimate-writes.mjs`) to the budget. The object adds the one row of each `setAlarm()` itself (`src/do.js`). Migrations run before the count starts.
* **Degrades like the requests.** The level and `pollScale` use the larger of the two shares: 50 % of the rows: polls stretched to 1.4; 75 %: rankings, board, match history, friends list refused (`503 over_budget`, `x-msc-budget: browse-over`); 80 %: no new sessions; the last 20 % is the reserve for matches in progress; 100 %: everything but `/admin`. The Worker learns the level from `x-msc-budget` as before and answers the refused classes itself.
* **Log.** One line `[budget] day N: requests U/D (x %), rows written R/B (y %), level L, pollScale S` each time either share first passes 50 / 75 / 80 / 100 % of a day (no ids; `wrangler tail` shows it, platform logging is off). `/v1/admin/stats` `budget` carries `rows`, `rowsBudget`, `rowsSharePct`, `rowsGuessed` and `billed` (requests that reached the object, refused ones included).
* **Persistence.** The `settings` row `budget` keeps its format `day:requests` (so a rollback to the previous code still reads it); the rows are in their own row `budget_rows` (`day:rows`). Both are written every 200 requests or 400 rows; a restarted object adds 200 requests and 400 rows of margin. With no `budget_rows` of today (a deploy in the middle of a day) the rows are guessed as one per request already counted (`rowsGuessed: true`); deploying near 00:00 UTC avoids the guess.
* **Request ceiling of this service.** `over` begins at exactly 100 % of the 45,000 (there is no factor above it; Galaxy's default was 125 %). What can pass it: `/admin` calls, alarms (at most 1,440 a day, one a minute, counted like any request), 200 per restart of the object, and what reaches the object before the Worker has seen the level (the Worker then answers every class itself for up to 15 minutes); `billed` shows used + refused. So the worst case is about 45,000 plus a few thousand at most only in a day of many restarts; the sum with Galaxy's 50,000 stays under 100,000.
* **Estimate, not a bill.** The figure is the platform's own `rowsWritten` per statement, so it should match the dashboard closely; compare `rows` in `/v1/admin/stats` with "rows written" in the dashboard after the first busy day and move the share if they differ.

### Request budget, measured against the code (1.3 wave 9)


**Friends (wave 10, estimator, a model not a measurement).** An open friends list is one held poll per 45 s: 80 requests per player-hour with a quiet list, 92 when a friend changes presence every 2 minutes (each change answers the held poll at once). It runs only while the online menu is open, so in the model hour (20 menu minutes) it adds 27 to 31 requests per player-hour (MIX 198 to about 227: roughly 430 player-hours a day on the whole account instead of 492). Friend operations (add, block, invite, status) are single small requests; presence needs none.
The 100,000 requests a day are **per account and shared with the Galaxy service**, so each service gets half: `DAILY_REQUEST_BUDGET` (45,000 here) keeps the sum
below the limit with the platform's own bookkeeping to spare. `node scripts/estimate-requests.mjs` runs simulated consoles in virtual time against the real
`createApp` and counts requests per player-hour for each state (every figure is a host estimate from an assumed hour: 10 min idle in the menu, 6 min browsing the board, 8 min in the ranked queue,
4 min waiting in an invite room, 28 min in matches, 6 matches):

| requests per player-hour | before (plain polls, 1.3 as first built) | after (held polls) |
|---|---|---|
| online menu, idle | 2 | 2 |
| browsing the board | 240 | 240 |
| ranked queue | 1,800 | 293 |
| invite room (host) | 2,267 | 440 |
| a match (28 min) | 903 | 95 |
| **mixed hour** | **934** | **198** |
| player-hours a day, whole account | 104 | 492 |
| player-hours a day at a 50/50 split with the Galaxy service | 52 | 246 |

How: a poll that has nothing to report waits inside the object (no CPU, no row) up to 12 s (45 s once a match is started) and answers at once when something changes; the 60 s alarm and
every request count against the budget; the object stretches polls by `pollScale` (up to 1.4) above 50 % of the budget, refuses browsing at 75 % and new sessions at 80 % (the last 20 % is for matches in progress)
and everything at 100 %, and the Worker then answers those classes itself. A console older than this change keeps its plain polls (about 934 an hour) and is unaffected by the rest. Per-address and per-account daily
ceilings keep one source from spending the day. Rows written: a held queue entry is written at most every 8 s. The real figures need the live service's dashboard (not available to this work):
read `/v1/admin/stats` (`budget`) after the first busy evening.

Maintenance notes: the object runs a 60 s housekeeping alarm only while rooms, queue entries or unsettled matches exist. Account deletion (`DELETE /v1/me`) and bans are in the API.
Match records are kept 90 days. Peer addresses live only in `rooms` rows and disappear a few minutes after a room ends.

## Problem reports and Delete My Online Account (public beta)
`POST /v1/report`, `POST` / `DELETE /v1/account`, the `X-Msc-Stats` header on leave and the admin routes `reports`, `reports/{id}`, `diag` are in PROTOCOL.md section 14; the code is `src/core/reports.js` (migration 1003, additive: five new tables and `daily.reports`; the previous code runs on the migrated database, tests/rollback in `test/core/reports.test.js`).
Limits: 48 KiB a report (only this route; the rest keep 8 KiB), 3 a player a UTC day, 30 days, 2,000 reports and 64 MiB (oldest dropped first). Cost: a report is about 3-5 rows written (the row, its two index rows, the player's daily count, the day's totals) and one request; a session summary on leave about 4 rows. Both are counted by the existing request and rows budgets, and a report is the first thing refused when the day gets busy (from 75 % of either budget). Reading them: `curl -H "Authorization: Bearer $ADMIN_TOKEN" https://<url>/v1/admin/reports`, then `/v1/admin/reports/<id>`; the daily totals: `/v1/admin/diag`. Workers Logs are on with invocation logs off.
Tests: `npx vitest run` (reports.test.js: limits, size cap, purge on delete, budget refusal, rollback); `node scripts/mutants.mjs report` and `session` / `delete` (33 mutants, all killed).

## Wave 13 (22 Nov 2026): TURN hard stop, wrong codes, pacing, daily report summary

### TURN hard stop (measured)
The estimate guard (`turn.js`: credentials a day, megabytes clients report) cannot see an abuser, who reports nothing (VitAdhoc security review H2). `core/turn_usage.js` reads what Cloudflare itself counted: the egress bytes of
the TURN key for the UTC month, from the GraphQL analytics API (`https://api.cloudflare.com/client/v4/graphql`, dataset `callsTurnUsageAdaptiveGroups`, field `egressBytes`, filtered by the key and `date_geq` the 1st of
the month). `POST /v1/turn` first brings that reading up to date (at most one read per `TURN_USAGE_CACHE_SEC`, 300 s, shared by every request; a failed read is retried after 60 s) and refuses with `503 turn_budget`
(`reason: usage`) once it is at or above `TURN_REAL_BUDGET_GB`. The estimate guard stays in force as the second line, and the `turn` kill switch still stops everything.
* **Secrets** (`wrangler secret put`, never in a file): `TURN_USAGE_API_TOKEN` (a separate token with the one permission **Account Analytics: Read**) and `TURN_USAGE_ACCOUNT_ID` (the account tag). Without either the measured guard is simply not running (the status says `configured:false`).
* **Fail safe.** A failed read (HTTP error, GraphQL `errors`, an answer without the expected path, a non-numeric or negative sum, bad JSON, network error, 5 s timeout) is recorded (`measured.lastError`, a short code, one log line) and never read as zero. It never lifts a block (the last good reading of the month stays) and never opens a door the estimate guard keeps shut. `TURN_USAGE_REQUIRED=1` is the strict mode: no good reading this month, or none for `TURN_USAGE_MAX_AGE_SEC` (6 h), means no new credentials. Default 0: with no reading the estimate guard decides, as before this change.
* **The query shape has not been tried against the live API** (written from Cloudflare's documentation; the builders have no network). One empty answer ("no groups") counts as zero used, so a wrong filter could read zero silently: compare `GET /v1/admin/stats` `turn.measured.usedBytes` (or `POST /v1/admin/turn-usage`, which forces a read) with the dashboard's TURN usage after the first relayed match, and again after the first day. If they differ, `TURN_USAGE_SCOPE=account` (every key of the account, no key filter) is the first thing to try.
* **One shared budget.** Cloudflare gives the account 1,000 GB of TURN egress a month, free; Galaxy, VitAdhoc and this service use the same account. Each service's `TURN_REAL_BUDGET_GB` is its share and **the three must agree**: with the default `key` scope each service measures its own TURN key, so the shares must add up to at most about 900 GB (this service 300, Galaxy 300, VitAdhoc 300; a 10 % margin for the five minutes a reading lags). If the three use one TURN key, use `account` scope everywhere with the whole budget (900) in each. Change the three numbers together.
* **Kill switch.** `TURN_USAGE_GUARD=0` (a variable, no redeploy of code) turns the measured guard off; `TURN_REAL_BUDGET_GB=0` too. The log carries one line each time the month's use first passes 50 / 75 / 90 / 100 % of the budget (`[turn] measured relay egress 60.0 GB of 300 GB (20 %)`): no token, account, key id or response text.
* Tests: `test/core/turn-usage.test.js` (mocked GraphQL endpoint, every failure shape, caching, in-flight sharing, rebuilt object, kill switches, privacy of the log).

### Wrong codes
`core/guess.js`: room codes (six characters) and friend codes (eight) that lead nowhere are counted per address and per account, in storage (table `guess_misses`), so the limits survive the object leaving memory (the in-memory
buckets of the rest of the service do not; VitAdhoc review M1). Per account 10 misses in 10 minutes and 40 a UTC day; per address 30 and 200 (higher, so a meet-up or a carrier NAT sharing one address is not locked out for the day), then `429 rate_limited` (`reason: code_guesses`) before the code is looked at. The address itself is not stored: only the first 16 hex digits of SHA-256(daily salt + address key). The salt lives in the same database, so this is not secrecy against someone who holds it; it is deleted with the day's rows (renewed at the first use of each UTC day, when the rows of earlier days go); the account's counter goes with the account. A request costs
one read of two rows; a miss two row writes (none once limited). Not covered: `GET /v1/rooms/{code}` for a code you are not in (403 `not_member` vs 404) is limited only by its own per-account bucket (150 a minute), as before.

### Request pacing the service dictates
PROTOCOL.md "Pacing hints". The model of `scripts/estimate-requests.mjs` (a host estimate of an assumed hour, `--client all`): a client that follows the hints (`paced`) needs fewer requests than one with fixed holds (`new`)
and far fewer than the plain polls of the first 1.3 build (`old`):

| requests per player-hour | old (plain polls) | new (held polls, fixed 12 s) | paced (service-dictated holds) |
|---|---|---|---|
| online menu, idle | 2 | 2 | 2 |
| ranked queue waiting | 1,800 | 293 | 133 |
| invite room waiting (host) | 2,267 | 440 | 353 |
| friends screen open | - | 80 | 80 |
| in a match | 903 | 95 | 93 |
| **mixed hour** (without the friends screen) | **934** | **198** | **170** |
| player-hours a day, whole account | 104 | 492 | 573 |

The client side (obeying `nextWaitSec` / `nextPollMs`) is not in the game yet; until it is, the hints change nothing (a 1.3 build ignores them). What is left of an invite room's 353 is the endpoint republish every 20 s that keeps the NAT mapping alive.

### Daily summary of problem reports
A cron trigger (`[triggers] crons = ["13 0 * * *"]`, 00:13 UTC) calls the object once a day (`scheduled()` in `src/worker.js` -> `GameServer.daily()`, an RPC, no HTTP route), which writes one `report_summary` row for the UTC day
that just ended and logs one line (`[report-summary] 2026-11-22: 7 reports (note 1, session_error 2, desync 1, connection 2, session_trouble 0, other 1), 41230 bytes, 18 sessions, 2 ended in error`). The row holds the counts by
kind, the ten most common builds and app versions, the bytes and the day's session aggregate; no profile id, name, address or report text. Read it with `GET /v1/admin/report-summary?days=7` (stored rows, newest first, and today so
far). Kinds are a heuristic over each stored report, decided when it arrives (`reports.kind`): `note` (the player wrote one), `session_error`, `desync`, `connection`, `session_trouble`, `other`; older reports count as `unknown`.
Rows are kept 90 days. Cost: one Worker and one Durable Object request a day. Reports stay in the object's SQLite (as before); the full reports are still read with `/v1/admin/reports`.
