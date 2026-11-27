#!/usr/bin/env python3
"""Post-link check of the Vita ELF for the multiplayer / Nearby / settings hooks that the game reaches only through weak references.

A weak reference to a missing definition links without a word: the call is skipped (`if (f) f();`) and the feature is simply off (Nearby counters, the pause-menu
VITA SETTINGS row, the keyboard hooks). native/platform/vita/vita_require.mk covers the symbols that live in archives; these live in objects that are linked directly,
so a bad link line (a source dropped from a make list) is the way to lose them, and nothing fails. Run it on the ELF of every console build:
    python3 native/platform/vita/tests/check_elf_hooks.py <elf> [--nm arm-vita-eabi-nm]
Exit 0 = every hook is defined in the ELF (or inlined by LTO: its witness string is there); 1 = the missing ones are listed. tests/test_link_requires.py checks that this list is still true against the sources
and that the parser here reads nm's output as intended.
"""
import subprocess
import sys

# symbol -> (what is off without it, a string that only the defining object holds)
# A ThinLTO link may inline a hook into its one weak caller and drop the symbol (found 15 Nov in the PGO ELF: np_vita_radio_rx_dropped, NpVitaSettingsRequestOpen and
# NpLanOnlineSessionTuning were not in the symbol table although their code was there), so "no symbol" alone is not "dropped". The witness is a string literal of the
# defining object that survives a release build (an environment variable name, a line the player reads): the hook is lost only when neither the symbol nor the witness is in
# the file, i.e. the object was not linked. None = no witness: the symbol is required.
HOOKS = {
    "np_vita_radio_rx_dropped": ("Nearby receive-drop counter in the [mp-lan] net lines (np_lan.cpp)", "MSC_LAN_DIALOG"),
    "np_vita_radio_ring_take_peak": ("Nearby receive-ring peak in the [mp-lan] net lines (np_lan.cpp)", "MSC_LAN_DIALOG"),
    "np_vita_radio_dialog_active": ("input is not held while the Nearby dialog is up (np_vita_input.cpp, np_vita_settings.cpp)", "MSC_LAN_DIALOG"),
    "np_vita_radio_system_init": ("AppUtil / CommonDialog set-up for Nearby at boot (np_vita_main.cpp)", "MSC_LAN_DIALOG"),
    "NpLanOnlineSessionTuning": ("online sessions keep the retail keep-alive and input transport (np_dwc_match.cpp)", "MSC_LAN_KEEPALIVE_MS"),
    "NpInputNormalizeForNetwork": ("LAN input is not normalised for the wire (np_lan.cpp)", None),
}


def defined_symbols(nm_output):
    """Names with a definition (type T, t, W, w, V, v, D, d, B, b, R, r) in `nm` output; U / u (undefined) and w without a value are not."""
    out = set()
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) == 3 and len(parts[1]) == 1 and parts[1] in "TtWwVvDdBbRrSs":
            out.add(parts[2])
    return out


def missing(nm_output, blob, hooks=HOOKS):
    """Hooks with neither a symbol nor their witness string in `blob` (the ELF's bytes)."""
    have = defined_symbols(nm_output)
    return [h for h, (_, witness) in hooks.items() if h not in have and not (witness and witness.encode() in blob)]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    elf = argv[1]
    nm = argv[argv.index("--nm") + 1] if "--nm" in argv else "arm-vita-eabi-nm"
    try:
        text = subprocess.run([nm, "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"check_elf_hooks: cannot run {nm} on {elf}: {e}")
        return 2
    with open(elf, "rb") as f:
        blob = f.read()
    bad = missing(text, blob)
    if bad:
        for h in bad:
            print(f"MISSING {h}: {HOOKS[h][0]}")
        return 1
    print(f"check_elf_hooks: all {len(HOOKS)} hooks are defined in {elf}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
