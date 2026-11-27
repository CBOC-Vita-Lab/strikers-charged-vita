# Online play from the game's own menus

Online play is started from the main menu's MULTIPLAYER entry, in the game's own look: every screen is a popup of the game (`FEPopupMenu`, the
type `POPUP_NP_TEXT`) or the game's own lobby (Create Game / Join Game). Nothing is drawn by the platform layer.

## How a player gets there

1. Main menu, MULTIPLAYER: choose ONLINE or LOCAL. LOCAL offers NEARBY and SAME WI-FI; BACK or Circle returns to the mode choice.
2. ONLINE opens HOST A ROOM / JOIN WITH A CODE / RANKED MATCH (1.3; with `MSC_ONLINE_RANKED=0` the third button is CANCEL). Circle cancels. No trip to the
   Vita's LiveArea, setting change, or restart is needed. RANKED MATCH opens Online Central (the game's own login, hub, ranked match and rankings);
   only ranked matches count on the leaderboards (docs/TECHNICAL.md, "Ranked matches").
3. First time only: the keyboard asks for a public name (2-10 letters or digits). The game makes the online account by itself (`POST /v1/register`);
   the token is stored in `online.dat`, the name in `online_name.txt` (the formats of the ranking client).
4. Join: the keyboard asks for the invite code. Host: the room is created.
5. A status popup follows the bring-up: "Connecting to the service...", "Finding a route...", "Waiting for your partner (code K7Q-M4D)...".
   Circle or CANCEL stops it at any point and returns to the main menu.
6. When a partner is connected the game's own lobby follows (Create Game / Join Game), then the usual draft and match.

The saved MULTIPLAYER LINK setting only preselects the first choice. A selection in the game lasts for this run and does not change the settings file or
environment. NEARBY retains the saved automatic/create/join radio preference. Cancelling the mode choice or the online host/join popup returns to the main menu
without starting a local radio. `MSC_ONLINE=1` is a developer shortcut that also puts Online Central on the main menu's old Wi-Fi entry; players reach it through RANKED MATCH.

One keyboard helper (`np_keyboard.h`: sceImeDialog on the Vita, an environment-driven test keyboard on the desktop) serves the name, the invite code and,
after the ranking client is merged, the leaderboard name.

## Multiplayer beta (mpbeta27)

