# cp/audio — real audio: virtio-sound + ALSA (lane C4-audio, wave 7, AD-9)

Plan: `docs/cyberpunk.md` AD-9 (§5) and P9 (§6, "virtio-sound + ALSA").  The
earlier decision `docs/audio.md` (GB-029: SDL dummy + `/dev/dsp` ENODEV) still
holds for `/dev/dsp`; this lane adds the ALSA path next to it.  Base: integ11
(13dc693b).  Every number was measured on **strix** (Ryzen AI MAX+ 395, Zen 5,
KVM, QEMU 10.2.1) unless it says otherwise.  Evidence:
`docs/cp-audio-evidence/*.txt`.

**Default OFF.**  Without the opt-in device the QEMU lines are byte-for-byte
unchanged and `/dev/snd/*` is ENOENT, exactly as before.  The device's output
goes to a WAV file through QEMU's `wav` audiodev only: no host sound server is
ever used (strix's PipeWire is the user's KDE session).

## 1. Pieces

| piece | file | what |
|---|---|---|
| driver | `src/kernel/virtio_snd.c`, `src/include/virtio_snd.h` | virtio-sound-pci (1af4:1059), modern virtio-pci transport like virtio-gpu; control queue polled, TX queue completed by INTx; the first output stream |
| ALSA ABI | `src/kernel/linux_snd.c`, `src/include/linux_snd.h` | `/dev/snd/controlC0` + `/dev/snd/pcmC0D0p`, the ioctl surface (§3), poll, the PCM state machine |
| hw_params engine | `src/kernel/linux_snd_params.c`, `src/include/linux_snd_params.h` | refine/choose (interval arithmetic + the dependency rules), device-free; host golden `src/host/linux_snd_params_test.c` |
| hooks | `fs.h`, `fs.c`, `file_obj.h`, `linux_compat_fd.c`, `arch/x64/trap.c`, `main.c` | FILE_TYPE_DEVSND (open/stat/fstat/lseek/close/poll), the compat ioctl route, one IRQ branch, the boot init |
| probe | `src/user/linux_probe_snd.c` | raw ioctls, no alsa-lib; 1000 Hz tone; XRUN recovery |
| host | `Makefile` (`CP_AUDIO_WAV`), `tools/cp/host-venus.sh`, `tools/cp/p5/p5lib.py`, `tools/cp/p8/boot_p8.py` | opt-in `-audiodev wav,… -device virtio-sound-pci,audiodev=…,streams=1` |
| WAV check | `tools/cp/audio/wavcheck.py` | duration, RMS, active time, FFT peak per 100 ms, tone seconds |
| alsa-lib | `tools/cp/audio/build-alsa.sh`, `tools/cp/audio/alsatone.c` | alsa-lib 1.2.14 against the glibc 2.31 sysroot, prefix /CP/WINE |
| Wine | `tools/cp/audio/build-wine-alsa.sh`, `tools/cp/build-wine.sh` (`CP_WINE_ALSA`) | Wine 11.0 with winealsa.drv |
| PE test | `src/user/cp/audiotone.c`, `tools/cp/p5/*` (tag `audio`), `tools/cp/audio/run-wine-audio.sh` | WASAPI + waveOut tones |

## 2. QEMU facts (measured, QEMU 10.2.1 on strix)

- `virtio-sound-pci` properties: `audiodev`, `streams` (default 2), `jacks`
  (0), `chmaps` (0), `vectors` (2, MSI-X; unused: the driver writes
  `queue_msix_vector = 0xFFFF` and takes INTx).
