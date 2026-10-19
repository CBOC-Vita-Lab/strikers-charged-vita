# Check The Download

Extract the **whole install ZIP** into a new folder on your computer.
Leave the `.vpk` intact. The ZIP's `SHA256SUMS` lists every other bundled
file, including the VPK, guides and notices.

## Optional Offline Checker

If you already have Python 3, open Terminal or PowerShell in the extracted
folder and run:

```powershell
# Windows
py -3 CHECK-FILES.py
```

```sh
# macOS or Linux
python3 CHECK-FILES.py
```

The checker reads local files only. It does not connect to the internet or
Vita, install anything, repair files, or change games/saves. `PASS` means
all files listed in `SHA256SUMS` match. A missing or changed file means you
should extract a fresh copy into a new folder before installing.

## Without Python

Check the VPK using your computer's built-in command and compare the result
with its line in `SHA256SUMS` (letter case does not matter):

| Computer | Command, From The Extracted Folder |
| --- | --- |
| Windows PowerShell | `Get-FileHash -Algorithm SHA256 *.vpk` |
| macOS Terminal | `shasum -a 256 *.vpk` |
| Linux Terminal | `sha256sum *.vpk` |

A matching checksum checks file consistency, not the trustworthiness of
the source, game compatibility or permission to redistribute. Get packages
from the project's official release page. Unlisted files are not checked.
