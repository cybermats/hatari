# Hatari remote debugger – status and roadmap

Working notes for the GDB stub + VS Code/DAP debugger work. Read this first
when picking the work up again. The work is split across:

| What | Where |
|---|---|
| GDB stub, tests, docs, this file | branch `gdbstub-debug-server` of [cybermats/hatari](https://github.com/cybermats/hatari) |
| DAP adapter / VS Code extension, `examples/vbl-colors` | [cybermats/vscode-hatari](https://github.com/cybermats/vscode-hatari) (`main`), checked out next to hatari |
| IntelliJ (LSP4IJ) copy of the sample | `~/code/vbl-colors` (local git repo only, absolute paths) |

## Goal

Debug Atari ST programs running in Hatari from an external editor instead of
the console debugger. Current focus: **assembler (vasm), ST/STFM/STE (68000)
only, Linux/macOS only**. A C example and relocation helper are present but
still need cross-compiler and CLion validation.
Editors: VS Code, and JetBrains CLion (via LSP4IJ for asm, built-in GDB
Remote Debug for C).

## Architecture

```
VS Code / CLion+LSP4IJ ──DAP──▶ vscode-hatari (TypeScript adapter, own repo)
                                   │  vasm listing → source mapping
                                   ▼  GDB Remote Serial Protocol, TCP 127.0.0.1
gdb-multiarch / CLion GDB ──RSP──▶ Hatari: src/debug/gdbstub.c (--gdb-port N)
```

- Hatari implements a standard GDB RSP server, so any gdb client works.
- The adapter talks RSP directly (no gdb binary), maps asm source lines via
  vasm listing files, and uses `monitor` (qRcmd) to run Hatari console
  debugger commands for things gdb has no packet for.
- Protocol contract (both sides code against it, incl. implementation
  notes): **[doc/gdbstub-protocol.md](doc/gdbstub-protocol.md)**.

## What's done

**Hatari stub** (`src/debug/gdbstub.c/.h`)
- Options `--gdb-port`, `--gdb-stop-on-exec`, `--gdb-offsets segments|text`.
- `DebugUI()` hands the session to the stub when a client is connected;
  console debugger unchanged otherwise. Per-VBL poll for connect / ^C in
  `src/sdl/gui_event.c`.
- Registers, memory (IO-area writes refused), c/s/vCont, Z0/Z1 breakpoints,
  Z2 write and Z3/Z4 read/access watchpoints, stop at program start
  (`T05library:;`), `qOffsets`,
  `qXfer:exec-file`, `monitor` passthrough with output capture.
- Stub breakpoints are tagged in `breakcond.c` and owned by the stub
  (separate from user `b` numbering, survive `b all` / snapshot restore).
- Hooks: `gemdos.c`/`tos.c` (program start), `main_sdl.c` (exit),
  `reset.c` (forget program state).

**VS Code adapter** (separate repo, [cybermats/vscode-hatari](https://github.com/cybermats/vscode-hatari))
- Source breakpoints via vasm listing (`-L`), function/conditional/hit
  count/instruction/data(read, write, access) breakpoints, logpoints.
- Multi-module source mapping from vlink `-M` maps (section positions for
  each separately assembled listing).
- Step in/over/out, pause, registers (SR flags broken out), decoded hardware
  panels, memory view, disassembly, Hatari commands in debug console. Raster
  stops, screen snapshots, and per-line CPU cycle profile annotations.
- Standalone DAP over stdio (`node dist/debugAdapter.js`) or
  `--port=N` (127.0.0.1 only) for LSP4IJ.
- `examples/vbl-colors`: small XBIOS/GEMDOS program (palette cycling,
  seconds counter) with a vasm build task + problem matcher, launch
  configuration and a guided debugging tour. End-to-end tested against
  Hatari with EmuTOS (line/data breakpoints, setVariable, step out, `info`).
- Release package contains a PNG icon, changelog, README, and bundled adapter.
  `RELEASING.md` documents Marketplace and Open VSX steps. `@vscode/vsce` is
  not a devDependency; `npm run package` runs it through `npx`.

**IntelliJ / LSP4IJ** (`~/code/vbl-colors`)
- `.run/build.run.xml` (Shell Script, runs `make`) and
  `.run/Debug vbl_colors in Hatari.run.xml` (LSP4IJ `DAPConfiguration`,
  stdio adapter, `*.s`/`*.i`/`*.asm` mappings, build as Before launch).
  The XML format was taken from LSP4IJ's source and checked with
  IntelliJ's serializer.
- Tried in IntelliJ: build, launch, program run to exit and clean
  disconnect work.

**Docs**: `doc/debugger.html` ("Remote debugging with GDB protocol", incl.
CLion), options in `doc/manual.html` and `doc/hatari.1`, and the
vscode-hatari README (incl. the LSP4IJ setup).

## Build & test

```sh
# Hatari (serial build is fine; tests need python3)
make -C build && (cd build && ctest)            # incl. gdbstub-rsp, gdbstub-gdb
#   gdbstub-gdb needs gdb-multiarch, otherwise it's skipped

# Extension (node via nvm), in the vscode-hatari checkout
source ~/.nvm/nvm.sh
npm install && npm run build && npm test
npx tsc --noEmit
npm run package                                  # -> hatari-debug-0.1.0.vsix
```

Manual gdb session:
```sh
build/src/hatari --tos none --gdb-port 1234 --gdb-offsets text tests/gdbstub/gdbstub_test.tos &
gdb-multiarch -ex 'set architecture m68k:68000' -ex 'set endian big' \
  -ex 'file tests/gdbstub/gdbstub_test.elf' -ex 'target remote localhost:1234'
```

Test fixtures: `tests/gdbstub/` (asm source, prebuilt `.tos` and `.elf`;
see `tests/gdbstub/CMakeLists.txt` for the DW_AT_comp_dir patch needed
when rebuilding the ELF). Extension fixtures: `test/fixtures/` in vscode-hatari.
vasm/vlink live in `~/.local/vbcc/bin`.

## Gotchas learned

- With `--gdb-port`, Hatari **always waits for the first client** at the first
  program start (needed for `--tos none`, where the program starts before any
  VBL poll).
- Real gdb with an ELF linked at 0 needs `--gdb-offsets text`; the adapter
  uses the default `segments`.
- Hatari (also upstream) ignores SIGTERM in the headless test setup
  (`SDL_VIDEODRIVER=dummy`); use `kill -9` for stuck instances.
- Breakpoints in the middle of an instruction never hit (e.g. `break *($pc+4)`
  on a 6-byte instruction), so the test program just keeps spinning.
- vasm `-Ftos` with `section text/data/bss`: use `text`/`data`/`bss`
  directives or `section .text,code` etc.; `vlink -bataritos` rejects plain
  `section text` names.
- Hatari run from `build/` doesn't find the packaged TOS image; pass
  `--tos /usr/share/hatari/tos.img` explicitly (otherwise it hangs in a
  FATAL dialog). `--tos none` lacks XBIOS, so vbl-colors needs a real TOS.
- EmuTOS prints ~7 `Bus Error reading at address ...` warnings at boot
  (PC=$e00d98, `tst.b (a0)`): that's its hardware probing (DSP at
  $ffffa200, RAM above 4 MB, ...), the same without `--gdb-port`. Harmless.
- IntelliJ started from the desktop has neither nvm's `node` nor
  `~/.local/vbcc/bin` on `PATH`: use absolute paths. In LSP4IJ DAP
  parameters `${workspaceFolder}` is the run configuration's working
  directory. "exit code 137" at the end is LSP4IJ killing the adapter
  after the session ended, not a crash.
- `pkill -f <name>` also matches the shell running it; track PIDs instead.

## Known limitations

- 68000 registers only; no DSP/FPU/MMU. Z3/Z4 watchpoints use the 68000 CPU
  data access path and are unavailable with other CPU models.
- Connect/^C checked once per VBL while running and every 100 ms while paused
  from the GUI.
- Adapter: exact caller frames are tracked only after the remote client connects;
  older stubs use a heuristic scan. Hatari's hit counts are "every N-th hit";
  exception catching only starts once the program runs.
- Profile annotations cover Hatari's last resume and need source mappings and
  a shared temporary-file filesystem. Screen and decoded hardware views still
  need a manual VS Code UI check.
- LSP4IJ: only launch / run / exit is confirmed in IntelliJ so far;
  breakpoints, stepping, variables, the evaluate box (Hatari commands) and
  data breakpoints are untested there, and the README still says
  "not tested".

## Roadmap

The remaining work needs real project and IDE validation, a reverse execution
design, and external publishing steps.

### 1. Use it on a real project (next)
- [x] Sample project (vbl-colors) for VS Code, and an IntelliJ/LSP4IJ copy.
- [x] IntelliJ + LSP4IJ: build, launch and run to exit.
- [ ] IntelliJ + LSP4IJ: verify breakpoints, stepping, variables, evaluate
      box and data breakpoints; update the vscode-hatari README with the
      findings (and maybe ship the `.run/` files with the example).
- [ ] Debug a real vasm project in VS Code; note friction points.

### 2. Asm usability
- [x] a. Multi-module programs: parse the vlink map file (`-M`) for per-module offsets.
- [x] b. DWARF line info from `vasmm68k_mot -Felf -dwarf=3` as an alternative to listings
      (also groundwork for C).
- [x] c. Track JSR/BSR/RTS through a `qHatari:callstack` stub query instead of
      scanning the stack. Tracking starts at connection; exceptions and unusual
      control flow can lose frames.
- [x] d. Read DRI/GST symbols from the `.tos` file itself (names without a listing).

### 3. ST-specific features
- [x] a. Decoded hardware register panels: Shifter (resolution, palette swatches),
      MFP timers/interrupts, Blitter, YM2149, DMA/FDC.
- [x] b. Raster-position breakpoints ("line 100, cycle 40"), run to next VBL/HBL
      (Hatari vars `VBL`, `HBL`, `LineCycles` exist already).
- [x] c. Screen view: stopped ST/STE video memory rendered in a VS Code webview.
- [x] d. Profiler integration: Hatari cycle profile shown inline in the editor.

The screen view's planar decoding and DAP request are tested; the VS Code
webview still needs a manual UI check. It shows a snapshot and does not cover
overscan or per-scanline palette changes. The DMA/FDC panel displays Hatari's
register mirror rather than live FDC status.

### 4. Stub (C) improvements
- [x] a. Read/access watchpoints (Z3/Z4) via the 68000 CPU data-access path.
- [x] b. Respond to connect/^C while emulation is paused from the GUI.
- [ ] c. Reverse step needs a full-state checkpoint/replay or instruction undo
      design. Hatari's `hi` history stores PCs for disassembly, not the register,
      memory, and device state needed to reverse execution.

### 5. C support
- [ ] a. m68k-atari-mint gcc toolchain + documented CLion example project
      (build, ELF+DWARF, Remote Debug config).
- [ ] b. Automatic relocation for gdb/CLion (gdb Python helper doing
      `add-symbol-file` from `qOffsets`), verify `--gdb-offsets text` in practice.

`examples/clion-c/` now has a freestanding C sample, dual TOS/ELF link scripts,
CLion Remote Debug instructions, and a `hatari-symbols` GDB Python helper.
The helper was smoke tested against the assembler fixture; the C build and
CLion UI still need the MiNT cross compiler and an IDE session.

### 6. Upstreaming / distribution
- [ ] a. Ask Hatari maintainers (mailing list / framagit) whether they want the
      stub before sending patches (the extension stays in its own repo).
- [x] b. Split into patch series: breakcond.c tagged breakpoints, stub + tests,
      docs. Upstream is framagit.org/hatari/hatari (GitHub is only a mirror).

The reviewed nine-patch series, including Z3/Z4, raster stops, call tracking,
and profile status, is on local branch `gdbstub-upstream-series` and under
`/tmp/gdbstub-upstream-patches/`.
- [ ] c. Publish the extension (cybermats/vscode-hatari) on VS Code
      Marketplace and Open VSX. The VSIX, icon, release notes, and publishing
      guide are ready; the release owner must verify the `hatari` publisher ID
      and complete registry account steps.
- [x] d. CI: GitHub Actions for Hatari build + ctest on the fork, and
      `npm test` + package on vscode-hatari. Workflows added; hosted run status
      needs inspection after the first push.

## Repo / remotes

- Before pushing either repository, review the commits and changed code.
- `origin` = https://framagit.org/hatari/hatari (official upstream)
- `cybermats` = git@github.com:cybermats/hatari.git (fork, branch pushed here)
- Fetch and review upstream changes before rebasing; a published branch rebase
  requires a reviewed `--force-with-lease` push.
- The branch history was rewritten (2026-10-24) to drop the extension
  commits; the extension's history lives on in vscode-hatari.
- vscode-hatari: `origin` = git@github.com:cybermats/vscode-hatari.git.
  Its sample expects `../hatari/build/src/hatari` relative to the repo.
