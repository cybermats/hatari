/*
  Hatari - gdbstub.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  gdbstub.c - GDB Remote Serial Protocol server, for debugging 68000
  programs running in Hatari with external debuggers (gdb, IDEs).

  The stub listens on a localhost TCP port.  New clients and the 0x03
  interrupt byte are checked by GdbStub_Poll() once per VBL or every 100 ms
  while the GUI is paused.  When emulation stops (connect, interrupt,
  breakpoint, step, exception...),
  DebugUI() hands the debugger session to GdbStub_Session() instead of
  the console debugger, which serves GDB packets until the client resumes
  emulation.  Breakpoints and write value-change watchpoints use tagged
  Hatari conditional breakpoints; read/access watchpoints use CPU data
  access hooks.  Monitor commands ("qRcmd") are
  executed by the normal Hatari debugger command parser.

  Only 68000 registers are supported (no FPU/MMU/DSP).
*/
const char GdbStub_fileid[] = "Hatari gdbstub.c";

#include "config.h"

#if HAVE_UNIX_DOMAIN_SOCKETS
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <poll.h>
# include <fcntl.h>
# include <errno.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "main.h"
#include "configuration.h"
#include "gui_event.h"
#include "hatari-glue.h"
#include "log.h"
#include "m68000.h"
#include "screen.h"
#include "stMemory.h"
#include "video.h"

#include "debug_priv.h"
#include "68kDisass.h"
#include "breakcond.h"
#include "debugcpu.h"
#include "debugInfo.h"
#include "debugui.h"
#include "gdbstub.h"


#if HAVE_UNIX_DOMAIN_SOCKETS

/* max packet size advertised to client (0x4000 = 16384 bytes) */
#define GDB_PACKET_SIZE    0x4000
/* receive / transmit buffer sizes (packet + framing) */
#define GDB_BUFFER_SIZE    (GDB_PACKET_SIZE + 16)
#define GDB_PACKET_RETRIES 10
/* RSP quotes reserved packet bytes by XORing this bit. */
#define GDB_ESCAPE_XOR     0x20
/* how often (ms) SDL events are processed while waiting for client */
#define GDB_POLL_MS        100
/* number of registers in 'g' packet: d0-d7, a0-a7, sr, pc */
#define GDB_NUM_REGS       18
#define GDB_REG_A7         15
#define GDB_REG_SR         16
#define GDB_REG_PC         17
/* address mask for 68000 */
#define GDB_ADDR_MASK      0x00ffffff
/* max number of stub breakpoints/watchpoints */
#define GDB_MAX_BPS        64
#define GDB_MAX_CALL_FRAMES 64
/* max watchpoint length */
#define GDB_MAX_WATCH_LEN  64
#define GDB_RASTER_MAX_LINE 599
#define GDB_RASTER_MAX_CYCLE 2048
/* max output size for monitor commands */
#define GDB_MAX_MONITOR_OUTPUT (1024*1024)

/* 68000 control-flow opcodes used for remote call tracking. */
#define GDB_BSR_MASK       0xff00
#define GDB_BSR_OPCODE     0x6100
#define GDB_JSR_MASK       0xffc0
#define GDB_JSR_OPCODE     0x4e80
#define GDB_RTD_OPCODE     0x4e74
#define GDB_RTS_OPCODE     0x4e75
#define GDB_RTR_OPCODE     0x4e77
#define GDB_RTE_OPCODE     0x4e73

/* GDB signal numbers used in stop replies */
#define GDB_SIGINT   0x02
#define GDB_SIGILL   0x04
#define GDB_SIGTRAP  0x05
#define GDB_SIGFPE   0x08
#define GDB_SIGBUS   0x0a

/* special return values from input functions */
enum {
	GDB_EOF = -1,      /* client disconnected / error */
	GDB_TIMEOUT = -2,  /* no data within given time */
	GDB_QUIT = -3      /* user requested Hatari quit */
};

/* command handler results */
typedef enum {
	GDB_CMD_STAY,      /* continue serving packets */
	GDB_CMD_RESUME,    /* resume emulation, client waits for stop reply */
	GDB_CMD_DETACH     /* client detached / killed */
} gdb_cmd_t;

/* stub owned breakpoint types */
typedef enum {
	GDB_BP_SW,         /* Z0 / Z1 */
	GDB_BP_WATCH,      /* Z2: value change */
	GDB_BP_READ,       /* Z3: data read */
	GDB_BP_ACCESS,     /* Z4: data read or write */
	GDB_BP_RASTER,     /* recurring scanline/cycle breakpoint */
	GDB_BP_NEXT_VBL,   /* run to next VBL */
	GDB_BP_NEXT_HBL,   /* run to next HBL */
	GDB_BP_EXEC        /* temporary breakpoint at program start */
} gdb_bp_type_t;

typedef struct {
	gdb_bp_type_t type;
	uint32_t addr;
	uint32_t len;
	int tag;           /* tag of corresponding Hatari breakpoint(s) */
} gdb_bp_t;

static int ListenFd = -1;
static int ClientFd = -1;
static int ListenPort;
static bool bStopOnExec = true;
static bool bTextOffsets;      /* all qOffsets values = TEXT start */
static bool bEverConnected;    /* whether any client has connected */
static bool bNoAck;            /* QStartNoAckMode active */
static bool bRunning;          /* client resumed, waits for stop reply */
static int ExceptionNr;        /* exception that invoked debugger */
static char StopReply[48] = "T02";

static gdb_bp_t Bps[GDB_MAX_BPS];
static int BpCount;
static int NextTag = 1;
static int PendingAccessBp = -1;
static int AccessBpCount;
/* Return addresses of calls executed since remote tracking began. */
static uint32_t CallReturns[GDB_MAX_CALL_FRAMES];
static unsigned int CallDepth;
static bool bCallTracking;

/* current program info, recorded at program start */
static bool bHavePrg;
static bool bInitialResetDone;
static bool bOffsetsInvalidated;
static char PrgPath[FILENAME_MAX];
static uint32_t PrgOffsets[3];

/* input buffer */
static uint8_t InBuf[4096];
static int InLen, InPos;

/* packet buffers */
static char RxPacket[GDB_BUFFER_SIZE];
static char TxPacket[2*GDB_BUFFER_SIZE];

static const char TargetXml[] =
	"<?xml version=\"1.0\"?>\n"
	"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
	"<target version=\"1.0\">\n"
	"<architecture>m68k</architecture>\n"
	"<feature name=\"org.gnu.gdb.m68k.core\">\n"
	"<reg name=\"d0\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"d1\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"d2\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"d3\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"d4\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"d5\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"d6\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"d7\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"a0\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"a1\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"a2\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"a3\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"a4\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"a5\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>\n"
	"<reg name=\"ps\" bitsize=\"32\" type=\"int32\"/>\n"
	"<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>\n"
	"</feature>\n"
	"</target>\n";

static const char HexChars[] = "0123456789abcdef";


/* ------------------------ helpers ------------------------ */

/**
 * Return value of given hex digit, or -1 if it's not one
 */
