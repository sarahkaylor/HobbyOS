#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>

/*
 * icons.h - 8x8 icon glyphs for HobbyOS text-mode windows.
 *
 * Window text is rendered through graphics_draw_glyph(), which maps every
 * byte to an 8x8 bitmap cell. Byte values 0x80..(0x80+ICON_COUNT-1) are
 * mapped here to small monochrome icons; all other bytes keep using the
 * ASCII font. An application draws an icon by printing the raw byte as a
 * normal character (it occupies one 8px text cell, exactly like a letter):
 *
 *     char row[2]; row[0] = (char)ICON_FOLDER; row[1] = '\0';
 *     print(row);            // a folder icon where the glyph lands
 *
 * The table is `static` (one copy per including TU) following the same
 * convention as font8x8 in font.h.
 *
 * Icon reference ('.' = transparent, '#' = ink):
 *
 *   FOLDER        FILE          TEXT          PROGRAM       NET
 *   ........      ........      ........      ........      ........
 *   ###.....      .######.      .######.      .######.      ..####..
 *   #######.      .#....#.      .#....#.      .#..##.#.     .##..##.
 *   #######.      .#....#.      .#.####.      .#.####.      ########
 *   #######.      .#....#.      .#....#.      .#.####.      .##..##.
 *   #######.      .#....#.      .#.####.      .#..##.#.     ..####..
 *   #######.      .#....#.      .#....#.      .######.      ........
 *   ........      .######.      .######.      ........      ........
 *
 *   DISK          UP            IMAGE         ARCHIVE       WARN
 *   ........      ........      ........      ........      ........
 *   .######.      ...#....      .######.      .######.      ...#....
 *   .#....#.      ..###...      .#....#.      .#....#.      ..###...
 *   .######.      .#####..      .#..##.#.     .#.##.#.      .#.#.#..
 *   ........      #######.      .#....#.      .#.##.#.      .#.#.#..
 *   .######.      ..###...      .#..##.#.     .#.##.#.      .#...#..
 *   .#.##.#.      ..###...      .#.####.      .#....#.      .#####..
 *   .######.      ........      .######.      .######.      ........
 */

#define ICON_BASE   0x80            /* first icon byte value            */
#define ICON_FOLDER 0x80            /* directory                        */
#define ICON_FILE   0x81            /* regular file, unknown type       */
#define ICON_TEXT   0x82            /* text document (.TXT/.LOG/...)    */
#define ICON_PROG   0x83            /* runnable program (.BIN)          */
#define ICON_NET    0x84            /* network filesystem (NFS mount)   */
#define ICON_DISK   0x85            /* local volume / hard disk         */
#define ICON_UP     0x86            /* parent directory ("..")          */
#define ICON_IMAGE  0x87            /* image file                       */
#define ICON_ARCH   0x88            /* archive file                     */
#define ICON_WARN   0x89            /* warning / error marker           */
#define ICON_COUNT  10

static const uint8_t icon8x8[ICON_COUNT][8] = {
  /* 0x80 ICON_FOLDER */
  {0x00, 0xE0, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0x00},
  /* 0x81 ICON_FILE */
  {0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7E},
  /* 0x82 ICON_TEXT */
  {0x00, 0x7E, 0x42, 0x3C, 0x42, 0x3C, 0x42, 0x7E},
  /* 0x83 ICON_PROG (page with play triangle) */
  {0x00, 0x7E, 0x72, 0x7A, 0x7E, 0x7A, 0x72, 0x7E},
  /* 0x84 ICON_NET (globe with equator) */
  {0x00, 0x3C, 0x66, 0xFF, 0x66, 0x3C, 0x00, 0x00},
  /* 0x85 ICON_DISK (stacked drive bars, bottom one has lights) */
  {0x00, 0x7E, 0x42, 0x7E, 0x00, 0x7E, 0x5A, 0x7E},
  /* 0x86 ICON_UP (up arrow) */
  {0x00, 0x10, 0x38, 0x7C, 0xFE, 0x38, 0x38, 0x00},
  /* 0x87 ICON_IMAGE (frame, sun, mountain) */
  {0x00, 0x7E, 0x42, 0x72, 0x42, 0x72, 0x7A, 0x7E},
  /* 0x88 ICON_ARCH (box with zipper) */
  {0x00, 0x7E, 0x42, 0x5A, 0x5A, 0x5A, 0x42, 0x7E},
  /* 0x89 ICON_WARN (triangle with bang) */
  {0x00, 0x10, 0x38, 0x54, 0x54, 0x44, 0x7C, 0x00},
};

#endif /* ICONS_H */
