/*
 * hb_shape_smoke.c — L5 host smoke: HarfBuzz shaping with deterministic
 * glyph-id / position checksums (browser.md §6/§7.5, lane L5).
 *
 * Shapes two fixed ASCII strings through HarfBuzz in two configurations:
 *   HB-OT — hb_ot font funcs, scale = font upem (design units);
 *   HB-FT — hb_ft font funcs over a FreeType memory face at a 16 px em
 *           (advances in 1/64 px); this FreeType + HarfBuzz pairing is the
 *           integration the HobbyOS / WebKit font backend will reuse.
 * For each shape it prints every glyph id / cluster / advance / offset and
 * records FNV-1a 64 checksums over the glyph-id array and over the position
 * array (byte-for-byte reproducible for a given HarfBuzz + FreeType + font).
 * A cross-check asserts both configurations produce identical glyph ids.
 * hb_icu_get_unicode_funcs() is called to exercise the harfbuzz-icu build —
 * the ICU component WebKit's FindHarfBuzz asks for.  Output is compared
 * byte-for-byte across two runs by the smoke gate (host_build_fonts.sh).
 *
 * Usage: hb_shape_smoke [font.ttf]
 *   default font: third_party/fonts/dejavu-2.37/ttf/DejaVuSans.ttf
 *
 * Host build/link (exact command): see host_build_fonts.sh / README.md.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hb.h>
#include <hb-ft.h>
#include <hb-icu.h>
#include <hb-ot.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
#define MAX_GLYPHS 64

static uint64_t fnv1a(uint64_t h, const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= FNV_PRIME;
  }
  return h;
}

/* Fold 32/64-bit values big-endian so hashes are host-endian independent. */
static uint64_t fnv1a_u32(uint64_t h, uint32_t v) {
  unsigned char be[4];
  for (int i = 0; i < 4; i++)
    be[i] = (unsigned char)(v >> (24 - 8 * i));
  return fnv1a(h, be, sizeof(be));
}

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

static const char *dir_name(hb_direction_t d) {
  switch (d) {
  case HB_DIRECTION_LTR:
    return "ltr";
  case HB_DIRECTION_RTL:
    return "rtl";
  case HB_DIRECTION_TTB:
    return "ttb";
  case HB_DIRECTION_BTT:
    return "btt";
  default:
    return "?";
  }
}

struct shape_result {
  uint64_t gids_hash;
  uint64_t pos_hash;
  unsigned int count;
  hb_codepoint_t gids[MAX_GLYPHS];
};

static int shape_string(const char *tag, hb_font_t *font, hb_buffer_t *buf,
                        const char *text, struct shape_result *res) {
  hb_buffer_clear_contents(buf);
  hb_buffer_add_utf8(buf, text, -1, 0, -1);
  hb_buffer_guess_segment_properties(buf);
  hb_shape(font, buf, NULL, 0);

  unsigned int count = 0;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &count);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &count);
  if (!info || !pos || count == 0 || count > MAX_GLYPHS)
    return -1;

  printf("%s text=\"%s\" glyphs=%u dir=%s script=%08x\n", tag, text, count,
         dir_name(hb_buffer_get_direction(buf)),
         (unsigned)hb_buffer_get_script(buf));

  uint64_t hg = FNV_OFFSET;
  uint64_t hp = FNV_OFFSET;
  for (unsigned int i = 0; i < count; i++) {
    printf("%s g=%u gid=%u cluster=%u adv=%d,%d off=%d,%d\n", tag, i,
           (unsigned)info[i].codepoint, (unsigned)info[i].cluster,
           (int)pos[i].x_advance, (int)pos[i].y_advance,
           (int)pos[i].x_offset, (int)pos[i].y_offset);
    hg = fnv1a_u32(hg, (uint32_t)info[i].codepoint);
    hp = fnv1a_u32(hp, (uint32_t)pos[i].x_advance);
    hp = fnv1a_u32(hp, (uint32_t)pos[i].y_advance);
    hp = fnv1a_u32(hp, (uint32_t)pos[i].x_offset);
    hp = fnv1a_u32(hp, (uint32_t)pos[i].y_offset);
  }
  printf("%s gids_hash=%016llx pos_hash=%016llx\n", tag,
         (unsigned long long)hg, (unsigned long long)hp);

  res->gids_hash = hg;
  res->pos_hash = hp;
  res->count = count;
  for (unsigned int i = 0; i < count; i++)
    res->gids[i] = info[i].codepoint;
  return 0;
}

