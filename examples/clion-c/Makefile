MINT_CC ?= m68k-atari-mintelf-gcc
VLINK ?= vlink

# The same ELF object is linked twice so both outputs have identical
# section-relative addresses.  The ELF retains the DWARF information.
CFLAGS = -m68000 -gdwarf-3 -g -O0 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables

all: demo.tos demo.elf

main.o: main.c
	$(MINT_CC) $(CFLAGS) -c -o $@ $<

demo.tos: main.o tos.ld
	$(VLINK) -bataritos -nostdlib -T tos.ld -o $@ main.o

demo.elf: main.o elf.ld
	$(VLINK) -belf32m68k -nostdlib -T elf.ld -o $@ main.o

clean:
	rm -f main.o demo.tos demo.elf

.PHONY: all clean
