#include <signal.h>
#include <ctype.h>

#define RAWTTY_IMPLEMENTATION
#include "rawtty.h"

#include "z180/z180.h"

#define MAXBREAKPTS 32
#define CMDBUFLEN 60

/* reference into main program */
extern void do_timers();
extern int VERBOSE;

static UINT8 *mem_ram = 0;
static UINT8 *mem_rom = 0;

static int dbg_quit = 0;
static int dbg_stepping = 0;

static int dbg_nstart = -1;
static int dbg_nend = -1;

#ifndef _WIN32
void sigint_handler(int s)	{
	// POSIX SIGINT handler
	// do nothing
}

void sigquit_handler(int s)	{
	// POSIX SIGQUIT handler
	printf("\nExiting emulation.\n");
	dbg_quit = 1; // make sure atexit is called
}
#endif

void disableCTRLC() {
#ifdef _WIN32
	HANDLE consoleHandle = GetStdHandle(STD_INPUT_HANDLE);
	DWORD consoleMode;
	GetConsoleMode(consoleHandle,&consoleMode);
	SetConsoleMode(consoleHandle,consoleMode&~ENABLE_PROCESSED_INPUT);
#else
	signal(SIGINT, sigint_handler);
#endif
}

int dbg_init(int stepping, UINT8 *ram, UINT8 *rom) {
	if (tty_init() == 0) {
		atexit(tty_deinit);
		dbg_stepping = stepping ? 1 : 0;
		mem_ram = ram;
		mem_rom = rom;

		//disableCTRLC();
		// on MINGW, keep CTRL+Break (and window close button) enabled
		// MINGW always calls atexit in these cases

		#ifndef _WIN32
			// on POSIX, route SIGQUIT (CTRL+\) to graceful shutdown
			signal(SIGQUIT, sigquit_handler);
		#endif

		return 0;
	}
	fprintf(stderr, "failed to initialize tty.\n");
	return -1;
}

int dbg_running() {
	return !dbg_quit;
}

void dbg_log(const char *fmt, ...) {
	va_list ap;
	if (VERBOSE) {
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
		if (!strchr(fmt, '\r'))
			tty_writeByte('\r');
	}
}

static UINT8 *dbg_getmemArray(device_t *device, offs_t pc) {
	UINT8 ROMBR, RAMUBR, RAMLBR, SCR;
	UINT8 *mem = NULL;
	cpu_translate_z180(device,AS_PROGRAM,0,&pc);
	if (((struct z180_device *)device)->m_type == Z180_TYPE_Z182) {
		ROMBR = cpu_get_state_z180(device,Z182_ROMBR);
		RAMUBR = cpu_get_state_z180(device,Z182_RAMUBR);
		RAMLBR = cpu_get_state_z180(device,Z182_RAMLBR);
		SCR = cpu_get_state_z180(device,Z182_SCR);
		if(!(SCR & 8) && (pc >>12) <= ROMBR)
			mem = mem_rom;
		else {
			mem = mem_ram;	
			if( (pc >>12) < RAMLBR || RAMUBR < (pc >>12) ) {
				tty_printf("error: RAM access outside bounds\n");
				mem = NULL;
			}
		}
	}
	else {
		mem = mem_ram;
	}
	return mem;
}

static UINT8 dbg_getmem(device_t *device, offs_t addr) {
	UINT8 ROMBR, RAMUBR, RAMLBR, SCR;
	UINT8 *mem = dbg_getmemArray(device, addr);
	return mem == NULL ? 0 : mem[addr];
}

static offs_t dbg_disassemble_one(device_t *device, offs_t pc, char *ibuf) {
	offs_t dres,i;
	UINT8 *mem = dbg_getmemArray(device, pc);
	if (mem == NULL) return 0;

	dres = cpu_disassemble_z180(device,ibuf,pc,&mem[pc],&mem[pc],0);
	tty_printf("%04x: ",pc);
	for (i=0;i<(dres &DASMFLAG_LENGTHMASK);i++) tty_printf("%02X",mem[pc+i]);
	for ( ;i<4;i++) {tty_print("  ");}
	tty_printf(" %s",ibuf);
	return dres&DASMFLAG_LENGTHMASK;
}

