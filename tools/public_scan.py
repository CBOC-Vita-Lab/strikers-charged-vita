#!/usr/bin/env python3
"""public_scan.py: scan what is about to be published for things that must never ship (tools15; the idea of Galaxy's public_scan.py, adapted to this repository).

    python3 -I tools/public_scan.py [--tree DIR] [--package FILE.vpk|.zip ...] [--patterns FILE] [--strict] [--no-git|--git-messages]

  --tree DIR      the release tree (default: this repository). Files = `git ls-files` when DIR is a git work tree, else every file below it (.git skipped).
                  The author/committer names and e-mails of the history of HEAD are scanned too (--no-git leaves that out; --git-messages adds the commit messages: the
                  internal branches carry session links and notes there, so use it only on the history that is really going to be published).
  --package F     a release package (.vpk / .zip): every member's NAME and its CONTENT are scanned. May be given more than once.
  --patterns F    the list of PERSONAL strings (owner names, e-mail addresses, nicknames, private host names ...). Default: the file named by the environment variable
                  STRIKERS_PUBLIC_SCAN_PATTERNS. THE LIST IS NEVER COMMITTED: it names the very strings that must not be in the repository, so keep it outside the repo
                  (for example ~/.config/strikers/public-scan-patterns.txt) and point the variable at it. Format, one per line: `#` starts a comment; a plain line is a
                  case-insensitive literal; `re:<regex>` is a regular expression (case-insensitive); `sha256:<64 hex>` is a file content that must not ship (for example a
                  known Nintendo art file). A report never prints a personal pattern, only its line number in that file, so a log of a scan leaks nothing.
  --strict        a missing or empty personal-pattern list is a failure (exit 3) instead of a warning. The release gate sets it with PUBLIC_SCAN_STRICT=1.

Built in (no personal data, safe to commit):
  NAMES (tree files and package members): libshacccg (the owner's console supplies it; it never ships), firmware (*.pup, PSP2UPDAT, os0/ vs0/ sce_module/), game data
  (disc images .iso .gcm .rvz .wbfs .ciso .gcz .tgc, the disc's system files, .dol, .arc .szs .brres .brlyt .brlan .bti .tpl .thp .bnr), the HD texture pack (hdtex.pack, *.hdpack,
  hdpack folders), Nintendo art (a path component `nintendo`, or character-art image files named after the game's characters).
  CONTENT: private keys, GitHub / Anthropic / AWS / Slack / Google tokens, `api token / secret / password = <long value>` assignments, Cloudflare account ids and API tokens,
  claude.ai session links.
Exit: 0 clean, 1 findings (one line each: where, rule, a masked excerpt), 2 usage, 3 --strict without a personal-pattern list.
"""
import hashlib
import os
import re
import subprocess
import sys
import zipfile

