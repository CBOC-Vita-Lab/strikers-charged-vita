# Easy Install With CBOC Vita Installer

**The game does not need this program.** It runs on the Vita by itself, and
[Manual Install](INSTALL.md) works from any computer. The installer just does
the copying and installing for you.

[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest)
is one small download for Mac (Intel or Apple Silicon, macOS 10.13+),
Windows (7 and later) and Linux. Nothing else to install: no Python, no Dolphin,
no FTP client.

1. Open the installer. It opens in your browser.
2. On the Vita, open VitaShell and press **SELECT**. The installer finds the
   Vita on your Wi-Fi by itself. (USB mode or the Vita's memory card in a card
   reader works too, and is faster.)
3. Click **Get** next to Mario Strikers Charged, or drop
   `Strikers-Charged-Vita-install.zip` / this repository's folder onto the
   window.
4. Choose your own game: a Wii USA Rev 1 (`R4QE01`) ISO, RVZ or WBFS, or a
   folder extracted with Dolphin. It checks it's the right disc and revision.
5. Choose `libshacccg.suprx` if it asks. It doesn't ask if the Vita already
   has it.
6. Click **Install**. Over Wi-Fi the game is installed with no button presses
   on the Vita. Over USB or a card reader, finish by pressing **X** on the VPK in
   VitaShell's `ux0:VPK` folder.

Your saves (`np-data/NAND/`) and settings (`np-player.txt`) are never touched.
When a new version comes out, open the installer with the Vita connected:
it shows **Update** next to Mario Strikers Charged. Updating only sends the
files that changed, so it takes seconds, and your game data is not sent again.
If the transfer stops, press **Install** again; finished files are skipped.

The game data is about 1.9 GB. Vita Wi-Fi manages about 2–4 MB/s, so USB is
the quickest route for a first install.

First launch help: [Mac](https://github.com/CBOC-Vita-Lab/vita-installer/blob/main/docs/mac-open.md).
On Windows, if SmartScreen appears, choose **More info > Run anyway**.
