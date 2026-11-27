#!/usr/bin/env python3
"""Does the game's bundled root list cover the server it will talk to?   (Run by a person; this one DOES connect.)

    python3 tools/net/check_https_roots.py HOST [PORT]

Connects to HOST with TLS, verifying against ONLY the roots of platform/sdk/net/np_https_roots.h (not the system store), and prints
the chain the server sent, each certificate's issuer, whether the bundled roots validate it, and the first problem if not.
Do this once the Worker's real address is known, and again whenever Cloudflare announces a CA change."""
import re
import socket
import ssl
import sys
from pathlib import Path

HEADER = Path(__file__).resolve().parents[2] / "platform/sdk/net/np_https_roots.h"


def bundled_pem():
    text = HEADER.read_text()
    pems = []
    for m in re.finditer(r"static const unsigned char (np_root_\w+)\[\] = \{(.*?)\};", text, re.S):
        if m.group(1).endswith("_sha256"):
            continue
        der = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(2)))
        pems.append(ssl.DER_cert_to_PEM_cert(der))
    return "".join(pems)


def main():
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 443
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)   # (not create_default_context: that would also load the system store)
    ctx.load_verify_locations(cadata=bundled_pem())
    ctx.check_hostname = True
    ctx.verify_mode = ssl.CERT_REQUIRED
    bad = 0
    for version in (ssl.TLSVersion.TLSv1_3, ssl.TLSVersion.TLSv1_2):
        ctx.minimum_version = ctx.maximum_version = version
        try:
            with socket.create_connection((host, port), timeout=10) as raw, ctx.wrap_socket(raw, server_hostname=host) as s:
                print("%s %s (%s): the bundled roots ACCEPT the chain" % (host, s.version(), s.cipher()[0]))
        except ssl.SSLCertVerificationError as e:
            bad = 1
            print("%s %s: the bundled roots REFUSE the chain: %s" % (host, version.name, e.verify_message))
        except (ssl.SSLError, OSError) as e:
            print("%s %s: not tested (%s)" % (host, version.name, e))
    if bad:
        print("Look at the chain the server sends with:\n  openssl s_client -showcerts -connect %s:%d -servername %s </dev/null | grep -E ' s:| i:'" % (host, port, host))
        print("then add the root that signs its top certificate to ROOTS in tools/net/gen_https_roots.py and regenerate np_https_roots.h.")
    return bad


if __name__ == "__main__":
    sys.exit(main())
