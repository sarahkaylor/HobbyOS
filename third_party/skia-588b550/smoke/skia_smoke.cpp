// skia_smoke.cpp -- fixed-scene raster smoke for the L6 host Skia build.
//
// Draws a deterministic 320x240 scene through the public Skia API paths the
// WebKit 2.54 Skia port uses (raster canvas, AA geometry, linear gradient via
// SkShaders::LinearGradient, dashed stroke, multiply blend, canvas transform)
// and prints a checksum of the tightly-packed RGBA pixels.  Also dumps the
// same bytes to a file so the build script can record a standard
// `sha256sum` of the pixel buffer.
//
// Build: part of third_party/skia-588b550 via build.sh; links only against
// the vendored static Skia (no fonts, no GPU, no external state).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <skia/core/SkCanvas.h>
#include <skia/core/SkColor.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkPath.h>
#include <skia/core/SkPathBuilder.h>
#include <skia/core/SkPathEffect.h>
#include <skia/core/SkRRect.h>
#include <skia/core/SkShader.h>
#include <skia/core/SkSpan.h>
#include <skia/core/SkSurface.h>
#include <skia/effects/SkDashPathEffect.h>
#include <skia/effects/SkGradient.h>

static constexpr int kWidth = 320;
static constexpr int kHeight = 240;

static uint64_t fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static sk_sp<SkShader> makeGradient() {
  const SkColor4f colors[] = {
      {0.10f, 0.20f, 0.90f, 1.0f},
      {0.90f, 0.30f, 0.10f, 1.0f},
      {0.95f, 0.90f, 0.20f, 1.0f},
  };
  const float pos[] = {0.0f, 0.5f, 1.0f};
  const SkPoint pts[2] = {{130.0f, 16.0f}, {306.0f, 16.0f}};
  SkGradient grad(SkGradient::Colors(SkSpan<const SkColor4f>(colors, 3),
                                     SkSpan<const float>(pos, 3),
                                     SkTileMode::kClamp),
                  SkGradient::Interpolation{});
  return SkShaders::LinearGradient(pts, grad, nullptr);
}

static void drawScene(SkCanvas* canvas) {
  canvas->clear(SK_ColorWHITE);

  SkPaint paint;
  paint.setAntiAlias(true);

  // filled rect
  paint.setColor(SkColorSetARGB(255, 30, 120, 220));
  canvas->drawRect(SkRect::MakeXYWH(16.0f, 16.0f, 96.0f, 64.0f), paint);

  // filled circle
  paint.setColor(SkColorSetARGB(255, 40, 180, 90));
  canvas->drawCircle(232.0f, 56.0f, 36.0f, paint);

  // linear gradient rect (SkShaders::LinearGradient, the 2.54 API)
  paint.setShader(makeGradient());
  canvas->drawRect(SkRect::MakeLTRB(130.0f, 16.0f, 306.0f, 96.0f), paint);
  paint.setShader(nullptr);

  // stroked rounded rect
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(5.0f);
  paint.setColor(SkColorSetARGB(255, 120, 40, 200));
  canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(16.0f, 120.0f, 150.0f, 220.0f), 18.0f, 18.0f),
                    paint);

  // dashed cubic path (SkPath is immutable in this revision -- build with
  // SkPathBuilder, exactly as WebCore's GraphicsContextSkia does)
  paint.setStrokeWidth(4.0f);
  paint.setColor(SkColorSetARGB(255, 20, 20, 20));
  const SkScalar intervals[] = {12.0f, 7.0f, 4.0f, 7.0f};
  paint.setPathEffect(SkDashPathEffect::Make(SkSpan<const SkScalar>(intervals, 4), 0.0f));
  SkPathBuilder builder;
  builder.moveTo(170.0f, 150.0f);
  builder.cubicTo(200.0f, 110.0f, 260.0f, 210.0f, 304.0f, 150.0f);
  const SkPath path = builder.detach();
  canvas->drawPath(path, paint);
  paint.setPathEffect(nullptr);
  paint.setStyle(SkPaint::kFill_Style);

  // transform + multiply blend
  canvas->save();
  canvas->rotate(12.0f, 240.0f, 190.0f);
  paint.setColor(SkColorSetARGB(150, 220, 40, 40));
  paint.setBlendMode(SkBlendMode::kMultiply);
  canvas->drawRect(SkRect::MakeXYWH(200.0f, 160.0f, 90.0f, 60.0f), paint);
  paint.setBlendMode(SkBlendMode::kSrcOver);
  canvas->restore();
}

int main(int argc, char** argv) {
  const char* dumpPath = (argc > 1) ? argv[1] : "skia_smoke.rgba";

  auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kWidth, kHeight));
  if (!surface) {
    fprintf(stderr, "skia_smoke: SkSurfaces::Raster failed\n");
    return 2;
  }
  drawScene(surface->getCanvas());

  SkPixmap pm;
  if (!surface->peekPixels(&pm)) {
    fprintf(stderr, "skia_smoke: peekPixels failed\n");
    return 3;
  }

  // Tightly-packed RGBA copy (drops any rowBytes padding) -- the checksum input.
  std::vector<uint8_t> tight;
  tight.reserve(static_cast<size_t>(kWidth) * kHeight * 4);
  for (int y = 0; y < kHeight; ++y) {
    const uint8_t* row = static_cast<const uint8_t*>(pm.addr()) +
                         static_cast<size_t>(y) * pm.rowBytes();
    tight.insert(tight.end(), row, row + static_cast<size_t>(kWidth) * 4);
  }

  const uint64_t fnv = fnv1a64(tight.data(), tight.size());
  printf("skia_smoke: surface=%dx%d rowBytes=%zu\n", kWidth, kHeight,
         static_cast<size_t>(pm.rowBytes()));
  printf("skia_smoke: fnv1a64=0x%016llx bytes=%zu\n", static_cast<unsigned long long>(fnv),
         tight.size());

  if (FILE* f = fopen(dumpPath, "wb")) {
    fwrite(tight.data(), 1, tight.size(), f);
    fclose(f);
    printf("skia_smoke: wrote %s\n", dumpPath);
  } else {
    fprintf(stderr, "skia_smoke: cannot write %s\n", dumpPath);
    return 4;
  }

  // Human-viewable artifact (P6 PPM).
  char ppmPath[4096];
  snprintf(ppmPath, sizeof(ppmPath), "%s.ppm", dumpPath);
  if (FILE* f = fopen(ppmPath, "wb")) {
    fprintf(f, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const SkColor c = pm.getColor(x, y);
        fputc(SkColorGetR(c), f);
        fputc(SkColorGetG(c), f);
        fputc(SkColorGetB(c), f);
      }
    }
    fclose(f);
    printf("skia_smoke: wrote %s\n", ppmPath);
  }
  return 0;
}