- With `streams=1` the device has one stream, an OUTPUT stream, and no
  capture voice is created (the `wav` backend has no input; with the default
  `streams=2` QEMU logs "Could not create a backend for voice
  'virtio-sound.in'", docs/audio.md §1).
- PCM_INFO of stream 0: formats 0x0E0078 (S8, U8, S16, U16, S32, U32,
  FLOAT: bits 3–6 and 17–19), rates 0x3FFF (5512…192000; 384000 absent),
  channels 1..2.
- q35 + OVMF: the device lands on INTx line 11 (vector 43 on the legacy PIC).
- Control queue 64 entries, TX queue 64 entries (21 three-descriptor
  messages).
- QEMU copies a TX message's PCM bytes when it pops the message and returns
  it once the audio backend has consumed them; the `wav` backend is paced in
  real time (rate-limited by the audio timer), so hw_ptr advances at 48 kHz.
- `-audiodev wav,id=cpsnd0,path=X,out.frequency=48000,out.channels=2,out.format=s16`:
  the WAV is 48 kHz s16 stereo, no resampling.  QEMU writes the RIFF sizes on
  a clean exit; `wavcheck.py` reads a zero-size header to EOF.

## 3. The ALSA surface

Control (`controlC0`): `PVERSION` (2.0.7), `CARD_INFO` (id `VirtIO`, driver
`virtio-snd`, name `VirtIO SoundCard`), `PCM_NEXT_DEVICE`, `PCM_INFO` (device
0 playback; capture ENOENT), `PCM_PREFER_SUBDEVICE`, `HWDEP/RAWMIDI_NEXT_DEVICE`
(-1), `SUBSCRIBE_EVENTS`, `POWER_STATE`, `ELEM_LIST/INFO/READ/WRITE` for
`Master Playback Volume` (integer ×2, 0..100) and `Master Playback Switch`
(boolean), applied in software on the S16 samples, `ELEM_LOCK/UNLOCK`,
`TLV_READ` (ENXIO).

PCM (`pcmC0D0p`): `PVERSION` (2.0.14), `INFO`, `TSTAMP`, `TTSTAMP`,
`USER_PVERSION`, `HW_REFINE`, `HW_PARAMS`, `HW_FREE`, `SW_PARAMS`,
`STATUS`, `STATUS_EXT`, `DELAY`, `HWSYNC`, `SYNC_PTR`, `PREPARE`, `RESET`,
`START`, `DROP`, `DRAIN`, `PAUSE`, `REWIND`, `FORWARD`, `XRUN`,
`WRITEI_FRAMES`; `RESUME` ENOSYS; `CHANNEL_INFO`/`LINK`/`READI`/`WRITEN`
EINVAL.  Single-open (a second open is EBUSY).  poll: POLLOUT at
`avail >= avail_min`, POLLOUT|POLLERR in XRUN/SETUP; keyed poll notification
from the completion IRQ.

Hardware shape offered: RW_INTERLEAVED only (no MMAP access), S16_LE, 2
channels, 48000 Hz (44100 if the device lacked 48000), period 1024..65536
bytes, 2..64 periods, buffer ≤ 512 KiB (the kernel ring).  alsa-lib's plug
plugin (`plughw`, `default`) converts format/rate/channels in user space.
`info` = INTERLEAVED | BLOCK_TRANSFER | BATCH | PAUSE.

Status/control mmap is **refused** (the compat mmap of a char device answers
-ENOTSUP).  alsa-lib 1.2.14 falls back silently: `map_status_data()` /
`map_control_data()` in `src/pcm/pcm_hw.c` point at a malloc'd
`snd_pcm_sync_ptr` when mmap returns MAP_FAILED, and every pointer update
becomes `SYNC_PTR` (measured: the kernel's close row counts them, §5).

Data path: WRITEI copies into the ring; the stream submits chunks that never
cross a period boundary (so a chunk is contiguous); full chunks go as soon as
written, a partial one only when the device would otherwise starve or while
draining.  hw_ptr += a chunk's frames when the device returns it.  Linux's
underrun rule (RUNNING and avail >= stop_threshold -> XRUN).  The virtio
stream is SET_PARAMS+PREPAREd at HW_PARAMS, STARTed at the first START and
only STOP/RELEASEd at HW_FREE/close; drop/xrun/prepare/pause/reset only change
what is submitted, with an epoch on each chunk so abandoned chunks' late
completions do not count.  A blocking WRITEI/DRAIN parks with the native
restart convention and is woken by the completion IRQ (a deadline of two
periods + 2 ms backs it up).

A trap found on the way: the route first used -2 as "parked", which is
-ENOENT — PCM_INFO(capture) answering ENOENT restarted the ioctl forever
(probe row C1 hung).  `LSND_PARKED` is now -100000.

## 4. Probe (raw ioctls)

`src/user/linux_probe_snd.c`, rows C1–C2 (control), P1–P8 (PCM).  Without the
device (every gate-2 boot) it checks that both nodes are ENOENT and passes.
With it (`bash tools/cp/qemu-slot.sh bash tools/cp/audio/run-probe-snd.sh`):
43 ok rows, `[LP] done rc=0`, among them

- HW_REFINE(any): rate 48000..48000, ch 2..2, format S16_LE, access
  RW_INTERLEAVED, period 256..16384 frames, buffer 512..131072;
- HW_PARAMS 480 × 4 → buffer 1920, period_time 10000 µs;
- START with an empty buffer → EPIPE; auto-start at start_threshold;
- poll(POLLOUT) on a full buffer woke after 9.5 ms (one 10 ms period);
- 2.0 s of frames took **1.992 s** (write 1.953 s + drain 0.040 s): the
  device paces at real time;
- hw_ptr monotonic, SYNC_PTR == STATUS, DELAY 1920 (a full buffer);
- stop writing → XRUN (hw == appl == 960), WRITEI → EPIPE, PREPARE recovers,
  0.5 s more tone; O_NONBLOCK fills to EAGAIN; PAUSE/unpause; DROP; HW_FREE;
  close + re-open (no EBUSY); the second opener got EBUSY.

Host side: `wavcheck.py --freq 1000 --min-tone-s 2.0` on the WAV: duration
2.515 s, 2.5 s of 1000.0 Hz, active RMS −9.2 dBFS, peak −6.2 dBFS (the
probe's 16000/32767 sine), verdict PASS.  Kernel close row: 256 chunks,
120 960 frames, 1 xrun (the deliberate one).  Evidence:
`docs/cp-audio-evidence/probe-snd-device.txt`.

## 5. alsa-lib

**Pin:** `alsa-lib-1.2.14.tar.bz2`, 1 115 517 bytes, sha256
`be9c88a0b3604367dd74167a2b754a35e142f670292ae47a2fdef27a2ee97a32`; its
`.sig` verified (integrator, 2026-11-17) as a Good signature from "ALSA
Release Team (Package Signing Key v1)", fingerprint
`F04DF50737AC1A884C4B3D718380596DA6E59C91`.  Fetched with the user's approval
to `strix:~/cp/lanes/audio-dl/` (read-only; nothing in git).

`tools/cp/audio/build-alsa.sh`: clang `--sysroot` (glibc 2.31 + gcc 10),
`--prefix=/CP/WINE --enable-shared --disable-static --disable-python
--disable-topology --disable-ucm --disable-alisp --disable-hwdep
--disable-old-symbols --without-debug` (rawmidi and seq stay: winealsa's MIDI
half includes `<alsa/asoundlib.h>`'s seq API).  libasound.so.2.0.0 needs at most
GLIBC_2.29; DT_NEEDED libm, libpthread, librt, libdl, libc, ld-linux.  The
config lands in `/CP/WINE/share/alsa` (the compiled-in config dir) and the
library in `/CP/WINE/lib` (already on every launcher's LD_LIBRARY_PATH), so
alsa-lib rides in the Wine stage with no harness change.

**Sysroot trap (affects every lane):** `~/cp/sysroot-glibc231`'s dev links
(`usr/lib/x86_64-linux-gnu/libpthread.so`, `libdl.so`, `librt.so`, `libm.so`
… and `lib64/ld-linux-x86-64.so.2`, 17 links) point into
`~/cp/lanes/ext2b/obj/sysroot-glibc231/`, which no longer exists.  ld then
silently links the STATIC `libpthread.a`/`libdl.a` into a shared object
(the first libasound had no DT_NEEDED libdl/libpthread), and Wine's
configure fails "pthread 64-bit development files not found".  This lane
uses a copy with the 17 links re-pointed: `~/cp/lanes/audio-sysroot`
(54 MB); both build scripts refuse a sysroot whose dev links dangle.

**Config.** alsa.conf gets one appended block: `plughw` and `default` are
`plug` → `mmap_emul` → `hw` on card 0, ctl default is card 0.  Why mmap_emul:
alsa-lib's plug inserts converters (lfloat, rate, route) whose slave must be
mmap-capable, and for a RW-only slave it does not insert `mmap_emul` in
front of them (pcm_plug.c `snd_pcm_plug_change_mmap` only fires when the
client's and the slave's access differ).  Measured: FLOAT_LE through
`plughw:0,0` → `snd_pcm_set_params: Invalid argument` (the last HW_REFINE
asks for access MMAP_* only); Wine's float mix format failed the same way
(`IAudioClient::Initialize` → AUDCLNT_E_ENDPOINT_CREATE_FAILED, `warn:alsa
Unable to set hw params: -22`).  With mmap_emul every case passes.  A real
data mmap in the kernel would remove the need; it needs a mapping of the
ring's pages into user space (vm.c), out of this lane's files.

**Host harness** (`tools/cp/audio/host-alsa-test.sh`, no boot, seconds):
the kernel's `linux_snd.c` + `linux_snd_params.c` built with
`LSND_HOST_TEST` into an LD_PRELOAD shim, a host model of the device
(QEMU's measured behaviour, real-time pacing, completed bytes to a WAV) and
the unmodified alsa-lib with the staged config minus alsa.conf's top-level
`@hooks` (those load the HOST's /etc/alsa/conf.d PipeWire defaults).  Every
open under /dev/snd and /dev/aload is intercepted and `ALSA_PLUGIN_DIR`
points nowhere: the host's sound devices are never touched.  Result
(`docs/cp-audio-evidence/host-alsa-test.txt`): plughw S16, plughw FLOAT_LE,
hw S16, default FLOAT_LE, default 44100 Hz (resampled): all PASS, 1.0 s of
tone each.

**Status mmap.**  Refused; alsa-lib's fallback is SYNC_PTR (§3).  Counted in
the guest: alsatone (1.5 s through `default`, 1200-frame periods) issued 364
SYNC_PTRs and 234 WRITEIs; Wine's WASAPI stream (2.0 s) 1233 SYNC_PTRs and
169 WRITEIs.

## 6. Wine

`tools/cp/audio/build-wine-alsa.sh` (work dir `~/cp/lanes/audio-wine`, never
`~/cp/wine-build-x` or `~/cp/lanes/ntsync-wine`): the x build's recorded
configure line (`~/cp/wine-build-x/build-x86_64/config.log`, 35 args) minus
`--without-alsa`, the same environment (CC/x86_64_CC/CFLAGS/PKG_CONFIG_LIBDIR/
LDFLAGS with `~/cp/wine-build-x/vkstub`), plus
`ALSA_CFLAGS=-I$ALSA/include ALSA_LIBS="-L$ALSA/lib -lasound"` and
`-L$ALSA/lib` appended to LDFLAGS (Wine 11's WINE_PACKAGE_FLAGS keeps only
`-lasound` for its `snd_pcm_hw_params_get_access_mask` link test; measured).
Source: `~/cp/wine-build-x/src/wine-11.0`, read-only.  Configure result:
`ALSA_LIBS = -lasound`, winealsa.drv enabled (Wine 11 defines no
HAVE_ALSA_* in config.h).  Build 16 jobs; install = build-wine.sh's
`install-lib` + strip, plus `libasound.so.2*` into `/CP/WINE/lib` and
`share/alsa` into `/CP/WINE/share`.  Stage 207 MB; vs the x stage: 81 files
added (winealsa.drv/.so, libasound, the alsa config), 767 differ (a full
rebuild: every binary changes), none removed.  winealsa.so needs ntdll.so,
libasound.so.2, libpthread, libdl, libc.  Audit: every ELF ≤ GLIBC_2.31.

**For the integrator's combined build** (C2-ntsync rebuilds Wine too): the
whole audio change to Wine is the configure delta above and one dependency,
alsa-lib (`tools/cp/audio/build-alsa.sh` output).  `tools/cp/build-wine.sh`
takes it as `CP_WINE_ALSA=<alsa prefix>` (…/destdir/CP/WINE): drops alsa
from WITHOUT, passes ALSA_CFLAGS/ALSA_LIBS, adds `-L` to LDFLAGS, requires
`ALSA_LIBS = …-lasound` after configure, and stages libasound + share/alsa.
No source patch to Wine.

**Guest test** (`tools/cp/audio/run-wine-audio.sh`: one P5-harness boot with
`P5_PROBES=audio` and `CP_AUDIO_WAV`; `src/user/cp/audiotone.c` built by
`tools/cp/p5/build-pe.sh`):

| step | path | result | WAV (host) |
|---|---|---|---|
| alsatone default 660 Hz S16 1.5 s | alsa-lib → plug → mmap_emul → hw | rc 0, 1.401 s write + 0.100 s drain | 1.5 s at 660 Hz |
| alsatone default 770 Hz FLOAT_LE 1.0 s | lfloat converter | rc 0 | 1.0 s at 770 Hz |
| audiotone wasapi 880 Hz 2 s | IMMDeviceEnumerator → IAudioClient (shared, mix format float32 48 kHz stereo, 200 ms) → IAudioRenderClient | rc 0, 2 xruns at stream start | 2.0 s at 880 Hz |
| audiotone waveout 550 Hz 1.5 s | winmm waveOutOpen(WAVE_MAPPER) → mmdevapi → winealsa | rc 0, 2 xruns at start | 1.4 s at 550 Hz |

Total WAV 5.98 s, active RMS −9.1 dBFS, peak −6.0 dBFS.  Wine sees two
render endpoints (`default` and `plughw:0,0`, "VirtIO SoundCard"); its
capture probe of `default` is ENOENT (no capture node), as expected.  The
two xruns per Wine stream are winealsa's start: its timer thread writes one
period per 10 ms tick and the first periods race the device; no audible gap
after that (the 880/550 Hz tone seconds equal the stream lengths).
Evidence: `docs/cp-audio-evidence/wine-audio.txt`.

## 7. The game

One P8 Venus boot to the main menu with the device on
(`tools/cp/audio/run-game-audio.sh`; Wine = the winealsa stage; the Venus ICD
is a copy of `~/cp/integ-venus/stage`; seeded prefix = a copy of integ's
`pfx-seeded.img`): **the menu arrives** (OCR NEW GAME / LOAD GAME / SETTINGS /
CREDITS / QUIT GAME at +352.7 s, game launch at +148.4 s).  The game opens
the PCM once (`hw_params rate=48000 ch=2 period=480 periods=4 buffer=1920`,
i.e. winealsa's 10 ms period, 40 ms ALSA buffer) after 8 endpoint-probe
open/close pairs, and keeps it open to the menu.

Host WAV (measured, then deleted: game content): **236.6 s** of audio,
**216.8 s active** (100 ms windows above −50 dBFS; the first 18 s are near
silence), active RMS **−19.9 dBFS**, peak **−2.0 dBFS**.  236.6 s of samples
in the ~235 s between the game's first frame (+165 s) and the end of the
boot (+400 s): the device was fed in real time with no long gaps.  The
sound device shares INTx line 10 with the Venus GPU (fences): the shared
branch served both (101 553 interrupts before the game's stream opened, all
GPU fences; tx 0).

Menu time with vs without the device (strix, consecutive boots, same tree,
Wine stage and ICD, n = 1 each; `docs/cp-audio-evidence/game-menu-audio.txt`):

| boot | boot→launch | launch→1st title prompt | 1st Space→menu | launch→menu |
|---|---|---|---|---|
| audio ON (audio-venus-on) | 148.4 s | 95.1 s | 109.2 s (2 Spaces) | **204.3 s** |
| audio OFF (audio-venus-off) | 140.8 s | 93.2 s | 46.1 s (2 Spaces) | **139.3 s** |
| integ14 P8 Venus (x Wine, no device) | 50.3 s | 127.7 s | 16.4 s (1 Space) | 144.1 s |

The boot→launch time (both of this lane's boots ~140 s vs integ14's 50 s)
is host load (four slots busy, this lane's gate batch beside it).  The ON
boot's extra minute is after the first Space (the intro sequence with sound
plays out at its real length; without an endpoint it is skipped faster —
plausible, not proven with n = 1).  FILL-FPS

## 8. Facts for other lanes

- `~/cp/sysroot-glibc231` is broken for linking: 17 dev symlinks
  (libpthread.so, libdl.so, librt.so, libutil.so, libresolv.so, libnss_*.so,
  lib64/ld-linux-x86-64.so.2 …) dangle into the deleted
  `~/cp/lanes/ext2b/obj/sysroot-glibc231`.  ld then links static archives
  into shared objects without a word (and configure scripts' pthread checks
  fail).  Fix (integrator, it is shared): re-point each link at the same
  sysroot's `lib/x86_64-linux-gnu/<target>`; `~/cp/lanes/audio-sysroot` is
  such a copy.  Builds made before the deletion (wine-build-x) are fine.
- A compat ioctl route that parks must not use -2 as its "parked" code:
  -ENOENT is -2 (§3).  `GPU_ABI_PARKED` is -2 too; gpu_abi.c never returns
  -ENOENT today, but it is the same trap.
- alsa-lib on a RW-only hw device needs mmap_emul under plug (§5); any lane
  that adds its own ALSA config must keep that.
- The x64 IRQ dispatch (trap.c) now has a virtio-sound branch before the GPU
  branch; it also serves a GPU/net device that shares the line.  C1-k7 owns
  trap.c: the hunk is 14 lines in `general_interrupt_inner`'s else-if chain,
  nowhere near the syscall entry/exit.
- `FILE_TYPE_DEVSND` is appended at the end of `file_type_t`; C2-ntsync's
  node type will land next to it (a trivial merge).
