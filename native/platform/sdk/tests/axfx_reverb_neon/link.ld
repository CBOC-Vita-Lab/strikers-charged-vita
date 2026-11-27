ENTRY(TestMain)
SECTIONS {
  . = 0x10000;
  .text : { *(.text .text.*) *(.rodata .rodata.*) }
  .ARM.exidx : { *(.ARM.exidx*) }
  . = ALIGN(0x1000);
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.* COMMON) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
