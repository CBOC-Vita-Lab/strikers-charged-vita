# HTTPS Client

The game's HTTPS client lives in `native/platform/sdk/net/np_https*`. The account, ranking and online lobby code use it to contact
the live service. Vita builds default to `https://strikers.psvita.dev` and fall back to `https://msc-online.cboc-vita.workers.dev` (see "Service hosts"); desktop builds need an explicit URL.
This page records how it is built, what it trusts and why, how it fails, and how to test it.

## What it is

- Our own TLS: [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.7 over `sceNet` sockets. Not `sceHttp`/`sceSsl`, so a stock Vita and a Vita
  with iTLS-Enso or tls_fixer behave the same, and the roots are ours, not `vs0:data/external/cert/CA_LIST.cer`.
- TLS 1.2 and 1.3, SNI, HTTP/1.1 `GET`/`POST` with a JSON body, `Content-Length`, chunked and close-delimited answers, optional keep-alive.
- One worker thread (Vita: user core 2, below the game's priority). The game thread calls `np_https_start`, `np_https_poll` (once a frame),
  `np_https_cancel`, `np_https_release`: each takes a spin lock for a few instructions and never waits on the network.
- Every step has a limit on a monotonic clock (`sceKernelGetProcessTimeWide`, not the RTC, so a clock change cannot stretch them):
  DNS + TCP + handshake (`connect_timeout_ms`, default 10 s), silence once connected (`io_timeout_ms`, 10 s), the whole request
  (`total_timeout_ms`, 30 s, at most 120 s). Cancel is noticed within about 50 ms (a DNS lookup inside `sceNetResolverStartNtoa` is aborted
  with `sceNetResolverAbort`). Four requests at most exist at once; an answer is limited to `max_response` (64 KiB default, 1 MiB hard).
- Redirects are never followed (`REDIRECT`), compressed answers are refused (`Accept-Encoding: identity`), conflicting `Content-Length`s and
  malformed chunks are `PROTOCOL` errors, a body longer than its `Content-Length` ends the kept connection.

API and error codes: `np_https.h`. Errors: `UNAVAILABLE` (no TLS built in, or the roots / random source did not load), `BAD_REQUEST`, `BUSY`,
`OFFLINE`, `CLOCK`, `DNS`, `CONNECT`, `TLS_HANDSHAKE`, `CERT` (with `cert_flags`: expired, not yet valid, untrusted, host mismatch, bad
signature, other), `IO`, `PROTOCOL`, `REDIRECT`, `HTTP_STATUS` (the body is kept), `TIMEOUT` (with the `stage`), `TOO_LARGE`, `CANCELLED`,
`MEMORY`. `np_https_error_text()` gives a sentence a player can act on.

## Verification is never off

There is no switch, environment variable or API that disables certificate or host-name checking (`mbedtls_ssl_conf_authmode(...,
VERIFY_REQUIRED)` is the only setting in the code). If the roots cannot be loaded (a corrupted header: each root is checked against its
SHA-256 first), or the random generator cannot be seeded, every request fails with `UNAVAILABLE` and nothing is sent. The test-only hooks
(`NP_HTTPS_TEST_HOOKS`: replacement roots, a fake clock) exist only in the host test build.

## The clock

Certificate dates need the console's date. A Vita whose battery or clock was reset shows 2000 or 2011, where every certificate would look
"not yet valid" and a player could not tell why. The client therefore compares the RTC (`sceRtcGetCurrentClock`, UTC) with the *build floor*:
the day `np_https_roots.h` was generated or the day `np_https.cpp` was compiled, whichever is later, minus one day. Earlier than that, the
request ends at once with `CLOCK` ("Set your Vita's date and time") and no network is used. Later, certificate dates are checked against the
RTC; a refusal for `EXPIRED` / `NOT_YET_VALID` is reported as `CERT` with that flag and the log shows the clock next to the certificate's window.

## The root set, and why

`native/tools/net/gen_https_roots.py` writes `np_https_roots.h` from the ca-certificates PEMs (DER, SHA-256 per root, 8 roots, about 11 KB).

| Root | Key | Valid until | Who may serve it |
| --- | --- | --- | --- |
| GTS Root R1 | RSA 4096 | 2036-06-22 | Google Trust Services, WR1-WR4 intermediates |
| GTS Root R4 | ECDSA P-384 | 2036-06-22 | Google Trust Services, WE1-WE4 intermediates |
| ISRG Root X1 | RSA 4096 | 2035-06-04 | Let's Encrypt (and the cross-signed ECDSA chains) |
| ISRG Root X2 | ECDSA P-384 | 2040-09-17 | Let's Encrypt ECDSA chains |
| SSL.com TLS ECC Root CA 2022 | ECDSA P-384 | 2046-08-19 | "Cloudflare TLS Issuing ECC CA" (SSL.com) |
| SSL.com TLS RSA Root CA 2022 | RSA 4096 | 2046-08-19 | "Cloudflare TLS Issuing RSA CA" (SSL.com) |
| SSL.com Root Certification Authority ECC | ECDSA P-384 | 2041-02-12 | SSL.com Transit ECC CA R2 (2016 root) |
| SSL.com Root Certification Authority RSA | RSA 4096 | 2041-02-12 | SSL.com Transit RSA CA R2 (2016 root) |

Root-selection research (30 Oct 2026; these research notes did not observe a live handshake):

- Cloudflare's certificate-authority page names the CAs that issue its edge certificates: Google Trust Services (cross-signed by GlobalSign),
  Let's Encrypt, SSL.com (cross-signed by Certum) and, for backup certificates only, Sectigo. A customer or `workers.dev` hostname can
  therefore chain to any of the first three, and Cloudflare may rotate between them.
- Cloudflare Radar's per-domain certificate history shows issuers `WE1` / `WR1` / `AE1` (Google Trust Services), `R13` (Let's Encrypt),
  `Cloudflare TLS Issuing ECC CA 1` / `RSA CA 1` (Cloudflare) and the same names with organisation SSL Corporation (SSL.com).
- Mozilla bug 1966632: sites on `Cloudflare TLS Issuing ECC CA 1` chain through `SSL.com TLS Transit ECC CA R2`, whose served chain had
  been cross-signed by Sectigo's `AAA Certificate Services`; Cloudflare said it would serve the `SSL.com TLS ECC Root CA 2022` chain
  instead. That is why the 2022 roots are in the set.
- Cloudflare's DigiCert G1 page says Cloudflare's own certificate authorities do not include DigiCert, so neither DigiCert nor Baltimore
  CyberTrust is needed (Baltimore expired in May 2025 anyway). The VitAdhoc list (GTS R1/R4, ISRG X1, DigiCert G2, USERTrust RSA, Amazon
  Root 1, GlobalSign R3) covers Google and Let's Encrypt but not SSL.com, and carries four roots Cloudflare does not use.
- When a server sends a root cross-signed by an old root (GTS Root R1 signed by GlobalSign, SSL.com Transit signed by Certum), the client
  still ends the chain at *our* copy of the self-signed root: the library looks for the issuer among its trusted roots first.

Known gaps, in order of likelihood: (1) if Cloudflare ever serves the `SSL.com Transit` intermediate with only its Sectigo cross-signature and
no way to our SSL.com roots, `AAA Certificate Services` would be needed (Mozilla no longer trusts it for TLS); (2) a Sectigo backup chain
(`Sectigo Public Server Authentication Root R46 / E46`) is not included. **Run
`python3 native/tools/net/check_https_roots.py msc-online.cboc-vita.workers.dev`** (it connects, verifying against only the bundled roots,
and says whether they accept) and again whenever Cloudflare announces a CA change. If a chain is not covered, add its root to `ROOTS` in
`gen_https_roots.py`, regenerate, and ship an update; the failure the player sees is `CERT` / untrusted, never a silent downgrade.

On 1 November 2026, the host checker accepted the live service's chain using only the bundled roots, for both TLS 1.2 and TLS 1.3.
That checks the current trust coverage with the host SSL library; it does not establish a physical-Vita Mbed TLS handshake or gameplay result.

## Service hosts (strikers.psvita.dev and the workers.dev address)

Both names are the same Worker with the same data. `online/np_service_hosts.h` decides which one a request uses; it sits above the TLS layer and changes nothing in it.

- Trust is the same for both: there is no certificate pinning, only the root set above, host-name and date checks (`VERIFY_REQUIRED`), for every host. A custom domain on Cloudflare gets its certificate from the same
  certificate authorities as workers.dev, so the roots already cover it. Run `python3 native/tools/net/check_https_roots.py <host>` for both names once the custom domain is live (it was not run for the new name here).
- The new host is tried first. A request moves to the old host only when it failed before any request byte was sent: the name did not resolve (`DNS`), the connect failed (`CONNECT`), the handshake failed for a reason that is not
  the certificate (`TLS_HANDSHAKE`), or a time limit ran out while resolving / connecting / handshaking (the connect limit on the new host is 6 s so the old one has time left). A request also moves when the new name answers with something that is not our service: the shared Cloudflare zone's challenge, rate-limit or block page (403 / 429 / 503 / 52x) never reached our Worker, so it is safe to send again. "Ours" = the first non-space byte of the body is `{` (the Worker always answers JSON; the TLS layer does not expose response headers, so `x-msc-proto` cannot be used); our own JSON errors, 429 and 503 included, are final. The log line is `[np-net] service host: answered by the zone (HTTP N), trying the other address`. The fallback is the same https request with the same
  headers (the User-Agent is `StrikersVita` for both) and full verification.
- Never a fallback: a refused certificate (`CERT`), a wrong clock (`CLOCK`), no network, a failure after sending, a redirect, or an HTTP answer from our service (any status). The player sees the certificate / clock sentence as before.
- Both hosts failing shows the old host's error: the sentence players already know.
- After a failure the new host is skipped for 5 minutes (this run only, nothing is stored); an answer from our service clears that at once.
- `MSC_ONLINE_HOST` unset (or any unknown value) is `auto`; `new` forces only the new host (no fallback), `old` only the old one. Tested against a local fake of the TLS layer: `native/tools/net/run_service_hosts_test.sh` (with 18 mutants).
- A build released before the owner deploys the custom domain finds no DNS record for the new name, falls back, and works as before; one released after works on the new name. While Cloudflare is still issuing the certificate
  a short window of `CERT` (host name) errors on the new name is possible; the fallback does not hide it on purpose. Players can set `MSC_ONLINE_HOST=old` then.

## Backends

`np_https.cpp` knows nothing of the TLS library: it uses `np_https_tls.h` (open, handshake, read, write, close_notify, certificate verdict,
details). The only backend is `np_https_tls_mbedtls.cpp`. mbedtls is not proven on a Vita (the one console-proven TLS stack in these ports is
OpenSSL 1.0.2 from VitaSDK in Monster Hunter Tri's port), so a second file implementing the same functions over OpenSSL can replace it
without touching the rest; select it with a different `NP_HAVE_*` define in `np_https_tls.h`.

## Building the library

`dependencies/lock.json` (`network_dependencies`) pins Mbed TLS to tag `mbedtls-3.6.7`, commit `068ff080b369adfac81509f9b57b2afabaf82dc5`
(3.6.5 is what the vitasdk package recipe builds; 3.6.6 and 3.6.7 are later security releases). `tools/build-mbedtls.py` fetches that commit
exactly like `tools/build-deps.py` does for the other dependencies (never resetting an existing checkout) and builds it with
`dependencies/Makefile.mbedtls` into a private prefix:

    python3 tools/build-mbedtls.py --target both --work <new-dir>   # <new-dir>/mbedtls/{vita,host32}: include/, lib/, build-receipt.json

Nothing is installed into VitaSDK. The pinned checkout is never patched. Only `ssl_tls13_keys.c` is compiled from a verified one-file output
overlay containing the exact upstream non-PSA TLS 1.3 transform-cleanup backport
[afa648aa](https://github.com/Mbed-TLS/mbedtls/commit/afa648aa44c698f6f01b1dd653edc3dfbe140355).
It frees earlier cipher allocations when a later setup step fails; the stable pin and feature profile are unchanged. The lock records patch,
upstream, original and overlaid source hashes. No PSA-only parent change or TLS 1.2 parser change is bundled.

Use a NEW work directory when changing patch, recipe, profile, builder or compiler provenance. Existing prefixes with different provenance
are rejected, not overwritten; `--rebuild` only rebuilds matching inputs. `build-inputs.json` records this identity, and the final receipt adds
actual `.d`, compile-input, object and archive hashes. An interrupted matching build may be retried without resetting the checkout. See
[backport provenance](../dependencies/mbedtls/patches/README.md).

vitasdk's unrelated patch repairs `net_sockets.c`, `timing.c` and the platform entropy
source for the Vita; the client profile (`dependencies/mbedtls/np_user_config.h`) builds none of the three: sockets and time-outs are ours
(`np_https_sys.cpp`), entropy is `sceKernelGetRandomNumber` through `mbedtls_hardware_poll`, the wall clock and `gmtime` are ours too
(`MBEDTLS_PLATFORM_TIME_MACRO`, `MBEDTLS_PLATFORM_GMTIME_R_ALT`), so the clock can be checked and faked in tests. The profile is TLS client
only (ECDHE with AES-GCM or ChaCha20-Poly1305; x25519, P-256, P-384; RSA and ECDSA signatures), no server, DTLS, tickets, PSK, file system.

Build the game with it by adding `NP_MBEDTLS=<prefix>` to the make line (the Vita package build script of your setup, with `NP_MBEDTLS=<work>/mbedtls/vita` among the make arguments).
Without it `np_https` is compiled without TLS and every request answers `UNAVAILABLE`.

## Network start-up

`np_https_sys.cpp` loads the NET module only if it is not loaded, calls `sceNetInit` with a 1 MiB pool and accepts `EBUSY` (the LAN code
may have initialised sceNet with its own pool first; sockets then live in that pool) and an already-initialised `sceNetCtl`. It frees only a
pool that it created, and never terminates sceNet or sceNetCtl.

## Console self-test

Set `MSC_NET_SELFTEST=https://<host>/<path>` in `np-player.txt` (or `env.txt`). Four seconds after start-up a thread does one `GET` through
the public API and logs, under `[net-https]`: the backend and version, network state, the console clock in UTC with the build floor, each stage
as it is reached, the result with the time of each step (DNS, TCP, TLS), the negotiated suite, the server certificate's subject, issuer and
validity, and, for a refusal, every certificate of the chain with the library's verdict (the idea of Monster Hunter Tri's
`LogRefusedCertificate`). The same certificate lines are logged by any refused handshake, not only by the self-test.

## Tests

The allocation-failure regression links the ordinary host32 archives and uses only synthetic keys, with no network:

```sh
W=build/dependencies-tls13
python3 tools/build-mbedtls.py --target host32 --work "$W"
bash native/tools/https/run_tls13_transform_test.sh "$W/mbedtls/host32" "$W/sources/Mbed-TLS_mbedtls" build/tls13-transform-test
```

All three runner paths are explicit; the output directory must not already exist. It checks live allocations after failed real TLS 1.3
AES-GCM setup, BEFORE fixture cleanup, plus successful setup and invalid-input controls. No production test hooks or altered test-only
library objects are used. Host results do not establish physical-Vita behavior or a frame-rate improvement.

`native/tools/https/run_https_test.sh` (set `MBEDTLS=<work>/mbedtls/host32`) builds the client for host32, starts
`tools/net/https_server.py` (a local HTTPS server that makes a private test CA on the fly: good, wrong host, expired, not yet valid,
untrusted root, RSA chain with an intermediate, TLS 1.2-only and 1.3-only, a non-TLS port, a silent port, a closed port, slow, trickling,
chunked, close-delimited, huge, redirect, compressed, truncated, kept-alive and flaky connections) and runs `https_test.cpp`: certificate
verdicts, the clock, fail-closed roots, every error code, size limits, POST, refused requests, cancel / busy / release, keep-alive, restart,
and the longest time any API call blocked the caller.
