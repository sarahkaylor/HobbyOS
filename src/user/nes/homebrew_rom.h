/*
 * homebrew_rom.h — a tiny hand-authored NES (NROM) program, as raw PRG bytes
 * (added to a 32 KiB cart at $8000; reset vector is set to $8000 by main.c).
 *
 * Program (hand-assembled 6502), all via the $2006/$2007 PPU port protocol:
 *  1. PPUADDR=$1000 -> write 16 bytes of $FF (pattern tile 0 lo+hi = solid
 *     bright).  Tile 1 is left $00 (dark).
 *  2. PPUADDR=$2000 -> write 32 nametable cells of tile 1 (a dark band across
 *     the top row); the rest of the nametable is already tile 0 (bright).
 *  3. Park in a tight JMP loop.
 * Result: a bright field with a dark 8px band across the top — a
 * deterministic, recognizable frame for the T5 screendump assertion.
 *
 * License: MIT, swarm-authored, license-clean (no copyrighted NES BIOS/ROM).
 */
#ifndef NES_HOMEBREW_ROM_H
#define NES_HOMEBREW_ROM_H

#define HOME_ROM_LEN 43
static const unsigned char home_rom[HOME_ROM_LEN] = {
  /* set PPUADDR = $1000 (pattern tile 0) */
  0xA9, 0x10,             /* LDA #$10                 $8000 */
  0x8D, 0x06, 0x20,       /* STA $2006  (hi)          $8002 */
  0xA9, 0x00,             /* LDA #$00                 $8005 */
  0x8D, 0x06, 0x20,       /* STA $2006  (lo) -> $1000 $8007 */
  /* write 16 bytes of $FF: tile 0 lo+hi = solid bright */
  0xA2, 0x10,             /* LDX #16                  $800A */
  0xA9, 0xFF,             /* LDA #$FF                 $800C */
                           /* loop0: */
  0x8D, 0x07, 0x20,       /*   STA $2007              $800E */
  0xCA,                   /*   DEX                    $8011 */
  0xD0, 0xFA,             /*   BNE loop0              $8012 */
  /* set PPUADDR = $2000 (nametable) */
  0xA9, 0x20,             /* LDA #$20                 $8014 */
  0x8D, 0x06, 0x20,       /* STA $2006  (hi)          $8016 */
  0xA9, 0x00,             /* LDA #$00                 $8019 */
  0x8D, 0x06, 0x20,       /* STA $2006  (lo) -> $2000 $801B */
  /* write 32 cells of tile 1 (dark top band) */
  0xA2, 0x20,             /* LDX #32                  $801E */
  0xA9, 0x01,             /* LDA #$01                 $8020 */
                           /* loopNT: */
  0x8D, 0x07, 0x20,       /*   STA $2007              $8022 */
  0xCA,                   /*   DEX                    $8025 */
  0xD0, 0xFA,             /*   BNE loopNT             $8026 */
  /* halt */
  0x4C, 0x28, 0x80        /* JMP $8028                $8028 */
};

#endif
