/*
 * dbg.c - debugger for z180 emulator
 *
 * Note: Windows is (currently?) not supported.
 *
 * Copyright (c) Gunnar Zötl 2022 <gz@tset.de>
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

extern int dbg_init(int stepping, UINT8 *ram, UINT8 *rom);
extern int dbg_running();
extern void dbg_log(const char *fmt, ...);

#ifdef DBG_MAIN
extern void dbg_instruction_hook(device_t *device, offs_t curpc);
void debugger_instruction_hook(device_t *device, offs_t curpc) {
	return dbg_instruction_hook(device, curpc);
}
#endif /* DBG_MAIN */
