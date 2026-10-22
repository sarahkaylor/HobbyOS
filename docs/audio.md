# GB-029 — Audio decision: virtio-sound, OSS /dev/dsp vs ALSA, SDL dummy/ALSA

Status: **DECISION RECORDED — SDL `dummy` audio driver (silent-safe by
construction) + `/dev/dsp` → `ENODEV` stub; no ALSA this program cycle.**
(DoD-minimal default selected per coordinator instruction — the decision is a
deliverable inside this ticket, not a human escalation to Sarah.)
Owner: wgfx (devices/graphics/input/audio lane). Ticket: GB-029 (M2-devices-sdl).
Depends on GB-027 (input/evdev, In Review). DoD: "recorded decision +
silent-safe SDL".

## 1. Measured QEMU device facts (this host, QEMU 10.2.1)

Probed with `qemu-system-x86_64 -device help` / `qemu-system-aarch64 -device
help` and live attach probes on 2026-10-17:

- **virtio-sound IS available**: `virtio-sound-pci` on x86_64 (PCI, alias
  `virtio-sound`) and `virtio-sound-device` on ARM (virtio-mmio). Attach probe:
  a q35 x86_64 guest with `-audiodev none,id=a0 -device virtio-sound-pci,
  audiodev=a0` starts cleanly (no device error); the aarch64 `-M virt` guest
  with `-device virtio-sound-device,audiodev=a0` likewise. So the virtio-sound
  EU is present in this QEMU and can be wired on both arches.
- **Audio backends**: `none`, `alsa`, `oss`, `wav` are all built
  (`-audiodev help`). A `wav` file backend attaches but logs
  `Could not create a backend for voice 'virtio-sound.in'` — virtio-sound uses
  the PCM stream API (virtio_snd_pcm_in/out), and only a real duplex backend
  (alsa / pulse / pa via a host, or `audio-driver=alsa`) feeds both
  directions. `none` attaches silently (sink dropped).
- Other sound devices present (emu of legacy): `AC97`, `ES1370`, `intel-hda`/
  `ich9-intel-hda` (+ `hda-duplex/micro/output` codecs), `sb16`, `adlib`,
  `gus`, `cs4231a`, `usb-audio`. All are the OSS/HD-audio legacy surface, not
  the virtio PCM API SDL's app wants.

## 2. The decision: how a static-musl SDL build talks to audio here

SDL 2 (static-musl, GB-031) has three audio drivers relevant to this port:
`dummy`, `alsa`, and `disk`/`oss`. What they need from the kernel:

| driver | kernel surface | notes |
|---|---|---|
| **dummy** | none | SDL_AUDIODRIVER=dummy — silent-safe by construction; no device node, no driver, frame timing only. **Always works.** |
| **alsa** | `/dev/snd/controlC0`, `/dev/snd/pcmC0D0p`, ALSA lib (`libasound`) | ALSA lib is a big dependency; QEMU's HDA/virtio sound is exposed to Linux guests *as* ALSA, but a static-musl SDL with the alsa driver needs musl-built libasound — heavy for this program. |
| **oss** | `/dev/dsp` (OSS DIGITAL) | Linux OSS emulation over the mixer. `open("/dev/dsp", O_RDWR)` -> the compat layer would need a `/dev/dsp` node + ioctl surface (`SNDCTL_DSP_*`, `SOUND_*`). |

### Decision (proposed)
**Ship SDL with the `dummy` audio driver now (`SDL_AUDIODRIVER=dummy`, or
`-DSDL_AUDIO_DISABLED`) and land `/dev/dsp` as an ENODEV stub — do NOT build the
ALSA driver path yet.** Rationale (all from the measured facts + dependency
facts):

1. **SDL is silent-safe by design**: if SDL_CreateAudioDevice finds no driver /
   no useable device it returns NULL and the game keeps running silent, rather
   than failing. So `dummy` (or SDL_AUDIO_DISABLED) is a *completely valid* DoD
   outcome: SDL always boots, produces no sound, never crashes.
2. **virtio-sound exists in QEMU but only earns its keep if a real backend is
   plumbed.** A `none` backend silently drops output — the same net effect as
   SDL dummy but with a device driver to write. The ALSA-in-guest path needs
   musl libasound (a new deep dependency) and a full `/dev/snd` chardev family
   (GB-031's SDL build already carries the fbcon+evdev cost; adding alsa libs
   doubles it). None of this is needed to *unblock the game* this month.
3. **The `decision` label + pause is exactly for this**: audio is genuinely
   optional for the M2 game-compat milestone (fallback is silent), and the
   coordinator's instruction is to record the options with measured facts and
   let Sarah pick whether to invest in virtio-sound this month. So: land the
   silent-safe path, record the decision, pause.

### What lands now (the silent-safe path)
- `/dev/dsp` compat node stub returns **ENODEV** (no OSS device), so any
  program that *tries* OSS gets a clean "no audio device" instead of a hang or
  garbage; SDL falls back to dummy.
- SDL build is *not* gated on this: `SDL_AUDIODRIVER=dummy` (or
  `-DSDL_AUDIO_DISABLED`) is the default, so the SDL app boots silent and runs.

### Not now
- virtio-sound driver in-kernel (a real `VIRTIO_SND` driver + `audio0` node).
- `/dev/snd/*` ALSA chardev family + musl libasound in the SDL profile.

## 3. Where this sits in the plan

gaming.md §8 (GB-029): "Audio: survey virtio-sound; stub `/dev/dsp`; SDL
dummy/ALSA decision". `docs/linux-abi.md` §9 shows `/dev/dsp` as a compat node
only "if the virtio-sound epic lands" — this decision says: let Sarah decide
whether the virtio-sound epic lands this month; the silent-safe SDL requirement
is met regardless. GB-051 (audio acceptance, T5) is gated behind this and can
only go green if audio is in-scope.

## 4. Decision record (to be filled on sign-off)
- [ ] CONFIRMED by Sarah: SDL dummy audio this month (recommended) / ALSA /
  virtio-sound driver
- [ ] if virtio-sound: new GB for the in-kernel `VIRTIO_SND` driver + `/dev/snd`
- [ ] if OSS-only: new GB for `/dev/dsp` with `SNDCTL_DSP_*` ioctls
