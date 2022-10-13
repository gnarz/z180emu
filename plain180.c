/*
 * plain180.c - plain z180system emulation, nothing but z180 CPU and RAM,
 *              derived from markiv.c
 *
 * Gunnar Zötl 2022 <gz@tset.de>
 *
 * This is a very simple system. It consists only of the CPU and RAM. The
 * contents of the file plain180rom.bin is copied into RAM starting at address
 * 0000 before the cpu starts.
 *
 * Note: Windows is (currently?) not supported.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>

#ifdef SOCKETCONSOLE
#define BASE_PORT 10180
#define MAX_SOCKET_PORTS 1
#include "sconsole.h"
#endif

#include <signal.h>

#include "z180/z180.h"

int VERBOSE = 0;

unsigned int ramsize = 512 * 1024; // available RAM
UINT8 _ram[1024 * 1024]; // max 1MB of RAM

#define RAMARRAY _ram
#define ROMARRAY NULL
#include "z180dbg.h"

unsigned int asci_clock = 16;

struct z180_device *cpu = NULL;
                       
UINT8 ram_read(offs_t A) {
	if (A < ramsize) return _ram[A];
	return 0xFF;
}

void ram_write(offs_t A,UINT8 V) {
    if (A < ramsize) _ram[A]=V;
}

int char_available() {
#ifdef SOCKETCONSOLE
	  return char_available_socket_port(0);
#else
      return _kbhit();
#endif
}

void asci_tx(device_t *device, int channel, UINT8 Value) {
	if (channel==0) {
	  //printf("TX: %c", Value);
#ifdef SOCKETCONSOLE
	  tx_socket_port(0, Value);
#else
	  fputc(Value,stdout);
#endif
	  //printf("\n");
	}
}

int asci_rx(device_t *device, int channel) {
	int ioData;
	if (channel==0) {
	  //ioData = 0xFF;
	  if(char_available()) {
#ifdef SOCKETCONSOLE
	  ioData = rx_socket_port(0);
#else
	    //printf("RX\n");
        ioData = getch();
#endif
		return ioData;
	  }
	}
	return -1;
}

int irq0ackcallback(device_t *device,int irqnum) {
}

UINT8 io_read (offs_t Port) {
  Port &= 0xff;
  uint8_t ioData = 0;
  switch (Port) {
    default:
	    printf("IO: Bogus read %x\n",Port);
		break;
  }
  return ioData;
}

void io_write (offs_t Port,UINT8 Value) {
  Port &= 0xff;
  switch (Port) {
    default:
	    printf("IO: Bogus write %x:%x\n",Port,Value);
		break;
  }
}

void do_timers() {
	//16X clock for ASCI
	//printf("asci_clk:%d\n",asci_clock);
	if (!--asci_clock) {
		z180asci_channel_device_timer(cpu->z180asci->m_chan0);
		z180asci_channel_device_timer(cpu->z180asci->m_chan1);
		asci_clock = 16;
	}
}

void boot1dma () {
   FILE* f;
   if (!(f=fopen("plain180rom.bin","rb"))) {
     printf("No ROM found.\n");
	 g_quit = 1;
   } else {
     size_t dummy_rd = fread(&_ram[0],1,524288,f);
     fclose(f);
   }
}

void io_device_update() {
#ifdef SOCKETCONSOLE
    // check socket open and optionally reopen it
    if (!is_connected_socket_port(0)) open_socket_port(0);
#endif
}

#ifndef _WIN32
void sigint_handler(int s)	{
	// POSIX SIGINT handler
	// do nothing
}

void sigquit_handler(int s)	{
	// POSIX SIGQUIT handler
	// make sure atexit is called
	printf("\nExiting emulation.\n");
	shutdown_socket_ports(); // close sockets to prevent waiting for a connection
	g_quit = 1; // make sure atexit is called
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

struct address_space ram = {ram_read,ram_write,ram_read};
//struct address_space rom = {rom_read,NULL,rom_read};
struct address_space iospace = {io_read,io_write,NULL};

int main(int argc, char** argv)
{
	printf("z180emu v1.0 plain180\n");

	disableCTRLC();
#ifndef _WIN32
	// on POSIX, route SIGQUIT (CTRL+\) to graceful shutdown
	signal(SIGQUIT, sigquit_handler);
#endif
	// on MINGW, keep CTRL+Break (and window close button) enabled
	// MINGW always calls atexit in these cases

#ifdef SOCKETCONSOLE
	init_TCPIP();
	init_socket_port(0); // ASCI Console
	atexit(shutdown_socket_ports);
#endif
	io_device_update(); // wait for serial socket connections

	if (argc==2 && !strcmp(argv[1],"d")) starttrace = 0;
	else if (argc==3 && !strcmp(argv[1],"d")) starttrace = atoll(argv[2]);
	VERBOSE = starttrace==0?1:0;

#ifdef _WIN32
	setmode(fileno(stdout), O_BINARY);
#endif

	boot1dma();

	cpu = cpu_create_z180("Z180",Z180_TYPE_Z180,18432000,&ram,NULL,&iospace,irq0ackcallback,NULL/*daisychain*/,
		asci_rx,asci_tx,NULL,NULL,NULL,NULL);
	//printf("1\n");fflush(stdout);
	cpu_reset_z180(cpu);
	//printf("2\n");fflush(stdout);

	struct timeval t0;
	struct timeval t1;
	gettimeofday(&t0, 0);
	int runtime=50000;

	//g_quit = 0;
	while(!g_quit) {
		if(instrcnt>=starttrace) VERBOSE=1;
		cpu_execute_z180(cpu,10000);
		//printf("3\n");fflush(stdout);
		io_device_update();
		/*if (!(--runtime))
			g_quit=1;*/
	}
	gettimeofday(&t1, 0);
	printf("instrs:%llu, time:%g\n",instrcnt, (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f);
}
