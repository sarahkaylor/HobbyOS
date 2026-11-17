/*
 * src/user/libreoffice/test/lo_test.c
 *
 * Automated In-Guest E2E Test Suite for LibreOffice on HobbyOS.
 * Executes all 78 tests across 4 tiers:
 *   - Tier 1: Feature Coverage (30 tests: 5 tests x 6 applications)
 *   - Tier 2: Boundary & Corner Cases (30 tests: 5 tests x 6 applications)
 *   - Tier 3: Cross-Feature Combinations (12 tests)
 *   - Tier 4: Real-World Scenarios (6 scenarios)
 *
 * Compliant with ISO/IEC 26300 (ODF), GEMINI.md (<30s runtime),
 * PROJECT.md interface contracts, and TEST_INFRA.md specifications.
 *
 * Unified build unit integrating all modular test components:
 *   - lo_test_utils.c     (Assertion engine, ODF validator, semihosting poweroff)
 *   - lo_test_writer.c    (StarWriter Tier 1 & Tier 2 tests)
 *   - lo_test_calc.c      (StarCalc Tier 1 & Tier 2 tests)
 *   - lo_test_impress.c   (Impress Tier 1 & Tier 2 tests)
 *   - lo_test_draw.c      (Draw Tier 1 & Tier 2 tests)
 *   - lo_test_math.c      (Math Tier 1 & Tier 2 tests)
 *   - lo_test_base.c      (Base Tier 1 & Tier 2 tests)
 *   - lo_test_tiers.c     (Tier 3 cross-feature & Tier 4 real-world scenarios)
 *   - lo_test_main.c      (Orchestration entry point & summary reporter)
 */

#include "lo_test.h"

#include "lo_test_utils.c"
#include "lo_test_core.c"
#include "lo_test_writer.c"
#include "lo_test_calc.c"
#include "lo_test_impress.c"
#include "lo_test_draw.c"
#include "lo_test_math.c"
#include "lo_test_base.c"
#include "lo_test_tiers.c"
#include "lo_test_actions.c"
#include "lo_test_tier5.c"
#include "lo_test_main.c"
