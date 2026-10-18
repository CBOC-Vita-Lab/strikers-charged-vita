/* 4 KiB of read-only padding. Linked in only when the code segment happens to end within
 * 3 KiB of the data segment's start, where vita-elf-create needs room for its module
 * data (tools/seg_gap.sh explains; Makefile.vita and Makefile.vitaprobe do the relink). */
__attribute__((used, section(".rodata.msc_segment_pad"))) const char msc_segment_pad[4096] = {1};
