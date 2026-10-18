# The palette's headers are big-endian on the disc: convert them in place once (src/fmt_endian.c).
s/^void TPLBind(TPLPalette\* pal) {$/extern void NpTplFixEndian(void*);\n&\n    NpTplFixEndian(pal);/
