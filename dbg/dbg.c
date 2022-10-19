#include <signal.h>

#define RAWTTY_IMPLEMENTATION
#include "rawtty.h"

#include "z180/z180.h"

/* reference into main program */
extern void do_timers();

static UINT8 *mem_ram = 0;
static UINT8 *mem_rom = 0;

static int dbg_quit = 0;
static int dbg_stepping = 0;

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

int dbg_init(UINT8 *ram, UINT8 *rom)
{
	if (tty_init() == 0) {
		atexit(tty_deinit);
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

int dbg_running()
{
	return !dbg_quit;
}


static UINT8 dbg_getmem(device_t *device, offs_t addr) {
	UINT8 ROMBR, RAMUBR, RAMLBR, SCR;
	UINT8 *mem = NULL;
	cpu_translate_z180(device,AS_PROGRAM,0,&addr);
	if (((struct z180_device *)device)->m_type == Z180_TYPE_Z182) {
		ROMBR = cpu_get_state_z180(device,Z182_ROMBR);
		RAMUBR = cpu_get_state_z180(device,Z182_RAMUBR);
		RAMLBR = cpu_get_state_z180(device,Z182_RAMLBR);
		SCR = cpu_get_state_z180(device,Z182_SCR);
		if(!(SCR & 8) && (addr >>12) <= ROMBR)
			mem = mem_rom;
		else {
			mem = mem_ram;	
			if( (addr >>12) < RAMLBR || RAMUBR < (addr >>12) )
				printf("RAM access outside bounds\n");
		}
	} else {
		mem = mem_ram;
	}
	return mem[addr];
}

void dbg_instruction_hook(device_t *device, offs_t curpc) {
	//printf(".");
	char ibuf[20];
	offs_t dres,i;
	char fbuf[10];
	UINT8 ROMBR, RAMUBR, RAMLBR, SCR;
	UINT8 *mem = NULL;

	do_timers();

	if (tty_checkKey() == K_ESCAPE) dbg_stepping = 1;
	if (!dbg_stepping) return;

		cpu_string_export_z180(device,STATE_GENFLAGS,fbuf);
		tty_printf("%s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X\r\n",fbuf,
		    cpu_get_state_z180(device,Z180_AF),
			cpu_get_state_z180(device,Z180_BC),
			cpu_get_state_z180(device,Z180_DE),
			cpu_get_state_z180(device,Z180_HL),
			cpu_get_state_z180(device,Z180_IX),
			cpu_get_state_z180(device,Z180_IY),
			cpu_get_state_z180(device,Z180_SP));
		cpu_translate_z180(device,AS_PROGRAM,0,&curpc);
		if (((struct z180_device *)device)->m_type == Z180_TYPE_Z182) {
			ROMBR = cpu_get_state_z180(device,Z182_ROMBR);
			RAMUBR = cpu_get_state_z180(device,Z182_RAMUBR);
			RAMLBR = cpu_get_state_z180(device,Z182_RAMLBR);
			SCR = cpu_get_state_z180(device,Z182_SCR);
			if(!(SCR & 8) && (curpc >>12) <= ROMBR)
				mem = mem_rom;
			else {
				mem = mem_ram;	
				if( (curpc >>12) < RAMLBR || RAMUBR < (curpc >>12) )
					tty_printf("Opcode fetch from RAM outside bounds\r\n");
			}
		}
		else {
			mem = mem_ram;
		}
		dres = cpu_disassemble_z180(device,ibuf,curpc,&mem[curpc],&mem[curpc],0);
		tty_printf("%05x: ",curpc);
		for (i=0;i<(dres &DASMFLAG_LENGTHMASK);i++) tty_printf("%02X",mem[curpc+i]);
		for ( ;i<4;i++) {tty_writeByte(' ');tty_writeByte(' ');}
		tty_printf(" %s",ibuf);
		if (strstr(ibuf,",(hl)")||strstr(ibuf," (hl)")||strstr(ibuf,"ldi")||strstr(ibuf,"ldd"))
			tty_printf("\tm:%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_HL)));
		else if (strstr(ibuf,",(de)"))
			tty_printf("\tm:%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_DE)));
		else if (strstr(ibuf,",(bc)"))
			tty_printf("\tm:%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_BC)));
		else if (strstr(ibuf,",(ix"))
			tty_printf("\tm:%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_IX)+(int8_t)mem[curpc+2]));
		else if (strstr(ibuf,",(iy"))
			tty_printf("\tm:%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_IY)+(int8_t)mem[curpc+2]));
		else if (strstr(ibuf,"(sp),"))	  // ex (sp),...
			tty_printf("\tm:%02X",dbg_getmem(device, cpu_get_state_z180(device,Z180_SP)));
		tty_newline();

	char lbuf[60];
	while (1) {
		tty_printf("> ");
		const char *line = tty_readLine(lbuf, 60);
		if (!line || line[0] == 'q') {
			dbg_quit = 1;
			dbg_stepping = 0;
			return;
		} else if (line[0] == 'r') {
			dbg_stepping = 0;
			return;
		} else if (line[0] == 0) {
			return;
		}
		tty_print("?\r\n");
	}

}