static int GdbStub_HexVal(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/**
 * Parse hex number from *str, advance *str past it.
 * Return false if there were no hex digits.
 */
static bool GdbStub_ParseHex(const char **str, uint32_t *value)
{
	const char *s = *str;
	uint32_t v = 0;
	int d;

	while ((d = GdbStub_HexVal((unsigned char)*s)) >= 0)
	{
		if (v > (UINT32_MAX >> 4))
			return false;
		v = (v << 4) | d;
		s++;
	}
	if (s == *str)
		return false;
	*str = s;
	*value = v;
	return true;
}

/**
 * Parse "addr,len" from given string, advance it past them.
 * Return false on syntax error.
 */
static bool GdbStub_ParseAddrLen(const char **str, uint32_t *addr, uint32_t *len)
{
	if (!GdbStub_ParseHex(str, addr))
		return false;
	if (**str != ',')
		return false;
	(*str)++;
	return GdbStub_ParseHex(str, len);
}

/**
 * Append 32-bit big endian value as hex to given buffer
 */
static char *GdbStub_PutHex32(char *out, uint32_t value)
{
	int shift;
	for (shift = 28; shift >= 0; shift -= 4)
		*out++ = HexChars[(value >> shift) & 0xf];
	return out;
}

/**
 * Return true if given (24-bit) address is in valid Atari memory
 */
static bool GdbStub_ValidAddress(uint32_t addr)
{
	addr &= GDB_ADDR_MASK;
	return get_mem_bank(addr).baseaddr != NULL;
}


/* ------------------------ socket I/O ------------------------ */

/**
 * Close the client connection and clean up the stub breakpoints
 */
static void GdbStub_CloseClient(void)
{
	if (ClientFd >= 0)
	{
		close(ClientFd);
		ClientFd = -1;
		fprintf(stderr, "GDB client disconnected.\n");
	}
	BreakCond_RemoveTagged(0);
	BpCount = 0;
	AccessBpCount = 0;
	CallDepth = 0;
	bCallTracking = false;
	PendingAccessBp = -1;
	set_x_funcs_gdb_data_watch(false);
	InLen = InPos = 0;
	bNoAck = false;
	bRunning = false;
	DebugCpu_SetSteps(0);
	DebugCpu_BreakpointsChanged();
}

/**
 * Read a byte from client with given timeout (ms, -1 = forever).
 * Return byte value, GDB_TIMEOUT or GDB_EOF.
 */
static int GdbStub_ReadByte(int timeout)
{
	struct pollfd pfd;
	ssize_t len;
	int ret;

	if (InPos < InLen)
		return InBuf[InPos++];
	if (ClientFd < 0)
		return GDB_EOF;

	pfd.fd = ClientFd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	ret = poll(&pfd, 1, timeout);
	if (ret == 0 || (ret < 0 && errno == EINTR))
		return GDB_TIMEOUT;
	if (ret < 0)
		return GDB_EOF;

	len = recv(ClientFd, InBuf, sizeof(InBuf), 0);
	if (len < 0 && (errno == EINTR || errno == EAGAIN))
		return GDB_TIMEOUT;
	if (len <= 0)
		return GDB_EOF;
	InLen = len;
	InPos = 0;
	return InBuf[InPos++];
}

/**
 * Wait for a byte from client, while keeping UI alive.
 * Return byte value, GDB_EOF or GDB_QUIT.
 */
static int GdbStub_WaitByte(void)
{
	int c;
	for (;;)
	{
		c = GdbStub_ReadByte(GDB_POLL_MS);
		if (c != GDB_TIMEOUT)
			return c;
		if (GuiEvent_PollQuit())
			return GDB_QUIT;
	}
}

/**
 * Write given data fully to client.  Return false on error.
 */
static bool GdbStub_Write(const char *data, size_t len)
{
	int flags = 0;
#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
	while (len > 0)
	{
		ssize_t ret = send(ClientFd, data, len, flags);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return false;
		data += ret;
		len -= ret;
	}
	return true;
}

/**
 * Send packet with given (binary) data to client, escape special chars
 * and wait for ack unless no-ack mode is active.
 * Return false on error.
 */
static bool GdbStub_SendPacketLen(const char *data, size_t len)
{
	uint8_t csum = 0;
	char *out = TxPacket;
	size_t i;
	int c, tries;

	if (ClientFd < 0)
		return false;

	*out++ = '$';
	for (i = 0; i < len && out < TxPacket + sizeof(TxPacket) - 5; i++)
	{
		char ch = data[i];
		if (ch == '$' || ch == '#' || ch == '}' || ch == '*')
		{
			*out++ = '}';
			csum += '}';
			ch ^= GDB_ESCAPE_XOR;
		}
		*out++ = ch;
		csum += (uint8_t)ch;
	}
	*out++ = '#';
	*out++ = HexChars[csum >> 4];
	*out++ = HexChars[csum & 0xf];

	for (tries = 0; tries < GDB_PACKET_RETRIES; tries++)
	{
		if (!GdbStub_Write(TxPacket, out - TxPacket))
			return false;
		if (bNoAck)
			return true;
		do {
			c = GdbStub_WaitByte();
			if (c < 0)
				return false;
		} while (c != '+' && c != '-' && c != '$');
		if (c == '+')
			return true;
		if (c == '$')
		{
			/* client sent new packet instead of ack, handle it next */
			InPos--;
			return true;
		}
	}
	return false;
}

/**
 * Send given string as packet to client
 */
static bool GdbStub_SendPacket(const char *str)
{
	return GdbStub_SendPacketLen(str, strlen(str));
}

/**
 * Read a packet from client into RxPacket (with escapes removed).
 * Return packet length, or GDB_EOF / GDB_QUIT.
 */
static int GdbStub_ReadPacket(void)
{
	int c, len, hi, lo;
	uint8_t csum;
	bool escape, overflow;

	for (;;)
	{
		c = GdbStub_WaitByte();
		if (c < 0)
			return c;
		if (c != '$')
		{
			/* ignore acks, and interrupt requests while stopped */
			continue;
		}
		len = 0;
		csum = 0;
		escape = overflow = false;
		for (;;)
		{
			c = GdbStub_WaitByte();
			if (c < 0)
				return c;
			if (c == '#')
				break;
			csum += (uint8_t)c;
			if (escape)
			{
				c ^= GDB_ESCAPE_XOR;
				escape = false;
			}
			else if (c == '}')
			{
				escape = true;
				continue;
			}
			if (len < GDB_BUFFER_SIZE - 1)
				RxPacket[len++] = c;
			else
				overflow = true;
		}
		if ((hi = GdbStub_WaitByte()) < 0)
			return hi;
		if ((lo = GdbStub_WaitByte()) < 0)
			return lo;
		RxPacket[len] = '\0';

		if (!bNoAck)
		{
			if (GdbStub_HexVal(hi) < 0 || GdbStub_HexVal(lo) < 0 ||
			    (GdbStub_HexVal(hi) << 4 | GdbStub_HexVal(lo)) != csum)
			{
				GdbStub_Write("-", 1);
				continue;
			}
			GdbStub_Write("+", 1);
		}
		if (overflow)
		{
			GdbStub_SendPacket("E01");
			continue;
		}
		return len;
	}
}

/**
 * Set up newly accepted client socket
 */
static void GdbStub_SetupClient(int fd)
{
	int flags, one = 1;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	ClientFd = fd;
	InLen = InPos = 0;
	bNoAck = false;
	bRunning = false;
	bEverConnected = true;
	fprintf(stderr, "GDB client connected.\n");
}

/**
 * Accept new client if one is pending, wait at max given time (ms).
 * Return true if client got connected.
 */
static bool GdbStub_Accept(int timeout)
{
	struct pollfd pfd;
	int fd;

	if (ListenFd < 0)
		return false;
	pfd.fd = ListenFd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, timeout) <= 0)
		return false;
	fd = accept(ListenFd, NULL, NULL);
	if (fd < 0)
		return false;
	if (ClientFd >= 0)
	{
		/* only one client at the time */
		close(fd);
		return false;
	}
	GdbStub_SetupClient(fd);
	return true;
}


/* ------------------------ breakpoints ------------------------ */

/**
 * Drop stub breakpoint entries whose Hatari breakpoints have
 * disappeared (should not happen, as user commands don't remove
 * tagged breakpoints, but make sure stub doesn't claim to have
 * breakpoints that aren't there)
 */
static void GdbStub_PruneBps(void)
{
	int i;
	for (i = BpCount - 1; i >= 0; i--)
	{
		if (Bps[i].tag && !BreakCond_TagExists(Bps[i].tag))
			Bps[i] = Bps[--BpCount];
	}
}

/**
 * Return index of stub breakpoint with given type & address, or -1
 */
static int GdbStub_FindBp(gdb_bp_type_t type, uint32_t addr, uint32_t len)
{
	int i;
	GdbStub_PruneBps();
	for (i = 0; i < BpCount; i++)
	{
		if (Bps[i].type == type && Bps[i].addr == addr &&
		    (type == GDB_BP_SW || type == GDB_BP_EXEC || Bps[i].len == len))
			return i;
	}
	return -1;
}

