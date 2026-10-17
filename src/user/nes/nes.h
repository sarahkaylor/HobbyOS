/*
 * nes.h — compact NES emulator for HobbyOS (GB-035).
 *
 * A small, single-header NES platform: a 6502 CPU core, a minimal PPU that
 * renders the 32x30 nametable from 8x8 pattern-table tiles to a 256x240
 * framebuffer, an NROM (mapper-0) cart, and 2 KiB of work RAM.  The CPU,
 * bus and PPU are pure C with no HobbyOS dependency so the host (T1)
 * golden suite drives them directly; only src/user/nes/main.c touches the
 * HobbyOS graphics/input API.
 *
 * License: MIT (swarm-authored, license-clean — no copyrighted NES BIOS or
 * commercial ROM; the test ROM is hand-authored machine code in this repo).
 */
#ifndef NES_NES_H
#define NES_NES_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef int16_t  s16;

#define NES_RAM_SIZE   0x0800        /* 2 KiB work RAM (mirrored 0x800..0x1FFF) */
#define NES_PRG_SIZE   0x8000        /* 32 KiB PRG ROM (NROM) */
#define NES_VRAM_SIZE  0x2000        /* nametable + attribute + pattern table */
#define NES_SCREEN_W   256
#define NES_SCREEN_H   240

/* 6502 CPU */
typedef struct {
  u8  a, x, y, sp;       /* accum, index regs, stack pointer */
  u8  p;                 /* status flags: N V - B D I Z C */
  u16 pc;                /* program counter */
  u16 cycles;            /* cycle counter (for the runtime's pacing) */
} nes_cpu;

/* PPU register window on the CPU bus ($2000..$2007) */
typedef struct {
  u8  ctrl, mask, status;
  u8  oam_addr;
  u8  oam[256];           /* sprite attribute memory (64 sprites x 4 bytes) */
  u16 vram_addr;
  u16 vram_latch;        /* two-stage $2006 address latch */
  u8  vram_buf;          /* the read buffer for $2007 */
  u8  vram[NES_VRAM_SIZE];
  /* rendering output */
  u8  fb[NES_SCREEN_W * NES_SCREEN_H];
} nes_ppu;

/* The machine: CPU + bus (RAM, PPU, cart) */
typedef struct {
  nes_cpu  cpu;
  nes_ppu  ppu;
  u8       ram[NES_RAM_SIZE];
  u8       prg[NES_PRG_SIZE];        /* PRG ROM image */
  u8       joy;                      /* controller state (bit i = button i) */
  u8       joy_strobe;               /* 0x4016 write strobe */
  u8       joy_shift;                /* serial read shift register */
} nes_machine;

/* Reset / step */
void nes_reset(nes_machine *m);
void nes_tick(nes_machine *m);                 /* run one instruction */
void nes_run(nes_machine *m, u16 max_cycles);  /* run until cycles exhausted */

/* Bus access (NES address space) */
u8  nes_read(nes_machine *m, u16 addr);
void nes_write(nes_machine *m, u16 addr, u8 val);

/* PPU render: draw the current nametable/pattern to the internal fb */
void nes_ppu_render(nes_machine *m);
/* Controller (0x4016 write strobe / read latch) */
void nes_joy_strobe(nes_machine *m, u8 val);
u8   nes_joy_read(nes_machine *m, int port);

#endif /* NES_NES_H */
