/* vlink linker script for building gdbstub_test.elf, an ELF executable
 * with DWARF debug info, linked at address 0 like TOS programs are,
 * with TEXT, DATA and BSS contiguous.
 */
PHDRS { text PT_LOAD; }
SECTIONS {
  . = 0;
  .text : { *(.text) } :text
  .data : { *(.data) } :text
  .bss : { *(.bss) } :text
  .debug_aranges 0 : { *(.debug_aranges) }
  .debug_info 0 : { *(.debug_info) }
  .debug_abbrev 0 : { *(.debug_abbrev) }
  .debug_ranges 0 : { *(.debug_ranges) }
  .debug_line 0 : { *(.debug_line) }
}