/**
 * Remove stub breakpoint at given index
 */
static void GdbStub_RemoveBpAt(int idx)
{
	if (Bps[idx].tag)
		BreakCond_RemoveTagged(Bps[idx].tag);
	else
	{
		if (--AccessBpCount == 0)
			set_x_funcs_gdb_data_watch(false);
	}
	PendingAccessBp = -1;
	Bps[idx] = Bps[--BpCount];
}

/**
 * Add stub breakpoint of given type.
 * Return false on failure.
 */
static bool GdbStub_AddBp(gdb_bp_type_t type, uint32_t addr, uint32_t len)
{
	char expression[64];
	uint32_t offset, size;
	int tag;

	if (BpCount >= GDB_MAX_BPS)
		return false;

	tag = (type == GDB_BP_READ || type == GDB_BP_ACCESS) ? 0 : NextTag++;
	if (type == GDB_BP_WATCH)
	{
		/* one value change tracking condition per (max) long */
		for (offset = 0; offset < len; offset += size)
		{
			const char *width;
			size = len - offset;
			if (size >= 4)
			{
				size = 4;
				width = "l";
			}
			else if (size >= 2)
			{
				size = 2;
				width = "w";
			}
			else
				width = "b";
			snprintf(expression, sizeof(expression), "($%x).%s ! ($%x).%s",
				 addr + offset, width, addr + offset, width);
			if (!BreakCond_AddTagged(expression, tag))
			{
				BreakCond_RemoveTagged(tag);
				return false;
			}
		}
	}
	else if (tag)
	{
		snprintf(expression, sizeof(expression), "pc=$%x", addr);
		if (!BreakCond_AddTagged(expression, tag))
			return false;
	}
	Bps[BpCount].type = type;
	Bps[BpCount].addr = addr;
	Bps[BpCount].len = len;
	Bps[BpCount].tag = tag;
	BpCount++;
	if (!tag)
	{
		if (AccessBpCount++ == 0)
			set_x_funcs_gdb_data_watch(true);
	}
	return true;
}

/** Add a video position breakpoint sampled at CPU instruction boundaries. */
static bool GdbStub_AddVideoBp(gdb_bp_type_t type, uint32_t line, uint32_t cycle)
{
	char expression[128];
	int tag;

	if (BpCount >= GDB_MAX_BPS)
		return false;
	tag = NextTag++;
	if (type == GDB_BP_RASTER)
	{
		if (cycle)
			snprintf(expression, sizeof(expression),
				 "RasterLine = %u && LineCycles > %u && VBL ! %u",
				 line, cycle - 1, (unsigned)nVBLs - 1);
		else
			snprintf(expression, sizeof(expression),
				 "RasterLine = %u && VBL ! %u",
				 line, (unsigned)nVBLs - 1);
	}
	else if (type == GDB_BP_NEXT_VBL)
		snprintf(expression, sizeof(expression), "VBL ! %u", (unsigned)nVBLs);
	else
		snprintf(expression, sizeof(expression), "HBL ! %u", (unsigned)nHBL);
	if (!BreakCond_AddTagged(expression, tag))
		return false;
	Bps[BpCount++] = (gdb_bp_t){ type, line, cycle, tag };
	return true;
}

/** Remove all video position breakpoints of the given type. */
static void GdbStub_RemoveVideoBp(gdb_bp_type_t type)
{
	int i;
	for (i = BpCount - 1; i >= 0; i--)
		if (Bps[i].type == type)
			GdbStub_RemoveBpAt(i);
}

/**
 * Return index of the pending program start breakpoint, or -1
 */
static int GdbStub_FindExecBp(void)
{
	int i;
	GdbStub_PruneBps();
	for (i = 0; i < BpCount; i++)
	{
		if (Bps[i].type == GDB_BP_EXEC)
			return i;
	}
	return -1;
}

/**
 * Remove pending program start breakpoint
 */
static void GdbStub_RemoveExecBp(void)
{
	int idx = GdbStub_FindExecBp();
	if (idx >= 0)
		GdbStub_RemoveBpAt(idx);
}

/**
 * Handle Z/z packets
 */
static void GdbStub_HandleBreakpoint(const char *pkt)
{
	bool insert = (pkt[0] == 'Z');
	uint32_t addr, len;
	gdb_bp_type_t type;
	const char *p;
	int idx;

	switch (pkt[1])
	{
	case '0':
	case '1':
		type = GDB_BP_SW;
		break;
	case '2':
		type = GDB_BP_WATCH;
		break;
	case '3':
		type = GDB_BP_READ;
		break;
	case '4':
		type = GDB_BP_ACCESS;
		break;
	default:
		GdbStub_SendPacket("");
		return;
	}
	if ((type == GDB_BP_READ || type == GDB_BP_ACCESS) &&
	    currprefs.cpu_model != 68000)
	{
		GdbStub_SendPacket("");
		return;
	}
	p = pkt + 2;
	if (*p++ != ',' || !GdbStub_ParseAddrLen(&p, &addr, &len))
	{
		GdbStub_SendPacket("E01");
		return;
	}
	addr &= GDB_ADDR_MASK;
	if (type != GDB_BP_SW && (len < 1 || len > GDB_MAX_WATCH_LEN ||
	    addr + len > GDB_ADDR_MASK + 1))
	{
		GdbStub_SendPacket("E01");
		return;
	}
	idx = GdbStub_FindBp(type, addr, len);
	if (!insert)
	{
		if (idx >= 0)
			GdbStub_RemoveBpAt(idx);
		GdbStub_SendPacket("OK");
		return;
	}
	if (idx >= 0 || GdbStub_AddBp(type, addr, len))
		GdbStub_SendPacket("OK");
	else
		GdbStub_SendPacket("E01");
}


/** Return whether CPU data access hooks are needed for Z3/Z4 watches. */
bool GdbStub_HasAccessWatchpoints(void)
{
	return AccessBpCount != 0;
}

/** Return whether a CPU access has hit a Z3/Z4 watchpoint. */
bool GdbStub_HasPendingAccessWatchpoint(void)
{
	return PendingAccessBp >= 0;
}

/** Disconnect a client whose active access watches cannot survive a CPU change. */
void GdbStub_CheckCpuModel(void)
{
	if (AccessBpCount && currprefs.cpu_model != 68000)
	{
		/* Existing Z3/Z4 hooks cannot observe another CPU model.  Tell
		 * the client by disconnecting instead of leaving accepted watches
		 * silently inactive. */
		fprintf(stderr, "GDB client disconnected: read/access watchpoints require a 68000 CPU.\n");
		GdbStub_CloseClient();
	}
}

/** Return whether remote 68000 subroutine call tracking is active. */
bool GdbStub_HasCallTracking(void)
{
	return bCallTracking && ClientFd >= 0;
}

/** Called after each instruction while connected. Only actual stack pushes
 * matching a JSR/BSR return address become frames. Return through several
 * frames (e.g. longjmp) discards all frames up to the matching address. */
void GdbStub_CpuInstruction(uint32_t instruction_pc, uint32_t next_pc, uint32_t sp)
{
	uint16_t opcode;
	uint32_t ret;
	unsigned int i;

	if (!GdbStub_HasCallTracking() || !GdbStub_ValidAddress(instruction_pc & GDB_ADDR_MASK))
		return;
	opcode = STMemory_ReadWord(instruction_pc & GDB_ADDR_MASK);
	if ((opcode & GDB_BSR_MASK) == GDB_BSR_OPCODE ||
	    (opcode & GDB_JSR_MASK) == GDB_JSR_OPCODE)
	{
		if (!GdbStub_ValidAddress(sp & GDB_ADDR_MASK) ||
		    !GdbStub_ValidAddress((sp + 3) & GDB_ADDR_MASK))
			return;
		ret = STMemory_ReadLong(sp & GDB_ADDR_MASK) & GDB_ADDR_MASK;
		if (ret != (Disasm_GetNextPC(instruction_pc) & GDB_ADDR_MASK) ||
		    (next_pc & GDB_ADDR_MASK) == ret)
			return;
		if (CallDepth == GDB_MAX_CALL_FRAMES)
		{
			memmove(CallReturns, CallReturns + 1,
				    (GDB_MAX_CALL_FRAMES - 1) * sizeof(CallReturns[0]));
			CallDepth--;
		}
		CallReturns[CallDepth++] = ret;
	}
	else if (opcode == GDB_RTS_OPCODE || opcode == GDB_RTD_OPCODE ||
		 opcode == GDB_RTR_OPCODE)
	{
		for (i = CallDepth; i > 0; i--)
			if (CallReturns[i-1] == (next_pc & GDB_ADDR_MASK))
			{
				CallDepth = i - 1;
				break;
			}
	}
	else if (opcode == GDB_RTE_OPCODE) /* handler calls cannot be distinguished */
		CallDepth = 0;
}

