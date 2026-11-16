# LibreOffice Suite for HobbyOS

This directory contains the port of the LibreOffice application suite to HobbyOS. Every default `make run` build compiles and stages all seven applications onto `disk.img` alongside IceWM's **Office** menu, toolbar shortcut, and XPM brand icons.

## Architecture & Module Layout

The LibreOffice port is organized into the following modular components:

- **`include/`**: Core shared headers defining:
  - OpenDocument Format (ODF) container definitions (`lo_zip.h`, `lo_xml.h`)
  - Document models and text/cell/shape structures (`lo_writer.h`, `lo_calc.h`, `lo_impress.h`, `lo_draw.h`, `lo_math.h`, `lo_base.h`, `lo_soffice.h`, `soffice.h`)
  - Virtual Communication Layer (VCL) display abstraction (`lo_vcl.h`) supporting native X11 windows (via `libX11.a`) and offscreen headless surfaces (`--headless`)
  - GTK3 / Adwaita window chrome, menu bars, toolbars, modals, and clipboard (`lo_ui.h`)
- **`core/`**: Core foundation engines:
  - ODF ZIP container packaging and extraction (`lo_zip.c`, using `libz.a`)
  - XML parsing and serialization (`lo_xml.c`, using `libexpat.a`)
  - VCL surface rasterization, anti-aliased FreeType2 typography (`DejaVu Sans`/`Serif`/`Mono` from `/X11/FONTS/`), and event dispatching (`lo_vcl.c`)
  - Shared GTK3 / Colibre-style UI chrome (`lo_ui.c`): menu bar, 2-row Standard + Formatting icon toolbars, modal dialogs (`About`, `Open`, `Save As`, `Print`, `Find`, `Info`), shared clipboard, shortcuts, and responsive zoom bar
- **`writer/`**: StarWriter text processor (`SWRITER.BIN`):
  - Document model: paragraphs, headings, styles, inline formatting, tables, image blocks, word count, 16-deep snapshot Undo/Redo
  - File formats: `.odt` (OpenDocument Text) read/write, `.txt` and HTML export filters
- **`calc/`**: StarCalc spreadsheet application (`SCALC.BIN`):
  - Document model: multi-sheet 2D cell grid, sheet tabs/switching, sorting, numbers, text, cell formatting, 16-deep Undo/Redo
  - Formula engine: RPN compiler and evaluator supporting `SUM`, `AVERAGE`, `MIN`, `MAX`, `COUNT`, `IF`, arithmetic
  - File formats: `.ods` (OpenDocument Spreadsheet) read/write, CSV import/export
- **`impress/`**: StarImpress presentation application (`SIMPRESS.BIN`):
  - Document model: ordered slide deck, layout frames, styled text boxes, geometric shapes, slide formatting, 16-deep Undo/Redo
  - Slideshow engine & export: interactive slideshow playback, `.odp` read/write, and multi-slide PDF-1.4 export
- **`draw/`**: StarDraw vector graphics application (`SDRAW.BIN`):
  - Document model: multi-page 2D vector canvas, shapes (lines, rectangles, ellipses, bezier curves, connectors, text boxes), Z-order, formatting, 16-deep Undo/Redo
  - Export & render: offscreen rasterization, `.odg` read/write, and SVG export
- **`math/`**: StarMath formula editor (`SMATH.BIN`):
  - Syntax parser: TeX-like equation syntax parser and AST generator with 5-category Unicode `Elements` dock
  - Typography engine: 2D mathematical layout, `.odf` read/write, and MathML serializer
- **`base/`**: StarBase relational database management application (`SBASE.BIN`):
  - Database engine: embedded SQLite 3 engine (`sqlite3.c`)
  - Functionality: Tables/Queries/Forms/Reports views, DDL/CRUD, SQL query execution, `.odb` read/write, and PDF-1.4 tabular report export
- **`soffice/`**: Unified CLI dispatcher and GUI Start Center (`SOFFICE.BIN`):
  - Unified command-line interface (`--writer`, `--calc`, `--impress`, `--draw`, `--math`, `--base`, `--headless`, `--convert-to`)
  - Interactive Start Center with file open, recent docs, templates, and launcher cards
- **`test/`**: In-guest automated test runner (`LO_TEST.BIN`):
  - Automated E2E test harness (`112` tests across Core Foundation, Tiers 1–4, Action Validation & Zero-Dead-Items, and Tier 5 Live X11 GUI E2E across all 7 apps)

## Building & Testing

- Default desktop build & run (automatically compiles and stages all 7 `.BIN` apps onto `disk.img`):
  ```bash
  make run
  ```
- Direct LibreOffice app or test builds (works for both `ARCH=arm` and `ARCH=intel`):
  ```bash
  make ARCH=arm lo_apps lo_test
  make ARCH=intel lo_apps lo_test
  ```
- Automated in-guest E2E suite (`< 30s`):
  ```bash
  make libreoffice_test
  ```
