# C debugging example for CLion and Hatari

This small freestanding 68000 program updates `counter` ten times, then
exits through GEMDOS. It builds a `.tos` program and an ELF symbol file from
the same object. Both linker scripts place TEXT at zero and lay out DATA and
BSS after it, so Hatari's `--gdb-offsets text` can relocate the ELF.

## Tools and build

Install an ELF-object MiNT cross compiler (`m68k-atari-mintelf-gcc`),
[vlink](http://sun.hasenbraten.de/vlink/), `make`, and Hatari built with its
GDB stub. The [MiNT cross-tools distribution](https://tho-otto.de/crossmint.php)
offers a `mintelf` variant for Linux, macOS, and Windows. Set `VLINK` to its
absolute path if it is not on `PATH`.

```sh
cd examples/clion-c
make VLINK=/path/to/vlink
file demo.tos demo.elf
readelf -SW demo.elf
```

The ELF should contain `.debug_info` and `.debug_line`, with `.text` at address
zero. `demo.tos` is the executable; `demo.elf` supplies symbols and source
lines to GDB. Rebuild both after each source change.

The cross compiler is not installed in the current development environment,
so this exact C build has not yet been verified here. The linker scripts were
checked with a vasm ELF object containing TEXT, DATA, BSS, and DWARF sections.

## CLion Remote Debug

Open this directory as a CMake project. CMake indexes `main.c`; its `atari`
target delegates the build to `make`. Ensure the cross compiler and vlink are
on CLion's environment `PATH` (or set `MINT_CC` and `VLINK` in the environment).
Build the `atari` target before debugging.

Create a **Remote Debug** run configuration:

| Field | Value |
|---|---|
| Debugger | `gdb-multiarch` or another GDB with m68k support |
| `target remote` args | `127.0.0.1:1234` |
| Symbol file | Absolute path to `demo.elf` |
| Sysroot and path mappings | Leave empty for this local example |

Start Hatari from the repository root, then start the CLion configuration:

```sh
build/src/hatari --tos none --sound off --gdb-port 1234 \
  --gdb-offsets text examples/clion-c/demo.tos
```

Hatari waits at the first instruction for GDB. CLion loads the ELF before
connecting, so GDB applies `qOffsets` automatically. Set a breakpoint in
`tick`, resume, inspect `counter` and `seed`, then step through the loop.
The behavior of CLion's UI with this specific C example still needs an IDE
session to verify. The configuration fields follow
[JetBrains' Remote Debug documentation](https://www.jetbrains.com/help/clion/remote-debug.html).

## GDB and late symbol loading

The normal GDB sequence also handles relocation automatically:

```gdb
file examples/clion-c/demo.elf
target remote 127.0.0.1:1234
break tick
continue
```

If you connected **before** specifying an ELF, use the bundled Python helper.
It reads Hatari's `qOffsets` packet and issues `add-symbol-file -o` with the
reported TEXT address:

```gdb
set architecture m68k:68000
set endian big
source examples/clion-c/hatari_symbols.py
target remote 127.0.0.1:1234
hatari-symbols examples/clion-c/demo.elf
```

Run `hatari-symbols` again after starting another program; it adds symbols
for the current TEXT address. The helper was smoke tested with the repository's
`gdbstub_test.tos` and `gdbstub_test.elf`: it loaded symbols at TEXT `0x1100`
and GDB resolved `$pc` to `start`.

GDB may warn when it tries to read the `.tos` executable through
`qXfer:exec-file`; it is not an ELF symbol file. Specify `demo.elf` as above.
