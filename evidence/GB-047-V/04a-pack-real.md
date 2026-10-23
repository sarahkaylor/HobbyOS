# S0/S1 acceptance evidence pack (GB-047)
- generated: 2026-10-23T19:41:47Z
- status: this index + per-artifact hashes; the archive lands on the
  shared store (`/mnt/fast` NAS) at acceptance. On swarm-farm-3 no host
  currently has `/mnt/fast` mounted, so placement is the documented,
  gated sub-step.

## Native-port / harness evidence (S0)
| kind | path | sha16 |
|---|---|---|
| evidence rig | `tools/evidence_rig.py` | 20ca92c902e8d646 |
| perf cycle | `tools/perf_game_cycle.py` | caea9f359cdf2b44 |
| replays | `tools/t5/replays/milliped.jsonl` | 8363a869c0cc93c8 |
| replays | `tools/t5/replays/nes.jsonl` | 30f49847954e537b |
| replays | `tools/t5/replays/pong.jsonl` | 22a33a057c2d92f6 |
| replays | `tools/t5/replays/xnibbler.jsonl` | c606a727e34651a6 |
| replays | `tools/t5/replays/xcrypt.jsonl` | 798a7c8cc5e5806a |
| perf baseline | `perf-baseline-REPORT.md` | b26a2cbbd99937b7 |
| perf global | `perf-global-REPORT.md` | d5319c6940594216 |
| game evidence | `evidence/gb-079/README.txt` | 94adbf5c77793f5e |
| game evidence | `xnibbler-EVIDENCE-REPORT.md` | f36463a426a1e5bc |

## Committed reports (S0/S1 gate history)
- `S0-S1-ACCEPTANCE-MANIFEST.json` (sha16 66df7f1e99a72687)
- `S0-S1-ACCEPTANCE.md` (sha16 d65cb07843e4b6ce)
- `S0-S1-EVIDENCE-MANIFEST.json` (sha16 b74743a622d75db4)
- `S0-S1-EVIDENCE-PACK.md` (sha16 91f19b84936342e5)
- `THEME-DESKTOP-REPORT.md` (sha16 939e412db750ec92)
- `THEME-VERIFY-REPORT.md` (sha16 ace9a3fab5f9141e)
- `THEME-WINDOW-REPORT.md` (sha16 18d28379ca63190c)
- `doom-native-track-REPORT.md` (sha16 a650724feabd403a)
- `perf-baseline-REPORT.md` (sha16 b26a2cbbd99937b7)
- `perf-global-REPORT.md` (sha16 d5319c6940594216)
- `stardust-native-track-REPORT.md` (sha16 9b3e36f3913e9ae6)
- `xnibbler-EVIDENCE-REPORT.md` (sha16 f36463a426a1e5bc)
