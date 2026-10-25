PHDRS { text PT_LOAD; }
SECTIONS {
  . = 0;
  .text : { *(.text.start) *(.text) *(.text.*) *(.rodata) *(.rodata.*) } :text
  .data : { *(.data) *(.data.*) } :text
  .bss : { *(.bss) *(.bss.*) *(COMMON) } :text
  .debug_aranges 0 : { *(.debug_aranges) }
  .debug_info 0 : { *(.debug_info) }
  .debug_abbrev 0 : { *(.debug_abbrev) }
  .debug_ranges 0 : { *(.debug_ranges) }
  .debug_loc 0 : { *(.debug_loc) }
  .debug_frame 0 : { *(.debug_frame) }
  .debug_pubnames 0 : { *(.debug_pubnames) }
  .debug_pubtypes 0 : { *(.debug_pubtypes) }
  .debug_line 0 : { *(.debug_line) }
  .debug_str 0 : { *(.debug_str) }
  .debug_line_str 0 : { *(.debug_line_str) }
  .debug_macro 0 : { *(.debug_macro) }
}
