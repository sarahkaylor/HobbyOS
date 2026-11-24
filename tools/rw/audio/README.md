# tools/rw/audio — the RimWorld audio gap, measured

One boot answers both halves of "why is RimWorld silent?" (report:
`docs/rimworld-audio-audit.md`):

```bash
bash tools/rw/audio/run-audio-probe.sh
```

| leg | binary | what it measures |
|---|---|---|
| platform | `src/user/linux_probe_snd.c` (`PROBE.ELF`) | raw ALSA ioctls on `/dev/snd/pcmC0D0p`, 2 s of a 1000 Hz tone; QEMU's `wav` audiodev writes `obj/rw-audio/probe.wav` |
| game-facing | `audio_probe_rm.c` (`RMPROBE.ELF`) | `dlopen` of the exact `/LIB64` shims the RimWorld harness stages, then FMOD's first two probes: `pa_simple_new()`, `snd_device_name_hint()`, `snd_pcm_open("default")` |

Both run **in the same guest** with `-device virtio-sound-pci`, so the serial
log separates a broken kernel route from a refusing compat library.

## Reading the result

```
[SND] ALSA nodes: /dev/snd/controlC0 /dev/snd/pcmC0D0p     <- kernel route up
[LP] ok   blocking WRITEI wrote 2.0 s total (96000)         <- raw ABI works
[RMAP] pa_simple_new -> stream=(nil) error=6               <- PA no server
[RMAP] snd_device_name_hint -> rc=0 devices=0              <- empty enumeration
[RMAP] snd_pcm_open("default") -> rc=-19                   <- ENODEV (hard-coded)
```

The WAV grade (`wavgrade.py`, no numpy needed) is the platform verdict:
48 kHz s16 stereo, >= 2.0 s of 1000 Hz, peak about -6.2 dBFS.

While the null shims are what RimWorld stages, `[RMAP]` must say refused even
though `/dev/snd` works.  After a real `libasound.so.2` (+ `/usr/share/alsa`)
is staged, `snd_pcm_open` must return 0 in that same row — that is the check
that the wiring change took effect.  Neither row is a RimWorld verdict: the
game is not run here.

## Notes

- The script copies `disk.img` (or `RW_AUDIO_IMG`) first; it never mutates the
  repo image.
- No KVM access -> TCG.  The raw probe's **real-time pacing** row is
  host-scheduling sensitive under TCG (two boots: 2.0 s of audio in 1.42 s and
  in 1.997 s of guest time); every other row and the WAV are correct.  Set
  `RW_AUDIO_KVM=1` on a host with a writable `/dev/kvm`.
- The game-facing client is a dynamic glibc program using the host's
  loader/libc because the game's pinned glibc 2.31 sysroot is not in this repo.
  The DSOs under test are the exact bytes `src/user/xcompat/stage.sh` installs.
- The device is attached by this script directly.  A RimWorld boot gets it only
  when `CP_AUDIO_WAV` is set (`Makefile:402-409`); the harness does not set it
  yet.