int main(int argc, char **argv) {
  const char *font_path = argc > 1 ? argv[1]
                                   : "third_party/fonts/dejavu-2.37/ttf/DejaVuSans.ttf";
  static const char *const strings[] = {"Hello, HobbyOS!", "AVATAR To"};
  const size_t n_strings = sizeof(strings) / sizeof(strings[0]);
  int rc = 1;

  size_t font_size = 0;
  unsigned char *font_data = read_file(font_path, &font_size);
  if (!font_data) {
    fprintf(stderr, "hb_shape_smoke: cannot read font file: %s\n", font_path);
    return 1;
  }

  printf("HB version=%s\n", hb_version_string());

  /* hb-icu call — exercises the harfbuzz-icu library + ICU linkage. */
  hb_unicode_funcs_t *icu = hb_icu_get_unicode_funcs();
  if (!icu) {
    fprintf(stderr, "hb_shape_smoke: hb_icu_get_unicode_funcs returned NULL\n");
    goto out_buf;
  }
  printf("HB-ICU unicode_funcs=ok category(U+0048)=%d category(U+0067)=%d\n",
         (int)hb_unicode_general_category(icu, 0x0048),
         (int)hb_unicode_general_category(icu, 0x0067));

  hb_blob_t *blob = hb_blob_create((const char *)font_data,
                                   (unsigned)font_size,
                                   HB_MEMORY_MODE_READONLY, NULL, NULL);
  hb_face_t *face = hb_face_create(blob, 0);
  unsigned int upem = hb_face_get_upem(face);
  printf("HB face upem=%u glyphs=%u\n", upem,
         (unsigned)hb_face_get_glyph_count(face));

  /* Path A: design units via hb-ot funcs. */
  hb_font_t *font_ot = hb_font_create(face);
  hb_ot_font_set_funcs(font_ot);
  hb_font_set_scale(font_ot, (int)upem, (int)upem);

  /* Path B: hb-ft funcs over a FreeType memory face, 16 px em. */
  FT_Library library = NULL;
  FT_Face ft_face = NULL;
  hb_font_t *font_ft = NULL;
  if (FT_Init_FreeType(&library) != 0 ||
      FT_New_Memory_Face(library, font_data, (FT_Long)font_size, 0, &ft_face) != 0) {
    fprintf(stderr, "hb_shape_smoke: FreeType init / memory face failed\n");
    goto out_hb;
  }
  font_ft = hb_ft_font_create_referenced(ft_face);
  if (!font_ft) {
    fprintf(stderr, "hb_shape_smoke: hb_ft_font_create_referenced failed\n");
    goto out_ft;
  }
  hb_font_set_scale(font_ft, 16 * 64, 16 * 64);

  hb_buffer_t *buf_ot = hb_buffer_create();
  hb_buffer_t *buf_ft = hb_buffer_create();

  uint64_t tot_ot_g = FNV_OFFSET, tot_ot_p = FNV_OFFSET;
  uint64_t tot_ft_g = FNV_OFFSET, tot_ft_p = FNV_OFFSET;
  int ok = 1;

  for (size_t i = 0; i < n_strings; i++) {
    struct shape_result a, b;
    if (shape_string("HB-OT", font_ot, buf_ot, strings[i], &a) != 0 ||
        shape_string("HB-FT", font_ft, buf_ft, strings[i], &b) != 0) {
      fprintf(stderr, "hb_shape_smoke: shaping failed\n");
      ok = 0;
      break;
    }
    int same = a.count == b.count &&
               memcmp(a.gids, b.gids, a.count * sizeof(hb_codepoint_t)) == 0;
    printf("HB-MATCH text=\"%s\" gids_ot_eq_ft=%s\n", strings[i],
           same ? "yes" : "NO");
    if (!same)
      ok = 0;
    tot_ot_g = fnv1a_u64(tot_ot_g, a.gids_hash);
    tot_ot_p = fnv1a_u64(tot_ot_p, a.pos_hash);
    tot_ft_g = fnv1a_u64(tot_ft_g, b.gids_hash);
    tot_ft_p = fnv1a_u64(tot_ft_p, b.pos_hash);
  }

  printf("HB-TOTAL strings=%zu ot_gids=%016llx ot_pos=%016llx "
         "ft_gids=%016llx ft_pos=%016llx\n",
         n_strings, (unsigned long long)tot_ot_g, (unsigned long long)tot_ot_p,
         (unsigned long long)tot_ft_g, (unsigned long long)tot_ft_p);
  if (ok)
    rc = 0;

  hb_buffer_destroy(buf_ft);
  hb_buffer_destroy(buf_ot);
  hb_font_destroy(font_ft);
out_ft:
  if (ft_face)
    FT_Done_Face(ft_face);
  if (library)
    FT_Done_FreeType(library);
out_hb:
  hb_font_destroy(font_ot);
  hb_face_destroy(face);
  hb_blob_destroy(blob);
out_buf:
  free(font_data);
  return rc;
}
