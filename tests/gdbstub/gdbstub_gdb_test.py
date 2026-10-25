#!/usr/bin/env python3
#
# Test Hatari GDB stub (--gdb-port) with a real GDB client.
#
# Starts Hatari in TOS-less mode with the gdbstub_test.tos test program
# and runs GDB batch commands against it, using gdbstub_test.elf (same
# program linked as ELF with DWARF line info) as symbol file, so that
# qOffsets relocation, symbol & source line breakpoints, stepping and
# watchpoints are tested.
#
# Exits with 77 (test skipped) if no m68k capable GDB is found.
#
# Usage: gdbstub_gdb_test.py <hatari binary> <gdbstub_test.tos> <gdbstub_test.elf>

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdbstub_test import free_port, TIMEOUT  # noqa: E402

SKIP = 77

GDB_NAMES = ("gdb-multiarch", "m68k-elf-gdb", "m68k-atari-mint-gdb",
             "m68k-atari-mintelf-gdb", "m68k-linux-gnu-gdb")

# (gdb command, expected output substring or None)
COMMANDS = (
    ("info symbol $pc", "start in section .text"),
    ("x/x &data_val", "0xcafebabe"),
    ("break bp_here", "file gdbstub_test.s, line 26."),
    ("continue", "Breakpoint 1, bp_here () at gdbstub_test.s:26"),
    ("p $d1", "= 100"),
    ("p/x $d0", "= 0x12345678"),
    ("next", "27\t"),
    ("stepi", "28\t"),
    ("info line *$pc", 'Line 28 of "gdbstub_test.s"'),
    ("watch *(int *)&counter", "atchpoint 2: *(int *)&counter"),
    ("continue", "New value = 100"),
    ("delete", None),
    ("monitor e 1+2", "$3"),
    ("monitor gdb status", "stop-on-exec: on"),
    ("break gdbstub_test.s:31", "line 31."),
    ("continue", "spin () at gdbstub_test.s:31"),
    ("delete", None),
    ("set var *(int *)&flag = 1", None),
    ("detach", "Inferior 1 (Remote target) detached"),
)


def find_gdb():
    """Return path to m68k capable gdb, or None"""
    for name in GDB_NAMES:
        path = shutil.which(name)
        if not path:
            continue
        try:
            out = subprocess.run([path, "-nx", "-batch", "-ex",
                                  "set architecture m68k:68000"],
                                 capture_output=True, text=True, timeout=TIMEOUT)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if "Undefined" not in out.stdout + out.stderr:
            return path
    return None


def start_hatari(hatari, prg, env, logpath):
    """Start Hatari listening on a free GDB port, retry with another
    port if listening fails.  Return (process, port) or (None, None)."""
    for _ in range(3):
        port = free_port()
        log = open(logpath, "w")
        proc = subprocess.Popen([hatari, "--log-level", "info", "--sound", "off",
                                 "--fast-forward", "on", "--tos", "none",
                                 "--gdb-port", str(port), "--gdb-offsets", "text",
                                 prg],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        log.close()
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            with open(logpath) as f:
                if "GDB stub listening" in f.read():
                    return proc, port
            if proc.poll() is not None:
                break
            time.sleep(0.05)
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    return None, None


def main():
    if len(sys.argv) != 4:
        print("Usage: %s <hatari> <gdbstub_test.tos> <gdbstub_test.elf>" % sys.argv[0])
        return 1
    hatari = sys.argv[1]
    prg, elf = os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[3])

    gdb = find_gdb()
    if not gdb:
        print("No m68k capable GDB found, skipping test.")
        return SKIP
    print("Using '%s'" % gdb)

    tmpdir = tempfile.mkdtemp(prefix="hatari-gdbstub-gdb-")
    env = dict(os.environ)
    env.update({"HOME": tmpdir, "HATARI_TEST": "gdbstub",
                "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
    logpath = os.path.join(tmpdir, "out.txt")
    proc, port = start_hatari(hatari, prg, env, logpath)
    if not proc:
        print("Test FAILED: Hatari didn't start listening on GDB port")
        with open(logpath) as f:
            print(f.read())
        shutil.rmtree(tmpdir, ignore_errors=True)
        return 1

    # each command echoes a marker, so that output can be split per command
    script = os.path.join(tmpdir, "commands.gdb")
    with open(script, "w") as f:
        f.write("set pagination off\nset confirm off\n")
        f.write("file %s\n" % elf)
        f.write("target remote localhost:%d\n" % port)
        for idx, (command, _) in enumerate(COMMANDS):
            f.write("echo @@@%d\\n\n%s\n" % (idx, command))
        f.write("echo @@@end\\n\n")

    ret = 1
    try:
        try:
            out = subprocess.run([gdb, "-nx", "-batch", "-x", script],
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True,
                                 timeout=TIMEOUT, env=env)
            output = out.stdout
        except subprocess.TimeoutExpired as err:
            output = "GDB timed out\n%s" % (err.stdout or "")
        print("--- GDB output ---")
        print(output)

        errors = []
        for idx, (command, expected) in enumerate(COMMANDS):
            start = output.find("@@@%d\n" % idx)
            end = output.find("@@@", start + 3)
            if start < 0:
                errors.append("command '%s' not reached" % command)
                break
            part = output[start:end if end > 0 else None]
            if expected and expected not in part:
                errors.append("'%s' output lacks '%s'" % (command, expected))
        if not errors:
            try:
                status = proc.wait(timeout=TIMEOUT)
                if status != 0:
                    errors.append("Hatari exit status %d" % status)
            except subprocess.TimeoutExpired:
                errors.append("Hatari didn't exit after detach")
        if errors:
            print("Test FAILED:\n- " + "\n- ".join(errors))
        else:
            print("Test PASSED.")
            ret = 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        if ret:
            print("--- Hatari output ---")
            with open(logpath) as f:
                print(f.read())
        shutil.rmtree(tmpdir, ignore_errors=True)
    return ret


if __name__ == "__main__":
    sys.exit(main())
