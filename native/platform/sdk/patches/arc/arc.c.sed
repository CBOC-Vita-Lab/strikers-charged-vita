# The archive's structure words are big-endian on the disc: convert them in place once (src/fmt_endian.c).
s/^BOOL ARCInitHandle(void\* arcStart, ARCHandle\* handle) {$/extern void NpArcFixEndian(void*);\n&\n    NpArcFixEndian(arcStart);/