/** Record the first matching CPU data access for the next stop reply. */
void GdbStub_CpuDataAccess(uint32_t addr, unsigned int size, bool write)
{
	int i;
	uint32_t end;

	if (!AccessBpCount || PendingAccessBp >= 0 || !size)
		return;
	addr &= GDB_ADDR_MASK;
	end = addr + size;
	for (i = 0; i < BpCount; i++)
	{
		const gdb_bp_t *bp = &Bps[i];
		if (bp->type != GDB_BP_ACCESS &&
		    (bp->type != GDB_BP_READ || write))
			continue;
		if ((addr < bp->addr + bp->len && end > bp->addr) ||
		    (end > GDB_ADDR_MASK + 1 &&
		     end - (GDB_ADDR_MASK + 1) > bp->addr))
		{
			PendingAccessBp = i;
			return;
		}
	}
}

/* ------------------------ registers ------------------------ */

/**
 * Return value of given GDB register number
 */
static uint32_t GdbStub_GetReg(int reg)
{
	if (reg < GDB_REG_SR)
		return Regs[REG_D0 + reg];
	if (reg == GDB_REG_SR)
		return M68000_GetSR();
	return M68000_GetPC();
}

/**
 * Set given GDB register to given value
 */
static void GdbStub_SetReg(int reg, uint32_t value)
{
	if (((reg == GDB_REG_A7 || reg == GDB_REG_PC) && value != GdbStub_GetReg(reg)) ||
	    (reg == GDB_REG_SR && ((value ^ M68000_GetSR()) & SR_SUPERMODE)))
		CallDepth = 0;
	if (reg < GDB_REG_SR)
		Regs[REG_D0 + reg] = value;
	else if (reg == GDB_REG_SR)
	{
		if ((value & 0xffff) != M68000_GetSR())
			M68000_SetSR(value & 0xffff);
	}
	else if (value != M68000_GetPC())
		M68000_SetPC(value);
}

/**
 * Handle 'g' packet
 */
static void GdbStub_ReadRegisters(void)
{
	char buf[GDB_NUM_REGS*8+1], *out = buf;
	int i;

	for (i = 0; i < GDB_NUM_REGS; i++)
		out = GdbStub_PutHex32(out, GdbStub_GetReg(i));
	*out = '\0';
	GdbStub_SendPacket(buf);
}

/**
 * Parse 8 hex digits (32-bit big endian) from given string.
 * Return false on error.
 */
static bool GdbStub_Hex32(const char *p, uint32_t *value)
{
	uint32_t v = 0;
	int i, d;
	for (i = 0; i < 8; i++)
	{
		if ((d = GdbStub_HexVal((unsigned char)p[i])) < 0)
			return false;
		v = (v << 4) | d;
	}
	*value = v;
	return true;
}

/**
 * Handle 'G' packet
 */
static void GdbStub_WriteRegisters(const char *p)
{
	uint32_t values[GDB_NUM_REGS];
	int i;

	if (strlen(p) < GDB_NUM_REGS*8)
	{
		GdbStub_SendPacket("E01");
		return;
	}
	for (i = 0; i < GDB_NUM_REGS; i++)
	{
		if (!GdbStub_Hex32(p + 8*i, &values[i]))
		{
			GdbStub_SendPacket("E01");
			return;
		}
	}
	/* Select the requested stack bank before writing A7. */
	GdbStub_SetReg(GDB_REG_SR, values[GDB_REG_SR]);
	for (i = 0; i < GDB_REG_SR; i++)
		GdbStub_SetReg(i, values[i]);
	GdbStub_SetReg(GDB_REG_PC, values[GDB_REG_PC]);
	GdbStub_SendPacket("OK");
}

/**
 * Handle 'p' & 'P' packets
 */
static void GdbStub_AccessRegister(const char *p, bool write)
{
	char buf[9];
	uint32_t reg, value;

	if (!GdbStub_ParseHex(&p, &reg) || reg >= GDB_NUM_REGS)
	{
		GdbStub_SendPacket("E01");
		return;
	}
	if (!write)
	{
		*GdbStub_PutHex32(buf, GdbStub_GetReg(reg)) = '\0';
		GdbStub_SendPacket(buf);
		return;
	}
	if (*p++ != '=' || !GdbStub_Hex32(p, &value))
	{
		GdbStub_SendPacket("E01");
		return;
	}
	GdbStub_SetReg(reg, value);
	GdbStub_SendPacket("OK");
}


/* ------------------------ memory ------------------------ */

/**
 * Handle 'm' packet, uses same side-effect free memory
 * access as debugger memdump command
 */
static void GdbStub_ReadMemory(const char *p)
{
	uint32_t addr, len, i;
	char *out;

	if (!GdbStub_ParseAddrLen(&p, &addr, &len))
	{
		GdbStub_SendPacket("E01");
		return;
	}
	if (len > (GDB_PACKET_SIZE - 1) / 2)
		len = (GDB_PACKET_SIZE - 1) / 2;

	out = RxPacket;	/* request already parsed, reuse buffer */
	for (i = 0; i < len; i++)
	{
		uint32_t a = (addr + i) & GDB_ADDR_MASK;
		uint8_t value;
		if (!GdbStub_ValidAddress(a))
			break;
		value = STMemory_ReadByte(a);
		*out++ = HexChars[value >> 4];
		*out++ = HexChars[value & 0xf];
	}
	if (len && i == 0)
	{
		GdbStub_SendPacket("E01");
		return;
	}
	*out = '\0';
	GdbStub_SendPacket(RxPacket);
}

/**
 * Handle 'M' (hex) & 'X' (binary) packets.  Writes to IO area are
 * refused, as they would change only the IO memory shadow array,
 * not the emulated device state.
 */
static void GdbStub_WriteMemory(const char *p, int pktlen, bool binary)
{
	uint32_t addr, len, i;
	size_t avail;

	if (!GdbStub_ParseAddrLen(&p, &addr, &len) || *p++ != ':' ||
	    len > GDB_PACKET_SIZE)
	{
		GdbStub_SendPacket("E01");
		return;
	}
	/* data bytes available in the packet */
	avail = (size_t)pktlen - (size_t)(p - RxPacket);
	if ((binary && avail < (size_t)len) ||
	    (!binary && avail < 2 * (size_t)len))
	{
		GdbStub_SendPacket("E01");
		return;
	}
	/* check everything first so that nothing is written on error */
	for (i = 0; i < len; i++)
	{
		uint32_t a = (addr + i) & GDB_ADDR_MASK;
		if (!GdbStub_ValidAddress(a) || memory_region_iomem(a) ||
		    (!binary && (GdbStub_HexVal((unsigned char)p[2*i]) < 0 ||
				 GdbStub_HexVal((unsigned char)p[2*i+1]) < 0)))
		{
			GdbStub_SendPacket("E01");
			return;
		}
	}
	for (i = 0; i < len; i++)
	{
		uint8_t value;
		if (binary)
			value = (uint8_t)p[i];
		else
			value = GdbStub_HexVal((unsigned char)p[2*i]) << 4 |
				GdbStub_HexVal((unsigned char)p[2*i+1]);
		STMemory_WriteByte((addr + i) & GDB_ADDR_MASK, value);
	}
	/* debugger changes shouldn't trigger watchpoints */
	if (len)
		BreakCond_UpdateTaggedTracking();
	GdbStub_SendPacket("OK");
}