static void dbg_print_status(device_t *device, offs_t curpc) {
	char fbuf[10];
	char ibuf[20];
	UINT8 *mem = dbg_getmemArray(device, curpc);
	if (mem == NULL) return;

	cpu_string_export_z180(device,STATE_GENFLAGS,fbuf);
	tty_printf("%s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X\r\n",fbuf,
	    cpu_get_state_z180(device,Z180_AF),
		cpu_get_state_z180(device,Z180_BC),
		cpu_get_state_z180(device,Z180_DE),
		cpu_get_state_z180(device,Z180_HL),
		cpu_get_state_z180(device,Z180_IX),
		cpu_get_state_z180(device,Z180_IY),
		cpu_get_state_z180(device,Z180_SP));
	dbg_disassemble_one(device, curpc, ibuf);
	if (strstr(ibuf,",(hl)")||strstr(ibuf," (hl)")||strstr(ibuf,"ldi")||strstr(ibuf,"ldd"))
		tty_printf("\t; =$%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_HL)));
	else if (strstr(ibuf,",(de)"))
		tty_printf("\t; =$%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_DE)));
	else if (strstr(ibuf,",(bc)"))
		tty_printf("\t; =$%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_BC)));
	else if (strstr(ibuf,",(ix"))
		tty_printf("\t; =$%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_IX)+(int8_t)dbg_getmem(device, curpc+2)));
	else if (strstr(ibuf,",(iy"))
		tty_printf("\t; =$%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_IY)+(int8_t)dbg_getmem(device, curpc+2)));
	else if (strstr(ibuf,"(sp),"))	  // ex (sp),...
		tty_printf("\t; =$%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_SP)));
	// TODO 16 bit values, values from (addr)
	tty_newline();
}

static int dbg_skipSpace(const char *buf, int len, int *ptr) {
	while (*ptr < len && isspace(buf[*ptr])) *ptr += 1;
	return buf[*ptr] != 0;
}

static int dbg_ensureEoln(const char *buf, int len, int *ptr) {
	int eoln = dbg_skipSpace(buf, len, ptr) == 0; 
	if (!eoln)
		tty_printf("error: end of line expected\r\n");
	return eoln;
}

struct breakpoint {
	offs_t pc;
	int temp;
} breakpoints[MAXBREAKPTS];
offs_t numbreakpts = 0;

static int dbg_break(offs_t pc, int temporary) {
	if (numbreakpts == MAXBREAKPTS) {
		tty_print("error: maximum number of breakpoints reached.\r\n");
		return 0;
	}
	int newpt = 0;
	while (newpt < numbreakpts && breakpoints[newpt].pc < pc) newpt += 1;
	// breakpoint already exists, even if as temporary?
	if (breakpoints[newpt].pc == pc && temporary == 0) {
		breakpoints[newpt].temp = 0;
		return 1;
	}
	// insert new breakpoint
	int n = numbreakpts;
	while (n > newpt)
		breakpoints[n] = breakpoints[n - 1];
	breakpoints[newpt].pc = pc;
	breakpoints[newpt].temp = temporary;
	numbreakpts += 1;
	if (temporary == 0) tty_printf("breakpoint set at $%04x\r\n", pc);
	return 1;
}

static int dbg_breakDel(offs_t pc) {
	int n = 0;
	while (n < numbreakpts && breakpoints[n].pc < pc) n += 1;
	if (n < numbreakpts && breakpoints[n].pc == pc) {
		int temporary = breakpoints[n].temp;
		for (; n < numbreakpts - 1; n += 1)
			breakpoints[n] = breakpoints[n + 1];
		numbreakpts -= 1;
		if (temporary == 0) tty_print("breakpoint deleted.\r\n");
		return 1;
	}
	return 0;
}

static int dbg_isBreak(offs_t pc) {
	if (numbreakpts == 0) return 0;
	if (pc < breakpoints[0].pc) return 0;
	if (breakpoints[numbreakpts - 1].pc < pc) return 0;
	int min = 0, max = numbreakpts - 1;
	while (1) {
		int med = min + ((max - min) >> 1);
		if (pc < breakpoints[med].pc && min < med)
			max = med;
		else if (breakpoints[med].pc < pc && med < max)
			min = med;
		else if (breakpoints[med].pc == pc) {
			if (breakpoints[med].temp) dbg_breakDel(pc);
			return 1;
		} else
			return 0;
	}
}

static int dbg_breakEnum() {
	int n = 0;
	tty_print("breakpoints:\r\n");
	while (n < numbreakpts) {
		tty_printf("$%04x %c\r\n", breakpoints[n].pc, breakpoints[n].temp ? '*' : ' ');
		n += 1;
	}
	return 1;
}

static int dbg_next(device_t *device, offs_t pc) {
	UINT8 op = dbg_getmem(device, pc);
	// call, call cc or rst
	if (op == 0xcd || (op & 0xc7) == 0xc4 || (op & 0xc7) == 0xc7) {
		offs_t dres;
		UINT8 ibuf[20];
		UINT8 *mem = dbg_getmemArray(device, pc);
		if (mem == NULL) return 0;
		dres = cpu_disassemble_z180(device,ibuf,pc,&mem[pc],&mem[pc],0);
		dbg_break(pc + dres, 1);
		dbg_stepping = 0;
	} else
		dbg_stepping = 1;
}

static void dbg_list(device_t *device, offs_t start, offs_t end) {
	if (end < start) return;
	char ibuf[20];
	offs_t addr = start;
	while (addr < end) {
		offs_t len = dbg_disassemble_one(device, addr, ibuf);
		if (len == 0) return;
		tty_newline();
		addr += len;
	}
	dbg_nstart = addr;
	dbg_nend = addr + (end - start);
}

static void dbg_examine(device_t *device, offs_t start, offs_t end) {
	if (end < start) return;
	offs_t addr = start;
	while (addr < end) {
		offs_t len = (end - addr) > 16 ? 16 : end - addr;
		int i = 0;
		tty_printf("%04x: ", addr);
		for (i = 0; i < 16; i += 1) {
			if (i < len)
				tty_printf("%02x ", dbg_getmem(device, addr + i));
			else
				tty_print("   ");
		}
		for (i = 0; i < 16; i += 1) {
			if (i < len) {
				UINT8 c = dbg_getmem(device, addr + i);
				tty_printf("%c", isprint(c) ? c : '.');
			} else
				tty_print(" ");
		}
		tty_newline();
		addr += len;
	}
	dbg_nstart = addr;
	dbg_nend = addr + (end - start);
}

static void dbg_help() {
	tty_print("debugger help\r\n");
	tty_print("h or ?          this help\r\n");
	tty_print("q               quit\r\n");
	tty_print("v 0|1           set verbose mode\r\n");
	tty_print("r [addr]        run starting from addr, defaults to current pc\r\n");
	tty_print("R               reset\r\n");
	tty_print("s               execute next op\r\n");
	tty_print("n               execute next op, stepping over call or rst\r\n");
	tty_print("b [addr]        set breakpoint at addr, defaults to current pc\r\n");
	tty_print("d [addr]        delete breakpoint at addr, defaults to current pc\r\n");
	tty_print("D               delete all breakpoints\r\n");
	tty_print("e               enumerate breakpoints\r\n");
	tty_print("l [start [end]] list (disassemble) memory. Start defaults to end+1 of the\r\n");
	tty_print("                last list call, or to pc if that is unset\r\n");
	tty_print("x [start [end]] examine (dump) memory. Start defaults to end+1 of the last\r\n");
	tty_print("                examine call, or to pc if that is unset\r\n");
	tty_print("ENTER           repeast last r, s, n, l or x command. For r, l and x all\r\n");
	tty_print("                arguments are truncated.\r\n");
	//tty_print("\r\n");
	//tty_print("\r\n");
}

// as all numbers we care about are in the range of 0..65535, we return -1
// for failure. Optional numbers can only be just before the end of the line
static int dbg_parseNum(const char *buf, int buflen, int *ptr, int optional) {
	int res = 0;
	int factor = 10;
	if (buf[*ptr] == 0) return -1;
	if (buf[*ptr] == '$') {
		factor = 16;
		*ptr += 1;
	}
	int ch = buf[*ptr];
	if (ch == 0) {
		if (!optional) tty_printf("error: invalid number\r\n");
		return -1;
	}
	while (*ptr < buflen && (factor == 16 ? isxdigit(ch) : isdigit(ch))) {
		if (ch >= '0' && ch <= '9')
			res = (res * factor) + (ch - '0');
		else if (factor == 16 && ch >= 'A' && ch <= 'F')
			res = (res * factor) + (ch - 'A') + 10;
		else if (factor == 16 && ch >= 'a' && ch <= 'f')
			res = (res * factor) + (ch - 'a') + 10;
		else {
			tty_printf("error: invalid number\r\n");
			return -1;
		}
		if (res > 65535) {
			tty_printf("error: number too big\r\n");
			return -1;
		}
		*ptr += 1;
		ch = *ptr < buflen ? buf[*ptr] : 0;
	}
	return res;
}

static void dbg_cli(device_t *device, offs_t curpc) {
	char lbuf[CMDBUFLEN];
	static char pbuf[CMDBUFLEN] = { 0 };

	while (1) {
		tty_printf("> ");
		const char *line = tty_readLine(lbuf, CMDBUFLEN);
		int ptr = 1;
		if (lbuf[0] && lbuf[0] != pbuf[0]) {
			dbg_nstart = -1;
			dbg_nend = -1;
		}
		again: dbg_skipSpace(lbuf, CMDBUFLEN, &ptr);
		if (!line || line[0] == 'q') {
			// quit debugger/emulator
			if (line && !dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			numbreakpts = 0;
			dbg_quit = 1;
			dbg_stepping = 0;
			*pbuf = 0;
			return;
		} else if (line[0] == 'h' || line[0] == '?') {
			// help
			if (!dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			dbg_help();
			*pbuf = 0;
			continue;
		} else if (line[0] == 'v') {
			// set verbose mode
			int val = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 0);
			if (val < 0 || !dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			VERBOSE = val ? 1 : 0;
			*pbuf = 0;
			continue;
		} else if (line[0] == 'r') {
			// run [a]
			int pc = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 1);
			if (!dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			if (pc >= 0) {
				cpu_set_pc_z180(device, pc);
			}
			dbg_stepping = 0;
			memcpy(pbuf, lbuf, CMDBUFLEN);
			return;
		} else if (line[0] == 'R') {
			// reset
			if (!dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			cpu_reset_z180(device);
			curpc = 0;
			dbg_print_status(device, curpc);
			*pbuf = 0;
			continue;
		} else if (line[0] == 's') {
			// single step
			if (line[0] != 0 && !dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			dbg_stepping = 1;
			memcpy(pbuf, lbuf, CMDBUFLEN);
			return;
		} else if (line[0] == 'n') {
			// n step over next cmd
			if (!dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			dbg_next(device, curpc);
			memcpy(pbuf, lbuf, CMDBUFLEN);
			return;
		} else if (line[0] == 'b') {
			// b [a] set breakpoint
			int pc = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 1);
			if (!dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			dbg_break(pc < 0 ? curpc : pc, 0);
			*pbuf = 0;
			continue;
		} else if (line[0] == 'd') {
			// d [a] delete breakpoint or all breakpoints
			int pc = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 1);
			if (!dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			dbg_breakDel(pc < 0 ? curpc : pc);
			*pbuf = 0;
			continue;
		} else if (line[0] == 'e') {
			// help
			if (!dbg_ensureEoln(lbuf, CMDBUFLEN, &ptr)) continue;
			dbg_breakEnum();
			memcpy(pbuf, lbuf, CMDBUFLEN);
			continue;
		} else if (line[0] == 'l') {
			// l [a [a]] list (disassemble)
			int start = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 1);
			dbg_skipSpace(lbuf, CMDBUFLEN, &ptr);
			int end = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 1);
			if (start == -1) start = dbg_nstart >= 0 ? dbg_nstart : curpc;
			if (end == -1) end = dbg_nend > 0 ? dbg_nend : start + 16;
			dbg_list(device, start, end);
			memcpy(pbuf, lbuf, CMDBUFLEN);
			continue;
		} else if (line[0] == 'x') {
			// x [a [a]] examine (dump)
			int start = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 1);
			dbg_skipSpace(lbuf, CMDBUFLEN, &ptr);
			int end = dbg_parseNum(lbuf, CMDBUFLEN, &ptr, 1);
			if (start == -1) start = dbg_nstart >= 0 ? dbg_nstart : curpc;
			if (end == -1) end = dbg_nend > 0 ? dbg_nend : start + 16;
			dbg_examine(device, start, end);
			memcpy(pbuf, lbuf, CMDBUFLEN);
			continue;
		} else if (line[0] == 0) {
			if (*pbuf == 0) continue;
			memcpy(lbuf, pbuf, CMDBUFLEN);
			if (lbuf[0] == 'l' || lbuf[0] == 'x' || lbuf[0] == 'r')
				lbuf[1] = 0;
			tty_cursorUp(1); tty_printf("> %s\r\n", lbuf);
			goto again;
		}
		tty_print("error: unknown command\r\n");
	}
}

static int first_entry = 1;

void dbg_instruction_hook(device_t *device, offs_t curpc) {
	do_timers();

	if (tty_checkKey() == K_ESCAPE) {
		tty_print("*escape*\r\n");
		dbg_stepping = 1;
	} else if (dbg_isBreak(curpc)) {
		tty_print("*breakpoint*\r\n");
		dbg_stepping = 1;
	}
	if (!dbg_stepping) return;

	if (first_entry) {
		tty_print("Entering debugger. Press ? or h for help.\r\n");
		first_entry = 0;
	}

	dbg_print_status(device, curpc);
	dbg_cli(device, curpc);
}
