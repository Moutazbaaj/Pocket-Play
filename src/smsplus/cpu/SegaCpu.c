#include <stdbool.h>
#include "SegaCpu.h"
#include "z80.h"

SegaCpuBus segaCpuBus;
int z80_cycle_count;
uint8_t *cpu_readmap[64];
uint8_t *cpu_writemap[64];

static z80 cpu;
static unsigned long sliceStart;
static bool interruptLine;
static bool nmiLine;

static uint8_t readMemory(void *context, uint16_t address) {
  (void)context;
  return segaCpuBus.mem_read(address);
}

static void writeMemory(void *context, uint16_t address, uint8_t value) {
  (void)context;
  segaCpuBus.mem_write(address, value);
}

static uint8_t readPort(z80 *machine, uint8_t port) {
  (void)machine;
  return segaCpuBus.port_read(port);
}

static void writePort(z80 *machine, uint8_t port, uint8_t value) {
  (void)machine;
  segaCpuBus.port_write(port, value);
}

// Reset registers but keep the console's selected port and memory handlers.
void segaCpuReset(void) {
  z80_init(&cpu);
  cpu.read_byte = readMemory;
  cpu.write_byte = writeMemory;
  cpu.port_in = readPort;
  cpu.port_out = writePort;
  cpu.sp = 0xdff0;
  cpu.int_data = 0xff;
  z80_cycle_count = 0;
  sliceStart = 0;
  interruptLine = nmiLine = false;
}

uint16_t segaCpuPC(void) { return cpu.pc; }

int z80_get_elapsed_cycles(void) {
  return z80_cycle_count + (int)(cpu.cyc - sliceStart);
}

// Preserve scanline cycle overrun and present the IRQ line until the VDP clears it.
void z80_execute(int cycles) {
  sliceStart = cpu.cyc;
  while ((int)(cpu.cyc - sliceStart) < cycles) {
    cpu.int_pending = interruptLine;
    z80_step(&cpu);
  }
  z80_cycle_count += (int)(cpu.cyc - sliceStart);
  sliceStart = cpu.cyc;
}

void z80_set_irq_line(int line, int state) {
  const bool asserted = state != CLEAR_LINE;
  if (line == INPUT_LINE_NMI) {
    if (asserted && !nmiLine) z80_gen_nmi(&cpu);
    nmiLine = asserted;
  } else {
    interruptLine = asserted;
    cpu.int_pending = asserted;
  }
}

// Persist the CPU's registers and line states without callbacks or pointers.
typedef struct {
  uint16_t pc, sp, ix, iy, wz;
  uint8_t a, b, c, d, e, h, l;
  uint8_t a2, b2, c2, d2, e2, h2, l2, f2;
  uint8_t i, r, flags, iffDelay, mode, interruptFlags;
  uint8_t reserved;
  int32_t cycles;
} CpuState;

bool segaCpuWrite(FILE *file) {
  const uint8_t flags = cpu.sf << 7 | cpu.zf << 6 | cpu.yf << 5 | cpu.hf << 4 |
                        cpu.xf << 3 | cpu.pf << 2 | cpu.nf << 1 | cpu.cf;
  const uint8_t interrupts = cpu.iff1 | cpu.iff2 << 1 | cpu.halted << 2 |
                             interruptLine << 3 | cpu.nmi_pending << 4 | nmiLine << 5;
  const CpuState state = {
    cpu.pc, cpu.sp, cpu.ix, cpu.iy, cpu.mem_ptr,
    cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l,
    cpu.a_, cpu.b_, cpu.c_, cpu.d_, cpu.e_, cpu.h_, cpu.l_, cpu.f_,
    cpu.i, cpu.r, flags, cpu.iff_delay, cpu.interrupt_mode, interrupts, 0, z80_cycle_count,
  };
  return fwrite(&state, sizeof(state), 1, file) == 1;
}

bool segaCpuRead(FILE *file) {
  CpuState state;
  if (fread(&state, sizeof(state), 1, file) != 1 || state.mode > 2 || state.cycles < 0 ||
      state.cycles > 100) return false;
  cpu.pc = state.pc; cpu.sp = state.sp; cpu.ix = state.ix; cpu.iy = state.iy; cpu.mem_ptr = state.wz;
  cpu.a = state.a; cpu.b = state.b; cpu.c = state.c; cpu.d = state.d;
  cpu.e = state.e; cpu.h = state.h; cpu.l = state.l;
  cpu.a_ = state.a2; cpu.b_ = state.b2; cpu.c_ = state.c2; cpu.d_ = state.d2;
  cpu.e_ = state.e2; cpu.h_ = state.h2; cpu.l_ = state.l2; cpu.f_ = state.f2;
  cpu.i = state.i; cpu.r = state.r; cpu.iff_delay = state.iffDelay; cpu.interrupt_mode = state.mode;
  cpu.sf = (state.flags >> 7) & 1; cpu.zf = (state.flags >> 6) & 1;
  cpu.yf = (state.flags >> 5) & 1; cpu.hf = (state.flags >> 4) & 1;
  cpu.xf = (state.flags >> 3) & 1; cpu.pf = (state.flags >> 2) & 1;
  cpu.nf = (state.flags >> 1) & 1; cpu.cf = state.flags & 1;
  cpu.iff1 = state.interruptFlags & 1;
  cpu.iff2 = (state.interruptFlags >> 1) & 1;
  cpu.halted = (state.interruptFlags >> 2) & 1;
  interruptLine = (state.interruptFlags >> 3) & 1;
  cpu.nmi_pending = (state.interruptFlags >> 4) & 1;
  nmiLine = (state.interruptFlags >> 5) & 1;
  z80_cycle_count = state.cycles;
  return true;
}