/* ------------------------ queries ------------------------ */

/**
 * Get current program TEXT/DATA/BSS addresses.
 * Return false if there's no program.
 */
static bool GdbStub_GetOffsets(uint32_t *text, uint32_t *data, uint32_t *bss)
{
	/* After a reset, the TOS basepage may still describe its shell or loader.
	 * An initial attach can use a valid basepage without a ProgramExec hook. */
	if (bOffsetsInvalidated)
		return false;
	*text = DebugInfo_GetTEXT();
	if (*text)
	{
		*data = DebugInfo_GetDATA();
		*bss = DebugInfo_GetBSS();
		return true;
	}
	/* e.g. TOS-less mode has no OS process info */
	if (!bHavePrg)
		return false;
	*text = PrgOffsets[0];
	*data = PrgOffsets[1];
	*bss = PrgOffsets[2];
	return true;
}

/**
 * Reply to qXfer read request for given object data
 */
static void GdbStub_XferReply(const char *annex, const char *data, size_t size)
{
	uint32_t offset, len;
	const char *p = annex;

	if (!GdbStub_ParseAddrLen(&p, &offset, &len))
	{
		GdbStub_SendPacket("E01");
		return;
	}
	if (len > GDB_PACKET_SIZE - 2)
		len = GDB_PACKET_SIZE - 2;
	if (offset >= size)
	{
		GdbStub_SendPacket("l");
		return;
	}
	if (len > size - offset)
		len = size - offset;
	/* RxPacket is free, request has been parsed */
	RxPacket[0] = (offset + len >= size) ? 'l' : 'm';
	memcpy(RxPacket + 1, data + offset, len);
	GdbStub_SendPacketLen(RxPacket, len + 1);
}

/**
 * Handle qXfer packets
 */
static void GdbStub_HandleXfer(const char *p)
{
	static const char features[] = "features:read:target.xml:";
	static const char execfile[] = "exec-file:read:";

	if (strncmp(p, features, strlen(features)) == 0)
	{
		GdbStub_XferReply(p + strlen(features), TargetXml, strlen(TargetXml));
		return;
	}
	if (strncmp(p, execfile, strlen(execfile)) == 0)
	{
		/* skip annex (pid) */
		p = strchr(p + strlen(execfile), ':');
		if (!p || !bHavePrg)
		{
			GdbStub_SendPacket("E01");
			return;
		}
		GdbStub_XferReply(p + 1, PrgPath, strlen(PrgPath));
		return;
	}
	GdbStub_SendPacket("");
}


/* ------------------------ monitor commands ------------------------ */

/**
 * Show stub status
 */
static void GdbStub_ShowStatus(void)
{
	uint32_t text, data, bss;
	int i;

	fprintf(stderr, "GDB stub on port %d, client %sconnected\n",
		ListenPort, ClientFd >= 0 ? "" : "not ");
	fprintf(stderr, "stop-on-exec: %s\n", bStopOnExec ? "on" : "off");
	fprintf(stderr, "offsets: %s\n", bTextOffsets ? "text" : "segments");
	fprintf(stderr, "no-ack mode: %s\n", bNoAck ? "on" : "off");
	if (bHavePrg)
		fprintf(stderr, "program: %s\n", PrgPath);
	if (GdbStub_GetOffsets(&text, &data, &bss))
		fprintf(stderr, "TEXT: $%06x, DATA: $%06x, BSS: $%06x\n",
			text, data, bss);
	fprintf(stderr, "%d stub breakpoint(s)%s\n", BpCount, BpCount ? ":" : ".");
	for (i = 0; i < BpCount; i++)
	{
		static const char *types[] = { "breakpoint", "watchpoint", "read watchpoint", "access watchpoint", "raster", "next VBL", "next HBL", "program start" };
		fprintf(stderr, "- %s at $%06x", types[Bps[i].type], Bps[i].addr);
		if (Bps[i].type == GDB_BP_WATCH || Bps[i].type == GDB_BP_READ ||
		    Bps[i].type == GDB_BP_ACCESS)
			fprintf(stderr, ", len %u", Bps[i].len);
		if (Bps[i].type == GDB_BP_RASTER)
			fprintf(stderr, ", cycle %u", Bps[i].len);
		fprintf(stderr, "\n");
	}
}

/**
 * Handle stub specific "gdb ..." monitor commands
 */
static void GdbStub_MonitorCommand(const char *args)
{
	while (isspace((unsigned char)*args))
		args++;
	if (strncmp(args, "raster", 6) == 0 && (!args[6] || isspace((unsigned char)args[6])))
	{
		const char *p = args + 6;
		char *end;
		unsigned long line, cycle;
		while (isspace((unsigned char)*p)) p++;
		if (strcmp(p, "clear") == 0)
		{
			GdbStub_RemoveVideoBp(GDB_BP_RASTER);
			fprintf(stderr, "Raster breakpoint cleared.\n");
			return;
		}
		line = strtoul(p, &end, 10);
		if (end == p || !isspace((unsigned char)*end) || line > GDB_RASTER_MAX_LINE)
			goto bad_raster;
		p = end;
		while (isspace((unsigned char)*p)) p++;
		cycle = strtoul(p, &end, 10);
		while (isspace((unsigned char)*end)) end++;
		if (end == p || *end || cycle > GDB_RASTER_MAX_CYCLE)
			goto bad_raster;
		GdbStub_RemoveVideoBp(GDB_BP_RASTER);
		if (!GdbStub_AddVideoBp(GDB_BP_RASTER, line, cycle))
			fprintf(stderr, "ERROR: Cannot add raster breakpoint.\n");
		else
			fprintf(stderr, "Raster breakpoint: line %lu, cycle %lu.\n", line, cycle);
		return;
	bad_raster:
		fprintf(stderr, "ERROR: Use 'gdb raster <line 0..599> <cycle 0..2048>' or 'gdb raster clear'.\n");
		return;
	}
	if (strcmp(args, "run-vbl") == 0 || strcmp(args, "run-hbl") == 0)
	{
		gdb_bp_type_t type = args[4] == 'v' ? GDB_BP_NEXT_VBL : GDB_BP_NEXT_HBL;
		GdbStub_RemoveVideoBp(type);
		if (!GdbStub_AddVideoBp(type, 0, 0))
			fprintf(stderr, "ERROR: Cannot set run-to-video breakpoint.\n");
		else
			fprintf(stderr, "Run to next %s armed; continue execution.\n", type == GDB_BP_NEXT_VBL ? "VBL" : "HBL");
		return;
	}
	if (strncmp(args, "stop-on-exec", 12) == 0)
	{
		const char *arg = args + 12;
		while (isspace((unsigned char)*arg))
			arg++;
		if (strcmp(arg, "on") == 0)
			GdbStub_SetStopOnExec(true);
		else if (strcmp(arg, "off") == 0)
			GdbStub_SetStopOnExec(false);
		else if (*arg)
		{
			fprintf(stderr, "ERROR: 'gdb stop-on-exec' accepts only 'on' or 'off'\n");
			return;
		}
		fprintf(stderr, "stop-on-exec: %s\n", bStopOnExec ? "on" : "off");
		return;
	}
	if (strncmp(args, "offsets", 7) == 0)
	{
		const char *arg = args + 7;
		while (isspace((unsigned char)*arg))
			arg++;
		if (*arg && !GdbStub_SetOffsetsMode(arg))
		{
			fprintf(stderr, "ERROR: 'gdb offsets' accepts only 'segments' or 'text'\n");
			return;
		}
		fprintf(stderr, "offsets: %s\n", bTextOffsets ? "text" : "segments");
		return;
	}
	if (strncmp(args, "status", 6) == 0 || !*args)
	{
		GdbStub_ShowStatus();
		return;
	}
	fprintf(stderr, "Hatari GDB stub commands:\n"
		"  gdb status              - show stub status\n"
		"  gdb stop-on-exec on|off - stop at program start\n"
		"  gdb offsets segments|text - qOffsets reply type\n"
		"  gdb raster <line> <cycle> | clear - recurring raster stop\n"
		"  gdb run-vbl | run-hbl - arm one-shot video stop\n"
		"Other commands are passed to Hatari debugger, see 'help'.\n");
}

