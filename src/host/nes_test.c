/*
 * nes_test.c — T1 host golden for the compact NES emulator core (GB-035).
 *
 * Compiled on the host: #includes nes.c directly (single translation unit).
 * Runs a hand-assembled 6502 program through the core and asserts
 * register/flag/memory/stack outcomes, a PPU render, and the controller.
 *
 * Exit 0 == all groups pass; prints a summary. License: MIT.
 */
#include <stdio.h>
#include <string.h>
#include "../user/nes/nes.h"
#include "../user/nes/homebrew_rom.h"
#include "../user/nes/nes.c"

static int passed = 0, failed = 0;
#define CHECK(cond, name) do { \
  if (cond) { passed++; printf("  PASS: %s\n", name); } \
  else { failed++; printf("  FAIL: %s\n", name); } \
} while (0)

int main(void){
  printf("NES T1 host golden\n");

  /* ---- 6502 execution ------------------------------------------------- */
  {
    nes_machine m;
    memset(&m, 0, sizeof(m));

    /* Program at $8000 (i.e. prg[0x0000]): see nes_test.c comment for trace. */
    static const u8 prog[] = {
      0xA9, 0x05,   /* LDA #$05            A=5, Z/N clear            */
      0x85, 0x20,   /* STA $20             [0x20]=5                  */
      0x18,         /* CLC                                            */
      0x69, 0x3A,   /* ADC #$3A            A=0x3F, no carry          */
      0x09, 0x40,   /* ORA #$40            A=0x7F                    */
      0x29, 0x0F,   /* AND #$0F            A=0x0F                    */
      0xE8,         /* INX                 X=1                       */
      0xE8,         /* INX                 X=2                       */
      0xCA,         /* DEX                 X=1                       */
      0x20, 0x20, 0x80, /* JSR $8020       call subroutine           */
      0xA6, 0x30,   /* LDX $30             X=[0x30]=7                */
      0xE0, 0x07,   /* CPX #$07            Z set, C set              */
      0xF0, 0x01,   /* BEQ +1              branch (Z) to $8018       */
      0xEA,         /* NOP (unreachable)                             */
      0x4C, 0x18, 0x80 /* JMP $8018        infinite loop             */
    };
    /* subroutine at $8020 (prg[0x20]): preserves A (PHA/PLA) */
    static const u8 sub[] = {
      0x48,               /* PHA                          */
      0xA9, 0x07, 0x85, 0x30, /* LDA #$07 ; STA $30        */
      0x68, 0x60          /* PLA ; RTS                    */
    };
    memcpy(&m.prg[0x0000], prog, sizeof(prog));
    memcpy(&m.prg[0x0020], sub, sizeof(sub));
    m.prg[0x7FFC] = 0x00;            /* reset vector -> $8000 */
    m.prg[0x7FFD] = 0x80;

    nes_reset(&m);
    CHECK(m.cpu.pc == 0x8000, "reset loads PC from vector $FFFC");
    nes_run(&m, 300);                /* enough to settle in the $8018 loop */

    CHECK(m.cpu.a == 0x0F, "ADC/ORA/AND residue A==0x0F");
    CHECK(m.cpu.x == 7, "LDX $30 after JSR/RTS sets X==7");
    CHECK(m.ram[0x20] == 5, "STA $20 wrote 5 to RAM");
    CHECK(m.ram[0x30] == 7, "subroutine STA $30 wrote 7 to RAM");
    CHECK(m.cpu.sp == 0xFD, "JSR/RTS balances the stack (sp back to 0xFD)");
    CHECK(m.cpu.p & 0x01, "CPX #$07 sets Carry");
    CHECK(m.cpu.p & 0x02, "CPX #$07 sets Zero");
    CHECK((m.cpu.p & 0x80) == 0, "result is non-negative (N clear)");
    CHECK(m.cpu.pc == 0x8018, "BEQ + JMP lands PC at $8018");
  }

  /* ---- PPU render ----------------------------------------------------- */
  {
    nes_machine m;
    memset(&m, 0, sizeof(m));
    /* Put a known tile (index 3) at nametable cell (0,0): vram[0x2000]=3.
       Pattern table tile 3, row 0 (vram 0x1000+3*16+0): set low bits 0xFF,
       hi bits 0x00 -> all pixels palette=1. */
    m.ppu.vram[0x0000] = 3;                      /* nametable cell (0,0) -> tile 3 */
    m.ppu.vram[0x1000 + 3*16 + 0] = 0xFF;         /* pattern table tile 3, row 0 low bits */
    m.ppu.vram[0x1000 + 3*16 + 8] = 0x00;         /* ... hi bits 0 -> palette=1 every px */
    nes_ppu_render(&m);
    u8 px = m.ppu.fb[0];             /* top-left pixel */
    CHECK(px > 48 && px < 96, "PPU renders tile pattern shades (0x00..0xFF tile)");
    /* a blank/hi-bits-0 tile elsewhere stays dark */
    CHECK(m.ppu.fb[NES_SCREEN_W + 0] < 48,
          "PPU leaves pixel-0 rows outside the drawn tile dark");

    /* PAL writes via $2006/$2007: draw tile 1 to vram cell (1,0). */
    (void)0;
  }

  /* ---- PPU address-latch write path ($2006/$2007) --------------------- */
  {
    nes_machine m;
    memset(&m, 0, sizeof(m));
    /* set PPUADDR = $1000 via the two-stage $2006 latch, then write a byte */
    nes_write(&m, 0x2006, 0x10);
    nes_write(&m, 0x2006, 0x00);
    CHECK(m.ppu.vram_addr == 0x1000, "$2006 latch commits PPUADDR=$1000");
    nes_write(&m, 0x2007, 0xFF);
    CHECK(m.ppu.vram[0x1000 & (NES_VRAM_SIZE-1)] == 0xFF,
          "$2007 writes through to the addressed VRAM byte");
    CHECK(m.ppu.vram_addr == 0x1001, "$2007 auto-increments PPUADDR by 1");
    /* second selector: PPUADDR = $2000 */
    nes_write(&m, 0x2006, 0x20);
    nes_write(&m, 0x2006, 0x00);
    CHECK(m.ppu.vram_addr == 0x2000, "$2006 latch commits PPUADDR=$2000");
    nes_write(&m, 0x2007, 0x01);
    CHECK(m.ppu.vram[0x0000] == 0x01, "$2007 writes the first nametable cell");
  }

  /* ---- controller ------------------------------------------------------ */
  {
    nes_machine m;
    memset(&m, 0, sizeof(m));
    m.joy = 0x05;                    /* A + D-pad-right? arbitrary bits 0 and 2 */
    nes_write(&m, 0x4016, 1);        /* strobe high */
    nes_write(&m, 0x4016, 0);        /* latch */
    u8 b0 = nes_read(&m, 0x4016);
    u8 b1 = nes_read(&m, 0x4016);
    CHECK((b0 & 1) == 1, "controller bit0 read back set");
    CHECK((b1 & 1) == 0, "controller bit1 (0 in joy) reads 0");
  }

  /* ---- homebrew ROM responds to the controller (GB-035 "plays a ROM") - */
  {
    int i, band;
    /* A released -> dark top band stays present (cells 0..31 = tile 1) */
    nes_machine m;
    memset(&m, 0, sizeof(m));
    for (i = 0; i < HOME_ROM_LEN; i++) m.prg[i] = home_rom[i];
    m.prg[0x7FFC] = 0x00; m.prg[0x7FFD] = 0x80;
    nes_reset(&m);
    m.joy = 0;
    nes_run(&m, 60000);
    CHECK(m.ppu.vram[0x1000] == 0xFF, "homebrew boots: pattern tile 0 solid bright");
    band = 0;
    for (i = 0; i < 32; i++)
      if (m.ppu.vram[(0x2000 + i) & (NES_VRAM_SIZE - 1)] == 1) band++;
    CHECK(band == 32, "homebrew: A released keeps the dark top band (resting frame)");

    /* A pressed -> the ROM clears the band (picture visibly changes) */
    nes_machine m2;
    memset(&m2, 0, sizeof(m2));
    for (i = 0; i < HOME_ROM_LEN; i++) m2.prg[i] = home_rom[i];
    m2.prg[0x7FFC] = 0x00; m2.prg[0x7FFD] = 0x80;
    nes_reset(&m2);
    m2.joy = 0x01;                       /* A = controller bit 0 */
    nes_run(&m2, 60000);
    band = 0;
    for (i = 0; i < 32; i++)
      if (m2.ppu.vram[(0x2000 + i) & (NES_VRAM_SIZE - 1)] == 0) band++;
    CHECK(band == 32, "homebrew: A pressed clears the band (ROM responds to input)");
  }

  printf("\nTests: passed=%d failed=%d -> %s\n",
         passed, failed, failed ? "FAILED" : "ALL PASSED");
  return failed ? 1 : 0;
}
