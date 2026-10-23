# Included And Required Files

The optional computer-side `PREPARE-GAME.py` helper is included under
[MIT](../CHECK-FILES-LICENSE.txt). It downloads an unmodified, checksum-pinned
nodtool directly from its [official release](https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.12).
The downloaded extractor stays on the user's computer; it is not a game asset.
The local install bundle includes the reviewed source, preparation tools,
all linked guides and notices alongside the ready-made VPK.

## Installed Build

The selected VPK contains the quiet player executable with the movie and LiveArea settings fixes,
byte-for-byte identical to the verified main-app update on a physical Vita.
Application metadata and matching shader resources are unchanged.
[RELEASE.json](../RELEASE.json) records the package, runtime and source hashes.

The package uses the project's generic icon and LiveArea graphics, recorded
in [ARTWORK.json](../ARTWORK.json), instead of the console's personal fan art.
The optional fan-art background and loading picture are omitted; the same
executable provides its built-in plain loading screen. Required font and
library notices are included. The movie, logging and settings changes are recorded
in `RELEASE.json`; packaged artwork and shaders are unchanged.

Personal artwork is kept separately and is not shared. Installation-generated
package records are not part of the VPK.

Supply your own supported game dump. Extracted retail data, music, movies,
logos, saves, NAND contents and test profiles are not included. A code
license does not grant rights to the original game assets.

[Store screenshots](store/screenshots/README.md) show existing physical-Vita
captures from the settings update. Their build context and hashes are recorded;
the gameplay capture is from the isolated diagnostic build, not evidence of
a fresh installation of the public VPK. The images contain no account names,
device addresses or private artwork. They illustrate the port and do not grant
rights to extract or redistribute the original game's artwork.
