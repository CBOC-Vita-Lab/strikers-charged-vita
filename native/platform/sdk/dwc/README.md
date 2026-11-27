# Online client: DWC login, server time and ranking

The game's Nintendo Wi-Fi Connection calls (`DWC_LoginAsync`, `DWC_GetDateTime`, `DWC_Rnk*`) are implemented here on top of the online
service (`server/PROTOCOL.md`, version 1). Nothing of the original wire protocol is used; only the C API the decompiled game calls.
Everything is off unless `MSC_ONLINE=1` makes the main menu's old Wi-Fi entry start the online flow (default: that entry is the LAN lobby).

## Layers

| file | what |
|---|---|
| `np_dwc_login.cpp`, `np_dwc_account.cpp`, `np_dwc_error.cpp` | `DWC_Init`, `DWC_InitFriendsMatch`, `DWC_LoginAsync`, `DWC_ProcessFriendsMatch` (the pump), `DWC_GetIngamesnCheckResult`, `DWC_GetDateTime`, the user-data block (`DWC_CreateUserData`, CRC), `DWC_GetLastErrorEx` / `DWC_ClearError` |
| `np_dwc_rank.cpp` | `DWC_RnkInitialize/Shutdown/PutScoreAsync/GetScoreAsync/Process/ResGetRow/ResGetRowCount`: the original's state machine and limits, row validation |
| `np_online_queue.cpp` | persistent retry queue for scores and match results (`<data_dir>/online_queue.dat`) |
| `np_online_api.h` | the seam: `NpOnline::Api` (Begin* / Poll* / Cancel), one implementation per back end |
| `np_online_http.cpp` + `np_online_json.cpp` | the Api over the service's HTTP/JSON protocol; transport is an interface |
| `np_online_https.cpp` | the transport on the TLS client `platform/sdk/net/np_https.h` (compiled when that header is in the tree); `NpOnlineCreateHttpApi` |
| `np_online_fake.cpp` | in-process fake service (tests, scripted runs): `MSC_ONLINE_API=fake` |
| `np_online_name.cpp`, `np_online_ime_vita.cpp` | the typed public name and the Vita keyboard (sceImeDialog) |
| `np_online_store.cpp` | the credentials file |
| `np_online_flow.cpp` | the main-menu entry without Miis (virtual profile slot), `MSC_ONLINE=1` |

## DWC call -> API -> endpoint

| DWC call | Api | HTTP |
|---|---|---|
| `DWC_LoginAsync` (first time) | `BeginLogin` | `GET /v1/info`, `POST /v1/register {name, region}` -> `pid`, `token` |
| `DWC_LoginAsync` (token stored) | `BeginLogin` | `GET /v1/info`, `GET /v1/me` (401 -> one new registration), `PUT /v1/me {name}` when the typed name changed |
| `DWC_GetIngamesnCheckResult` | - | 2 when the service answered `bad_name` / `name_taken` (the callback then reports success with profile 0, the stored name is forgotten) |
| `DWC_GetDateTime` | `BeginTime` (refresh every 30 min) | `GET /v1/time` (and `serverMs` of `/v1/info` at login) |
| `DWC_RnkPutScoreAsync` | `BeginPutScore` (via the queue) | `POST /v1/rank/put` |
| `DWC_RnkGetScoreAsync` ORDER/TOPLIST/NEAR/FRIENDS | `BeginGetScores` | `POST /v1/rank/get` (mode 0..3) |
| `DWC_RnkProcess`, `ResGetRow`, `ResGetRowCount` | `PollPutScore` / `PollGetScores` | - |
| matchmaking layer: `NpOnline::QueueResult` | `BeginReportResult`, then `BeginMatchStatus` until final | `POST /v1/matches/{id}/result`, `GET /v1/matches/{id}` |

## Decisions

* **Token storage**: `<data_dir>/online.dat` (`ux0:data/mscharged/online.dat`), not the save. The save slot is a Wii-format file that
  gets copied and backed up; a credential inside it would travel with every copy. Atomic write, mode 0600, CRC; never logged (the client
  has no log line that prints a token or an `Authorization` header). The save only gets the profile id (offset 0x1C, > 0), login id data
  type 1 and a fresh CRC-32 at 0x3C, so the game's own `ValidateSaveSlot` saves it.
* **Public code** (`XXXX-XXXX`, from `/v1/register` and `/v1/me`): stored in `<data_dir>/online_code.txt`, read with `NpOnline::GetPublicCode()`; the game has no screen that takes a friend code, so the platform UI may show it.
* **Identity** = secret token + profile id + typed display name (2..10 characters, the service's rules, `np_online_name.h`).
* **Errors** reach the game as the numbers its pop-up table maps (`np_dwc_error.cpp`): 20100 transport, 20101 service down / switched off,
  20102 refused, 20103 auth, 20104 unreadable answer, 20109 rate limited, 20110 update required, 80430 timeout.
  `NpOnline::GetStatusMessage()` holds a one-line explanation (kill-switch message, "update required (x.y)", "set the date and time").
* **Score order** (PROTOCOL.md section 8): results are reported and polled until final; a put that arrives meanwhile waits (up to 30 s,
  then the game is told "no response" and the put stays queued). `clamped:true` is logged; the game's next read of its own row shows
  the server's value.
* **Retry queue**: an entry is on disk before the first send, removed when accepted. 1, 2, 4, 8 s then every 15 s, 24 attempts or 24 h at most;
  4xx refusals are dropped. Headers `X-Msc-Attempt`, `X-Msc-Build`, `X-Msc-Mode` carry attempts, build and mode; every request carries
  `X-Msc-Proto`, `X-Msc-App`, `X-Msc-Build` and (when non-empty) `X-Msc-Rules`.
* **Modified rules**: while the NK-bug (Boo) fix of `MSC_GAME_FIXES` is on, scores are not submitted and `RulesAllowRanked()` is false for the
  matchmaking layer (`MSC_ONLINE_ALLOW_MODIFIED=1` lifts it for tests).
* **Privacy**: only name, region, score, the game's 128-byte record, and what PROTOCOL.md lists are sent.

## Switches (all default off / unset)

`MSC_ONLINE=1` online flow; `MSC_ONLINE_URL=https://...` service; `MSC_ONLINE_API=fake` in-process fake; `MSC_ONLINE_NAME` public name on hosts
(and an override everywhere); `MSC_ONLINE_TIMEOUT_MS` request timeouts; `MSC_ONLINE_APP_VERSION`; `MSC_ONLINE_ALLOW_MODIFIED=1` (tests).

## Tests

`tools/dwc/run_dwc_test.sh` (unit tests against the fake), `tools/dwc/run_dwc_http_test.sh <server dir>` (the HTTP client against the service's local fake).
