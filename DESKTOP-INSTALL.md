# Mario Strikers Charged

This package keeps the intro, stadium-preview and movie-audio fixes, with runtime logging disabled.
Use CBOC Vita Installer **0.1.8 or newer** so it recognizes this release.
Use the **Apple Silicon Mac 0.1.9 preview** for the current desktop route.
Intel Mac and Windows packages are still validation candidates. Until their
interactive checks pass, use the [manual installation guide](INSTALL.md).

Requires a homebrew-enabled Vita, VitaShell, your own **Wii USA Rev 1
(`R4QE01`)** ISO/RVZ, and `libshacccg.suprx` obtained legitimately. A complete
validated `sys`/`files` extraction also works.

`libshacccg.suprx` is a one-time Vita module, not a desktop program. The installer
reuses a supported copy already installed in `ur0:data/` when the connection can
read it. Otherwise, it asks for the file and shows any manual copy step.
It is not included in this download.

1. Open [CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases) and connect your Vita.
2. Choose `Strikers-quiet-20261019-install.zip` from the [matching release](https://github.com/CBOC-Vita-Lab/strikers-charged-vita/releases).
3. Select your game file. Add the shader compiler only if prompted.
4. Choose default or custom artwork, then click **Install**.

Download the named ZIP from **Releases > Assets**, or use the complete repository
folder. Leave the release ZIP zipped; the installer opens it. Private releases
need to be downloaded in your browser using an account with access.
For an extracted folder, select the whole folder containing
`cboc-release.json` and its matching VPK, not just one file from it.

No separate Python or Dolphin installation is needed. The first ISO/RVZ
preparation downloads a verified extraction tool; later runs can use its cache.
The app shows the required space and any final VitaShell action. Saves and
player settings are kept. An existing installation requires explicit update
confirmation and a backup.

Keep the computer and Vita awake through preparation, copying and checks.
Leave room on the computer for an extracted copy of your disc, in addition to
the ISO/RVZ. Use the Vita space requirement shown by the installer, which includes
temporary staging and backups.
The progress display covers the current step, not a guaranteed total time.
If disconnected, reconnect the same Vita and choose **Resume**. If saves or
settings changed, use the fresh review offered by 0.1.9 rather than repeatedly
resuming an outdated review. Do not delete the job or restore older saves.

For compatible updates, choose **Update app** to skip disc preparation and keep
verified game data. It requires the completed install receipt on the same
computer and Vita. Missing or changed data requires **Full setup**. An update
still needs your confirmation; matching files are not recopied.

This is a private release. The executable matches the verified update on a
physical Vita, but full installation through this desktop app still needs a
hardware check. Custom art changes the package images, not the game executable.
