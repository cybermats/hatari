#!/usr/bin/env python3
#
# Test for Hatari GDB Remote Serial Protocol stub (--gdb-port).
#
# Starts Hatari in TOS-less mode with the gdbstub_test.tos test program,
# connects to it with a minimal GDB RSP client and checks the packets
# a debugger uses.  Uses only Python standard library.
#
# Usage: gdbstub_test.py <hatari binary> <gdbstub_test.tos>

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

# instruction offsets within TEXT, see gdbstub_test.s
OFF_LOOP = 8       # addq.l #1,d1
OFF_BP = 18        # move.l d1,d2
OFF_STEP1 = 20     # lea counter(pc),a0
OFF_STEP2 = 24     # move.l d1,(a0)
OFF_WATCH_PC = 26  # lea flag(pc),a1 (after write to counter)
OFF_SPIN = 30      # tst.l (a1) / beq.s spin loop
OFF_COUNTER = 42
OFF_FLAG = 46

REG_D0, REG_D1, REG_D2 = 0, 1, 2
REG_SR, REG_PC = 16, 17
NUM_REGS = 18

TIMEOUT = 20


class TestError(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise TestError(msg)


class RspClient:
    """Minimal GDB remote serial protocol client"""

    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(TIMEOUT)
        self.buf = b""
        self.noack = False

    def _getc(self):
        if not self.buf:
            data = self.sock.recv(4096)
            if not data:
                raise TestError("connection closed by Hatari")
            self.buf = data
        c = self.buf[0:1]
        self.buf = self.buf[1:]
        return c

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        csum = sum(data) & 0xff
        frame = b"$" + data + b"#" + b"%02x" % csum
        self.sock.sendall(frame)
        if not self.noack:
            c = self._getc()
            check(c == b"+", "expected ack '+', got %r" % c)

    def send_raw(self, data):
        self.sock.sendall(data)

    def recv(self):
        """Receive a packet, return its (unescaped) payload as bytes"""
        while True:
            c = self._getc()
            if c == b"$":
                break
            check(c in (b"+", b"-"), "unexpected byte %r outside packet" % c)
        raw = b""
        while True:
            c = self._getc()
            if c == b"#":
                break
            raw += c
        cs = self._getc() + self._getc()
        check(int(cs, 16) == sum(raw) & 0xff, "bad checksum in %r" % raw)
        if not self.noack:
            self.sock.sendall(b"+")
        out = bytearray()
        esc = False
        for b in raw:
            if esc:
                out.append(b ^ 0x20)
                esc = False
            elif b == 0x7d:
                esc = True
            else:
                out.append(b)
        return bytes(out)

    def cmd(self, data):
        self.send(data)
        return self.recv().decode("latin-1")

    def expect(self, data, reply):
        got = self.cmd(data)
        check(got == reply, "'%s' -> expected '%s', got '%s'" % (data, reply, got))
        return got

    def xfer(self, obj):
        """Read whole qXfer object"""
        data = b""
        while True:
            self.send("qXfer:%s:%x,%x" % (obj, len(data), 0x800))
            reply = self.recv()
            check(reply[:1] in (b"m", b"l"), "bad qXfer reply %r" % reply[:20])
            data += reply[1:]
            if reply[:1] == b"l":
                return data

    def regs(self):
        reply = self.cmd("g")
        check(len(reply) == NUM_REGS * 8, "bad 'g' reply length %d" % len(reply))
        return [int(reply[i*8:i*8+8], 16) for i in range(NUM_REGS)]

    def monitor(self, command):
        """Run monitor command, return its output"""
        self.send("qRcmd," + command.encode().hex())
        output = b""
        while True:
            reply = self.recv()
            if reply.startswith(b"O") and reply != b"OK":
                output += bytes.fromhex(reply[1:].decode())
                continue
            check(reply == b"OK", "qRcmd '%s' -> %r" % (command, reply))
            return output.decode("latin-1")

    def variable(self, name):
        output = self.monitor("e " + name)
        match = re.search(r"\$([0-9a-fA-F]+)", output)
        check(match is not None, "cannot read %s: %r" % (name, output))
        return int(match.group(1), 16)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def launch(hatari, args, env, logpath):
    """Start Hatari with GDB port, retry with another port if it fails
    to listen on it.  Last arg is the program.  Return (process, port, socket)."""
    for _ in range(3):
        port = free_port()
        log = open(logpath, "w")
        # program path needs to be last
        proc = subprocess.Popen([hatari] + args[:-1] +
                                ["--gdb-port", str(port), args[-1]],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        log.close()
        try:
            return proc, port, connect(port, proc)
        except TestError:
            if proc.poll() is None:
                raise
            with open(logpath) as f:
                if "listen" not in f.read():
                    raise
    raise TestError("Hatari failed to listen on GDB port")


def connect(port, proc):
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        if proc.poll() is not None:
            raise TestError("Hatari exited early with %d" % proc.returncode)
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            time.sleep(0.1)
    raise TestError("could not connect to Hatari GDB port %d" % port)


def run_tests(gdb, prg, reconnect, fifo):
    with open(prg, "rb") as f:
        prgdata = f.read()
    tlen = int.from_bytes(prgdata[2:6], "big")
    dlen = int.from_bytes(prgdata[6:10], "big")
    text_bytes = prgdata[0x1c:0x1c + tlen]

    # --- connection setup, like gdb does it ---
    reply = gdb.cmd("qSupported:multiprocess+;swbreak+;hwbreak+;"
                    "qRelocInsn+;vContSupported+;xmlRegisters=i386")
    for feature in ("PacketSize=4000", "QStartNoAckMode+",
                    "qXfer:features:read+", "qXfer:exec-file:read+",
                    "swbreak+", "hwbreak+", "vContSupported+"):
        check(feature in reply.split(";"), "qSupported lacks '%s': %s" % (feature, reply))
    gdb.expect("vMustReplyEmpty", "")
    gdb.expect("Hg0", "OK")

    xml = gdb.xfer("features:read:target.xml").decode()
    check("<architecture>m68k</architecture>" in xml, "no m68k arch in target.xml")
    check("org.gnu.gdb.m68k.core" in xml, "no m68k core feature in target.xml")
    names = [part.split('"')[1] for part in xml.split("<reg name=")[1:]]
    check(names == ["d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
                    "a0", "a1", "a2", "a3", "a4", "a5", "fp", "sp", "ps", "pc"],
          "unexpected target.xml registers: %s" % names)

    # rest without acks
    gdb.expect("QStartNoAckMode", "OK")
    gdb.noack = True

    gdb.expect("qTStatus", "")
    gdb.expect("unknownpacket", "")
    gdb.expect("?", "T05library:;")
    gdb.expect("qfThreadInfo", "m1")
    gdb.expect("qsThreadInfo", "l")
    gdb.expect("qAttached", "1")
    gdb.expect("qC", "QC1")
    gdb.expect("qSymbol::", "OK")
    print("OK: connection setup, target.xml, stop at exec")

    # --- program info ---
    reply = gdb.cmd("qOffsets")
    offsets = dict(item.split("=") for item in reply.split(";"))
    text = int(offsets["Text"], 16)
    data = int(offsets["Data"], 16)
    bss = int(offsets["Bss"], 16)
    check(data == text + tlen and bss == data + dlen,
          "inconsistent qOffsets: %s" % reply)
    path = gdb.xfer("exec-file:read:").decode()
    check(os.path.samefile(path, prg), "exec-file '%s' != '%s'" % (path, prg))
    print("OK: qOffsets %s, exec-file" % reply)

    # --- registers & memory ---
    regs = gdb.regs()
    check(regs[REG_PC] == text, "PC $%x != TEXT $%x at exec stop" % (regs[REG_PC], text))
    check(regs[REG_SR] & 0xffff0000 == 0, "SR upper bits set")
    reply = gdb.cmd("m%x,%x" % (text, tlen))
    check(bytes.fromhex(reply) == text_bytes, "TEXT memory doesn't match program")
    reply = gdb.cmd("p%x" % REG_PC)
    check(int(reply, 16) == text, "p PC mismatch")
    gdb.expect("p%x" % NUM_REGS, "E01")
    gdb.expect("p100000010", "E01")  # register number must not wrap
    print("OK: g, p, m")

    # --- exact subroutine stack (BSR -> JSR -> RTS -> RTS) ---
    scratch = text + 0x100
    saved_sp = regs[15]
    code = bytearray(0x22)
    code[0:2] = b"\x61\x06"                 # bsr.s scratch+8
    code[2:4] = b"\x4e\x71"                 # nop (return target)
    code[8:10] = b"\x4e\xb9"                # jsr scratch+0x20
    code[10:14] = (scratch + 0x20).to_bytes(4, "big")
    code[14:16] = b"\x4e\x75"               # rts
    code[0x20:0x22] = b"\x4e\x75"           # rts
    gdb.expect("M%x,%x:%s" % (scratch, len(code), code.hex()), "OK")
    gdb.expect("P%x=%08x" % (REG_PC, scratch), "OK")
    gdb.expect("qHatari:callstack", "S;%x" % scratch)
    gdb.expect("s", "T05")
    gdb.expect("qHatari:callstack", "S;%x;%x" % (scratch + 8, scratch + 2))
    gdb.expect("s", "T05")
    gdb.expect("qHatari:callstack", "S;%x;%x;%x" %
               (scratch + 0x20, scratch + 14, scratch + 2))
    gdb.expect("s", "T05")
    gdb.expect("qHatari:callstack", "S;%x;%x" % (scratch + 14, scratch + 2))
    gdb.expect("s", "T05")
    gdb.expect("qHatari:callstack", "S;%x" % (scratch + 2))
    # Continue uses the call tracker even without single-stepping.
    gdb.expect("P%x=%08x" % (15, saved_sp), "OK")
    gdb.expect("P%x=%08x" % (REG_PC, scratch), "OK")
    gdb.expect("Z0,%x,2" % (scratch + 0x20), "OK")
    gdb.expect("c", "T05swbreak:;")
    gdb.expect("qHatari:callstack", "S;%x;%x;%x" %
               (scratch + 0x20, scratch + 14, scratch + 2))
    gdb.expect("z0,%x,2" % (scratch + 0x20), "OK")
    gdb.expect("P%x=%08x" % (REG_PC, text), "OK")
    gdb.expect("P%x=%08x" % (15, saved_sp), "OK")
    print("OK: exact BSR/JSR/RTS call stack")

    # --- breakpoint ---
    bp = text + OFF_BP
    gdb.expect("Z0,%x,2" % bp, "OK")
    gdb.expect("Z0,%x,2" % bp, "OK")   # idempotent
    gdb.expect("c", "T05swbreak:;")
    regs = gdb.regs()
    check(regs[REG_PC] == bp, "breakpoint PC $%x != $%x" % (regs[REG_PC], bp))
    check(regs[REG_D0] == 0x12345678, "D0 = $%x" % regs[REG_D0])
    check(regs[REG_D1] == 100, "D1 = %d at breakpoint" % regs[REG_D1])
    gdb.expect("z0,%x,2" % bp, "OK")
    gdb.expect("z0,%x,2" % bp, "OK")   # removing non-existing one
    print("OK: Z0 breakpoint")

    # --- stub breakpoints are separate from user ones ---
    loop = text + OFF_LOOP
    gdb.expect("Z0,%x,2" % loop, "OK")
    out = gdb.monitor("b pc = $%x" % (text + OFF_SPIN))
    out = gdb.monitor("b")
    check("1 conditional CPU breakpoints" in out and "(gdb)" in out,
          "'b' listing: %r" % out)
    out = gdb.monitor("b 1")    # removes the user breakpoint
    out = gdb.monitor("b all")
    out = gdb.monitor("b")
    check("No conditional CPU breakpoints" in out and "(gdb)" in out,
          "stub breakpoint removed by 'b all': %r" % out)
    gdb.expect("z0,%x,2" % loop, "OK")
    # Z0, user removes all, Z0 re-sent -> breakpoint must still work
    gdb.expect("Z0,%x,2" % bp, "OK")
    gdb.monitor("b all")
    gdb.expect("Z0,%x,2" % bp, "OK")
    # back to (not taken) bne.s before the breakpoint
    gdb.expect("P%x=%08x" % (REG_PC, bp - 2), "OK")
    gdb.expect("c", "T05swbreak:;")
    check(gdb.regs()[REG_PC] == bp, "breakpoint after 'b all' didn't hit")
    gdb.expect("z0,%x,2" % bp, "OK")
    print("OK: stub vs. user breakpoints")

    # --- stepping ---
    # late interrupt request (crossing the stop reply) is ignored
    # while stopped, i.e. it doesn't cause a stray T02 later on
    gdb.send_raw(b"\x03")
    gdb.expect("s", "T05")
    gdb.expect("?", "T05")
    pc = gdb.regs()[REG_PC]
    check(pc == text + OFF_STEP1, "PC $%x after step" % pc)
    gdb.expect("vCont?", "vCont;c;C;s;S")
    gdb.expect("vCont;s:1", "T05")
    pc = gdb.regs()[REG_PC]
    check(pc == text + OFF_STEP2, "PC $%x after vCont step" % pc)
    print("OK: s, vCont;s")

    # --- register & memory writes ---
    gdb.expect("P%x=deadbeef" % REG_D2, "OK")
    gdb.expect("p%x" % REG_D2, "deadbeef")
    check(gdb.regs()[REG_D2] == 0xdeadbeef, "P write not visible in g")
    regs = gdb.regs()
    regs[3] = 0x01020304
    gdb.expect("G" + "".join("%08x" % r for r in regs), "OK")
    check(gdb.regs() == regs, "G write mismatch")
    # G supplies the active A7 for its requested SR mode.
    changed = regs.copy()
    changed[REG_SR] ^= 0x2000
    changed[15] = 0x00002000
    gdb.expect("G" + "".join("%08x" % r for r in changed), "OK")
    check(gdb.regs()[15] == changed[15], "G SR/A7 bank switch lost requested A7")
    gdb.expect("G" + "".join("%08x" % r for r in regs), "OK")
    gdb.expect("m400000,4", "E01")   # unmapped (> 4MB ST-RAM)
    gdb.expect("M400000,1:00", "E01")
    gdb.expect("M%x,4:11223344" % bss, "OK")
    gdb.expect("m%x,4" % bss, "11223344")
    # binary write with bytes needing escaping: # $ } *
    gdb.send(b"X%x,4:" % (bss + 4) + b"}\x03}\x04}]}\x0a")
    check(gdb.recv() == b"OK", "X write failed")
    gdb.expect("m%x,4" % (bss + 4), "23247d2a")
    gdb.expect("m%x,4" % data, "cafebabe")
    # malformed / oversized packets
    for pkt in ("mzz", "m1100", "M1100", "M1100,4", "M1100,4:00",
                "M1100,4:zz000000", "M1100,ffffffff:00",
                "M1100,100000001:00", "M100001100,1:00",
                "M1100,80000000:00", "X1100,80000000:", "X1100,4:ab",
                "Mff8240,2:0777"):     # IO area writes are refused
        gdb.expect(pkt, "E01")
    reply = gdb.cmd("m1000,ffffffff")
    check(0 < len(reply) < 0x4000, "oversized m reply length %d" % len(reply))
    gdb.expect("m%x,4" % text, text_bytes[:4].hex())
    print("OK: P, G, M, X, bad packets")

    # --- monitor commands ---
    out = gdb.monitor("e 1+2")
    check("$3" in out, "'e 1+2' output: %r" % out)
    out = gdb.monitor("r")
    check("12345678" in out and "DEADBEEF" in out.upper(), "'r' output: %r" % out)
    out = gdb.monitor("c")
    check("not supported" in out, "'c' monitor command not refused: %r" % out)
    out = gdb.monitor("gdb status")
    check("stop-on-exec: on" in out, "'gdb status' output: %r" % out)
    gdb.expect("?", "T05")
    # debugger log file redirection works & doesn't break capture
    logfile = os.path.join(os.environ["HOME"], "debug.log")
    out = gdb.monitor("f %s" % logfile)
    check("opened" in out, "'f <file>' output: %r" % out)
    out = gdb.monitor("r")
    out = gdb.monitor("f")
    check("closed" in out, "'f' output: %r" % out)
    with open(logfile) as f:
        check("12345678" in f.read(), "'r' output missing from log file")
    out = gdb.monitor("r")
    check("12345678" in out, "'r' output after 'f': %r" % out)
    gdb.send("qRcmd," + "6d" * 5000)     # too long command
    check(gdb.recv() == b"E01", "too long qRcmd accepted")
    script = os.path.join(os.environ["HOME"], "remote-script.txt")
    with open(script, "w") as f:
        f.write("reset hard\nquit\ncont\n")
    out = gdb.monitor("parse " + script)
    check(out.count("not supported") == 3, "nested remote commands escaped guard: %r" % out)
    check(gdb.regs()[REG_PC] == text + OFF_STEP2, "remote script changed execution state")
    gdb.monitor("setopt --gdb-stop-on-exec off")
    check("stop-on-exec: off" in gdb.monitor("gdb status"), "setopt did not disable stop-on-exec")
    gdb.monitor("setopt --gdb-stop-on-exec on")
    print("OK: qRcmd")

    # An access hit must still update the simultaneous Z2 baseline.
    counter = text + OFF_COUNTER
    gdb.expect("P%x=%08x" % (REG_PC, text + OFF_STEP2), "OK")
    gdb.expect("M%x,4:00000000" % counter, "OK")
    gdb.expect("Z2,%x,4" % counter, "OK")
    gdb.expect("Z4,%x,4" % counter, "OK")
    gdb.expect("s", "T05awatch:%x;" % counter)
    gdb.expect("z4,%x,4" % counter, "OK")
    gdb.expect("s", "T05")
    gdb.expect("z2,%x,4" % counter, "OK")
    gdb.expect("P%x=%08x" % (REG_PC, text + OFF_STEP2), "OK")
    gdb.expect("M%x,4:00000000" % counter, "OK")
    print("OK: simultaneous Z2/Z4 baseline")

    # --- watchpoint ---
    counter = text + OFF_COUNTER
    gdb.expect("Z2,%x,4" % counter, "OK")
    gdb.expect("vCont;c", "T05watch:%x;" % counter)
    regs = gdb.regs()
    check(regs[REG_PC] == text + OFF_WATCH_PC, "watchpoint PC $%x" % regs[REG_PC])
    gdb.expect("m%x,4" % counter, "00000064")
    gdb.expect("z2,%x,4" % counter, "OK")
    print("OK: Z2 watchpoint")

    # A read watchpoint ignores CPU writes and RSP 'm' requests.
    gdb.expect("Z3,%x,4" % counter, "OK")
    gdb.expect("P%x=%08x" % (REG_PC, text + OFF_STEP2), "OK")
    gdb.expect("s", "T05")
    check(gdb.regs()[REG_PC] == text + OFF_WATCH_PC,
          "read watchpoint unexpectedly stopped on a write")
    gdb.expect("z3,%x,4" % counter, "OK")
    flag = text + OFF_FLAG
    gdb.expect("Z3,%x,4" % flag, "OK")
    gdb.expect("Z3,%x,4" % flag, "OK")  # idempotent
    gdb.expect("m%x,4" % flag, "00000000")
    gdb.expect("c", "T05rwatch:%x;" % flag)
    check(gdb.regs()[REG_PC] == text + OFF_SPIN + 2,
          "read watchpoint stopped at wrong PC")
    gdb.expect("Z4,%x,4" % flag, "OK")
    gdb.expect("z3,%x,4" % flag, "OK")
    # Removing Z3 must keep CPU data hooks active for the remaining Z4.
    gdb.expect("c", "T05awatch:%x;" % flag)
    check(gdb.regs()[REG_PC] == text + OFF_SPIN + 2,
          "remaining access watchpoint stopped at wrong PC")
    gdb.expect("z4,%x,4" % flag, "OK")
    print("OK: Z3 read watchpoint")

    # Match a byte inside a long CPU write, even when its value is unchanged.
    gdb.expect("Z4,%x,1" % (counter + 1), "OK")
    gdb.expect("P%x=%08x" % (REG_PC, text + OFF_STEP2), "OK")
    gdb.expect("c", "T05awatch:%x;" % (counter + 1))
    check(gdb.regs()[REG_PC] == text + OFF_WATCH_PC,
          "access watchpoint stopped at wrong PC")
    gdb.expect("z4,%x,1" % (counter + 1), "OK")
    print("OK: Z4 access watchpoint")

    # --- interrupt while running ---
    # client write to watched location doesn't trigger watchpoint
    gdb.expect("Z2,%x,4" % (bss + 8), "OK")
    gdb.expect("M%x,4:55555555" % (bss + 8), "OK")
    gdb.send("c")
    time.sleep(0.5)
    gdb.send_raw(b"\x03")
    gdb.expect_reply = gdb.recv()
    check(gdb.expect_reply == b"T02", "interrupt reply %r" % gdb.expect_reply)
    pc = gdb.regs()[REG_PC]
    check(text + OFF_SPIN <= pc <= text + OFF_SPIN + 2, "PC $%x after interrupt" % pc)
    print("OK: interrupt")

    # --- disconnect while running, reconnect ---
    gdb.send("c")
    gdb = reconnect()
    gdb.expect("?", "T02")
    pc = gdb.regs()[REG_PC]
    check(text + OFF_SPIN <= pc <= text + OFF_SPIN + 2, "PC $%x after reconnect" % pc)
    out = gdb.monitor("b")    # watchpoint removed on disconnect
    check("(gdb)" not in out, "stub breakpoints left after disconnect: %r" % out)
    print("OK: reconnect")

    # --- GDB control while the GUI has paused emulation ---
    gdb.send("c")
    with open(fifo, "w") as control:
        control.write("hatari-stop\n")
    time.sleep(0.2)
    gdb.send_raw(b"\x03")
    check(gdb.recv() == b"T02", "interrupt while GUI-paused failed")
    gdb.expect("s", "T05")  # must also resume the GUI pause to execute one instruction
    print("OK: interrupt and step while GUI-paused")

    gdb.send("c")
    with open(fifo, "w") as control:
        control.write("hatari-stop\n")
    time.sleep(0.2)
    gdb = reconnect()
    gdb.expect("?", "T02")
    gdb.expect("s", "T05")
    print("OK: reconnect and step while GUI-paused")

    # --- video position controls (sampled at instruction boundaries) ---
    before = gdb.variable("VBL")
    check("armed" in gdb.monitor("gdb run-vbl"), "run-vbl not armed")
    gdb.expect("c", "T05vbl:;")
    check(gdb.variable("VBL") != before, "run-vbl did not advance a frame")
    before = gdb.variable("HBL")
    check("armed" in gdb.monitor("gdb run-hbl"), "run-hbl not armed")
    gdb.expect("c", "T05hbl:;")
    check(gdb.variable("HBL") != before, "run-hbl did not advance a line")

    out = gdb.monitor("gdb raster 600 40")
    check("ERROR" in out, "out-of-range raster line accepted")
    out = gdb.monitor("gdb raster 100 0")
    check("Raster breakpoint" in out, "cycle-zero raster breakpoint rejected")
    gdb.expect("c", "T05raster:;")
    check(gdb.variable("RasterLine") == 100, "cycle-zero raster stopped on wrong line")
    out = gdb.monitor("gdb raster 100 40")
    check("Raster breakpoint" in out, "raster breakpoint rejected: %r" % out)
    gdb.expect("c", "T05raster:;")
    check(gdb.variable("RasterLine") == 100, "wrong raster line")
    check(gdb.variable("LineCycles") >= 40, "raster cycle threshold not reached")
    before = gdb.variable("VBL")
    gdb.expect("c", "T05raster:;")
    check(gdb.variable("VBL") != before, "raster re-triggered in same frame")
    check("cleared" in gdb.monitor("gdb raster clear"), "raster not cleared")
    print("OK: run-vbl, run-hbl and recurring raster breakpoint")

    # --- let program exit ---
    flag = text + OFF_FLAG
    gdb.expect("M%x,4:00000001" % flag, "OK")
    gdb.expect("D", "OK")
    print("OK: detach")
    return gdb


def main():
    if len(sys.argv) != 3:
        print("Usage: %s <hatari> <gdbstub_test.tos>" % sys.argv[0])
        return 1
    hatari, prg = sys.argv[1], os.path.abspath(sys.argv[2])

    tmpdir = tempfile.mkdtemp(prefix="hatari-gdbstub-")
    env = dict(os.environ)
    env.update({"HOME": tmpdir, "HATARI_TEST": "gdbstub",
                "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
    os.environ["HOME"] = tmpdir
    logpath = os.path.join(tmpdir, "out.txt")
    fifo = os.path.join(tmpdir, "control.fifo")
    args = ["--log-level", "info", "--sound", "off", "--fast-forward", "on",
            "--tos", "none", "--cmd-fifo", fifo, prg]
    proc = None
    ret = 1
    try:
        proc, port, sock = launch(hatari, args, env, logpath)
        state = {"sock": sock}

        def reconnect():
            state["sock"].close()
            time.sleep(0.2)
            state["sock"] = connect(port, proc)
            return RspClient(state["sock"])

        run_tests(RspClient(sock), prg, reconnect, fifo)
        state["sock"].close()
        try:
            status = proc.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            raise TestError("Hatari didn't exit after detach")
        check(status == 0, "Hatari exit status %d" % status)
        # Changing CPU model invalidates accepted 68000 access watches.
        # The client must be disconnected instead of left with a silent Z3.
        proc, port, sock = launch(hatari, args, env, logpath)
        model_gdb = RspClient(sock)
        model_gdb.sock.settimeout(5)
        model_gdb.expect("?", "T05library:;")
        model_offsets = dict(item.split("=") for item in model_gdb.cmd("qOffsets").split(";"))
        model_gdb.expect("Z3,%s,4" % model_offsets["Bss"], "OK")
        model_gdb.monitor("setopt --cpulevel 1")
        model_gdb.send("c")
        try:
            model_gdb.recv()
        except TestError as err:
            check("connection closed" in str(err), "CPU model transition: %s" % err)
        else:
            raise TestError("client was not disconnected after CPU model change")
        sock.close()
        print("OK: CPU model transition closes active access watches")
        print("Test PASSED.")
        ret = 0
    except (TestError, OSError) as err:
        print("Test FAILED: %s" % err)
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait()
        if ret and os.path.exists(logpath):
            print("--- Hatari output ---")
            with open(logpath) as f:
                print(f.read())
        shutil.rmtree(tmpdir, ignore_errors=True)
    return ret


if __name__ == "__main__":
    sys.exit(main())
