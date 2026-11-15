# cp/sys gate evidence — strix, 2026-11-15

Host: strix (AMD RYZEN AI MAX+ 395, Zen 5, AVX-512, KVM, 7.0.0-43-generic).
Lane tree `~/cp/lanes/sys` (cp-sys), base tree `~/cp/base` (cyberpunk @ 6e73e34e).
Every QEMU boot ran under `flock /tmp/cp-qemu.lock`.  Extracts (filtered from
the raw consoles, which stay on strix under `obj/`):

- `gates-summary.txt` — one line per probe gate (lane + base) and unit run
- `probe-rows.txt` — the `[LP]` rows of every probe, lane then base
- `join-repeat.txt` — three more join runs per tree (flake check)
- `unit-tests.txt` — the failing assertions of `run_unit_tests_intel.sh`, lane and base
- `wine-smoke-ladder.txt` — the Wine smoke's serial ladder (final run)
- `wine-build-and-host.txt` — `build-wine.sh all` summary lines + host identity

Commands: docs/cp-sys.md §9.
