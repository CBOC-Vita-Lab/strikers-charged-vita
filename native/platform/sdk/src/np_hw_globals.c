// The "hardware globals" of the Wii SDK headers (low-memory variables at 0x8000xxxx on the console).
// tools/mkinc.sh turns their AT_ADDRESS declarations into plain externs; this file defines the ones
// that game or SDK code reads. Hardware register blocks (PI/MI/DI/VI/AI/DSP/EXI/SI/IPC registers)
// are NOT defined on purpose: anything that touches them is a function that needs a native
// replacement, and the link says so.
#include <revolution/os.h>

OSContext* OS_CURRENT_CONTEXT_PHYS;
OSContext* OS_CURRENT_CONTEXT;
OSContext* OS_CURRENT_FPU_CONTEXT;
OSThreadQueue OS_THREAD_QUEUE;
OSThread* OS_CURRENT_THREAD;
u32 OS_TV_FORMAT;
s64 OS_SYSTEM_TIME;
volatile s32 OS_EXI_LAST_INSERT[2];
u16 OS_GC_PAD_3_BTN;
u8 OS_PAD_FLAGS;
void* OS_DEBUG_MONITOR;
u32 OS_DEBUG_MONITOR_SIZE;
u32 OS_PHYSICAL_MEM1_SIZE;
u32 OS_SIMULATED_MEM1_SIZE;
u32 OS_PHYSICAL_MEM2_SIZE;
u32 OS_SIMULATED_MEM2_SIZE;
u32 OS_HOLLYWOOD_REV;
volatile u16 OS_DVD_DEVICE_CODE;
u8 OS_BI2_DEBUG_FLAG;
void* OS_USABLE_MEM1_END;
u8 OS_SC_PRDINFO[0x100];
OSBootInfo OS_BOOT_INFO;
OSDebugInterface OS_DEBUG_INTERFACE;
OSBI2* OS_DVD_BI2;
// The bus clock: 243 MHz (the timer runs at a quarter of it). Set before main so that the game's
// static initialisers can use OS_TIME_SPEED; OSInit sets it again.
unsigned long OS_BUS_CLOCK_SPEED = 243000000ul;
