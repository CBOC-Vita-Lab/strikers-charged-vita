#!/usr/bin/env python3
"""Post-link check of a Vita game image: which weak references stayed null.

A weak reference never pulls an archive member (vita_require.mk says why). This lists every weak UNDEFINED symbol of the linked ELF (arm-vita-eabi-nm: "w"/"v"
with no address) and compares it with the table below of the ones that are meant to be null in the player build. Anything else is an unintended null: a hook or
a Sce import whose feature is silently off. Exit 1 on an unexpected one; "PENDING" entries (the four sceJpeg calls of images up to r7) are warnings, and with --strict they are errors (images from the jpeg guard on).

    python3 tools/check-image-weak.py /out/v13-w8r7/game/vita/pkg/np/mscharged-np.elf
"""
import subprocess
import sys

MEANT_NULL = {
    # toolchain / libc
    "_ITM_deregisterTMCloneTable": "toolchain (transactional memory clone tables)",
    "_ITM_registerTMCloneTable": "toolchain",
    "__deregister_frame_info": "toolchain (crtstuff)",
    "__register_frame_info": "toolchain (crtstuff)",
    "__gnu_Unwind_Find_exidx": "toolchain: libgcc falls back to the linker's __exidx_start",
    "__libc_fini": "toolchain",
    "_pthread_stack_default_user": "pthread-embedded default stack size; the pthread_create wrap (np_vita_process.cpp) gives every attribute-less thread 1 MiB",
    "_ZTHN8AxDspHle13t_onMixWorkerE": "TLS init wrapper of a constant-initialised thread_local (ax_memory.cpp): nothing to run",
    # lab / harness / profiling hooks, absent from the player build by design
    "NpVitaEndRun": "lab status file hook (np_harness.cpp)",
    "NpVitaGameDataOptional": "smoke application only (np_vita_main.cpp)",
    "_Z14glxGetSwapModev": "harness (np_harness_game.cpp)",
    "__gcov_dump": "PGO-generate builds only",
    "__llvm_profile_write_file": "PGO-generate builds only",
    "np_vita_alloc_fixed": "provided by the recompiled build (kubridge fixed-address blocks); native: any address (np_plat_vita.cpp)",
}
# Meant to be null for a stated reason (kubridge: decision of the coordinator, 13 Nov 2026).
MEANT_NULL.update({
    "kuKernelFlushCaches": "kubridge is an OPTIONAL plug-in: a strong import of a kernel module that is not installed could stop the app from loading. The weak stub stays unrequired; "
                           "only the cached-arena flush uses it (default arena is CDRAM; the fallback is the sync-vm flush).",
    "kuKernelRegisterExceptionHandler": "same: optional kubridge. Without it np_vita_crash.cpp logs 'kubridge is not loaded: no on-screen crash report' and the game runs.",
})
# The four sceJpeg calls must be DEFINED from r8 on (Makefile.vpk NP_JPEG_REQUIRE): weak-undefined here is a regression, hardware JPEG is dead again.
# For an older image (r7) they are listed as pending.
PENDING = {
    "sceJpegInitMJpegWithParam": "r7 and earlier only: SceJpeg weak stub member not pulled, MSC_THP_HWJPEG could never open the codec",
    "sceJpegFinishMJpeg": "same",
    "sceJpegGetOutputInfo": "same",
    "sceJpegDecodeMJpegYCbCr": "same",
}
if "--strict" in sys.argv:
    sys.argv.remove("--strict")
    PENDING = {}


def weak_undefined(elf):
    out = subprocess.run(["arm-vita-eabi-nm", elf], capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in ("w", "v"):
            names.add(parts[1])
    return names


def env_lock_sizes(elf):
    """newlib's getenv/setenv lock through __env_lock/__env_unlock, which are EMPTY (2 bytes, a bare return) in the SDK's libc. np_vita_envlock.cpp defines real ones."""
    out = subprocess.run(["arm-vita-eabi-nm", "-S", elf], capture_output=True, text=True, check=True).stdout
    sizes = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] in ("__env_lock", "__env_unlock"):
            sizes[parts[3]] = int(parts[1], 16)
    return sizes


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    found = weak_undefined(argv[1])
    bad = sorted(n for n in found if n not in MEANT_NULL and n not in PENDING)
    for n in sorted(found):
        if n in PENDING:
            print("PENDING %-36s %s" % (n, PENDING[n]))
    for n in bad:
        print("UNEXPECTED NULL %s" % n)
    stale = sorted(n for n in list(MEANT_NULL) + list(PENDING) if n not in found)
    for n in stale:
        print("note: %s is no longer a weak undefined (defined now, or not referenced)" % n)
    sizes = env_lock_sizes(argv[1])
    for n in ("__env_lock", "__env_unlock"):
        if sizes.get(n, 0) <= 8:
            print("ENV LOCK NOT REAL: %s is %s bytes (the SDK's empty one is 2)" % (n, sizes.get(n, "missing")))
            bad.append(n)
    print("%d weak undefined symbols: %d meant, %d pending, %d unexpected" % (len(found), len([n for n in found if n in MEANT_NULL]), len([n for n in found if n in PENDING]), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
