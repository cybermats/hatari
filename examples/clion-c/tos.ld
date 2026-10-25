SECTIONS {
  . = 0;
  .text : { *(.text.start) *(.text) *(.text.*) *(.rodata) *(.rodata.*) }
  .data : { *(.data) *(.data.*) }
  .bss : { *(.bss) *(.bss.*) *(COMMON) }
}
