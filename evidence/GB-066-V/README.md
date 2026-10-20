# GB-066-V — verification evidence (DEEP Verifier, 2026-10-20)

Subject: GB-066 (#74) "T4 device leg" — verdict **RED**.
Revision run: `0d381008f3c3626a5cd2d0a335a129ab3b13f78f` = `refs/heads/main` (`git ls-remote origin main`), clean worktree.

| # | file | what it is |
|---|---|---|
| 01 | `01-t4_device_selftest.log` | `bash tools/gate/t4_device.sh --self-test` → wrong-shim FAIL + absent-probe FAIL, `fail_count=0`, exit 0 |
| 02 | `02-make-t4_probe.log` | the ticket's literal command `make obj/t4_probe.elf` → **No rule to make target** |
| 02b | `02b-make-t4_probe_ARCH-intel.log` | `make ARCH=intel obj/intel/t4_probe.elf` → exit 0, static x86-64 musl ELF (the real target; `OBJ_DIR = obj/$(ARCH)`) |
| 03 | `03-legA_capture.log` | `make ARCH=intel obj/intel/t4_legA.out` → leg A on real Linux |
| 04 | `04-t3_intel_staged.log` | real staged intel wave: `T3_STAGE='mcopy … ::T4PROBE.BIN && mdir …'` |
| 05 | `05-t4_device_gate_real.log` | the device gate over that wave's serial → exit 1, "probe never arrived … FAIL, not a SKIP" |
| 06 | `06-t4_parity_device_intel.log` | `bash src/host/t4_parity.sh --device intel` → SKIP … and exit **0** |
| 07 | `07-wiring-and-wave-facts.txt` | `make -n host_tests` wiring + wave counters |

Environment: §11.5 staging rsynced (`libcxx-21.1.8`, `llvm-21.1.8/dist-staging`); `/usr/sbin:/sbin` on PATH for `mkfs.fat`.

## Findings

1. **Staging worked, the probe still never ran.** `mdir` on the built image shows `T4PROBE  BIN  19328` — the fixture is on the image — and the serial log contains **0** occurrences of `T4PROBE` and **0** of `t4-probe-arrived`. The probe is not in the boot wave's program list, so nothing launches it.
2. **The device gate fails honestly** (exit 1, "This is a FAIL, not a SKIP (DoD clause 3)") — clause 3 is satisfied; clause 1 (leg B runs in the guest, per-row diff) is not.
3. **The two instruments disagree, and the older one reads as PASS.** `src/host/t4_parity.sh --device intel` prints
   `t4 device: SKIP (device-leg runner TBD; host differential is the gate today)` and then
   `t4_parity: PASS (host differential locked; device leg gated)` with **exit 0**. A SKIP that exits 0 is
   exactly the failure mode this ticket was minted for.
4. `make obj/t4_probe.elf` (the ticket's command) does not exist; the real target is `$(OBJ_DIR)/t4_probe.elf`
   (`obj/intel/t4_probe.elf`). Ticket bug, not a code bug — recorded so the next run does not stop there.
5. Wiring: `make -n host_tests | grep t4_device` → `bash tools/gate/t4_device.sh --self-test` only. The
   real gate (`t4_device_gate`) is not in `host_tests` and not in the merge gate.
