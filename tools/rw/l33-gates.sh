#!/usr/bin/env bash
# L33 regression set: unit tier then the M2/M12/M3/M4 gates, in order.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
echo "=== unit ==="
TOOLS_SKIP=1 ./run_unit_tests_intel.sh 2>&1 | grep -E "Tests run|Tests failed"
echo "=== m2_mono ==="
TOOLS_SKIP=1 bash tools/gate/m2_mono.sh 2>&1 | tail -2
echo "=== m2_threads ==="
TOOLS_SKIP=1 bash tools/gate/m2_threads.sh 2>&1 | tail -2
echo "=== m12 ==="
TOOLS_SKIP=1 bash tools/gate/m12_thread_ceiling.sh 2>&1 | tail -2
echo "=== m3_window_probe ==="
TOOLS_SKIP=1 python3 tools/gate/m3_window_probe.py 2>&1 | tail -2
echo "=== m4_gl_probe ==="
TOOLS_SKIP=1 python3 tools/gate/m4_gl_probe.py 2>&1 | tail -2
echo "=== l33 gates done ==="
