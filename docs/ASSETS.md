# Included Files

## In The VPK

- The game executable, built from this repository.
- The startup shader cache, the app icon and the LiveArea graphics, recorded
  in [artwork.json](../store/artwork.json).
- License texts for the port and every library it uses.

[RELEASE.json](RELEASE.json) records the package and executable hashes and
which package members were carried over unchanged from the previous release.

## In The Install ZIP

The VPK, plus the guides, notices, checksums, the full source and two optional
computer-side helpers:

- `PREPARE-GAME.py` extracts and checks your disc. It downloads an unmodified,
  checksum-pinned [nodtool](https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.12)
  to your computer only.
- `tools/check-files.py` checks the download against `SHA256SUMS`.

Both are [MIT licensed](../licenses/CBOC-tools-MIT.txt).

## Not Included

Your game data (disc, extracted files, music, movies), saves, firmware and the
Vita's shader compiler module (`libshacccg.suprx`). Supply your own. The code
licenses do not grant any rights to the original game's assets.

The [screenshots](../store/screenshots/README.md) were captured on a PS Vita
and contain no personal information.