/**
 * Execute given monitor command, capture its output to a malloced
 * buffer. Return buffer (or NULL on failure), set its size to *size.
 *
 * Output is captured by redirecting stdout & stderr file descriptors,
 * debugOutput is not touched, so commands can change it (e.g. 'f'),
 * and output for user opened debugger log file still goes there.
 */
static char *GdbStub_RunMonitor(const char *cmd, size_t *size)
{
	int saved_out, saved_err;
	char *buf;
	long len;
	FILE *fp;

	*size = 0;
	fp = tmpfile();
	if (!fp)
		return NULL;

	fflush(stdout);
	fflush(stderr);
	if (debugOutput)
		fflush(debugOutput);
	saved_out = dup(STDOUT_FILENO);
	saved_err = dup(STDERR_FILENO);
	if (saved_out < 0 || saved_err < 0)
	{
		if (saved_out >= 0)
			close(saved_out);
		if (saved_err >= 0)
			close(saved_err);
		fclose(fp);
		return NULL;
	}
	dup2(fileno(fp), STDOUT_FILENO);
	dup2(fileno(fp), STDERR_FILENO);

	if (strncmp(cmd, "gdb", 3) == 0 && (!cmd[3] || isspace((unsigned char)cmd[3])))
		GdbStub_MonitorCommand(cmd + 3);
	else
		DebugUI_ParseRemoteCommand(cmd);

	fflush(stdout);
	fflush(stderr);
	if (debugOutput)
		fflush(debugOutput);
	dup2(saved_out, STDOUT_FILENO);
	dup2(saved_err, STDERR_FILENO);
	close(saved_out);
	close(saved_err);

	/* fds share file offset, so it's at the end of output */
	len = lseek(fileno(fp), 0, SEEK_CUR);
	if (len < 0)
	{
		fclose(fp);
		return NULL;
	}
	if (len > GDB_MAX_MONITOR_OUTPUT)
		len = GDB_MAX_MONITOR_OUTPUT;
	buf = malloc(len + 1);
	if (buf)
	{
		rewind(fp);
		len = fread(buf, 1, len, fp);
		buf[len] = '\0';
		*size = len;
	}
	fclose(fp);
	return buf;
}

/**
 * Handle qRcmd packet: run command, send its output as O packets
 */
static void GdbStub_HandleRcmd(const char *hex)
{
	char cmd[MAX_DEBUG_CMD_LEN];
	size_t i, size, offset;
	char *output;

	if (strlen(hex) / 2 >= sizeof(cmd))
	{
		GdbStub_SendPacket("E01");
		return;
	}
	for (i = 0; hex[2*i] && hex[2*i+1]; i++)
	{
		int hi = GdbStub_HexVal((unsigned char)hex[2*i]);
		int lo = GdbStub_HexVal((unsigned char)hex[2*i+1]);
		if (hi < 0 || lo < 0)
		{
			GdbStub_SendPacket("E01");
			return;
		}
		cmd[i] = hi << 4 | lo;
	}
	cmd[i] = '\0';

	output = GdbStub_RunMonitor(cmd, &size);
	if (!output)
	{
		GdbStub_SendPacket("E01");
		return;
	}
	/* output in O packets, max (GDB_PACKET_SIZE-2)/2 bytes each */
	for (offset = 0; offset < size; )
	{
		size_t chunk = size - offset;
		char *out = RxPacket;
		if (chunk > (GDB_PACKET_SIZE - 2) / 2)
			chunk = (GDB_PACKET_SIZE - 2) / 2;
		*out++ = 'O';
		for (i = 0; i < chunk; i++)
		{
			uint8_t c = output[offset + i];
			*out++ = HexChars[c >> 4];
			*out++ = HexChars[c & 0xf];
		}
		*out = '\0';
		if (!GdbStub_SendPacket(RxPacket))
			break;
		offset += chunk;
	}
	free(output);
	GdbStub_SendPacket("OK");
}


/* ------------------------ execution control ------------------------ */

/**
 * Handle continue/step: optional new PC address in given string
 */
static void GdbStub_Resume(const char *addr, bool step)
{
	uint32_t pc;
	if (addr && GdbStub_ParseHex(&addr, &pc))
	{
		if (pc != M68000_GetPC())
			CallDepth = 0;
		M68000_SetPC(pc);
	}
	DebugCpu_SetSteps(step ? 1 : 0);
}

/**
 * Handle vCont packet, return command result
 */
static gdb_cmd_t GdbStub_HandleVCont(const char *p)
{
	if (strcmp(p, "?") == 0)
	{
		GdbStub_SendPacket("vCont;c;C;s;S");
		return GDB_CMD_STAY;
	}
	if (*p++ != ';')
	{
		GdbStub_SendPacket("");
		return GDB_CMD_STAY;
	}
	/* single thread: first action decides */
	switch (*p)
	{
	case 'c':
	case 'C':
		GdbStub_Resume(NULL, false);
		return GDB_CMD_RESUME;
	case 's':
	case 'S':
		GdbStub_Resume(NULL, true);
		return GDB_CMD_RESUME;
	default:
		GdbStub_SendPacket("");
		return GDB_CMD_STAY;
	}
}

/**
 * Handle given query packet
 */
static void GdbStub_HandleQuery(const char *pkt)
{
	uint32_t text, data, bss;
	char buf[96];

	if (strncmp(pkt, "qSupported", 10) == 0)
	{
		GdbStub_SendPacket("PacketSize=4000;QStartNoAckMode+;"
				   "qXfer:features:read+;qXfer:exec-file:read+;"
				   "swbreak+;hwbreak+;vContSupported+");
	}
	else if (strncmp(pkt, "qXfer:", 6) == 0)
		GdbStub_HandleXfer(pkt + 6);
	else if (strcmp(pkt, "qOffsets") == 0)
	{
		if (GdbStub_GetOffsets(&text, &data, &bss))
		{
			/* for gdb with linked executable, relocate all by TEXT */
			if (bTextOffsets)
				data = bss = text;
			snprintf(buf, sizeof(buf), "Text=%x;Data=%x;Bss=%x", text, data, bss);
			GdbStub_SendPacket(buf);
		}
		else
			GdbStub_SendPacket("E01");
	}
	else if (strcmp(pkt, "qHatari:callstack") == 0)
	{
		unsigned int i;
		int pos = snprintf(RxPacket, sizeof(RxPacket), "S;%x", M68000_GetPC());
		bCallTracking = true;
		for (i = CallDepth; i > 0 && pos > 0 && (size_t)pos < sizeof(RxPacket); i--)
			pos += snprintf(RxPacket + pos, sizeof(RxPacket) - pos,
					";%x", CallReturns[i-1]);
		GdbStub_SendPacket(RxPacket);
	}
	else if (strncmp(pkt, "qRcmd,", 6) == 0)
		GdbStub_HandleRcmd(pkt + 6);
	else if (strcmp(pkt, "qC") == 0)
		GdbStub_SendPacket("QC1");
	else if (strcmp(pkt, "qfThreadInfo") == 0)
		GdbStub_SendPacket("m1");
	else if (strcmp(pkt, "qsThreadInfo") == 0)
		GdbStub_SendPacket("l");
	else if (strncmp(pkt, "qAttached", 9) == 0)
		GdbStub_SendPacket("1");
	else if (strncmp(pkt, "qSymbol", 7) == 0)
		GdbStub_SendPacket("OK");
	else
		GdbStub_SendPacket("");
}

/**
 * Handle given packet.  Return what to do next.
 */
