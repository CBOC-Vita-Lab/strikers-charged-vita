# Name

Mario Strikers Charged Vita

# Version

1.0 (public preview)

Title ID: `MSCN00003`. The existing SFO app version is `01.00`.

# Screenshots (Optional)

![Gameplay on physical Vita, isolated settings-test build](https://raw.githubusercontent.com/CBOC-Vita-Lab/strikers-charged-vita/main/store/screenshots/gameplay-vita.png)

Gameplay is from the isolated diagnostic build used for the settings update,
not a fresh installation of the public VPK.

![Settings on physical Vita, normal-title quiet runtime](https://raw.githubusercontent.com/CBOC-Vita-Lab/strikers-charged-vita/main/store/screenshots/settings-vita.png)

The settings capture uses the normal-title runtime matching the public VPK.
[Screenshot provenance](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/blob/main/store/screenshots/README.md).

# Author

CBOC-Vita, with upstream contributors credited in
[CREDITS.md](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/blob/main/CREDITS.md).

# AI status

Vibecoded. Port-specific implementation was largely produced with Claude and
ChatGPT under human direction and hardware feedback. The existing upstream
game reconstruction and libraries retain their original authors and notices.

# Description

A native PS Vita port of Mario Strikers Charged based on the mscharged-decomp
source reconstruction. It includes Vita controls, seven camera zoom levels,
GPU skinning, a startup shader cache and an in-game LiveArea settings overlay.

This public preview includes the recent intro and stadium-preview movie fixes,
the movie audio-buffer fix and disabled runtime logging. Around 30 FPS has been
achieved in testing, with performance varying by scene. Bugs and further
performance improvements remain.

# Short Description

Native Mario Strikers Charged port for PS Vita; requires the owner's Wii game data.

# Download Link

https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/latest/download/MarioStrikersCharged.vpk

VPK SHA-256: `43543e5bce2f162f6678bc2baacbdcbeb03a4593493c0e401a5ad6534623400a`

# Release Link

https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases/tag/v1.0

# Sourcecode Link (Optional)

https://github.com/CBOC-Vita-Lab/strikers-charged-vita/tree/v1.0

# Data Files Link (Optional)

None. Retail game files and the proprietary shader compiler are not bundled.

# Requirements (Optional)

- Homebrew-enabled PS Vita.
- The owner's Wii USA Rev 1 (`R4QE01`) dump, with the complete `sys/` and `files/`
  folders in `ux0:data/mscharged/game/`.
- Legitimately obtained `libshacccg.suprx` in `ur0:data/`.
- About 3 GB free for the extracted game, app and saves.
- [Desktop or manual preparation instructions](https://github.com/CBOC-Vita-Lab/strikers-charged-vita#install).

# Icon

128 x 128 generic project icon, identical to the VPK's icon:

https://raw.githubusercontent.com/CBOC-Vita-Lab/strikers-charged-vita/main/native/platform/vita/vpk/icon0.png

# Release Status And Notices

Please consider this public preview for a new VitaDB entry. The following
limitations are disclosed in the repository and release:

- Combined-package licensing and corresponding-source review are unfinished;
  GPL/LGPL compliance is not claimed as verified.
- bythos14 and contributors are credited for
  [kubridge](https://github.com/bythos14/kubridge). The build references its
  header/export definitions at `417ddde9a744eba98d769382c1c1372b8e119139`, where
  no license statement was found. The redistribution basis remains unresolved;
  the plugin is not bundled. Attribution is not presented as permission.
- The selected executable was installed and read back on a physical Vita and
  its settings overlay was checked. Complete fresh VPK/desktop installation
  and wider gameplay acceptance for this exact package remain open.

[Third-party notices](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/blob/main/THIRD_PARTY_NOTICES.md)
and [known issues](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/blob/main/KNOWN-ISSUES.md)
contain the details. No commercial game files, private fan art, saves or
proprietary shader compiler are part of this submission.
