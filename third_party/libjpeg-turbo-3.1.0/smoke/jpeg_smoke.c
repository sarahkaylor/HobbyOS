/*
 * libjpeg-turbo 3.1.0 host smoke test (L6 lane browser/l6-libs1).
 *
 * Encodes a generated 128x128 grayscale gradient (quality 95) through the
 * libjpeg API into memory (jpeg_mem_dest), decodes it back (jpeg_mem_src),
 * and compares against the source.  JPEG is lossy, so "compare" is a
 * tolerance check (max/mean absolute error) on a smooth ramp — it guards
 * against codec breakage, not codec quality.  Prints the decoded-pixel
 * fingerprint + stats so repeated runs are comparable (checksum-stability
 * gate).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define W 128
#define H 128
#define MAX_TOLERATED 16 /* per-pixel abs error, smooth ramp @ q95 */

/* LIBJPEG_TURBO_VERSION in the generated jconfig.h is an unquoted token
 * (3.1.0), so it must be stringified for printing. */
#define JT_STR2(x) #x
#define JT_STR(x) JT_STR2(x)
#ifdef LIBJPEG_TURBO_VERSION
#define JT_VERSION_STR JT_STR(LIBJPEG_TURBO_VERSION)
#else
#define JT_VERSION_STR "unknown"
#endif

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

int main(void)
{
  unsigned char *img, *dec;
  unsigned char *enc = NULL;
  unsigned long enc_len = 0;
  struct jpeg_compress_struct cinfo;
  struct jpeg_decompress_struct dinfo;
  struct jpeg_error_mgr jerr, derr;
  int maxd = 0;
  double sum = 0.0;
  size_t i;
  int x, y, simd;

  img = malloc((size_t)W * H);
  dec = malloc((size_t)W * H);
  if (!img || !dec) {
    return 2;
  }
  for (y = 0; y < H; y++) {
    for (x = 0; x < W; x++) {
      img[(size_t)y * W + x] = (unsigned char)((x * 2 + y) & 0xff);
    }
  }

  /* --- encode --- */
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &enc, &enc_len);
  cinfo.image_width = W;
  cinfo.image_height = H;
  cinfo.input_components = 1;
  cinfo.in_color_space = JCS_GRAYSCALE;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 95, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = &img[(size_t)cinfo.next_scanline * W];
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  if (!enc || enc_len == 0) {
    printf("jpeg_smoke: FAIL empty encode output\n");
    return 1;
  }

  /* --- decode --- */
  dinfo.err = jpeg_std_error(&derr);
  jpeg_create_decompress(&dinfo);
  jpeg_mem_src(&dinfo, enc, enc_len);
  jpeg_read_header(&dinfo, TRUE);
  dinfo.out_color_space = JCS_GRAYSCALE;
  jpeg_start_decompress(&dinfo);
  if (dinfo.output_width != W || dinfo.output_height != H ||
      dinfo.output_components != 1) {
    printf("jpeg_smoke: FAIL unexpected output %ux%u comps=%d\n",
           dinfo.output_width, dinfo.output_height, dinfo.output_components);
    return 1;
  }
  while (dinfo.output_scanline < dinfo.output_height) {
    JSAMPROW row = &dec[(size_t)dinfo.output_scanline * W];
    jpeg_read_scanlines(&dinfo, &row, 1);
  }
  jpeg_finish_decompress(&dinfo);
  jpeg_destroy_decompress(&dinfo);

  /* --- compare --- */
  for (i = 0; i < (size_t)W * H; i++) {
    int d = (int)img[i] - (int)dec[i];
    if (d < 0) {
      d = -d;
    }
    if (d > maxd) {
      maxd = d;
    }
    sum += d;
  }
  if (maxd > MAX_TOLERATED) {
    printf("jpeg_smoke: FAIL maxdiff=%d exceeds %d\n", maxd, MAX_TOLERATED);
    return 1;
  }

#ifdef WITH_SIMD
  simd = 1;
#else
  simd = 0;
#endif
  printf("jpeg_smoke: OK libjpeg_turbo=%s abi=%d simd=%d q=95 %dx%d gray "
         "maxdiff=%d meandiff=%.3f encoded=%lu fnv=%016llx\n",
         JT_VERSION_STR, JPEG_LIB_VERSION, simd, W, H, maxd,
         sum / ((double)W * H), enc_len,
         (unsigned long long)fnv1a(dec, (size_t)W * H));

  free(enc);
  free(img);
  free(dec);
  return 0;
}
