
extern int dbg_init(UINT8 *ram, UINT8 *rom);
extern int dbg_running();

extern void dbg_instruction_hook(device_t *device, offs_t curpc);

void debugger_instruction_hook(device_t *device, offs_t curpc) {
	return dbg_instruction_hook(device, curpc);
}
