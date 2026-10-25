; Test program for the Hatari GDB stub test (gdbstub_test.py).
;
; Works in TOS-less (--tos none) mode.  Code is position independent
; (no relocations), so TEXT contents in memory match the file contents.
; Python test uses the fixed instruction offsets given below, so if this
; is changed, update the offsets in gdbstub_test.py too.
;
; Both the TOS program and the ELF symbol file (with DWARF line info,
; used by gdbstub_gdb_test.py) are committed, so that tests don't need
; an assembler.  They're built with vasm & vlink:
;   vasmm68k_mot -Ftos -no-opt -o gdbstub_test.tos gdbstub_test.s
;   vasmm68k_mot -Felf -dwarf=3 -no-opt -o gdbstub_test.o gdbstub_test.s
;   vlink -belf32m68k -nostdlib -T gdbstub_test.ld -o gdbstub_test.elf gdbstub_test.o
;
; Note: gdbstub_gdb_test.py refers to source line numbers of this file.

	section .text,code
start:					; offset 0
	move.l	#$12345678,d0		; 0: 6 bytes
	moveq	#0,d1			; 6: 2 bytes
loop:
	addq.l	#1,d1			; 8: 2 bytes
	cmpi.l	#100,d1			; 10: 6 bytes
	bne.s	loop			; 16: 2 bytes
bp_here:
	move.l	d1,d2			; 18: breakpoint test location
	lea	counter(pc),a0		; 20: step lands here
	move.l	d1,(a0)			; 24: watchpoint triggers on this write
	lea	flag(pc),a1		; 26: watchpoint stop PC
spin:
	tst.l	(a1)			; 30: spin until debugger sets the flag
	beq.s	spin			; 32
	clr.w	-(sp)			; 34: Pterm(0)
	move.w	#$4c,-(sp)
	trap	#1

	even
counter:
	dc.l	0			; 42: watchpoint target
flag:
	dc.l	0			; 46: set by debugger to exit spin loop

	section .data,data
data_val:
	dc.l	$cafebabe

	section .bss,bss
bss_buf:
	ds.l	4
