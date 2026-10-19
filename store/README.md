# Store Listing

This directory holds the VitaDB submission for the existing
`strikers-settings-20261019` public preview. It is not a new game build.

## Submission

Submitted through CBOC-Vita on 2026-10-19:
[VitaDB contribution #122](https://github.com/Rinnegatamante/VitaDB-contributions/issues/122).
The posted text is in [vitadb-submission.md](vitadb-submission.md).
This is a listing request, not confirmation that the game is available in VitaDB.
VitaDB's maintainers decide whether to add the entry. No hold was requested.

The public VPK, installation ZIP and their checksums are unchanged. The entry
uses title ID `MSCN00003`, the release tag as its version, and the VPK's existing
128 x 128 generic icon. `PARAM.SFO` reports `APP_VER` and `VERSION` as `01.00`;
that does not distinguish this update from older builds. The release tag and
executable checksum identify the specific version.

## Player Setup

The store download is the VPK, not the complete installation ZIP. **CBOC Vita
Installer is optional.** Players still need their own Wii USA Rev 1 (`R4QE01`)
files and `libshacccg.suprx`. [Manual Install](../INSTALL.md) covers setup from
Windows, Mac or Linux using VitaShell. The [optional desktop helper](../DESKTOP-INSTALL.md)
automates setup on Apple Silicon Mac. Ordinary updates replace the VPK and keep
game data, saves and settings. No computer is needed while playing.

## Media

- [Generic icon](../native/platform/vita/vpk/icon0.png), identical to the packaged icon.
- [Physical-Vita screenshots and provenance](screenshots/README.md).

No personal fan art, game-file downloads or proprietary shader module is supplied.
AI use is disclosed as vibecoded: port-specific implementation was largely
AI-generated, with human direction and hardware feedback, on an existing
third-party source base.

## Remaining Checks

[Third-party notices](../THIRD_PARTY_NOTICES.md) document the unresolved
kubridge licensing basis and combined-package/source review. These are not
marked verified by the submission. [Known issues](../KNOWN-ISSUES.md) separately
record the outstanding fresh-install and wider gameplay checks. No new device
tests or installations were run for the listing.

For a later release, publish a tagged VPK and matching source, keep `MSCN00003`,
update these details, and notify VitaDB through its
[contribution repository](https://github.com/Rinnegatamante/VitaDB-contributions).
No VitaDB automatic-update webhook or NeoVitaDB listing is configured yet.