NAME_RULES = [
    ("libshacccg", re.compile(r"libshacc?cg|shacccg\.suprx", re.I)),
    ("firmware", re.compile(r"(^|/)PSP2UPDAT|\.pup$|(^|/)(os0|vs0|sce_module)/", re.I)),
    ("game data (disc image)", re.compile(r"\.(iso|gcm|rvz|wbfs|ciso|gcz|tgc)$|\.nkit\.", re.I)),
    ("game data (disc system file)", re.compile(r"(^|/)(boot\.bin|bi2\.bin|fst\.bin|apploader\.img|opening\.bnr|banner\.bnr)$", re.I)),
    ("game data (extension)", re.compile(r"\.(dol|arc|szs|brres|brlyt|brlan|bti|tpl|thp|bnr)$", re.I)),
    ("HD pack", re.compile(r"(^|/)hdtex\.pack$|\.hdpack$|(^|/)hd[-_ ]?(tex[-_ ]?)?pack(/|$)", re.I)),
    ("Nintendo art path", re.compile(r"(^|/)[^/]*nintendo[^/]*(/|$)|(^|/)[^/]*(mario|luigi|peach|yoshi|wario|waluigi|bowser|donkey[-_ ]?kong|daisy|toad)[^/]*\.(png|jpe?g|tga|dds|bmp|gif|tpl|bti)$", re.I)),
]
# content rules: (name, regex over bytes)
CONTENT_RULES = [
    ("private key", re.compile(rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY")),
    ("GitHub token", re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{20,}")),
    ("Anthropic key", re.compile(rb"\bsk-ant-[A-Za-z0-9_-]{20,}")),
    ("AWS key id", re.compile(rb"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("Slack token", re.compile(rb"\bxox[abposr]-[A-Za-z0-9-]{10,}")),
    ("Google API key", re.compile(rb"\bAIza[0-9A-Za-z_-]{35}")),
    ("secret assignment", re.compile(rb"(?i)\b(?:api[_-]?token|api[_-]?key|auth[_-]?token|secret[_-]?key|client[_-]?secret|password|passwd)[\"']?\s*[:=]\s*[\"']?[A-Za-z0-9_\-+/=]{24,}")),
    ("Cloudflare account id", re.compile(rb"(?i)(?:account[_-]?id|accountid|CF_ACCOUNT)[\"']?\s*[:=]\s*[\"']?[0-9a-f]{32}\b")),
    ("Cloudflare API token", re.compile(rb"(?i)(?:CLOUDFLARE|CF)_API_(?:TOKEN|KEY)[\"']?\s*[:=]\s*[\"']?[A-Za-z0-9_\-]{20,}")),
    ("Cloudflare dashboard link with an id", re.compile(rb"dash\.cloudflare\.com/[0-9a-f]{32}")),
    ("claude.ai session link", re.compile(rb"claude\.ai/(?:code|chat)/[A-Za-z0-9_-]{8,}")),
]
MAX_SCAN = 64 << 20


class Personal:
    def __init__(self, path):
        self.path, self.lits, self.res, self.hashes = path, [], [], {}
        self.count = 0
        if not path:
            return
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                s = line.strip()
                if not s or s.startswith("#"):
                    continue
                self.count += 1
                if s.startswith("sha256:"):
                    self.hashes[s[7:].strip().lower()] = n
                elif s.startswith("re:"):
                    self.res.append((n, re.compile(s[3:].encode("utf-8"), re.I)))
                else:
                    self.res.append((n, re.compile(re.escape(s.encode("utf-8")), re.I)))

    def find(self, data):
        return [n for n, rx in self.res if rx.search(data)]


def mask(b):
    s = b.decode("latin-1", "replace")
    s = "".join(c if c.isprintable() else "?" for c in s)
    return s[:4] + "..." + "(%d chars)" % len(s) if len(s) > 8 else "(%d chars)" % len(s)


def scan_name(where, name, out):
    for rule, rx in NAME_RULES:
        if rx.search(name):
            out.append("%s: name rule '%s'" % (where, rule))


def scan_data(where, data, pers, out, label=""):
    for rule, rx in CONTENT_RULES:
        m = rx.search(data)
        if m:
            line = data.count(b"\n", 0, m.start()) + 1
            out.append("%s%s: line %d: content rule '%s': %s" % (where, label, line, rule, mask(m.group(0))))
    for n in pers.find(data):
        out.append("%s%s: personal pattern #%d of the list matches (not printed)" % (where, label, n))
    if pers.hashes:
        h = hashlib.sha256(data).hexdigest()
        if h in pers.hashes:
            out.append("%s%s: content hash is on the list (line %d): must not ship" % (where, label, pers.hashes[h]))


def tree_files(tree):
    try:
        r = subprocess.run(["git", "-C", tree, "ls-files", "-z"], capture_output=True, timeout=120)
        if r.returncode == 0 and r.stdout:
            return [p.decode("utf-8", "surrogateescape") for p in r.stdout.split(b"\0") if p], True
    except (OSError, subprocess.SubprocessError):
        pass
    files = []
    for dp, dn, fn in os.walk(tree):
        dn[:] = [d for d in dn if d != ".git"]
        files += [os.path.relpath(os.path.join(dp, f), tree) for f in fn]
    return files, False


def scan_tree(tree, pers, use_git, out):
    files, is_git = tree_files(tree)
    for rel in files:
        scan_name(rel, rel, out)
        p = os.path.join(tree, rel)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        if os.path.getsize(p) > MAX_SCAN:
            out.append("%s: larger than %d MiB: not scanned (a release tree has no such file)" % (rel, MAX_SCAN >> 20))
            continue
        with open(p, "rb") as f:
            scan_data(rel, f.read(), pers, out)
    if use_git and is_git:
        fmt = "%an%n%ae%n%cn%n%ce%n%B%x00" if use_git == "messages" else "%an%n%ae%n%cn%n%ce%x00"
        r = subprocess.run(["git", "-C", tree, "log", "--format=" + fmt], capture_output=True, timeout=300)
        if r.returncode == 0:
            for i, chunk in enumerate(r.stdout.split(b"\0")):
                scan_data("git history", chunk, pers, out, " (commit #%d from the newest)" % (i + 1))
    return len(files)


def scan_package(pkg, pers, out):
    n = 0
    with zipfile.ZipFile(pkg) as z:
        for zi in z.infolist():
            if zi.is_dir():
                continue
            n += 1
            where = "%s!%s" % (os.path.basename(pkg), zi.filename)
            scan_name(where, zi.filename, out)
            if zi.file_size > MAX_SCAN:
                out.append("%s: larger than %d MiB: not scanned" % (where, MAX_SCAN >> 20))
                continue
            scan_data(where, z.read(zi), pers, out)
    return n


def main(argv):
    tree, pkgs, patterns, strict, use_git, i = None, [], os.environ.get("STRIKERS_PUBLIC_SCAN_PATTERNS") or None, os.environ.get("PUBLIC_SCAN_STRICT") == "1", True, 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__)
            return 2
        elif a == "--tree" and i + 1 < len(argv):
            tree = argv[i + 1]; i += 2
        elif a == "--package" and i + 1 < len(argv):
            pkgs.append(argv[i + 1]); i += 2
        elif a == "--patterns" and i + 1 < len(argv):
            patterns = argv[i + 1]; i += 2
        elif a == "--strict":
            strict = True; i += 1
        elif a == "--no-git":
            use_git = False; i += 1
        elif a == "--git-messages":
            use_git = "messages"; i += 1
        else:
            print(__doc__)
            return 2
    if tree is None and not pkgs:
        tree = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    if patterns and not os.path.isfile(patterns):
        print("public_scan: the pattern list %s does not exist" % patterns)
        return 3 if strict else 2
    pers = Personal(patterns)
    if pers.count == 0:
        msg = "public_scan: no personal-pattern list (set STRIKERS_PUBLIC_SCAN_PATTERNS to a file kept OUTSIDE the repository, or --patterns): owner names and e-mails are NOT checked"
        if strict:
            print(msg + ": --strict")
            return 3
        print("WARNING " + msg)
    if tree and os.path.realpath(patterns or os.devnull).startswith(os.path.realpath(tree) + os.sep):
        print("public_scan: the pattern list is inside the tree being scanned: it would be committed. Move it outside the repository.")
        return 2
    out, nfiles = [], 0
    if tree:
        nfiles += scan_tree(tree, pers, use_git, out)
    for p in pkgs:
        nfiles += scan_package(p, pers, out)
    for line in out:
        print("FOUND " + line)
    print("public_scan: %d files/members scanned, %d personal patterns, %d findings: %s" % (nfiles, pers.count, len(out), "FAIL" if out else "CLEAN"))
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
