/*
 * homebrew_rom.h — a small hand-authored NES (NROM) homebrew, as raw PRG bytes
 * (added to a 32 KiB cart at $8000; reset vector is set to $8000 by main.c).
 *
 * Program (hand-assembled 6502):
 *   1. Pattern tile 0 = solid bright (write 16 bytes of $FF at PPUADDR=$1000);
 *      tile 1 stays dark ($00).
 *   2. Pattern tile 3 = solid mid-grey ($FF hi bytes at PPUADDR=$1038) for the
 *      demo sprite's glyph.
 *   3. Place one sprite via OAM ($2003/$2004): tile 3 at (x=12, y=12).
 *   4. Draw a dark top band now (32 nametable cells = tile 1 at PPUADDR=$2000),
 *      so the default boot frame is a bright field with a dark top band and a
 *      visible sprite — demonstrating background + OAM sprite rendering.
 *   5. In a tight loop (main), read the NES controller ($4016 strobe + serial):
 *      - A released -> (re)draw the dark band (the resting frame);
 *      - A pressed  -> clear it (write tile 0 = bright over the band).
 *      So the ROM visibly responds to the A button — "boots and plays a ROM".
 *   6. Every loop iteration also toggles a single nametable cell ($2000+0x34,
 *      row 1, col 20, outside the band and sprite) between tile dark / bright.
 *      This makes the ROM ANIMATE ON ITS OWN — the rendered frame changes each
 *      pass with NO input — so a T5 screendump pair 2 s apart differs in the
 *      game area (GB-035-V's "advance a frame on its own" green path).
 *
 * License: MIT, swarm-authored, license-clean (no copyrighted NES BIOS/ROM).
 */
#ifndef NES_HOMEBREW_ROM_H
#define NES_HOMEBREW_ROM_H

#define HOME_ROM_LEN 161
static const unsigned char home_rom[HOME_ROM_LEN] = {
  /* tile0 = solid bright (16 bytes of $FF at PPUADDR=$1000) */
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
  0x20, 0x61, 0x80,       /* JSR band_dark            $8014 */
  /* tile3 = solid mid-grey: 8 bytes of $FF at PPUADDR=$1038 (hi planes) */
  0xA9, 0x10,             /* LDA #$10                 $8017 */
  0x8D, 0x06, 0x20,       /* STA $2006  (hi)          $8019 */
  0xA9, 0x38,             /* LDA #$38                 $801C */
  0x8D, 0x06, 0x20,       /* STA $2006  (lo)->$1038   $801E */
  0xA2, 0x08,             /* LDX #8                   $8021 */
  0xA9, 0xFF,             /* LDA #$FF                 $8023 */
                          /* sp: */
  0x8D, 0x07, 0x20,       /*   STA $2007              $8025 */
  0xCA,                   /*   DEX                    $8028 */
  0xD0, 0xFA,             /*   BNE sp                 $8029 */
  /* OAM: sprite 0 = (y=12, tile=3, attr=0, x=12) via $2003/$2004 */  0xA9, 0x00,             /* LDA #$00                 $802C */
  0x8D, 0x03, 0x20,       /* STA $2003  (oam_addr=0)  $802E */
  0xA9, 0x0D,             /* LDA #$0D  (y=12->13)     $8031 */
  0x8D, 0x04, 0x20,       /* STA $2004                $8033 */
  0xA9, 0x03,             /* LDA #$03  (tile 3)       $8036 */
  0x8D, 0x04, 0x20,       /* STA $2004                $8038 */
  0xA9, 0x00,             /* LDA #$00  (attr)         $803B */
  0x8D, 0x04, 0x20,       /* STA $2004                $803D */
  0xA9, 0x0C,             /* LDA #$0C  (x=12)         $8040 */
  0x8D, 0x04, 0x20,       /* STA $2004                $8042 */
  /* main: */
  0xA9, 0x01,             /* LDA #$01                 $8045 */
  0x8D, 0x16, 0x40,       /* STA $4016  (strobe hi)   $8047 */
  0xA9, 0x00,             /* LDA #$00                 $804A */
  0x8D, 0x16, 0x40,       /* STA $4016  (strobe lo)   $804C */
  0xAD, 0x16, 0x40,       /* LDA $4016  (bit0 = A)    $804F */
  0x29, 0x01,             /* AND #$01                 $8052 */
  0xD0, 0x06,             /* BNE a_down               $8054 */
  0x20, 0x61, 0x80,       /* JSR band_dark            $8055 */
  0x4C, 0x8B, 0x80,       /* JMP blink                $8058 (was JMP main) */
                          /* a_down: */
  0x20, 0x76, 0x80,       /* JSR band_bright          $805B */
  0x4C, 0x8B, 0x80,       /* JMP blink                $805E (was JMP main) */
  /* band_dark ($8061): write 32 cells of $01 at PPUADDR=$2000 */
  0xA9, 0x20, 0x8D, 0x06, 0x20,
  0xA9, 0x00, 0x8D, 0x06, 0x20,
  0xA2, 0x20, 0xA9, 0x01,
                          /* bd: */
  0x8D, 0x07, 0x20, 0xCA, 0xD0, 0xFA, 0x60,
  /* band_bright ($8076): write 32 cells of $00 at PPUADDR=$2000 */
  0xA9, 0x20, 0x8D, 0x06, 0x20,
  0xA9, 0x00, 0x8D, 0x06, 0x20,
  0xA2, 0x20, 0xA9, 0x00,
                          /* bb: */
  0x8D, 0x07, 0x20, 0xCA, 0xD0, 0xFA, 0x60,
  /* blink ($808B): toggle nametable cell $2000+0x34 between tile $01 and $00,
     then jump back to main.  Called every loop iteration (self-animation). */
  0xA9, 0x20,             /* LDA #$20  (PPUADDR hi)   $808B */
  0x8D, 0x06, 0x20,       /* STA $2006                $808D */
  0xA9, 0x34,             /* LDA #$34  (0x2000+0x34)  $8090 */
  0x8D, 0x06, 0x20,       /* STA $2006                $8092 */
  0xA5, 0x01,             /* LDA $01   (blink ctx)    $8095 */
  0x49, 0x01,             /* EOR #$01                 $8097 */
  0x85, 0x01,             /* STA $01                  $8099 */
  0x8D, 0x07, 0x20,       /* STA $2007  (write cell)  $809B */
  0x4C, 0x44, 0x80        /* JMP main                 $809E */
};

#endif
