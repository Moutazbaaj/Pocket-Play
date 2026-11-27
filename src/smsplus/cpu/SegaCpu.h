#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { CLEAR_LINE, ASSERT_LINE, INPUT_LINE_IRQ0 = 0, INPUT_LINE_NMI = 32 };

typedef struct {
  uint8_t (*mem_read)(uint16_t address);
  void (*mem_write)(uint16_t address, uint8_t value);
  uint8_t (*port_read)(uint16_t port);
  void (*port_write)(uint16_t port, uint8_t value);
} SegaCpuBus;

extern SegaCpuBus segaCpuBus;
extern int z80_cycle_count;
extern uint8_t *cpu_readmap[64];
extern uint8_t *cpu_writemap[64];

void segaCpuReset(void);
uint16_t segaCpuPC(void);
void z80_execute(int cycles);
void z80_set_irq_line(int line, int state);
int z80_get_elapsed_cycles(void);
bool segaCpuWrite(FILE *file);
bool segaCpuRead(FILE *file);
