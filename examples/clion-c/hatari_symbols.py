"""Load an Atari program's ELF symbols at Hatari's current TEXT address.

In GDB: source /path/to/hatari_symbols.py
        target remote 127.0.0.1:1234
        hatari-symbols /path/to/program.elf

This is useful when the ELF was not specified with GDB's ``file`` command
before connecting.  In that usual case GDB consumes qOffsets itself.
"""

import re
import shlex

import gdb


_OFFSETS = re.compile(r"Text=([0-9a-fA-F]+);Data=([0-9a-fA-F]+);Bss=([0-9a-fA-F]+)")


class HatariSymbols(gdb.Command):
    """Load ELF symbols at the TEXT address from Hatari's qOffsets reply."""

    def __init__(self):
        super().__init__("hatari-symbols", gdb.COMMAND_FILES)

    def invoke(self, argument, from_tty):
        args = gdb.string_to_argv(argument)
        if len(args) != 1:
            raise gdb.GdbError("usage: hatari-symbols /path/to/program.elf")
        response = gdb.execute("maintenance packet qOffsets", to_string=True)
        match = _OFFSETS.search(response)
        if match is None:
            raise gdb.GdbError("Hatari did not return qOffsets (is a program running?)")
        text = int(match.group(1), 16)
        # -o applies the same load offset to all ELF sections, which is
        # correct for the linked-at-zero symbol file used by this example.
        gdb.execute("add-symbol-file %s -o %#x" %
                    (shlex.quote(args[0]), text), from_tty=from_tty)
        gdb.write("Hatari TEXT relocation: %#x\n" % text)


HatariSymbols()
