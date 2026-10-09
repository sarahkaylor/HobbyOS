# P3c host stress — A/B evidence (PRE-fix vs POST-fix WK5Alloc)

Pattern reproduced on host: run-802's failing shape — multi-MB large allocs
(analogous to the 6.7 MB CNN body) + 16 B parser-style churn + realloc growth
(storms doubling 64 B -> 4 MiB) + full free storms, all interleaved, while
p3_host_sbrk is armed to fail spuriously (every 7th call) modeling the OS
sys_brk covered-check race at pool-thread spawn.

## PRE-fix build (wk5alloc from perf-w2, same source as run-802 binary)
> ./alloc_stress_before
phase6 (fetch-scale failing pattern) begin
... 11984 "phase6: large alloc NULL" / "phase6: realloc grow NULL" lines ...
phase6 (fetch-scale failing pattern) done
phase7 (sbrk hard-fail fallback) done
RESULT: FAIL
=> exactly the run-802 NULL chain (fetchArena -> sbrk=-1 -> NULL -> fastMalloc BCRASH),
   surfaced whenever any sbrk call fails. Full dump: host-stress-before.log (huge).

## POST-fix build (this lane's WK5Alloc: bounded sbrk retry + mmap fallback)
> ./alloc_stress_fixed
phase1 (fixed sizes) done
phase2 (random churn) done, live=350
phase3 (calloc/aligned) done
bench N=2000 avg=20256: mine=4930.4us naive=9060.1us speedup=1.8x
bench P=50000 64B: mine=728.6us naive=31838.6us speedup=43.7x
phase6 (fetch-scale failing pattern) begin
phase6 (fetch-scale failing pattern) done
phase7 (sbrk hard-fail fallback) done
RESULT: PASS  (exit 0)

No NULLs anywhere in phase6 despite spurious sbrk failures (fallback+retry).
Phase7 proves the mmap arena fallback serves even when sbrk fails EVERY call.
64 B churn win retained: 43.7x (vs the pre-fix 41-44x baseline; FINDINGS 46x).

Files: alloc_stress.c (extended), alloc_stress_fixed / alloc_stress_before (binaries),
      wk5alloc_fixed.o / wk5alloc_before.o, host-stress-fixed.log / host-stress-before.log
