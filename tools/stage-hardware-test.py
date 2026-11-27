#!/usr/bin/env python3
"""Stage isolated Strikers source snapshots and fail-closed test VPKs.

stage --source DIR --destination NEW_DIR accepts exported v1.1 or current
snapshots. package --built VPK --donor PUBLIC_VPK --output NEW_VPK requires
the independently recorded built eboot SHA-256. verify performs the same gate.
No device operations, builds, source mutation, or production repacking occur.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys
import zipfile
import zlib

TITLE = "CDXMP0001"
PRIVATE = "ux0:data/mscharged-perf-test"
DISC = "ux0:data/mscharged/game"
MAIN = "native/platform/vita/src/np_vita_main.cpp"
OS = "native/platform/sdk/src/os_core.cpp"
NAND = "native/platform/sdk/src/nand_native.cpp"
SETTINGS = "native/platform/vita/src/np_vita_settings.cpp"
MAKE = "native/platform/vita/vita.mk"
VPK_MAKE = "native/platform/vita/Makefile.vpk"
CACHES = ("dependencies/vitaGL/source/ffp.c", "dependencies/vitaGL/source/vgl.c")
REQUIRED = (MAIN, OS, NAND, SETTINGS, MAKE, VPK_MAKE, *CACHES)
ROOT_FILES = ("THIRD_PARTY_NOTICES.md",)
HEADER = "native/platform/vita/src/np_hardware_isolation.h"
MAX_FILES = 40000
MAX_FILE = 64 * 1024 * 1024
MAX_TOTAL = 1024 * 1024 * 1024
SKIP = {".git", "build", "__pycache__", ".pytest_cache", ".superpowers"}
SOURCE_ROOTS = {"native", "renderer", "dependencies", "licenses", "tools"}
TEXT_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".mk"}
PATH_NAMES = (
    "MSC_INPUT_SCRIPT", "MSC_SYMBOLS", "MSC_DIAG_DIR", "MSC_HDTEX_PATH",
    "MSC_VGX_PRESENT_CSV", "MSC_VGX_TEXDUMP", "MSC_DUMP_DIR",
    "MSC_VGX_STATS", "MSC_VGX_FRAMESTATS", "MSC_VGX_SIM_CACHE", "MSC_VGX_GLSTATS",
    "MSC_VGX_PROGRESS_DUMP", "MSC_VGX_CAPTURE",
    "MSC_VGX_SHADERLIST", "MSC_VGX_DRAWLOG", "NP_AUDIO_WAV",
    "MSC_VGX_SHADERTEXT_LOG",   # (17 Nov, pack24: the shader text log of tools/pack17 was refused as "a path in a key not on the list", so no console miss could be turned into a shader text)
    "NP_DET_TRACE",   # (15 Nov: the plain-vs-PGO det trace; without it the line was refused and no det.txt was ever written)
)
PINS = {"NP_DATA_DIR": PRIVATE + "/np-data", "MSC_GAME_ROOT": DISC, "NP_DISC_DIR": DISC,
        "NP_IMPORT_SAVE": "0", "MSC_VIRTUAL_TIME": "0", "NP_VIRTUAL_TIME": "0",
        "MSC_FPS_MODEL": "0", "MSC_VGX_PRESENT_CLOCK": "real", "MSC_DYNRES_FAKE": "0",
        "MSC_DYNRES_CYCLE": "0"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(path, *, new=False):
    path = Path(os.path.abspath(path))
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError("Symlink path is forbidden: " + str(path))
    if new and path.exists():
        raise ValueError("Destination must not exist: " + str(path))
    if new and (any(p.casefold() in {"mscn00003", "mscharged"} for p in path.parts)
                or path.parts[1:2] in (("Volumes",), ("media",), ("mnt",))):
        raise ValueError("Protected/device destination is forbidden")
    return path


def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise ValueError("Unsupported source shape: " + label)
    return text.replace(old, new, 1)


def guard_header(disc=DISC):
    pins_map = dict(PINS, MSC_GAME_ROOT=disc, NP_DISC_DIR=disc)
    paths = ", ".join(json.dumps(x) for x in PATH_NAMES)
    pins = "\n".join('    setenv(%s, %s, 1);' % (json.dumps(k), json.dumps(v)) for k, v in pins_map.items())
    checks = "\n".join('    if (!strcmp(name, %s)) return !strcmp(value, %s);' % (json.dumps(k), json.dumps(v)) for k, v in pins_map.items())
    return '''// Generated hardware-test isolation: startup/config checks only, never a frame hook.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
static inline bool NpHardwareDataAllowed(const char* p) {
    return p && !strcmp(p, "ux0:data/mscharged-perf-test/np-data");
}
static inline bool NpHardwarePrivatePath(const char* p) {
    const char* root = "ux0:data/mscharged-perf-test/";
    if (!p || strncmp(p, root, strlen(root))) return false;
    p += strlen(root);
    if (!*p || strchr(p, ':') || strchr(p, '\\\\')) return false;
    while (*p) {
        const char* end = strchr(p, '/');
        const size_t n = end ? size_t(end - p) : strlen(p);
        if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (!end) return true;
        p = end + 1;
        if (!*p) return false;
    }
    return false;
}
static inline bool NpHardwareAccept(const char* name, const char* value) {
    if (!name || !*name || !value) return false;
    for (const char* p = name; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
''' + checks + '''
    const char* paths[] = {''' + paths + '''};
    for (const char* key : paths) if (!strcmp(name, key)) {
        if (NpHardwarePrivatePath(value)) return true;
        std::fprintf(stderr, "[vita] test build: %s=%s refused (a path outside the test title's own folder)\\n", name, value);
        return false;
    }
    // MSC_STADIUM_CUT uses <stadium_digits>:<mask> syntax (e.g. 4:21248, 13:16, table,4:0x5300);
    // validate its strict numeric grammar here before the generic ':' device-path rejection below.
    if (!strcmp(name, "MSC_STADIUM_CUT")) {
        if (!strcmp(value, "0") || !strcmp(value, "off") || !strcmp(value, "none") || !strcmp(value, "table")) return true;
        auto isNum = [](const char*& p) -> bool {
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2; const char* s = p;
                while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) ++p;
                return p > s;
            }
            const char* s = p;
            while (*p >= '0' && *p <= '9') ++p;
            return p > s;
        };
        const char* p = value;
        if (isNum(p) && !*p) return true;
        p = value;
        bool any = false;
        while (*p) {
            while (*p == ',' || *p == ' ') ++p;
            if (!*p) break;
            any = true;
            if (!strncmp(p, "table", 5) && (!p[5] || p[5] == ',' || p[5] == ' ')) { p += 5; continue; }
            const char* d = p;
            while (*p >= '0' && *p <= '9') ++p;
            if (p == d || *p != ':') {
                std::fprintf(stderr, "[vita] test build: %s refused (invalid stadium-cut value)\\n", name);
                return false;
            }
            ++p;
            if (!isNum(p) || (*p && *p != ',' && *p != ' ')) {
                std::fprintf(stderr, "[vita] test build: %s refused (invalid stadium-cut value)\\n", name);
                return false;
            }
        }
        return any;
    }
    // Unknown arbitrary NAME=VALUE entries must not introduce a path escape.
    if (strchr(value, '/') || strchr(value, '\\\\') || strchr(value, ':') || strstr(value, "..")) {
        std::fprintf(stderr, "[vita] test build: %s refused (a path in a key that is not on the test build's path list)\\n", name);
        return false;
    }
    return true;
}
static inline void NpHardwarePinEnvironment() {
''' + pins + '''
}
struct NpHardwareEnvScope {
    NpHardwareEnvScope() { NpHardwarePinEnvironment(); }
    ~NpHardwareEnvScope() { NpHardwarePinEnvironment(); }
};
'''


def transform(name, raw, disc=DISC, logging=False):
    if Path(name).suffix not in TEXT_SUFFIXES and name != VPK_MAKE:
        return raw
    text = raw.decode("utf-8")
    text = re.sub(r"(?:ux0:/data|ux0:data|/data)/mscharged(?=/|[\"\s\\),;]|$)",
                  lambda match: match.group(0).replace("mscharged", "mscharged-perf-test"), text)
    # The sole shared read-only exception is the extracted disc, never save/cache state.
    text = re.sub(r"(?:ux0:/data|ux0:data|/data)/mscharged-perf-test/game(?=/|[\"\s\\),;]|$)", disc, text)
    if name.startswith("dependencies/vitaGL/"):
        text = text.replace("ux0:data/shader_cache", PRIVATE + "/ffp-cache")
        text = text.replace("ux0:data/vgl_cache", PRIVATE + "/vgl-cache")
        text = text.replace("ux0:data/vitagl_gpucrash.sgx", PRIVATE + "/gpucrash.sgx")
    if name == MAIN:
        text = '#include "np_hardware_isolation.h"\n' + text
        start = text.index("void ImportRecompiledSave() {")
        end = text.index("\n}\n", start) + 2
        text = text[:start] + "void ImportRecompiledSave() { /* Disabled unconditionally in isolated tests. */ }" + text[end:]
        text = replace_once(text, "void LoadEnvFile() {", "void LoadEnvFile() {\n    NpHardwareEnvScope hardware_scope;", "config entry")
        text = replace_once(text, "            setenv(line, equals + 1, 1);", "            if (!NpHardwareAccept(line, equals + 1)) {\n                line = end ? end + 1 : nullptr;\n                continue;\n            }\n            setenv(line, equals + 1, 1);", "config assignment")
    if name == OS:
        text = replace_once(text, 'c->disc_dir = env_str("NP_DISC_DIR", "' + disc + '");', 'c->disc_dir = "' + disc + '";', "SDK disc")
        text = replace_once(text, 'c->data_dir = env_str("NP_DATA_DIR", "' + PRIVATE + '");', 'c->data_dir = "' + PRIVATE + '/np-data";', "SDK data")
    if name == NAND:
        text = '#include "../../vita/src/np_hardware_isolation.h"\n' + text
        text = replace_once(text, "s32 NANDInit(void) {", 's32 NANDInit(void) {\n#if defined(__vita__)\n    if (!NpHardwareDataAllowed(NpSdk_GetConfig()->data_dir)) return NAND_RESULT_FATAL_ERROR;\n#endif', "NAND mkdir guard")
    if name == SETTINGS:
        text = '#include "np_hardware_isolation.h"\n' + text
        old = '''void Shot(const char* name, const uint32_t* pixels) {
    sceIoMkdir("ux0:/data/mscharged-perf-test/shots", 0777);
    char path[160];
    std::snprintf(path, sizeof(path), "ux0:/data/mscharged-perf-test/shots/settings-%s.ppm", name);
'''
        new = '''void Shot(const char* name, const uint32_t* pixels) {
    if (!name || !*name) return;
    for (const char* p = name; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return;
    char path[160];
    const int length = std::snprintf(path, sizeof(path), "ux0:data/mscharged-perf-test/shots/settings-%s.ppm", name);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(path) || !NpHardwarePrivatePath(path)) return;
    sceIoMkdir("ux0:/data/mscharged-perf-test/shots", 0777);
'''
        text = replace_once(text, old, new, "settings screenshot path guard")
    if name == MAKE:
        text = "# Generated isolated hardware-test profile; overrides apply to all build variants.\noverride NP_TITLE_ID := CDXMP0001\noverride NP_PLAYER := 1\noverride NP_RELEASE := 0\noverride NP_LOGGING := " + str(int(bool(logging))) + "\noverride LAB := 0\n" + text
    if name == VPK_MAKE:
        text = replace_once(text, "-s TITLE_ID=$(TID_game) -s APP_VER=", "-s TITLE_ID=$(TID_game) -s CONTENT_ID=EP9000-CDXMP0001_00-HARDWARETEST0000 -s APP_VER=", "test SFO content ID")
    if name == "native/platform/compat/np_touchdiag.cpp":
        text = text.replace('"MSCN00003"', '"CDXMP0001"')
        text = text.replace('\\"MSCN00003\\"', '\\"CDXMP0001\\"')
        text = text.replace("title=MSCN00003", "title=CDXMP0001")
    return text.encode("utf-8")


def stage(source, destination, *, private_disc=False, logging=False):
    source = safe_path(source)
    destination = safe_path(destination, new=True)
    if not source.is_dir() or source == destination or source in destination.parents or destination in source.parents:
        raise ValueError("Source and new destination must be disjoint directories")
    for name in REQUIRED:
        if not (source / name).is_file():
            raise ValueError("Missing required source: " + name)
    if (source / HEADER).exists():
        raise ValueError("Source is already isolated; use an original snapshot")
    originals = {}
    total = 0
    for name in ROOT_FILES:
        path = safe_path(source / name)
        if not path.is_file() or path.stat().st_size > MAX_FILE:
            raise ValueError("Missing/oversized root build input: " + name)
        originals[name] = path.read_bytes()
        total += len(originals[name])
    for root in sorted(SOURCE_ROOTS):
        folder = source / root
        if folder.is_symlink():
            raise ValueError("Top-level source root is a symlink: " + root)
        if not folder.exists():
            continue
        for current, dirs, files in os.walk(folder, followlinks=False):
            for child in dirs + files:
                if (Path(current) / child).is_symlink():
                    raise ValueError("Source symlink forbidden: " + child)
            dirs[:] = sorted(d for d in dirs if d not in SKIP)
            for child in sorted(files):
                path = Path(current) / child
                if child in SKIP or (path.suffix in {".pyc", ".o", ".a", ".vpk", ".elf", ".velf", ".suprx"} and child != "Makefile.vpk"):
                    continue
                size = path.stat().st_size
                total += size
                if not path.is_file() or size > MAX_FILE or total > MAX_TOTAL or len(originals) >= MAX_FILES:
                    raise ValueError("Source snapshot exceeds bounded regular-file staging policy")
                name = path.relative_to(source).as_posix()
                originals[name] = path.read_bytes()
    disc = PRIVATE + "/game" if private_disc else DISC
    outputs = {name: transform(name, raw, disc, logging) for name, raw in originals.items()}
    outputs[HEADER] = guard_header(disc).encode()
    changes = []
    diff = []
    for name in sorted(outputs):
        before, after = originals.get(name, b""), outputs[name]
        if before != after:
            changes.append({"path": name, "before_sha256": sha(before), "after_sha256": sha(after)})
            diff.extend(difflib.unified_diff(before.decode().splitlines(True), after.decode().splitlines(True), fromfile="a/" + name, tofile="b/" + name))
    manifest = {"schema": 1, "title_id": TITLE, "private_root": PRIVATE, "read_only_disc": disc,
                "profile": {"NP_PLAYER": 1, "NP_LOGGING": int(bool(logging)), "LAB": 0},
                "tool_sha256": sha(Path(__file__).read_bytes()), "changes": changes,
                "source_files": {name: sha(raw) for name, raw in sorted(originals.items())},
                "staged_files": {name: sha(raw) for name, raw in sorted(outputs.items())}}
    # All validation and transformations finish before creating the new destination.
    destination.mkdir(parents=True, exist_ok=False)
    for name, raw in sorted(outputs.items()):
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as out:
            out.write(raw)
        path.chmod(0o755 if (source / name).exists() and (source / name).stat().st_mode & 0o111 else 0o644)
    (destination / "hardware-isolation.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (destination / "hardware-isolation.diff").write_text("".join(diff))
    return manifest


def read_zip(path):
    path = safe_path(path)
    if not path.is_file() or path.stat().st_size > MAX_TOTAL:
        raise ValueError("ZIP must be a bounded regular file")
    members = {}
    total = 0
    try:
        with zipfile.ZipFile(path) as archive:
            if len(archive.infolist()) > MAX_FILES:
                raise ValueError("Too many ZIP members")
            seen = set()
            for item in archive.infolist():
                name = item.filename
                parts = PurePosixPath(name).parts
                if (not name or item.orig_filename != name or name.startswith("/") or "\\" in name or ":" in name or "\0" in name
                        or any(p in ("", ".", "..") for p in name.rstrip("/").split("/"))
                        or not parts or name.casefold() in seen or stat.S_ISLNK(item.external_attr >> 16)
                        or item.flag_bits & 1):
                    raise ValueError("Unsafe/duplicate ZIP member: " + name)
                seen.add(name.casefold())
                total += item.file_size
                if item.file_size > MAX_FILE or total > MAX_TOTAL:
                    raise ValueError("ZIP exceeds size bounds")
                if not item.is_dir():
                    members[name] = archive.read(item)
    except (zipfile.BadZipFile, RuntimeError, EOFError, NotImplementedError) as error:
        raise ValueError("Malformed ZIP: " + str(error)) from error
    return members


def parse_sfo(raw):
    try:
        magic, version, keys, data, count = struct.unpack_from("<4sIIII", raw)
        if magic != b"\0PSF" or version != 0x101 or count > 128 or not (20 + count * 16 <= keys <= data <= len(raw)):
            raise ValueError("Malformed SFO header")
        result = {}
        for i in range(count):
            key, fmt, length, maximum, offset = struct.unpack_from("<HHIII", raw, 20 + i * 16)
            if keys + key >= data or length > maximum or data + offset + maximum > len(raw):
                raise ValueError("Malformed SFO entry")
            keyend = raw.find(b"\0", keys + key, data)
            if keyend < 0:
                raise ValueError("Unterminated SFO key")
            name = raw[keys + key:keyend].decode("ascii")
            if name in result:
                raise ValueError("Duplicate SFO key")
            value = raw[data + offset:data + offset + length]
            if fmt == 0x204:
                if not value or value[-1:] != b"\0" or b"\0" in value[:-1]:
                    raise ValueError("Invalid SFO string")
                result[name] = value[:-1].decode("utf-8")
            else:
                result[name] = value
        return result
    except (struct.error, UnicodeError) as error:
        raise ValueError("Malformed SFO: " + str(error)) from error


ART = {"sce_sys/icon0.png", "sce_sys/pic0.png", "sce_sys/livearea/contents/bg.png",
       "sce_sys/livearea/contents/startup.png", "sce_sys/livearea/contents/template.xml",
       "loading.png", "sce_sys/livearea/contents/bg0.png", "sce_sys/livearea/contents/settings.png"}


def donor_allowed(name):
    return name in ART or name.startswith("shadercache/")


def verify(package_path, eboot_sha256):
    if not re.fullmatch(r"[0-9a-f]{64}", eboot_sha256):
        raise ValueError("An independently recorded built eboot SHA-256 is required")
    members = read_zip(package_path)
    if any(not (n in {"eboot.bin", "sce_sys/param.sfo"} or donor_allowed(n) or n.startswith("licenses/")) for n in members):
        raise ValueError("Package contains unapproved payloads")
    if "eboot.bin" not in members or "sce_sys/param.sfo" not in members:
        raise ValueError("Package missing executable/SFO")
    parsed = parse_sfo(members["sce_sys/param.sfo"])
    content = parsed.get("CONTENT_ID", "")
    if parsed.get("TITLE_ID") != TITLE or not isinstance(content, str) or not re.fullmatch(r"[A-Z]{2}[0-9]{4}-CDXMP0001_00-[A-Z0-9]{16}", content):
        raise ValueError("Protected, stale, missing or malformed SFO identity")
    eboot = members["eboot.bin"]
    if not eboot.startswith(b"SCE\0") or sha(eboot) != eboot_sha256:
        raise ValueError("Executable provenance mismatch or non-SELF executable")
    return {"title_id": TITLE, "content_id": content, "eboot_sha256": sha(eboot), "isolation_gate": "structure-only",
            "package_sha256": sha(Path(package_path).read_bytes()),
            "members": {name: sha(raw) for name, raw in sorted(members.items())}}


def read_bounded(path, limit=MAX_FILE):
    path = safe_path(path)
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError("Missing/oversized regular file: " + str(path))
    return path.read_bytes()


def elf_segments(raw):
    try:
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", raw)
        if (header[0][:7] != b"\x7fELF\x01\x01\x01" or header[2] != 40 or header[3] != 1
                or header[8] != 52 or header[9] != 32 or not 1 <= header[10] <= 128
                or header[5] < 52 or header[5] + header[10] * 32 > len(raw)):
            raise ValueError("Malformed/non-ARM ELF")
        segments = []
        for index in range(header[10]):
            phdr = struct.unpack_from("<8I", raw, header[5] + index * 32)
            if phdr[1] + phdr[4] > len(raw) or phdr[4] > MAX_FILE or (phdr[0] == 1 and phdr[5] < phdr[4]):
                raise ValueError("Malformed ELF segment")
            segments.append((phdr, raw[phdr[1]:phdr[1] + phdr[4]]))
        if not any(phdr[0] == 1 for phdr, _ in segments):
            raise ValueError("ELF has no loadable image")
        return header, segments
    except struct.error as error:
        raise ValueError("Truncated ELF") from error


def verify_self(eboot, velf):
    # Layout is VitaSDK vita-toolchain/src/self.h; compare the actual load data,
    # not merely a supplied digest. Both compressed and uncompressed fself work.
    header, segments = elf_segments(velf)
    # vita-make-fself updates sce_module_info_raw.module_nid in memory before
    # hashing/packing. Reproduce only that documented input transformation.
    modified = bytearray(velf)
    if header[1] == 0xFE00:
        module_offset = segments[0][0][1] + segments[0][0][3]
    elif header[1] == 0xFE04:
        segment_index, offset = header[4] >> 30, header[4] & 0x3fffffff
        if segment_index >= len(segments):
            raise ValueError("Invalid VELF module-info segment")
        module_offset = segments[segment_index][0][1] + offset
    else:
        raise ValueError("Expected Vita executable VELF")
    if module_offset + 92 > len(velf):
        raise ValueError("Truncated VELF module-info")
    nid = int.from_bytes(hashlib.sha256(hashlib.sha256(velf).digest()).digest()[:4], "big")
    struct.pack_into("<I", modified, module_offset + 52, nid)
    _, segments = elf_segments(modified)
    try:
        fields = struct.unpack_from("<4sIHHI14Q", eboot)
        if fields[0] != b"SCE\0" or fields[1] != 3 or fields[3] != 1 or fields[6] != len(velf) or fields[7] != len(eboot):
            raise ValueError("Malformed SELF header")
        elf_offset, phdr_offset, section_offset = fields[11], fields[12], fields[14]
        control_offset, control_size = fields[16], fields[17]
        if (elf_offset < 128 or elf_offset + 52 > len(eboot) or phdr_offset + len(segments) * 32 > len(eboot)
                or section_offset + len(segments) * 32 > len(eboot) or control_offset + control_size > len(eboot)):
            raise ValueError("SELF metadata escapes container")
        embedded = struct.unpack_from("<16sHHIIIIIHHHHHH", eboot, elf_offset)
        if embedded[0][:7] != header[0][:7] or embedded[1:5] != header[1:5] or embedded[10] != len(segments):
            raise ValueError("SELF/VELF headers differ")
        cursor, end, digest_found = control_offset, control_offset + control_size, False
        while cursor < end:
            kind, size, _, _ = struct.unpack_from("<4I", eboot, cursor)
            if size < 16 or cursor + size > end:
                raise ValueError("Malformed SELF control info")
            if kind == 4:
                if digest_found or size < 80 or eboot[cursor + 36:cursor + 68] != hashlib.sha256(modified).digest():
                    raise ValueError("SELF/VELF digest mismatch")
                digest_found = True
            cursor += size
        if not digest_found:
            raise ValueError("SELF missing VELF digest")
        for index, (phdr, expected) in enumerate(segments):
            actual_phdr = struct.unpack_from("<8I", eboot, phdr_offset + index * 32)
            if actual_phdr[:7] != phdr[:7]:
                raise ValueError("SELF/VELF segment headers differ")
            offset, length, compression, encryption = struct.unpack_from("<4Q", eboot, section_offset + index * 32)
            if encryption != 2 or offset < fields[5] or offset + length > len(eboot) or compression not in (1, 2):
                raise ValueError("Unsafe SELF segment")
            payload = eboot[offset:offset + length]
            if compression == 2:
                decoder = zlib.decompressobj()
                payload = decoder.decompress(payload, len(expected) + 1)
                padding = decoder.unused_data
                if not decoder.eof or len(padding) > 3 or padding.strip(b"\0") or decoder.unconsumed_tail:
                    raise ValueError("Malformed/compression-bomb SELF segment")
            else:
                padding = payload[len(expected):]
                if len(padding) > 3 or padding.strip(b"\0"):
                    raise ValueError("Invalid SELF segment padding")
                payload = payload[:len(expected)]
            if payload != expected:
                raise ValueError("SELF/VELF segment bytes differ")
    except (struct.error, zlib.error) as error:
        raise ValueError("Malformed SELF") from error


def verify_build(package_path, eboot_sha256, velf_path, stage_manifest):
    result = verify(package_path, eboot_sha256)
    manifest_path = safe_path(stage_manifest)
    manifest_raw = read_bounded(manifest_path, 16 * 1024 * 1024)
    try:
        manifest = json.loads(manifest_raw)
        disc = manifest["read_only_disc"]
        if (manifest["schema"] != 1 or manifest["title_id"] != TITLE or manifest["private_root"] != PRIVATE
                or disc not in {DISC, PRIVATE + "/game"} or manifest["profile"] not in ({"NP_PLAYER": 1, "NP_LOGGING": 0, "LAB": 0}, {"NP_PLAYER": 1, "NP_LOGGING": 1, "LAB": 0})
                or manifest["tool_sha256"] != sha(Path(__file__).read_bytes())):
            raise ValueError("Stale/unsafe source isolation manifest")
        files = manifest["staged_files"]
        if not isinstance(files, dict) or not 1 <= len(files) <= MAX_FILES or any(n not in files for n in (*REQUIRED, HEADER, *ROOT_FILES)):
            raise ValueError("Incomplete staged source inventory")
        for name, digest in files.items():
            path = PurePosixPath(name)
            if (path.is_absolute() or "\\" in name or ":" in name or any(p in {"", ".", ".."} for p in name.split("/"))
                    or not re.fullmatch(r"[0-9a-f]{64}", digest)):
                raise ValueError("Unsafe source inventory entry")
            if sha(read_bounded(manifest_path.parent / name)) != digest:
                raise ValueError("Staged source changed after isolation: " + name)
        if files[HEADER] != sha(guard_header(disc).encode()):
            raise ValueError("Isolation header differs from reviewed transformation")
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise ValueError("Malformed source manifest") from error
    velf = read_bounded(velf_path)
    _, segments = elf_segments(velf)
    load_data = b"\0".join(data for phdr, data in segments if phdr[0] == 1)
    for required in (TITLE, PRIVATE + "/np-data", PRIVATE + "/ffp-cache", disc):
        if required.encode() not in load_data:
            raise ValueError("Compiled isolation marker missing: " + required)
    if b"MSCN00003" in load_data or b"ux0:data/shader_cache" in load_data:
        raise ValueError("Protected title/global cache remains in compiled image")
    # Compiler-pooled format strings may mention an allowed disc path followed
    # by repair prose. Bound the path token, but check the whole literal for
    # traversal so whitespace cannot hide a later /../ continuation.
    roots = re.compile(rb"(?:ux0:/data|ux0:data|/data)/mscharged(?=/|[\x00\s]|$)[^\x00\s]*")
    for match in roots.finditer(load_data):
        value = match.group().decode("utf-8", errors="replace")
        literal_end = load_data.find(b"\0", match.start())
        literal = load_data[match.start():literal_end if literal_end >= 0 else len(load_data)]
        # Only the exact shared disc and its canonical read-only descendants survive.
        if (not (value == DISC or value.startswith(DISC + "/")) or "//" in value or "\\" in value
                or ":" in value[4:] or re.search(rb"[/\\]\.\.?(?=[/\\\x00\s]|$)", literal)):
            raise ValueError("Protected writable root remains in compiled image: " + value[:160])
    verify_self(read_zip(package_path)["eboot.bin"], velf)
    result.update(isolation_gate="compiled-and-source-verified", velf_sha256=sha(velf), stage_manifest_sha256=sha(manifest_raw), read_only_disc=disc)
    return result


def package(built, donor, output, eboot_sha256, *, velf=None, stage_manifest=None):
    output = safe_path(output, new=True)
    # Verify the new identity and executable BEFORE touching the output.
    if (velf is None) != (stage_manifest is None):
        raise ValueError("Both VELF and stage manifest are required for compiled verification")
    if velf is None:
        verify(built, eboot_sha256)
    else:
        verify_build(built, eboot_sha256, velf, stage_manifest)
    built_members, donor_members = read_zip(built), read_zip(donor)
    members = {n: raw for n, raw in built_members.items() if n in {"eboot.bin", "sce_sys/param.sfo"} or n.startswith("licenses/")}
    members.update({n: raw for n, raw in donor_members.items() if donor_allowed(n)})
    # 1.3 (front20): the LiveArea layout is the build's own (native/platform/vita/vpk/livearea/template.xml: no Settings tile, a newer content-rev), not the 1.2 donor's
    # (whose tile started the game with -settings). The art files stay the donor's.
    if "sce_sys/livearea/contents/template.xml" in built_members:
        members["sce_sys/livearea/contents/template.xml"] = built_members["sce_sys/livearea/contents/template.xml"]
    if not any(n.startswith("shadercache/") for n in members):
        raise ValueError("Public donor has no shader cache")
    with output.open("xb") as stream:
        with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, raw in sorted(members.items()):
                info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, raw)
    return verify(output, eboot_sha256) if velf is None else verify_build(output, eboot_sha256, velf, stage_manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    stage_parser = modes.add_parser("stage")
    stage_parser.add_argument("--source", required=True, type=Path)
    stage_parser.add_argument("--destination", required=True, type=Path)
    stage_parser.add_argument("--logging", action="store_true", help="Build with NP_LOGGING=1 so the isolated app writes its own log.txt under the private root (console timing sessions); the default is the quiet player build")
    stage_parser.add_argument("--private-disc", action="store_true", help="Pin assets to the disposable private game copy for fault-injection trials")
    package_parser = modes.add_parser("package")
    package_parser.add_argument("--built", required=True, type=Path)
    package_parser.add_argument("--donor", required=True, type=Path)
    package_parser.add_argument("--output", required=True, type=Path)
    package_parser.add_argument("--eboot-sha256", required=True)
    package_parser.add_argument("--velf", required=True, type=Path, help="Exact vita-elf-create output consumed by vita-make-fself")
    package_parser.add_argument("--stage-manifest", required=True, type=Path)
    verify_parser = modes.add_parser("verify")
    verify_parser.add_argument("--package", required=True, type=Path)
    verify_parser.add_argument("--eboot-sha256", required=True)
    verify_parser.add_argument("--velf", required=True, type=Path)
    verify_parser.add_argument("--stage-manifest", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "stage":
            result = stage(args.source, args.destination, private_disc=args.private_disc, logging=args.logging)
            print(json.dumps({"title_id": result["title_id"], "changed_files": len(result["changes"])}, sort_keys=True))
        elif args.mode == "package":
            print(json.dumps(package(args.built, args.donor, args.output, args.eboot_sha256, velf=args.velf, stage_manifest=args.stage_manifest), indent=2, sort_keys=True))
        else:
            print(json.dumps(verify_build(args.package, args.eboot_sha256, args.velf, args.stage_manifest), indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError) as error:
        print("STOP: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
