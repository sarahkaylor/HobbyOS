# GB-033 — M2 gate: static-musl SDL app T4 differential (2026-10-27)

Gate run (local; no CI claimed). Fresh branch `ticket/33-sdl-m2-gate` off
origin/main, worktree hermes-e046ff70.

## T4 differential (Linux host vs the static-PIE image the loader maps)

The gate's T4 half: the SAME `tools/sdl/sdl_test_app.c` translation unit
(a) compiles to the static-PIE ELF the GB-013 loader maps, and (b) links into
the host golden suite. Verdict from real executions:

```
$ make sdl_test_app_test_host        # builds obj/sdl_test_app_pie + host binary
$ file obj/sdl_test_app_pie
  ELF 64-bit LSB pie executable, x86-64, static-pie linked
$ readelf -h obj/sdl_test_app_pie | grep -E "Type|Class|Machine"
  Type:   DYN (Position-Independent Executable file)   # loader-accepted class
  Class:  ELF64   Machine: Advanced Micro Devices X86-64
$ ./sdl_test_app_test_host
  === GB-032 SDL test app: T1 surface logic + T4 ELF differential ===
  ... surface geometry / clear / blit-out-&in-bounds clip / echo KEY_ABS / ...
  ... ELF: static-PIE needs no low window / plan mappable / ...
  sdl_test_app_test_host: 20 checks, 0 failed
```
20/20 PASS. Wired into `host_tests` (`$(SDL_TEST_APP_HOST)` at Makefile:3706).

## Honest scope — the in-guest half is GB-032's (recorded dependency, not a taken failure)

Per the coordinator's explicit split on this card: the host/port-side surface,
blit and input-echo artifact is real (leg A PASS above); the M2 **in-guest**
"shows a window and responds to input" leg is GB-032's (#32, `[DEEP]`, decision)
— the gating fix is the guest-side evdev/virtio path, a human touchpoint I
cannot promote. So GB-033 closes on the T4 differential + this recorded
dependency, per the DoD's "T4 differential" tier. Evidence before close is law.

## Files
- this report (evidence/GB-033/VERDICT.md)
