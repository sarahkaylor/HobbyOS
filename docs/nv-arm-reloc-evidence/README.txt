Folded-pointer-table gate evidence (docs/nv-arm-reloc.md).

Host: this VM (nv-hermes), worktree .worktrees/t_4caf48a4 on branch nv/wp-reloc-guard,
clang + ld.lld cross build, QEMU 10.2.1.  Short excerpts: each file is the gate's
own output (or the tier summary) with a header naming the command and the tree
state; the full build logs stayed in /tmp on the build host.

gate-red-new-table.txt    RED 1: the pre-round-3 static `nv_rm_if` table put back
                          (reverted): make exit 2, NEW nv_rm_mock_static_probe,
                          while the relocation-coverage check stays green (433/433)
gate-red-grew-table.txt   RED 2: g_reloc_names[] gains one name (3 -> 4 words):
                          make exit 2, GREW g_reloc_names 3 -> 4
gate-green-unit.txt       both reverted: make exit 0, 20 tables / 412 words,
                          417/417 words applied, inventory matches
gate-green-desktop.txt    MODE=desktop: exit 0, 18 tables / 406 words / 411 applied
gate-green-test.txt       MODE=test:    exit 0, 18 tables / 406 words / 411 applied
gate-union-mode.txt       --mode soak (no section of its own): union path, exit 0
inventory-by-symbol-unit.txt  `--by-symbol` on the unit image: the audit list's
                          mechanical source (docs/nv-arm-reloc.md §4)
tier-arm-unit.txt         ARM unit tier, gate active: 472 run / 0 failed
tier-intel-unit.txt       intel unit tier: 589 run / 0 failed, no gate on the
                          intel link (unchanged from the documented baseline)
tier-intel-kvm-denied.txt why the intel run above is TCG: /dev/kvm is root:kvm and
                          this user is not in the kvm group