static gdb_cmd_t GdbStub_HandlePacket(int len)
{
	const char *pkt = RxPacket;

	switch (pkt[0])
	{
	case '?':
		GdbStub_SendPacket(StopReply);
		break;
	case 'g':
		GdbStub_ReadRegisters();
		break;
	case 'G':
		GdbStub_WriteRegisters(pkt + 1);
		break;
	case 'p':
		GdbStub_AccessRegister(pkt + 1, false);
		break;
	case 'P':
		GdbStub_AccessRegister(pkt + 1, true);
		break;
	case 'm':
		GdbStub_ReadMemory(pkt + 1);
		break;
	case 'M':
		GdbStub_WriteMemory(pkt + 1, len, false);
		break;
	case 'X':
		GdbStub_WriteMemory(pkt + 1, len, true);
		break;
	case 'c':
		GdbStub_Resume(pkt + 1, false);
		return GDB_CMD_RESUME;
	case 's':
		GdbStub_Resume(pkt + 1, true);
		return GDB_CMD_RESUME;
	case 'C':
	case 'S':
		/* ignore signal, but handle optional ";addr" */
		pkt = strchr(pkt, ';');
		GdbStub_Resume(pkt ? pkt + 1 : NULL, RxPacket[0] == 'S');
		return GDB_CMD_RESUME;
	case 'Z':
	case 'z':
		GdbStub_HandleBreakpoint(pkt);
		break;
	case 'H':
	case 'T':
		GdbStub_SendPacket("OK");
		break;
	case 'D':
		GdbStub_SendPacket("OK");
		return GDB_CMD_DETACH;
	case 'k':
		Main_SetQuitValue(0);
		return GDB_CMD_DETACH;
	case 'q':
		GdbStub_HandleQuery(pkt);
		break;
	case 'Q':
		if (strcmp(pkt, "QStartNoAckMode") == 0)
		{
			GdbStub_SendPacket("OK");
			bNoAck = true;
		}
		else
			GdbStub_SendPacket("");
		break;
	case 'v':
		if (strncmp(pkt, "vCont", 5) == 0)
			return GdbStub_HandleVCont(pkt + 5);
		if (strncmp(pkt, "vKill", 5) == 0)
		{
			GdbStub_SendPacket("OK");
			Main_SetQuitValue(0);
			return GDB_CMD_DETACH;
		}
		GdbStub_SendPacket("");
		break;
	default:
		GdbStub_SendPacket("");
		break;
	}
	return GDB_CMD_STAY;
}

/**
 * Return GDB signal number for given CPU exception number
 */
static int GdbStub_ExceptionSignal(int nr)
{
	switch (nr)
	{
	case 2:		/* bus error */
	case 3:		/* address error */
		return GDB_SIGBUS;
	case 4:		/* illegal instruction */
	case 8:		/* privilege violation */
	case 10:	/* line-A */
	case 11:	/* line-F */
		return GDB_SIGILL;
	case 5:		/* division by zero */
		return GDB_SIGFPE;
	default:	/* CHK, TRAPV, trace... */
		return GDB_SIGTRAP;
	}
}

/**
 * Set StopReply according to why debugger was entered
 */
static void GdbStub_SetStopReply(debug_reason_t reason)
{
	int i, idx, tag;
	if (reason != REASON_CPU_BREAKPOINT)
		PendingAccessBp = -1;

	switch (reason)
	{
	case REASON_USER:
		snprintf(StopReply, sizeof(StopReply), "T%02x", GDB_SIGINT);
		break;
	case REASON_CPU_EXCEPTION:
		snprintf(StopReply, sizeof(StopReply), "T%02x",
			 GdbStub_ExceptionSignal(ExceptionNr));
		break;
	case REASON_CPU_BREAKPOINT:
		/* program start? */
		idx = GdbStub_FindExecBp();
		if (idx >= 0 && Bps[idx].addr == (M68000_GetPC() & GDB_ADDR_MASK))
		{
			GdbStub_RemoveBpAt(idx);
			snprintf(StopReply, sizeof(StopReply), "T%02xlibrary:;", GDB_SIGTRAP);
			break;
		}
		tag = BreakCond_GetCpuHitTag();
		for (i = 0; i < BpCount; i++)
		{
			if (Bps[i].tag == tag)
				break;
		}
		if (PendingAccessBp >= 0)
		{
			idx = PendingAccessBp;
			PendingAccessBp = -1;
			snprintf(StopReply, sizeof(StopReply), "T%02x%s:%x;",
				 GDB_SIGTRAP, Bps[idx].type == GDB_BP_READ ? "rwatch" : "awatch",
				 Bps[idx].addr);
		}
		else if (tag && i < BpCount && Bps[i].type == GDB_BP_WATCH)
			snprintf(StopReply, sizeof(StopReply), "T%02xwatch:%x;",
				 GDB_SIGTRAP, Bps[i].addr);
		else if (tag && i < BpCount && Bps[i].type == GDB_BP_RASTER)
		{
			char expression[128];
			/* Re-arm for another frame before returning control to client. */
			if (Bps[i].len)
				snprintf(expression, sizeof(expression),
					 "RasterLine = %u && LineCycles > %u && VBL ! %u",
					 Bps[i].addr, Bps[i].len - 1, (unsigned)nVBLs);
			else
				snprintf(expression, sizeof(expression),
					 "RasterLine = %u && VBL ! %u",
					 Bps[i].addr, (unsigned)nVBLs);
			BreakCond_RemoveTagged(tag);
			if (!BreakCond_AddTagged(expression, tag))
			{
				fprintf(stderr, "ERROR: Raster breakpoint could not be re-armed.\n");
				Bps[i] = Bps[--BpCount];
			}
			snprintf(StopReply, sizeof(StopReply), "T%02xraster:;", GDB_SIGTRAP);
		}
		else if (tag && i < BpCount &&
			 (Bps[i].type == GDB_BP_NEXT_VBL || Bps[i].type == GDB_BP_NEXT_HBL))
		{
			gdb_bp_type_t type = Bps[i].type;
			GdbStub_RemoveBpAt(i);
			snprintf(StopReply, sizeof(StopReply), "T%02x%s:;", GDB_SIGTRAP,
				 type == GDB_BP_NEXT_VBL ? "vbl" : "hbl");
		}
		else if (tag && i < BpCount && Bps[i].type == GDB_BP_SW)
			snprintf(StopReply, sizeof(StopReply), "T%02xswbreak:;", GDB_SIGTRAP);
		else
			snprintf(StopReply, sizeof(StopReply), "T%02x", GDB_SIGTRAP);
		break;
	default:
		snprintf(StopReply, sizeof(StopReply), "T%02x", GDB_SIGTRAP);
		break;
	}
	ExceptionNr = 0;
}


/* ------------------------ public API ------------------------ */

/** Return whether the GDB stub has a listening socket. */
bool GdbStub_IsEnabled(void)
{
	return ListenFd >= 0;
}

/**
 * Open (or close with 0) GDB stub listening port.
 * Return error string, or NULL for success.
 */
const char *GdbStub_SetPort(int port)
{
	struct sockaddr_in address;
	int fd, one = 1;

	if (ListenFd >= 0)
	{
		close(ListenFd);
		ListenFd = -1;
	}
	ListenPort = port;
	if (port == 0)
		return NULL;
	if (port < 0 || port > 65535)
		return "Invalid port number";

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		perror("GDB stub socket creation error");
		return "Can't create TCP socket";
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(fd, 1) < 0)
	{
		perror("GDB stub socket bind/listen error");
		close(fd);
		return "Can't listen on given port";
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	ListenFd = fd;
	Log_Printf(LOG_INFO, "GDB stub listening on 127.0.0.1:%d\n", port);
	return NULL;
}

/**
 * Set whether emulation stops on program start
 */
void GdbStub_SetStopOnExec(bool enable)
{
	bStopOnExec = enable;
	if (!enable)
		GdbStub_RemoveExecBp();
}

/**
 * Set qOffsets reply mode: "segments" (TEXT/DATA/BSS start addresses)
 * or "text" (all offsets are TEXT start address, for gdb with linked
 * executables).  Return false for unknown mode.
 */
