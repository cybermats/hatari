# Hatari GDB stub – protocol contract

Contract between the GDB stub (src/debug/gdbstub.c) and the VS Code /
DAP adapter (https://github.com/cybermats/vscode-hatari). Keep both in sync with this file.

Scope: 68000 machines
only (ST/STFM/STE). Linux/macOS only (POSIX sockets). No DSP, no FPU, no MMU.

## Transport / lifecycle
- Hatari option: `--gdb-port <port>` (0 = disabled, default). Listens on
  127.0.0.1 only, TCP, one client at a time.
- Accepting clients and detecting the `0x03` interrupt byte happens in a
  non-blocking poll called once per VBL, or every 100 ms while emulation is
  paused from the GUI (next to `Control_CheckUpdates()` in
  `src/sdl/gui_event.c`).
- When a client connects, Hatari stops emulation (enters the debugger with
  REASON_USER) and serves packets in a blocking loop. While stopped, SDL events
  should still be processed enough that the window doesn't hang (best effort).
- While stopped and a GDB client is connected, the debugger "front end" is the
  stub instead of readline. Without a client, the normal console debugger works
  exactly as before.
- Client disconnect (or `D`) => remove all breakpoints the stub created and
  resume emulation. `k` => quit Hatari.
- Packet framing `$data#cs`, `+`/`-` acks, `QStartNoAckMode` supported.
  Binary escaping (`}`) for `X` packets. Max packet size advertised in
  `qSupported` as `PacketSize=4000` (hex => 16384 bytes).

## qSupported reply (at least)
`PacketSize=4000;QStartNoAckMode+;qXfer:features:read+;qXfer:exec-file:read+;swbreak+;hwbreak+;vContSupported+`

## Registers
Order, each 32-bit big-endian hex (8 hex chars), total 18 registers:
`d0..d7 (0-7), a0..a7 (8-15), sr (16), pc (17)`.
- a7 is the *active* stack pointer (USP or SSP depending on SR.S).
- `g`, `G`, `p n`, `P n=v` supported. sr transferred as 32-bit (upper 16 = 0).
- `qXfer:features:read:target.xml:...` returns a target description with
  `<architecture>m68k</architecture>` and feature `org.gnu.gdb.m68k.core`
  containing exactly those 18 regs, using gdb's standard m68k names.
  Names: `d0 d1 d2 d3 d4 d5 d6 d7 a0 a1 a2 a3 a4 a5 fp sp ps pc` (fp=a6, sp=a7,
  ps=sr); types int32, data_ptr for fp/sp, code_ptr for pc.
- Extra (Hatari-specific, read-only) values USP/SSP are available via monitor:
  `monitor r` prints the full Hatari register dump.

## Memory
- `m addr,len` / `M addr,len:hex` / `X addr,len:bin`.
- Uses debugger-safe access. Reads of the IO area ($FF8000-$FFFFFF) MUST NOT
  have hardware side effects; if a side-effect-free read isn't possible for a
  region, return what the existing debugger memdump (`m` command) would show.
  Unmapped/invalid address => `E01`.
- Addresses are 24-bit on 68000; mask to 24 bits.

## Execution
- `c` / `c addr`, `s` / `s addr`, `vCont?` => `vCont;c;C;s;S`, `vCont;c`,
  `vCont;s` (thread ids ignored), `?` => last stop reply.
- `s` executes exactly one instruction (same semantics as console `s`).
- Thread packets: `Hg`, `Hc` => `OK`; `qC` => `QC1`; `qfThreadInfo` => `m1`,
  `qsThreadInfo` => `l`; `T1` => `OK`. `qAttached` => `1`.

## Stop replies
Format `Txx` + optional `;reason:;` fields:
- breakpoint hit: `T05swbreak:;`
- step complete: `T05`
- user interrupt (0x03) / initial connect: `T02`
- watchpoint: `T05watch:ADDR;` / `rwatch:` / `awatch:`
- program exec'd (new TOS program started via GEMDOS Pexec, stopped at its first
  instruction = TEXT start): `T05library:;`  (client then re-reads `qOffsets`)
- CPU exceptions (only those enabled in Hatari's exception debug mask,
  `--debug-except`): bus error / address error `T0a`, illegal / privilege /
  line-A / line-F `T04`, div by zero `T08`, CHK / TRAPV `T05`, trace `T05`.
- Hatari exit while client connected: `W00` (or `Wxx` with exit code), then close.

## Breakpoints
- `Z0,addr,kind` / `z0,addr,kind` software breakpoint, `Z1`/`z1` same thing.
  Implemented via Hatari's existing address breakpoints (breakcond.c). Adding
  the same address twice is idempotent. Removing a non-existing one => `OK`.
- `Z2`/`Z3`/`Z4` write/read/access watchpoints. Z2 uses BreakCond value-change
  conditions. Z3/Z4 observe 68000 CPU data accesses, including same-value
  writes for Z4; instruction fetches and debugger memory requests are excluded.
- Breakpoints created through the stub must not clash with user breakpoints made
  via monitor commands or the console.

## Program / relocation info
- `qOffsets` => `Text=XXXXXX;Data=YYYYYY;Bss=ZZZZZZ` = run-time start
  addresses of the current program's TEXT, DATA, BSS segments (from the
  basepage, same values as Hatari debugger variables TEXT/DATA/BSS).
  If no program is running => reply `E01`.
