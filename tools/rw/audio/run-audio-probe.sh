#!/usr/bin/env bash
# tools/rw/audio/run-audio-probe.sh — one guest boot that measures BOTH halves
# of RimWorld's audio question (docs/rimworld-audio-audit.md):
#
#   LEG 1 (platform): src/user/linux_probe_snd.c (raw ALSA ioctls, no alsa-lib)
#     plays a 1000 Hz tone through /dev/snd/pcmC0D0p.  QEMU's `wav` audiodev
#     writes it to $OUT/probe.wav.
#   LEG 2 (game-facing libraries): audio_probe_rm.c loads the exact DSOs the
#     RimWorld harness stages in /LIB64 (src/user/xcompat/pulse_null.c +
#     alsa_null.c) and calls FMOD's first two probes:
#       pa_simple_new(), snd_device_name_hint(), snd_pcm_open("default").
#
# Both run in the SAME boot as the virtio-sound device, so the log separates
# "the kernel route is broken" from "the compat library refuses".
#
# Env: RW_AUDIO_IMG=<disk.img>   base image (default $ROOT/disk.img; copied)
#      RW_AUDIO_TIMEOUT=<s>      boot deadline (default 900)
#      RW_AUDIO_KVM=1            use KVM when /dev/kvm is writable
#      CP_AUDIO_WAV is NOT used: this script owns the wav path.
#
# Exit 0 when the WAV holds >= 2.0 s of the probe tone.  The RM rows are
# printed and summarised but do not decide the exit code: while the null
# libasound is staged they must say "refused", and once a real client lands
# they must say the open succeeded.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
. tools/host/hostcaps.sh

OUT="${RW_AUDIO_OUT:-$ROOT/obj/rw-audio}"
IMG_BASE="${RW_AUDIO_IMG:-$ROOT/disk.img}"
TIMEOUT_S="${RW_AUDIO_TIMEOUT:-900}"
IMG="$OUT/disk.img"
SER="$OUT/serial.log"
WAV="$OUT/probe.wav"
mkdir -p "$OUT"

die() { echo "rw-audio: $*" >&2; exit 2; }
[ -f "$IMG_BASE" ] || die "no base image at $IMG_BASE (build disk.img first)"
command -v mcopy >/dev/null && command -v mmd >/dev/null || die "mtools missing"
command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 missing"
[ -n "${HB_OVMF:-}" ] && [ -f "$HB_OVMF" ] || die "no x86_64 firmware (set HB_OVMF)"

echo "== build: compat DSOs (the bytes the RimWorld harness stages)"
make -j"$(nproc)" ARCH=intel xcompat_dsos >/dev/null || die "xcompat_dsos failed"

echo "== build: raw ALSA probe (probe_snd) + game-facing probe (audio_probe_rm)"
SND="$OUT/probe_snd.elf"
if command -v musl-gcc >/dev/null; then
  musl-gcc -static-pie -fPIE -O1 -g -o "$SND" src/user/linux_probe_snd.c -lm || die "probe_snd build"
else
  # No musl-tools on this host: a glibc static-PIE is still ET_DYN with no
  # PT_INTERP, which is the class the compat loader accepts.  The probe is a
  # plain ioctl program, so the libc choice does not change the ABI under test.
  gcc -static-pie -fPIE -O1 -g -o "$SND" src/user/linux_probe_snd.c -lm || die "probe_snd build (gcc)"
fi
RM="$OUT/audio_probe_rm.elf"
gcc -O1 -o "$RM" tools/rw/audio/audio_probe_rm.c -ldl \
  -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 -Wl,-rpath,/LIB64 || die "audio_probe_rm build"

echo "== stage: copy $IMG_BASE -> $IMG"
cp --reflink=auto "$IMG_BASE" "$IMG" || die "image copy"
STAGE() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy $1 -> $2"; }
mmd -i "$IMG" ::/LIB64 ::/lib64 ::/lib >/dev/null 2>&1 || true
mmd -i "$IMG" ::/lib/x86_64-linux-gnu >/dev/null 2>&1 || true

# The DSOs exactly as src/user/xcompat/stage.sh installs them for RimWorld.
for so in libasound.so.2 libpulse-simple.so.0 libpulse.so.0 libX11.so.6 libXi.so.6 libudev.so.1; do
  STAGE "obj/xcompat/$so" "::/LIB64/$so"
done
# The dynamic client needs a loader + libc.  The game pins glibc 2.31, but that
# sysroot is not part of this repo; the host's glibc is what this probe runs on.
for f in libc.so.6 libm.so.6 libdl.so.2; do
  p="$(gcc -print-file-name=$f)"
  [ -f "$p" ] || die "host $f not found"
  STAGE "$p" "::/LIB64/$f"
  STAGE "$p" "::/lib/x86_64-linux-gnu/$f"
done
LD_SO="$(gcc -print-file-name=ld-linux-x86-64.so.2)"
[ -f "$LD_SO" ] || die "host ld-linux not found"
STAGE "$LD_SO" ::/lib64/ld-linux-x86-64.so.2
STAGE "$LD_SO" ::/LIB64/ld-linux-x86-64.so.2

STAGE "$SND" ::PROBE.ELF
STAGE "$RM" ::RMPROBE.ELF
printf 'PROBE.ELF\nRMPROBE.ELF\n' > "$OUT/wave.cfg"
STAGE "$OUT/wave.cfg" ::WAVE.CFG

cp "$HB_OVMF" "$OUT/OVMF.fd"
ACCEL=(-accel tcg,thread=multi)
if [ "${RW_AUDIO_KVM:-0}" = 1 ] && [ -w /dev/kvm ]; then
  ACCEL=(-enable-kvm -cpu host)
  echo "== boot: KVM"
else
  echo "== boot: TCG (no writable /dev/kvm); the raw probe's real-time-pacing row"
  echo "         is expected to fail here — grade the WAV, not that row"
fi

rm -f "$SER" "$WAV"
echo "== boot: virtio-sound-pci -> $WAV (deadline ${TIMEOUT_S}s, serial $SER)"
timeout "$TIMEOUT_S" qemu-system-x86_64 -M q35,vmport=on "${ACCEL[@]}" \
  -smp 8 -m 8192M -pflash "$OUT/OVMF.fd" -display none -serial "file:$SER" \
  -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
  -drive file="$IMG",format=raw,id=disk0,if=none \
  -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
  -device virtio-tablet-pci -action shutdown=poweroff \
  -audiodev "wav,id=cpsnd0,path=$WAV,out.frequency=48000,out.channels=2,out.format=s16" \
  -device virtio-sound-pci,audiodev=cpsnd0,streams=1 \
  >"$OUT/qemu.out" 2>"$OUT/qemu.err"
QRC=$?
echo "== qemu rc=$QRC (0 = the guest halted and shut down)"

echo
echo "== platform leg: [SND] + [LP]"
grep -aE '\[SND\]|\[LP\] (ok|FAIL|done|tone)' "$SER" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
echo
echo "== game-facing leg: [RMAP]"
grep -aE '\[RMAP\]' "$SER" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
echo
echo "== host WAV"
python3 tools/rw/audio/wavgrade.py "$WAV" --freq 1000 --min-tone-s 2.0
WRC=$?
[ "$WRC" = 0 ] && { echo "rw-audio: PASS (the guest produced the probe tone)"; exit 0; }
echo "rw-audio: FAIL (no measurable tone in $WAV)"
exit 1
