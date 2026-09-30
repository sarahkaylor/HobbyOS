/*
 * ft_memface_smoke.c — L5 host smoke: FreeType memory-face loader + glyph
 * renderer with deterministic checksums (browser.md §6/§7.5, lane L5).
 *
 * Exercises the exact FreeType usage the HobbyOS font backend will need: the
 * face is created with FT_New_Memory_Face() over a caller-owned buffer —
 * FreeType never opens a filesystem path (no FT_New_Face, no FT_Open_Face
 * with a file name anywhere in this program).  Reading the font file into
 * the buffer is done by this host harness only; on HobbyOS the same buffer
 * will be filled by the font backend / disk layer.
 *
 * For each of 'H', 'g', '8' at 12/16/24/32 pixel sizes it renders with the
 * default hinted grayscale target and prints the glyph metrics plus an
 * FNV-1a 64 checksum of the rendered bitmap.  Raw bitmap bytes are written
 * to a dump file in a fixed order (size-major, glyph-minor) so an external
 * sha256 comparison across runs is possible too.  Output is byte-for-byte
 * reproducible for a given font file and FreeType build; the smoke gate
 * (host_build_fonts.sh) runs the binary twice and diffs the two runs.
 *
 * Usage: ft_memface_smoke [font.ttf] [bitmap_dump.bin]
 *   defaults: third_party/fonts/dejavu-2.37/ttf/DejaVuSans.ttf
 *             ./ft_bitmap_dump.bin
 *
 * Host build (exact command): see host_build_fonts.sh / README.md.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t fnv1a(uint64_t h, const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= FNV_PRIME;
  }
  return h;
}

/* Fold a 64-bit value big-endian so hashes are host-endian independent. */
static uint64_t fnv1a_u64(uint64_t h, uint64_t v) {
  unsigned char be[8];
  for (int i = 0; i < 8; i++)
    be[i] = (unsigned char)(v >> (56 - 8 * i));
  return fnv1a(h, be, sizeof(be));
}

static unsigned char *read_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  unsigned char *buf = malloc((size_t)len);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_size = (size_t)len;
  return buf;
}

int main(int argc, char **argv) {
  const char *font_path = argc > 1 ? argv[1]
                                   : "third_party/fonts/dejavu-2.37/ttf/DejaVuSans.ttf";
  const char *dump_path = argc > 2 ? argv[2] : "ft_bitmap_dump.bin";
  static const int sizes[] = {12, 16, 24, 32};
  static const char glyphs[] = {'H', 'g', '8'};
  const size_t n_sizes = sizeof(sizes) / sizeof(sizes[0]);
  const size_t n_glyphs = sizeof(glyphs) / sizeof(glyphs[0]);
  int rc = 1;

  size_t font_size = 0;
  unsigned char *font_data = read_file(font_path, &font_size);
  if (!font_data) {
    fprintf(stderr, "ft_memface_smoke: cannot read font file: %s\n", font_path);
    return 1;
  }

  FT_Library library = NULL;
  FT_Face face = NULL;
  FILE *dump = NULL;
  if (FT_Init_FreeType(&library) != 0) {
    fprintf(stderr, "ft_memface_smoke: FT_Init_FreeType failed\n");
    goto out_buf;
  }

  /* The memory face: FreeType reads font_data in place and never touches a
     filesystem path — the shape the HobbyOS font backend needs. */
  if (FT_New_Memory_Face(library, font_data, (FT_Long)font_size, 0, &face) != 0) {
    fprintf(stderr, "ft_memface_smoke: FT_New_Memory_Face failed\n");
    goto out_lib;
  }

  FT_Int major, minor, patch;
  FT_Library_Version(library, &major, &minor, &patch);
  printf("FT version=%d.%d.%d\n", (int)major, (int)minor, (int)patch);
  printf("FT face family=%s style=%s glyphs=%ld upem=%u\n",
         face->family_name ? face->family_name : "?",
         face->style_name ? face->style_name : "?",
         (long)face->num_glyphs, (unsigned)face->units_per_EM);
  printf("FT metrics ascender=%ld descender=%ld height=%ld max_advance_width=%ld\n",
         (long)face->ascender, (long)face->descender, (long)face->height,
         (long)face->max_advance_width);

  dump = fopen(dump_path, "wb");
  if (!dump) {
    fprintf(stderr, "ft_memface_smoke: cannot open dump file: %s\n", dump_path);
    goto out_face;
  }

  uint64_t total = FNV_OFFSET;
  size_t dumped = 0;
  int renders = 0;

  for (size_t s = 0; s < n_sizes; s++) {
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)sizes[s]) != 0) {
      fprintf(stderr, "ft_memface_smoke: FT_Set_Pixel_Sizes(%d) failed\n",
              sizes[s]);
      goto out_dump;
    }
    for (size_t g = 0; g < n_glyphs; g++) {
      FT_ULong ch = (FT_ULong)(unsigned char)glyphs[g];
      if (FT_Load_Char(face, ch, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
        fprintf(stderr, "ft_memface_smoke: FT_Load_Char(U+%04lX) failed\n", ch);
        goto out_dump;
      }
      FT_GlyphSlot slot = face->glyph;
      FT_Bitmap *bm = &slot->bitmap;
      if (bm->pitch < 0) {
        fprintf(stderr, "ft_memface_smoke: negative bitmap pitch not handled\n");
        goto out_dump;
      }
      size_t nbytes = (size_t)bm->rows * (size_t)bm->pitch;
      uint64_t h = fnv1a(FNV_OFFSET, bm->buffer, nbytes);

      printf("FT render size=%d ch=%c cp=U+%04lX gid=%u bitmap=%ux%u pitch=%d "
             "left=%d top=%d advance=%ld hash=%016llx\n",
             sizes[s], glyphs[g], ch, (unsigned)FT_Get_Char_Index(face, ch),
             (unsigned)bm->width, (unsigned)bm->rows, (int)bm->pitch,
             (int)slot->bitmap_left, (int)slot->bitmap_top,
             (long)slot->advance.x, (unsigned long long)h);

      if (fwrite(bm->buffer, 1, nbytes, dump) != nbytes) {
        fprintf(stderr, "ft_memface_smoke: dump write failed\n");
        goto out_dump;
      }
      dumped += nbytes;

      total = fnv1a_u64(total, (uint64_t)(unsigned)sizes[s]);
      total = fnv1a_u64(total, (uint64_t)ch);
      total = fnv1a_u64(total, (uint64_t)FT_Get_Char_Index(face, ch));
      total = fnv1a_u64(total, (uint64_t)(long)slot->advance.x);
      total = fnv1a_u64(total, (uint64_t)(long)slot->advance.y);
      total = fnv1a_u64(total, (uint64_t)(long)slot->bitmap_left);
      total = fnv1a_u64(total, (uint64_t)(long)slot->bitmap_top);
      total = fnv1a_u64(total, (uint64_t)(unsigned)bm->width);
      total = fnv1a_u64(total, (uint64_t)(unsigned)bm->rows);
      total = fnv1a(total, bm->buffer, nbytes);
      renders++;
    }
  }

  printf("FT-TOTAL renders=%d dump_bytes=%zu dump_file=%s hash=%016llx\n",
         renders, dumped, dump_path, (unsigned long long)total);
  rc = (renders == (int)(n_sizes * n_glyphs)) ? 0 : 1;

out_dump:
  fclose(dump);
out_face:
  FT_Done_Face(face);
out_lib:
  FT_Done_FreeType(library);
out_buf:
  free(font_data);
  return rc;
}
