/*
 * homebrew_rom.h — a small hand-authored NES (NROM) homebrew, as raw PRG bytes
 * (added to a 32 KiB cart at $8000; reset vector is set to $8000 by main.c).
 *
 * Program (hand-assembled 6502):
 *   1. Pattern tile 0 = solid bright (write 16 bytes of $FF at PPUADDR=$1000);
 *      tile 1 stays dark ($00).
 *   2. Draw a dark top band now (32 nametable cells = tile 1 at PPUADDR=$2000),
 *      so the default boot frame is a bright field with a dark top band.
 *   3. In a tight loop, read the NES controller ($4016 strobe + serial read):
 *      - A released -> (re)draw the dark band (the resting frame);
 *      - A pressed  -> clear it (write tile 0 = bright over the band).
 *      So the ROM visibly responds to the A button — "boots and plays a ROM".
 *
 * License: MIT, swarm-authored, license-clean (no copyrighted NES BIOS/ROM).
 */
#ifndef NES_HOMEBREW_ROM_H
#define NES_HOMEBREW_ROM_H

#define HOME_ROM_LEN 94
static const unsigned char home_rom[HOME_ROM_LEN] = {
  0xA9, 0x10,             /* LDA #$10                 $8000 */
  0x8D, 0x06, 0x20,       /* STA $2006  (hi)          $8002 */
  0xA9, 0x00,             /* LDA #$00                 $8005 */
  0x8D, 0x06, 0x20,       /* STA $2006  (lo)->$1000   $8007 */
  0xA2, 0x10,             /* LDX #16                  $800A */
  0xA0, 0xFF,             /* LDY #$FF                 $800C */
                           /* p0: */
  0x8C, 0x07, 0x20,       /*   STY $2007              $800E */
  0xCA,                   /*   DEX                    $8011 */
  0xD0, 0xFA,             /*   BNE p0                 $8012 */
  0x20, 0x34, 0x80,       /* JSR band_dark            $8014 */
   /* main: */
  0xA9, 0x01,             /* LDA #$01                 $8017 */
  0x8D, 0x16, 0x40,       /* STA $4016  (strobe hi)   $8019 */
  0xA9, 0x00,             /* LDA #$00                 $801C */
  0x8D, 0x16, 0x40,       /* STA $4016  (strobe lo)   $801E */
  0xAD, 0x16, 0x40,       /* LDA $4016  (bit0 = A)    $8021 */
  0x29, 0x01,             /* AND #$01                 $8024 */
  0xD0, 0x06,             /* BNE a_down               $8026 */
  0x20, 0x34, 0x80,       /* JSR band_dark            $8028 */
  0x4C, 0x17, 0x80,       /* JMP main                 $802B */
   /* a_down: */
  0x20, 0x49, 0x80,       /* JSR band_bright          $802E */
  0x4C, 0x17, 0x80,       /* JMP main                 $8031 */
   /* band_dark ($8034): write 32 cells of $01 at PPUADDR=$2000 */
  0xA9, 0x20, 0x8D, 0x06, 0x20,
  0xA9, 0x00, 0x8D, 0x06, 0x20,
  0xA2, 0x20, 0xA9, 0x01,
                           /* bd: */
  0x8D, 0x07, 0x20, 0xCA, 0xD0, 0xFA, 0x60,
   /* band_bright ($8049): write 32 cells of $00 at PPUADDR=$2000 */
  0xA9, 0x20, 0x8D, 0x06, 0x20,
  0xA9, 0x00, 0x8D, 0x06, 0x20,
  0xA2, 0x20, 0xA9, 0x00,
                           /* bb: */
  0x8D, 0x07, 0x20, 0xCA, 0xD0, 0xFA, 0x60
};

#endif