bool GdbStub_SetOffsetsMode(const char *mode)
{
	if (strcmp(mode, "segments") == 0)
		bTextOffsets = false;
	else if (strcmp(mode, "text") == 0)
		bTextOffsets = true;
	else
		return false;
	return true;
}

/**
 * Enter debugger on user / client request, like debugger shortcut does
 */
static void GdbStub_Interrupt(void)
{
	bool running = Main_PauseEmulation(true);
	DebugUI(REASON_USER);
	/* A remote continue/step takes control even if the GUI was paused. */
	if (running || bRunning)
		Main_UnPauseEmulation();
}

/**
 * Called on emulation reset: forget current program
 */
void GdbStub_Reset(void)
{
	bool initial = !bInitialResetDone;
	/* Startup cold reset happens before any program could be observed.
	 * Distinguish it from later resets so attaching to an already running
	 * TOS program can still use its basepage for qOffsets. */
	bInitialResetDone = true;
	if (ListenFd < 0)
		return;
	if (!initial)
		bOffsetsInvalidated = true;
	bHavePrg = false;
	CallDepth = 0;
	PrgPath[0] = '\0';
	memset(PrgOffsets, 0, sizeof(PrgOffsets));
	GdbStub_RemoveExecBp();
}

/**
 * Called once per VBL, or periodically from the paused SDL event loop:
 * accept new clients, check for disconnect and interrupt (0x03) requests.
 */
void GdbStub_Poll(void)
{
	int c;

	if (ListenFd < 0)
		return;

	if (ClientFd < 0)
	{
		if (GdbStub_Accept(0))
			GdbStub_Interrupt();
		return;
	}
	for (;;)
	{
		c = GdbStub_ReadByte(0);
		if (c == GDB_TIMEOUT)
			return;
		if (c == GDB_EOF)
		{
			GdbStub_CloseClient();
			return;
		}
		if (c == 0x03)
		{
			/* discard rest */
			InPos = InLen;
			GdbStub_Interrupt();
			return;
		}
	}
}

/**
 * Called when a program is started (Pexec or TOS-less test program).
 * Records program info, and sets temporary breakpoint at program start
 * if stub should stop there.
 */
void GdbStub_ProgramExec(const char *path, const uint32_t *offsets)
{
	if (ListenFd < 0)
		return;

	if (!realpath(path, PrgPath))
	{
		strncpy(PrgPath, path, sizeof(PrgPath) - 1);
		PrgPath[sizeof(PrgPath) - 1] = '\0';
	}
	memcpy(PrgOffsets, offsets, sizeof(PrgOffsets));
	bHavePrg = true;
	bOffsetsInvalidated = false;
	CallDepth = 0;

	if (!bStopOnExec)
		return;
	/* wait for client at program start only on first connection */
	if (ClientFd < 0 && bEverConnected)
		return;

	GdbStub_RemoveExecBp();
	if (GdbStub_AddBp(GDB_BP_EXEC, offsets[0] & GDB_ADDR_MASK, 0))
		DebugCpu_BreakpointsChanged();
}

/**
 * Return true if GDB stub should handle debugger session
 * instead of the console debugger.
 */
bool GdbStub_WantsSession(debug_reason_t reason)
{
	int idx;

	if (ClientFd >= 0)
		return true;
	/* program start breakpoint waiting for client? */
	if (reason == REASON_CPU_BREAKPOINT)
	{
		idx = GdbStub_FindExecBp();
		if (idx >= 0 && Bps[idx].addr == (M68000_GetPC() & GDB_ADDR_MASK))
			return true;
	}
	/* console debugger handles this, don't report it later */
	ExceptionNr = 0;
	return false;
}

/**
 * Record exception number that invokes debugger
 */
void GdbStub_SetException(int nr)
{
	ExceptionNr = nr;
}

/**
 * Debugger session with GDB client as the front end.  Returns when
 * emulation should be continued.
 */
void GdbStub_Session(debug_reason_t reason)
{
	bool report = bRunning;
	gdb_cmd_t cmd;
	int len;

	GdbStub_SetStopReply(reason);

	if (ClientFd < 0)
	{
		/* stopped at program start, wait for client */
		fprintf(stderr, "Program start: waiting for GDB client connection on port %d...\n",
			ListenPort);
		Screen_StatusbarMessage("Waiting for GDB", 100);
		while (!GdbStub_Accept(GDB_POLL_MS))
		{
			if (GuiEvent_PollQuit() || ListenFd < 0)
			{
				/* don't stop again at program start */
				bEverConnected = true;
				return;
			}
		}
		report = false;
	}
	Screen_StatusbarMessage("GDB stopped", 100);

	bRunning = false;
	if (report && !GdbStub_SendPacket(StopReply))
	{
		GdbStub_CloseClient();
		return;
	}

	for (;;)
	{
		len = GdbStub_ReadPacket();
		if (len == GDB_QUIT)
			return;
		if (len < 0)
		{
			GdbStub_CloseClient();
			return;
		}
		cmd = GdbStub_HandlePacket(len);
		if (cmd == GDB_CMD_RESUME)
		{
			bRunning = true;
			return;
		}
		if (cmd == GDB_CMD_DETACH)
		{
			GdbStub_CloseClient();
			return;
		}
	}
}

/**
 * Hatari exits: tell it to client and close sockets
 */
void GdbStub_Exit(int exitval)
{
	char buf[8];

	if (ClientFd >= 0)
	{
		snprintf(buf, sizeof(buf), "W%02x", exitval & 0xff);
		/* don't wait for ack */
		bNoAck = true;
		GdbStub_SendPacket(buf);
		GdbStub_CloseClient();
	}
	if (ListenFd >= 0)
	{
		close(ListenFd);
		ListenFd = -1;
	}
}

#else /* !HAVE_UNIX_DOMAIN_SOCKETS */

/** Report that remote debugging is unavailable on this platform. */
const char *GdbStub_SetPort(int port)
{
	return port ? "GDB stub is not supported on this platform" : NULL;
}
/** Ignore stop-on-exec selection without a remote debugger. */
void GdbStub_SetStopOnExec(bool enable) { }
/** Reject qOffsets mode selection without a remote debugger. */
bool GdbStub_SetOffsetsMode(const char *mode) { return false; }
/** Report that the remote debugger is unavailable. */
bool GdbStub_IsEnabled(void) { return false; }
/** Ignore remote connection polling on unsupported platforms. */
void GdbStub_Poll(void) { }
/** Ignore program starts when the remote debugger is unavailable. */
void GdbStub_ProgramExec(const char *path, const uint32_t *offsets) { }
/** Ignore resets when the remote debugger is unavailable. */
void GdbStub_Reset(void) { }
/** Leave debugger sessions to the console debugger. */
bool GdbStub_WantsSession(debug_reason_t reason) { return false; }
/** Ignore remote sessions when the remote debugger is unavailable. */
void GdbStub_Session(debug_reason_t reason) { }
/** Ignore exception numbers when the remote debugger is unavailable. */
void GdbStub_SetException(int nr) { }
/** Ignore CPU data accesses when the remote debugger is unavailable. */
void GdbStub_CpuDataAccess(uint32_t addr, unsigned int size, bool write) { }
/** Report that there are no remote access watchpoints. */
bool GdbStub_HasAccessWatchpoints(void) { return false; }
/** Report that no remote access watchpoint has fired. */
bool GdbStub_HasPendingAccessWatchpoint(void) { return false; }
/** Ignore CPU model changes without a remote debugger. */
void GdbStub_CheckCpuModel(void) { }
/** Report that remote call tracking is unavailable. */
bool GdbStub_HasCallTracking(void) { return false; }
/** Ignore instructions when remote call tracking is unavailable. */
void GdbStub_CpuInstruction(uint32_t instruction_pc, uint32_t next_pc, uint32_t sp) { }
/** Ignore exit notification when the remote debugger is unavailable. */
void GdbStub_Exit(int exitval) { }

#endif /* !HAVE_UNIX_DOMAIN_SOCKETS */