- `qXfer:exec-file:read::off,len` => host path of the current program (the
  file that GEMDOS HD Pexec'd), or `E01` if unknown.
- Stopping at program start: when a client is attached and a program is started
  via GEMDOS HD Pexec (gemdos.c around `Event_DoPrgExecActions()`), the stub
  stops at the program's first instruction (TEXT start) and reports
  `T05library:;`. This can be disabled with `--gdb-stop-on-exec off`
  (default on) or toggled at runtime with `monitor gdb stop-on-exec on|off`.
  Running a program directly (`hatari --tos none prog.tos` or as autostart)
  goes through this Pexec path too.

## Call stack extension
- `qHatari:callstack` enables subroutine tracking for the current remote
  connection and returns `S;PC;RET1;RET2;...` (hex addresses), with the
  current PC first and return addresses from innermost to outermost call.
  Repeated queries read the current stack. A standard GDB client never
  enables the extra per-instruction tracking unless it sends this query.
- The stub records JSR/BSR pushes and matches RTS/RTD/RTR return PCs. It
  clears frames on RTE, program start, reset, disconnect, or an explicit
  PC/SP register change. Tracking starts when the query is first sent, so
  calls made earlier are unavailable. Exception frames, stack switching and
  non-standard control flow are not represented.

## Monitor commands
- `qRcmd,<hex>` => executes the text as a Hatari debugger command line via the
  normal debugger parser (same as typing it at the console, incl. expression
  expansion), captures everything it prints, and returns it as `O<hex>` packets
  followed by `OK`. Commands that would resume emulation (c, s, n, ...) are
  rejected with an error message (use gdb packets for that).
- Stub-specific monitor commands, prefixed `gdb `:
  `gdb stop-on-exec on|off`, `gdb status`,
  `gdb raster <line> <cycle>`, `gdb raster clear`, `gdb run-vbl`, `gdb run-hbl`.
  Raster uses the scanline returned by `Video_GetPosition()` and stops when
  `LineCycles` reaches the requested cycle. The recurring raster breakpoint
  stops once per VBL. `run-vbl` and `run-hbl` arm one-shot stops; resume with
  the normal `c`/`vCont;c` packet afterward. Replies are `T05raster:;`,
  `T05vbl:;`, and `T05hbl:;`.
  Video positions are sampled at CPU instruction boundaries, so the stop may
  be a few cycles later than requested. A single instruction can also skip a
  very short position window. These commands use tagged stub breakpoints and
  are removed on client disconnect.

## Testing environment notes
- Tests run hatari with `--tos none --sound off --fast-forward on`,
  `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, `HOME=<tmpdir>`, and the test
  program path as last argument (see tests/cpu/run_test.sh).
- `vasmm68k_mot -Ftos -L prog.lst -o prog.tos prog.s`
  produces a TOS executable plus a listing that maps `section:offset` to
  source lines. `-Felf -dwarf=3` gives a relocatable ELF .o with DWARF line info.

## Implementation notes / additions (C stub, as implemented)
These are additions/clarifications; nothing above was changed incompatibly.
- Stop-on-exec when NO client has connected yet: Hatari sets the TEXT-start
  breakpoint anyway and, when it is reached, blocks waiting for the first
  client connection (window kept alive). The connecting client's `?` then
  returns `T05library:;` (not `T02`), and qOffsets is already valid.
  This is what makes `--tos none prog.tos` (program starts at reset, before
  any VBL poll) debuggable from the first instruction. After the first client
  has connected once, later program starts only stop while a client is
  connected (no waiting after a detach).
- `--gdb-offsets segments|text` (default `segments` = format above).
  `text` makes qOffsets reply `Text=T;Data=T;Bss=T` (all = TEXT start),
  which is what real gdb needs for ELF executables linked at 0 with
  contiguous TEXT/DATA/BSS (gdb treats the values as offsets and also
  requires Bss==Data). Runtime toggle: `monitor gdb offsets segments|text`.
  The VS Code adapter should keep using the default.
- `monitor gdb status` also prints `offsets: segments|text`.
- Refused monitor commands (cont/step/next/dsp*/quit/reset): the error
  message is sent as `O` output packet(s), followed by `OK` (not `Exx`).
- `k` and `vKill` quit Hatari (exit code 0); `k` gets no reply.
- Watchpoint length 1..64 bytes. Z2 splits the range into byte/word/long
  value-change conditions. Z3/Z4 match overlapping CPU byte/word/long accesses
  and report `T05rwatch:ADDR;` or `T05awatch:ADDR;` after the instruction.
- Uninitialized-exception-handler stops (`--debug-except nohandler`) => `T05`.
- Connect and 0x03 are checked once per VBL while running and every 100 ms
  while emulation is paused from the Hatari GUI/shortcut. A remote continue
  or step resumes a GUI pause.
- Real gdb without an ELF file needs `set architecture m68k:68000` and
  `set endian big` (tdesc carries no byte order).
- 0x03 received while stopped (e.g. crossing a stop reply) is ignored; it never causes a later stray T02.
- Stub (tagged) breakpoints are owned by the stub: they are not part of
  the user `b` numbering (listed separately with "(gdb)"), and `b all`,
  `b <N>`, debugger scripts and memory snapshot restore don't remove them.
- `M`/`X` to the IO area ($FF8000-$FFFFFF) => `E01` (would only change the
  shadow array). `M`/`X` with len > 0x4000 or too little data => `E01`.
  Client memory writes re-baseline Z2 watchpoints (no spurious watch stop).
- qRcmd longer than 4096 chars => `E01`. Monitor output is captured from
  stdout/stderr; if the user opened a debugger log file with `monitor f
  <file>`, output that Hatari sends to that log goes to the file instead.
- Emulator reset forgets the current program (qOffsets/exec-file => E01
  until the next program start) and drops a pending exec breakpoint;
  `monitor gdb stop-on-exec off` also drops a pending exec breakpoint.
