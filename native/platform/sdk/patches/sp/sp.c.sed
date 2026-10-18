# The sound table of an audio bank is big-endian on the disc: convert it in place once (src/fmt_endian.c).
s/^void SPInitSoundTable(SPSoundTable\* table, u32 aramBase, u32 zeroBase) {$/extern void NpSpFixEndian(void*);\n&\n    NpSpFixEndian(table);/
