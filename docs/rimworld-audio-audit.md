# RimWorld audio on the Linux-ABI compat class — the missing route

Status: **root cause identified and measured on the current tip
(`eaf006a9708e`)**.  Audio is not blocked by the kernel, a syscall or a missing
device node: the kernel's virtio-sound → `/dev/snd` ALSA route works and was
measured producing a tone in the same guest.  RimWorld is silent because the
compat layer stages **null audio libraries** in `/LIB64`, and because a RimWorld
boot never attaches the virtio-sound device those libraries would need.

This report replaces the earlier assumption "audio is unreachable" with the
first failing call at each layer, and with a measured sample from the platform
half.

Evidence (all captured on this host, QEMU 10.2.1, TCG — KVM was not available
to the runner):

| file | what it shows |
|---|---|
| `docs/rw-audio-evidence/tip-route.txt` | one guest boot with `-device virtio-sound-pci`: `[SND]` bring-up, `/dev/snd` nodes, the raw ALSA probe's 120,960 frames |
| `docs/rw-audio-evidence/rm-audio-probe.txt` | the **same boot**, a compat client loading the RimWorld-staged `/LIB64/libpulse-simple.so.0` and `/LIB64/libasound.so.2`: every probe refuses |
| `docs/rw-audio-evidence/wav-grade.txt` | the host WAV QEMU's `wav` audiodev wrote from that boot: 48 kHz s16 stereo, 2.516 s, 1000.4 Hz, −6.2 dBFS peak |
| `docs/rw-audio-evidence/shim-disasm.txt` | the shipped shim bytes: `snd_pcm_open` returns `-19` unconditionally; `pa_simple_new` returns NULL with error 6 |
| `tools/rw/audio/` | the two probes and their boot recipe (reproducible) |

## 1. What RimWorld asks for

Measured from the game binary and from a real guest run (host inventory:
`docs/rimworld-evidence.md` §4; guest log: `docs/rimworld-launch-log.md` §20.3):

- Unity 2022.3 / FMOD.  Its **first** output plugin is PulseAudio:
  `dlopen("libpulse-simple.so.0")`, which resolves the async `pa_*` names as
  well (`libpulse.so.0` on a real system).
- Its **second** plugin is ALSA: `dlopen("libasound.so.2")` and the 19 `snd_*`
  entry points in `src/user/xcompat/required-alsa.h`
  (`snd_device_name_hint` for enumeration, then `snd_pcm_open`,
  `snd_pcm_hw_params*`, `snd_pcm_prepare`, `snd_pcm_writei`).
- The measured guest log shows both probes and then the failure:
  ```
  [RWMAIN] dlopen ok libpulse-simple.so.0 ...
  [RWMAIN] dlopen ok libasound.so.2 ...
  FMOD failed to initialize the output device.: "Error initializing output device. " (60)
  Forced to initialize FMOD to to the device driver's system output rate 48000, ...
  FMOD failed to initialize the output device.: "An error occured that wasn't supposed to. Contact support." (33)
  Unable to initalize any audio device (even FMOD nosound device), ...
  ```
  (`docs/rimworld-launch-log.md:2553-2556`.)

## 2. Required vs present (compat layer, current tip)

| required by FMOD | present in a RimWorld image | behaviour |
|---|---|---|
| `libpulse-simple.so.0`, `libpulse.so.0` (21 names) | **yes — a null shim** (`src/user/xcompat/pulse_null.c`, built by `src/user/xcompat/Makefile.inc:168-181`) | `pa_simple_new()` returns NULL, `*error = 6` (`PA_ERR_CONNECTIONREFUSED`). There is no PulseAudio protocol implementation in the tree and no server. |
| `libasound.so.2` (19 names) | **yes — a null shim** (`src/user/xcompat/alsa_null.c`, built by `Makefile.inc:104-108`) | `snd_device_name_hint()` returns an **empty** NULL-terminated list (0 devices); `snd_pcm_open()` returns `-19` (`ENODEV`) with the comment `no hardware, ever (v1)` (`alsa_null.c:65`). The bytes say it unconditionally: `mov $0xffffffed,%eax; ret`. |
| a real ALSA client (`libasound` + `/usr/share/alsa`) | **no** | no real alsa-lib, no `alsa.conf`, no `pcm/*.conf` anywhere in the RimWorld image |
| `/dev/snd/controlC0`, `/dev/snd/pcmC0D0p` | **only if a virtio-sound device is attached** | RimWorld's QEMU line attaches none. `linux_snd_init()` creates the nodes only when `virtio_snd_init() == 0` (`src/kernel/linux_snd.c:230-233`), and the opt-in device is added **only** by `CP_AUDIO_WAV` (`Makefile:402-409`), which `tools/rw/run-rimworld-desktop.sh` never sets. |
| a kernel PCM route | **yes, working** | `src/kernel/virtio_snd.c` + `src/kernel/linux_snd.c`: virtio-sound-pci (1af4:1059), control/TX queues, the ALSA control + PCM ABI subset, RX completion IRQ, poll. |

The silent-safe decision in `docs/audio.md` (GB-029: SDL `dummy` +
`/dev/dsp` → `ENODEV`) is a **different track** — it is the native static-musl
SDL build.  It is not a decision that the compat class has no audio, and it does
not cover FMOD or `/dev/snd`.

## 3. The first failing calls (measured in one boot)

Boot: current tip kernel, `MODE=test`, the RimWorld `/LIB64` DSOs staged exactly
as `src/user/xcompat/stage.sh` stages them (`stage.sh:38`), plus
`-device virtio-sound-pci,audiodev=wav` so the kernel route is **up**:

