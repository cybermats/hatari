/*
  Hatari - gdbstub.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Public header for the GDB Remote Serial Protocol server (gdbstub).
*/

#ifndef HATARI_GDBSTUB_H
#define HATARI_GDBSTUB_H

/* needs debugui.h for debug_reason_t */

/* for options.c */
extern const char *GdbStub_SetPort(int port);
extern void GdbStub_SetStopOnExec(bool enable);
extern bool GdbStub_SetOffsetsMode(const char *mode);
extern bool GdbStub_IsEnabled(void);

/* for gui_event.c, called once per VBL or while paused */
extern void GdbStub_Poll(void);

/* for gemdos.c & tos.c, when program is started */
extern void GdbStub_ProgramExec(const char *path, const uint32_t *offsets);

/* for reset.c */
extern void GdbStub_Reset(void);

/* for debugui.c */
extern bool GdbStub_WantsSession(debug_reason_t reason);
extern void GdbStub_Session(debug_reason_t reason);
extern void GdbStub_SetException(int nr);

/* CPU data accesses (instruction fetches and debugger memory are excluded). */
extern void GdbStub_CpuDataAccess(uint32_t addr, unsigned int size, bool write);
extern bool GdbStub_HasAccessWatchpoints(void);
extern bool GdbStub_HasPendingAccessWatchpoint(void);
extern void GdbStub_CheckCpuModel(void);
/* Track subroutine returns while a remote client is connected. */
extern bool GdbStub_HasCallTracking(void);
extern void GdbStub_CpuInstruction(uint32_t instruction_pc, uint32_t next_pc, uint32_t sp);

/* for main, when Hatari exits */
extern void GdbStub_Exit(int exitval);

#endif /* HATARI_GDBSTUB_H */
