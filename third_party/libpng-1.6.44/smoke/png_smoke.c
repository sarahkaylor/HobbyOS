/*
 * libpng 1.6.44 host smoke test (L6 lane browser/l6-libs1).
 *
 * Writes a generated 64x64 RGBA gradient PNG, decodes it back, re-encodes the
 * decoded pixels, decodes again, and requires byte-exact pixel equality at
 * every step (PNG is lossless).  Prints FNV-1a fingerprints of the decoded
 * pixels so repeated runs are comparable (checksum-stability gate).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>

#define W 64
#define H 64

static uint64_t fnv1a(const unsigned char *p, size_t n)
{
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static int write_png(const char *path, const unsigned char *px)
{
  FILE *f;
  png_structp png;
  png_infop info;
  int y;

  f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "write_png: cannot open %s for write\n", path);
    return 1;
  }
  png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) {
    fclose(f);
    return 1;
  }
  info = png_create_info_struct(png);
  if (!info) {
    png_destroy_write_struct(&png, NULL);
    fclose(f);
    return 1;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return 1;
  }
  png_init_io(png, f);
  png_set_IHDR(png, info, W, H, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  for (y = 0; y < H; y++) {
    png_write_row(png, (png_bytep)(px + (size_t)y * W * 4));
  }
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(f);
  return 0;
}

static int read_png(const char *path, unsigned char *px)
{
  FILE *f;
  png_structp png;
  png_infop info;
  png_byte sig[8];
  png_bytep rows[H];
  int y;

  f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "read_png: cannot open %s for read\n", path);
    return 1;
  }
  if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
    fprintf(stderr, "read_png: bad PNG signature in %s\n", path);
    fclose(f);
    return 1;
  }
  png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) {
    fclose(f);
    return 1;
  }
  info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(f);
    return 1;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 1;
  }
  png_init_io(png, f);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);
  if (png_get_image_width(png, info) != W ||
      png_get_image_height(png, info) != H ||
      png_get_bit_depth(png, info) != 8 ||
      png_get_color_type(png, info) != PNG_COLOR_TYPE_RGBA ||
      png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
    fprintf(stderr, "read_png: unexpected IHDR in %s\n", path);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 1;
  }
  for (y = 0; y < H; y++) {
    rows[y] = (png_bytep)(px + (size_t)y * W * 4);
  }
  png_read_image(png, rows);
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(f);
  return 0;
}

int main(int argc, char **argv)
{
  const char *p1 = argc > 1 ? argv[1] : "png_smoke_1.png";
  const char *p2 = argc > 2 ? argv[2] : "png_smoke_2.png";
  size_t raw_size = (size_t)W * H * 4;
  unsigned char *a, *b, *c;
  int x, y;
  int rc = 0;

  a = malloc(raw_size);
  b = malloc(raw_size);
  c = malloc(raw_size);
  if (!a || !b || !c) {
    return 2;
  }
  for (y = 0; y < H; y++) {
    for (x = 0; x < W; x++) {
      size_t o = ((size_t)y * W + x) * 4;
      a[o + 0] = (unsigned char)(x * 4);
      a[o + 1] = (unsigned char)(y * 4);
      a[o + 2] = (unsigned char)((x * 7 + y * 3) & 0xff);
      a[o + 3] = (unsigned char)0xff;
    }
  }

  if (write_png(p1, a) != 0 || read_png(p1, b) != 0) {
    rc = 1;
  } else if (memcmp(a, b, raw_size) != 0) {
    printf("png_smoke: FAIL decode != source pixels\n");
    rc = 1;
  } else if (write_png(p2, b) != 0 || read_png(p2, c) != 0) {
    rc = 1;
  } else if (memcmp(b, c, raw_size) != 0) {
    printf("png_smoke: FAIL re-encode mismatch\n");
    rc = 1;
  }
  if (rc != 0) {
    return rc;
  }

  printf("png_smoke: OK version=%s %dx%d rgba8 decode_fnv=%016llx "
         "reencode_fnv=%016llx roundtrip=exact\n",
         PNG_LIBPNG_VER_STRING, W, H,
         (unsigned long long)fnv1a(b, raw_size),
         (unsigned long long)fnv1a(c, raw_size));
  return 0;
}