```
[SND] virtio-sound 1af4:1059 bar=0x... jacks=0 streams=1 chmaps=0 ctlq=64 txq=64
[SND] ALSA nodes: /dev/snd/controlC0 /dev/snd/pcmC0D0p

[RMAP] pulse dlopen ok handle=0x1040000040
[RMAP] pa_simple_new -> stream=(nil) error=6          <- PA_ERR_CONNECTIONREFUSED
[RMAP] alsa dlopen ok handle=0x10400015c0
[RMAP] snd_device_name_hint -> rc=0 devices=0          <- empty enumeration
[RMAP] snd_pcm_open("default") -> rc=-19 pcm=(nil)     <- ENODEV

[LP] ok   blocking WRITEI wrote 2.0 s total (96000)    <- raw ALSA ABI: works
[LP] done rc=0                                        <- 43/43 raw rows green
```

So, in order:

1. **PulseAudio leg:** `pa_simple_new()` in `/LIB64/libpulse-simple.so.0`
   returns NULL with `PA_ERR_CONNECTIONREFUSED` (6).  Not a hang, not a missing
   symbol — a deliberate "no daemon" answer.
2. **ALSA enumeration:** `snd_device_name_hint()` in `/LIB64/libasound.so.2`
   returns an empty list.  A real FMOD ALSA probe has nothing to open; the
   shim refuses one step earlier than `snd_pcm_open`.
3. **ALSA open:** `snd_pcm_open("default")` returns `-19 ENODEV`, hard-coded —
   independent of whether `/dev/snd/pcmC0D0p` exists.
4. **Device node:** in a real RimWorld boot there is no `/dev/snd` at all,
   because that QEMU line has no virtio-sound device.

The kernel side is not the wall: the same boot's raw ALSA client opened
`controlC0` and `pcmC0D0p`, refined 48000 Hz / 2 ch / S16_LE, played 120,960
frames, and the host WAV contains **2.516 s of 1000.4 Hz at −6.2 dBFS**
(`wav-grade.txt`).

## 4. What to change (ranked, smallest first)

1. **Attach the device to RimWorld boots.**  The mechanism already exists:
   `CP_AUDIO_WAV=/abs/out.wav` adds
   `-audiodev wav,... -device virtio-sound-pci,audiodev=...,streams=1`
   (`Makefile:402-409`).  `tools/rw/run-rimworld-desktop.sh` must pass it (and
   `tools/rw/README.md` must say so), otherwise `virtio_snd_init()` returns
   `-1` and `/dev/snd` is ENOENT (`linux_snd.c:230-233`).
2. **Stop staging a null `libasound.so.2` and stage a real one.**  The real
   client exists as a build recipe, not as an artifact: `tools/cp/audio/build-alsa.sh`
   builds alsa-lib 1.2.14 against the pinned glibc 2.31 and installs
   `libasound.so.2*` with `share/alsa` (its `alsa.conf` block maps
   `default`/`plughw` onto `hw:0,0`, which is this kernel's card).  That build
   lives on strix at `~/cp/lanes/audio-alsa` and is **not in git**, so nothing
   in the RimWorld tree currently contains a functional ALSA client.  Stage the
   DSO at `/LIB64/libasound.so.2` and the config at `/usr/share/alsa/`, and make
   sure the null shim cannot shadow it (`src/user/xcompat/stage.sh:38` lists
   `libasound.so.2` in the required set).
3. **Keep the PulseAudio shim as the "no server" answer** (it is the correct
   behaviour on a host with no daemon), and let FMOD fall back to ALSA — the
   guest log shows it does dlopen `libasound.so.2`.  Implementing a PA server or
   a `pa_simple` client over the same `/dev/snd` route is the larger option, and
   it also has to convert float32 → S16 because the kernel offers S16_LE only.
4. **Grade it on the game, not on the probe.**  The pass condition is: a
   RimWorld boot with the device attached and the real libasound staged produces
   a non-empty host WAV while the game is at the menu, and `Player.log` no
   longer contains `FMOD failed to initialize the output device` /
   `Unable to initalize any audio device`.  Until that run exists, N1's audio
   row stays unmet.

## 5. Limits of this report

- **No RimWorld run.**  This host has no game tree (`/srv/nfs` is empty) and
  `linuxci` (192.168.0.25) refuses this runner's key.  No claim is made that N1
  audio passes; the claim is the measured gap above.
- The guest route measurement ran under **TCG** (no KVM access on this runner).
  The raw probe's **real-time-pacing** row is host-scheduling sensitive under
  TCG: two boots measured 2.0 s of audio consumed in 1.42 s and in 1.997 s of
  guest time.  The canonical run is the 1.997 s one (`[LP] done rc=0`, 43/43
  rows); the WAV is correct in both.  On the KVM hosts the CP lane measured the
  row passing (`docs/cp-audio.md` §4).
- The RimWorld-path probe client is a dynamic glibc program staged with the
  host's glibc 2.39 loader/libs (the game's pinned 2.31 sysroot is not present
  on this runner).  The DSOs under test are the exact bytes the RimWorld
  harness stages.

## 6. Reproduce

```bash
# 1. the kernel route + the game-facing DSO refusals, one boot
bash tools/rw/audio/run-audio-probe.sh      # stages probes, boots with the device
# prints: [SND]/[LP] rows, [RMAP] rows, then grades the WAV
```

`tools/rw/audio/README.md` lists the manual staging steps and the QEMU line.