Multiplayer launches as a public beta: the owner has one Vita, so the first matches between two Vitas happen in players' hands and the problem reports are how the port learns.
- **Labels.** The Multiplayer menu's entry points read ONLINE BETA / LOCAL BETA, NEARBY BETA / SAME WI-FI BETA, FRIENDS BETA / PLAY WITH ANYONE BETA / RANKED MATCH BETA
  (`np_mpbeta.h` Tag; every tagged label at most 24 characters in English, Spanish and French). The main menu's MULTIPLAYER entry and the ONLINE CENTRAL title are written
  in place as MULTIPLAYER BETA / ONLINE CENTRAL BETA (`np_lan_text.h` kBetaKeys, within the disc's own lengths). `MSC_MP_BETA=0`: the 1.3.0 labels.
- **The notice.** The first time the Multiplayer menu opens: "Multiplayer is in beta. If something goes wrong, the game will offer to send a problem report. Thank you!" with
  OK (state `S_BETA_NOTICE`; marker `mp-beta-seen.txt`; `MSC_MP_BETA_NOTICE=0` never, `always` every visit).
- **The result screen** of a LAN or online match (session state GAME_END) shows one line low on the screen: "Multiplayer is in beta. Something wrong? Choose REPORT A
  PROBLEM in the Multiplayer menu." (`np_notice.cpp`, render only).
- **REPORT A PROBLEM** is the mode choice's third button (it was CANCEL; Circle still goes back; `MSC_MP_REPORT_ENTRY=0` restores CANCEL): the consent popup of SEND A
  REPORT, or, without an online account, the report is saved as `ux0:data/mscharged/mp-report.txt` and the player is told.
- **A desync** (the consoles' checksums differ) ends the match with the game's sync-error popup, now in plain words: "The two Vitas got out of step, so this match ends
  here. Sorry! The next time you open Multiplayer, the game will offer to send a problem report." The first mismatch of each kind is logged with both values
  (`NpLanNoteMismatch`) and goes to the report.
- **Another build.** A partner whose frames the link refused (another build or rules): the connection-lost popup says "The other Vita has another version of the game, so
  the match could not start. Update both Vitas to the latest version."; the lobby's "no game to join" sentence ends with the same advice. The wire and the fingerprint are
  unchanged.
- **The offer** now also comes after a ranked result the retry queue had to drop, and after the last run closed inside a multiplayer session (`mp-session.txt`, written at the
  lobby, removed when the session is left; `MSC_MP_CRASH_MARK=0` off), and without an online account (the report is then saved to the file).

## Problem reports and deleting your account (public beta)

- **After an online game that ended with an error** (a desync, a lost connection, a lost link, an online failure sentence) the next time the Multiplayer menu opens it
  shows one popup: "Your last online game ended with a problem. Send a report to help fix it? You will see what is sent first." with SEND A REPORT and NOT NOW. Either button
  ends the offer for that game; a new game that ends with an error offers it again. SEND A REPORT then shows the consent popup (exactly what is sent: your online name, the
  last lines of the multiplayer log, your last connection check, numbers from your last online game; IP addresses, ids and codes removed; no friend list; kept 30 days) with
  SEND and CANCEL (CANCEL is lit first). SEND shows "Sending the report..." (Circle cancels) and then one popup with the result: sent, the limit of 3 a day, no connection,
  or the service not taking reports now. States `S_RPT_OFFER`, `S_RPT_ASK`, `S_RPT_SEND` in `np_menu_flow.cpp`.
- **DELETE MY ONLINE ACCOUNT and SEND A REPORT** are also rows of SETTINGS > MULTIPLAYER (docs/SETTINGS.md); the online menu itself has no delete entry.
- Both use the old service address unless `MSC_ONLINE_URL` / `MSC_ONLINE_HOST` say otherwise, like every other call of the online client, and nothing calls them unless the
  player pressed a button. `POST /v1/report` and `POST /v1/account` (signed with the account token; the game's HTTPS client has GET and POST only, so the service takes POST
  as well as DELETE for the account). An older service answers 404: the game says it cannot do that yet and changes nothing.

## State machine

`np_menu_flow.cpp` is stepped once per game loop from `NpLanMenuUpdate` (`NpMenuStep`); it never blocks. HTTPS and DNS run on worker threads, the keyboard
is a system dialog, the transport is polled with a bounded number of packets, and at most one disk access or one request start happens in a frame.
Every wait has a limit on the game clock (`np_time_ns`, virtual in scripted runs): account 20 s, service 30 s, route 60 s, partner 10 min; the transport
has its own limits and sentences underneath. States: choose, account, name keyboard, register, code keyboard, connect, failure, closing.
`np_menu_ui.cpp` is the screen: it pushes the popup, replaces its text while it is open (`FEPopupMenu::NpSetMessage`; all status texts have the same
line count so the box does not change size) and reads Circle from the pad (the popup holds the exclusive input lock).

## Failure sentences (one plain sentence each, in a popup with OK)

| Situation | Text |
| --- | --- |
| Console not on Wi-Fi | Check your Wi-Fi: this Vita is not connected to a network. |
| Console clock before this build | Set your Vita's date and time (Settings > Date & Time), then try again. |
| No service address | The online service address is not set. Add MSC_ONLINE_URL=https://... to np-player.txt. |
| Service says update | Different version: please update the game to X. |
| Service full or down | The service is busy. Try again in a moment. |
| Name taken or refused | That name is already taken / is not allowed. Choose another one. (asks again) |
| Wrong code | the transport's sentence for a room that does not exist |
| No route | No direct route to your partner was found. Try another Wi-Fi network. |
| Nobody came | Nobody joined the room in time. |
| Lobby finds a console with another build | Different version: ... / Different rules (theirs: X, ours: Y). ... (also for Nearby and Same Wi-Fi) |

## Reconnect, and why a match is not resumed

A link lost in the lobby or during a match makes the game end its session (its own "connection lost" popup, whose text now says why the link ended and that
a match cannot be picked up again). The lobby that follows offers RECONNECT / LEAVE for 30 s (the service drops silent tickets after 30 s; a room lives
minutes). RECONNECT calls `NpNetReconnect` (a new socket, STUN, the endpoints posted again, new punch keys; the transport's own rebuild limit applies) in
the same room; LEAVE, a failure or running out of time goes back to the main menu through the lobby's own back transition. A cancel is always possible.

When the link is back the lobby starts over, in three steps (`NpMenuLobbyStep` returns 2, `NetworkStartScene::Update` acts on it): the loss latch of the
virtual link is cleared (`NpLanLinkRestored`, otherwise every send of the next session fails), the lobby object forgets the session it had joined or hosted
(`LANLobby::Shutdown`: a lobby still in `LAN_JOIN_JOINED` refuses a new JOIN with "failed to join game"), and the scene goes back to CREATE / JOIN. On the
desktop build the lobby also pumps the transport each frame (the Vita does that in `np_vita_lan_frame`); without it a restored lobby with no session goes
quiet and the peers declare each other lost again.

A match is never resumed. The game is deterministic lockstep: both consoles must have applied exactly the same inputs on every tick, and it has no
snapshot and no rollback. After a break one console may have applied ticks the other never received, and nothing can reconcile them. The match is ended
cleanly and the lobby offers a new one in the same room.

## The warning during a match

When the online service fails while a link is up (a refusal after a release, a ban, the kill switch, the account wiped) the transport stops asking it, keeps
the links and sets `OnlineInfo::warning` ("<reason> The match goes on.") and calls `OnlineConfig::onNotice`. The player is told with a banner: a line of
text in the game's own font, low on the screen, for about 9 seconds (half a second fade in and out), drawn on top of the front-end scenes of the frame by
`FERender::RenderNotice` -> `NpNoticeRender` (`np_notice.cpp`). It is not a popup: a popup holds the game's input lock and would starve the lockstep.
It reads no pad, takes no lock, allocates nothing, and the match never waits for it. It is shown once per sentence, from the first frame of the match
after the warning appeared. `MSC_ONLINE_NOTICE=0` turns it off (the warning stays in the log), `MSC_ONLINE_NOTICE_MS` sets how long it stays (default 9000).

## A popup is closed only when it is ready

A popup is a scene whose resources load asynchronously. Closing it before it finished loading leaves the resource manager marking a destroyed scene as
ready a few frames later (a crash or silent corruption in `FEResourceManager::Update`). On a fast link the service answers within a few frames of the
status popup opening, so `NpMenuUiClose` defers the pop until the popup's scene is ready, and a failure popup follows only after the scene stack settled.

The same goes for the input lock. A popup holds the game's exclusive input lock from its creation until its scene is destroyed, a few frames after it was
popped, and `FEPopupMenu::~FEPopupMenu` pops the top entry of the lock stack whoever owns it. A popup pushed in that gap (RECONNECT / LEAVE right after
the game's own "connection lost" popup) would take a lock above the dead one, its destructor would pop the new popup's entry, and the new popup would never
have input focus: Cross did nothing and the player could not choose. `NpMenuUiShow` therefore waits (at most 300 frames) while the top lock belongs to a
scene that is no longer on the stack.

## Keys

| Key | Where | Default |
| --- | --- | --- |
| `MSC_LAN_MODE=online` | Optional saved preference for the in-game mode chooser, not a gate | Nearby |
| `MSC_ONLINE_URL` | np-player.txt / env: `https://a,https://b` | servers.json, then the build's `NP_ONLINE_URL` (Vita builds: https://strikers.psvita.dev with the old https://msc-online.cboc-vita.workers.dev as the fallback, see `online/np_service_url.h`, `online/np_service_hosts.h` and `MSC_ONLINE_HOST`) |
| `MSC_ONLINE_NAME` | env: the public name (skips the name keyboard) | asked once |
| `MSC_LAN_INPUT_DELAY` | Settings, INPUT DELAY: STANDARD (unset: the game starts at 4 and settles at 2) / AUTO (`auto`) / 2 / 3 / 4 / 6. A choice is the host's start value and floor: the delay holds there and a slow link still raises it for a while (not a fixed delay) | STANDARD |
| `MSC_NET_RELAY_ONLY=1` | Settings, CONNECTION: AUTO / RELAY ONLY | AUTO. Reader: the relay worker's transport (not in this tree) |
| `MSC_KEYBOARD_NAME`, `_CODE`, `_CODE_FILE`, `_CANCEL`, `_DELAY_FRAMES` | desktop test keyboard | none |
| `MSC_NET_TEST_WIFI=0`, `MSC_NET_TEST_CLOCK=0` | desktop: pretend Wi-Fi is off / the clock is wrong | |
| `MSC_ONLINE_NOTICE`, `MSC_ONLINE_NOTICE_MS` | the in-match banner for a transport warning: on/off, how long (ms) | on, 9000 |
| `MSC_WAIT_BANNER` | the "Waiting for the other player... (N s)" line after 1.5 s of lockstep steps without the partner's input (LAN and online; render only) | on |

## Tests

`native/tools/mp/run_online_menu_test.sh` (`BIN=<host32 exe>`, `NODE=<node>`; skips without one). `CASES=pair,cancel,wrong,lost`, `DUMP_EVERY=25` with the GL build
writes PNGs under `<outdir>/<case>/.../pics`. The service refuses loopback addresses as endpoints, so the cases that publish an endpoint run behind the NAT
simulator (`tools/net/natsim_proxy.cpp`; `PROXY=<exe>`, else the test compiles one). Real time, two game processes: do not wrap in a build lock.
- `pair`: two consoles drive the real menus (MULTIPLAYER -> HOST A ROOM / JOIN WITH A CODE -> lobby -> draft -> a short match) over the online backend and
  the local fake service; the lockstep checks of `native_pair.py` must pass. Three seconds into open play the fake service is wiped (the next poll of both
  consoles is refused): both transports must report the warning, both consoles must show the banner, and the match must run to its end unchanged.
- `cancel`: Circle on "Waiting for your partner" and on the choice popup return to the main menu; hosting again works.
- `wrong`: a code no room has: one sentence in a popup, OK returns to the main menu.
- `lost`: the simulator cuts the link while both consoles are in the lobby; the game ends its session, the lobby offers RECONNECT, the simulator restores the
  route and RECONNECT brings the lobby back, where the host creates a new game and the joiner joins it; a second run keeps the route down and RECONNECT must end with a sentence inside its window.
Every failed check is printed again right above the summary line (the gate shows only the tail).
