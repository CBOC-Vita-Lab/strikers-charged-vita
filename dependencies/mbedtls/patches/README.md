# Mbed TLS 3.6.7 Backport

`0001-tls13-transform-cleanup.patch` is the exact non-PSA source hunk from upstream
[afa648aa44c698f6f01b1dd653edc3dfbe140355](https://github.com/Mbed-TLS/mbedtls/commit/afa648aa44c698f6f01b1dd653edc3dfbe140355)
(2026-10-22, "Fix non-PSA TLS 1.3 transform cleanup"). Upstream file:
`library/ssl_tls13_keys.c`. Mbed TLS is Apache-2.0 OR GPL-2.0-or-later; the upstream
file's license header is retained in the generated overlay.

The stable 3.6.7 pin and client profile are unchanged. No PSA-only parent change,
TLS 1.2 parser change, or other development-branch changes are included. The fix
frees already allocated cipher contexts when a later transform setup step fails.
The real TLS 1.3 handshake/application owners free only the outer transform on
these errors, so cleanup belongs to the failed setup operation.

`dependencies/lock.json` binds the patch, upstream commit, original source, and
overlaid source hashes. `tools/build-mbedtls.py` verifies the clean pinned
checkout, applies the patch with `git apply` in a fresh one-file output overlay,
and never patches the checkout itself. Use a new work root: old unpatched or
otherwise different prefix receipts are rejected, not overwritten.

`native/tools/https/run_tls13_transform_test.sh PREFIX SOURCE NEW_OUT` links the
ordinary host32 archives with allocation-failure injection. It checks nested
allocations before fixture cleanup, including failures at all four allocations
in the real client AES-GCM transform path. It uses only synthetic key material
and no network or production test hooks. This is host correctness evidence,
not a physical-Vita, exploit, or frame-rate claim.
