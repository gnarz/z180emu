/*
 * markiv.c - Mark IV emulation.
 *
 * Copyright (c) Michal Tomek 2018-2019 <mtdev79b@gmail.com>
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

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#include <fcntl.h>
#define fileno _fileno
#else
#include <signal.h>
#endif

#include "z180/z180.h"
#include "ide/ide.h"
#include "ds1202_1302/ds1202_1302.h"
#define DBG_MAIN
#include "dbg/dbg.h"

int VERBOSE = 0;

// so far only 512k EEPROM+512k RAM is supported
UINT8 _ram[1048576]; // lo 512k is ROM

#define RAMARRAY _ram
#define ROMARRAY NULL

struct ide_controller *ic0;
FILE* if00;
int ifd00;
struct ide_drive *id00;

uint8_t idemap[16] = {ide_data,ide_error_r,ide_sec_count,ide_sec_num,ide_cyl_low,ide_cyl_hi,ide_dev_head,ide_status_r,
					 0,0,0,0,0,0,ide_altst_r,0};

unsigned int asci_clock = 16;

rtc_ds1202_1302_t *rtc;

UINT8 xmem_bank;

struct z180_device *cpu;
                       
UINT8 ram_read(offs_t A) {
 	return _ram[A];
}

void ram_write(offs_t A,UINT8 V) {
    if (A >= 524288) _ram[A]=V; // low 512k is eprom
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

// 74174 D-flop
UINT8 rtc_latch;

UINT8 rtc_read() {
	UINT8 x;
	//D0=data out
	x=ds1202_1302_read_data_line(rtc);
	x |= rtc_latch;
	dbg_log("RTC read: %02x\n",x);
	return x;
}

void rtc_write(UINT8 value) {
	// DS1302
	// D7=data in,D6=CLK,D5=data in /EN,D4=/RST
	dbg_log("RTC write: %02x\n",value);
	ds1202_1302_set_lines(rtc,value&0x10?1:0,value&0x40?1:0,value&0x20?0:(value&0x80?1:0));
	rtc_latch = value & 0xf0;
}

UINT8 io_read (offs_t Port) {
  Port &= 0xff;
  uint8_t ioData = 0;
  switch (Port) {
	case  0x80:
	case  0x81:
	case  0x82:
	case  0x83:
	case  0x84:
	case  0x85:
	case  0x86:
	case  0x87:
	case  0x8e:
	  ioData=ide_read8(ic0,idemap[Port-0x80]);
	break;
	case 0x8a:  // MarkIV RTC
		ioData=rtc_read();
	break;
    default:
	    dbg_log("IO: Bogus read %x\n",Port);
	break;
  }
  return ioData;
}

void io_write (offs_t Port,UINT8 Value) {
  Port &= 0xff;
  switch (Port) {
	case  0x80:
	case  0x81:
	case  0x82:
	case  0x83:
	case  0x84:
	case  0x85:
	case  0x86:
	case  0x87:
	case  0x8e:
	  ide_write8(ic0,idemap[Port-0x80],Value);
	break;
	case 0x88:
		if (Value & 0x80) ide_reset_begin(ic0);
		dbg_log("Setting XMEM bank: %02x\n",Value & 0x1f);
		xmem_bank = Value & 0x1f;
	break;
	case 0x8a:  // MarkIV RTC
		rtc_write(Value);
	break;
    default:
	    dbg_log("IO: Bogus write %x:%x\n",Port,Value);
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

int boot1dma (const char *romfile) {
   FILE* f;
   if (!(f=fopen(romfile,"rb"))) {
     printf("ROM file %s not found.\n", romfile);
	 return -1;
   } else {
     size_t dummy_rd = fread(&_ram[0],1,524288,f);
     fclose(f);
   }
   return 0;
}

void io_device_update() {
#ifdef SOCKETCONSOLE
    // check socket open and optionally reopen it
    if (!is_connected_socket_port(0)) open_socket_port(0);
#endif
}

void CloseIDE() {
   ide_free(ic0);
}

void InitIDE() {
   ic0=ide_allocate("IDE0");
   if (if00=fopen("cf00.dsk","r+b")) {
     ifd00=fileno(if00);
     ide_attach(ic0,0,ifd00);
   }
   ide_reset_begin(ic0);
   atexit(CloseIDE);
}

struct address_space ram = {ram_read,ram_write,ram_read};
//struct address_space rom = {rom_read,NULL,rom_read};
struct address_space iospace = {io_read,io_write,NULL};

void destroy_rtc()
{
	ds1202_1302_destroy(rtc,1);
}

void help(const char *prg) {
	printf("%s [-h|-?] [-v] [-d] [-r romfile]", prg);
	printf("  -h|-?      display this help\n");
	printf("  -v         start emulator in verbose mode\n");
	printf("  -d         start emulator in debugger mode\n");
	printf("  -r romfile start emulator with another rom file\n");
}

int main(int argc, char** argv)
{
	printf("z180emu v1.0 Mark IV. Press escape to enter debugger.\n");

	int opt;
	int debugger = 0;
	const char *romfile = "markivrom.bin";
	while ((opt = getopt(argc, argv, "h?vdr:")) != -1) {
		switch (opt) {
			case 'h': case '?':
				help(argv[0]);
				exit(0);
			case 'v':
				VERBOSE = 1;
				break;
			case 'd':
				debugger = 1;
				break;
			case 'r':
				romfile = optarg;
				break;
			default:
				printf("invalid option.\n");
				help(argv[0]);
				exit(1);
		}
	}

#ifdef SOCKETCONSOLE
	init_TCPIP();
	init_socket_port(0); // ASCI Console
	atexit(shutdown_socket_ports);
#endif
	io_device_update(); // wait for serial socket connections

#ifdef _WIN32
	setmode(fileno(stdout), O_BINARY);
#endif

	if (boot1dma(romfile) == -1) exit(1);

	InitIDE();

	rtc = ds1202_1302_init("RTC",1302);
	ds1202_1302_reset(rtc);
	atexit(destroy_rtc);

	cpu = cpu_create_z180("Z180",Z180_TYPE_Z180,18432000,&ram,NULL,&iospace,irq0ackcallback,NULL/*daisychain*/,
		asci_rx,asci_tx,NULL,NULL,NULL,NULL);
	cpu_reset_z180(cpu);

	struct timeval t0;
	struct timeval t1;
	gettimeofday(&t0, 0);
	int runtime=50000;

	if (dbg_init(debugger, RAMARRAY, ROMARRAY) == -1) exit(1);

	while(dbg_running()) {
		cpu_execute_z180(cpu,10000);
		io_device_update();
	}

	gettimeofday(&t1, 0);
	//printf("instrs:%llu, time:%g\n",instrcnt, (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f);
	printf("time:%g\n", (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f);
}
