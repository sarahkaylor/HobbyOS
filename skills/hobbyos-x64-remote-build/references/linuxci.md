# linuxci — the HobbyOS x86_64 build host

Facts below were verified on 2026-11-10 from this Mac, over ssh, not read from a doc.

## Identity

| | |
|---|---|
| Hostname / address | `linuxci` / **192.168.0.25** (static) |
| SSH alias | `~/.ssh/config` → `Host linuxci` pinned to `HostName 192.168.0.25` |
| OS | Ubuntu 26.04.1 LTS, kernel 7.0.0-43-generic |
| CPU / RAM | **48 vCPU**, x86_64, nested KVM (`/dev/kvm` present) |
| Disk | `/dev/sda1`, 242 GB, ~160 GB free in `$HOME` |
| Account | `sarah`, passwordless `sudo -n`, `$HOME=/home/sarah` |
| Login | **publickey only** (`Authentications that can continue: publickey`); password auth is disabled |
| Proxmox | guest **126** on `c2` (192.168.0.19) |
| Toolchain | clang/clang++ 21.1.8, ld.lld, llvm-objcopy/ar/nm/readelf 21.1.8, GNU make 4.4.1, mtools, dosfstools, qemu 10.2.1, zstd 1.5.7, pigz, xz, ccache, python3.14 |
| Firmware | `~/.local/share/OVMF/OVMF_CODE_4M.fd` (also `/usr/share/OVMF/OVMF_CODE_4M.fd`) |

## Access

Passwordless auth works from this Mac with `~/.ssh/id_ed25519`
(`ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPYB18IuSqcLPeGzHe7AqywKR/+2zV/VBexmryn7EwZ5`).
Verify in one line:

```bash
ssh -o BatchMode=yes linuxci 'hostname; nproc'     # → linuxci / 48
```

Re-installing that key (needs a shell that can already log in — 192.168.0.123, or the
Proxmox console for guest 126). Note **192.168.0.123 refuses this Mac's source IP**
with an sshd banner `Not allowed at this time`, so it is not usable as a jump host
from here — it works only as the *origin* of a key install:

```bash
# run ON 192.168.0.123
ssh sarah@192.168.0.25 'mkdir -p ~/.ssh && chmod 700 ~/.ssh && \
  echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPYB18IuSqcLPeGzHe7AqywKR/+2zV/VBexmryn7EwZ5" \
  >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'
```

Neither `dgx1`/`c3`/`llm` had a key on this box either — the account's
`authorized_keys` is the only path in.

### Two traps around `linuxci`

1. **DNS is wrong.** `dscacheutil -q host -a name linuxci` answers `192.168.0.39`,
   which is *not* this machine. The ssh config pin (`Host linuxci → 192.168.0.25`)
   must win; otherwise connections go to an unrelated host and fail with
   `Permission denied (publickey)` or timeout. Keep `--host 192.168.0.25` as the
   escape hatch.
2. **`scp` does not expand `$HOME`/`~` remotely** (SFTP-based scp). Use absolute
   paths (`/home/sarah/...`).

## Package installs done on 2026-11-10

The stock image was missing two things that only fail **~5 minutes into** a cold
`disk.img` build, at the X11 server stage:

```bash
sudo apt-get install -y meson                          # pixman / libxkbfile / xserver (meson builds)
sudo apt-get install -y x11-xkb-utils xkb-data         # host xkbcomp for icewm's keymap
```

Without them: `third_party/x11/build.sh: meson: command not found` →
`[x11] ERROR: build.sh did not produce .../XHOBBY.BIN` → `make: *** [Makefile:2450:
x11-staged] Error 1`, and then `[x11:intel] ERROR: xkm: host xkbcomp missing`.
`remote_build.sh --check` now reports both up front.

## What a build needs in `$HOME`

| Path | Why |
|---|---|
| `~/hobbyos-x64-builds/<stamp>-<rev>/src` | per-run throwaway tree (deleted on success) |
| `~/hobbyos-x64-builds/cache/dist-staging` | optional in-OS LLVM (`--with-tools`), 227 MB, reused across runs |
| ~10 GB free | 1.0 GiB image + objects + X11 build tree per run |

## Reference timings (48-core box, `-j32`)

| Stage | Cold | Warm |
|---|---|---|
| zstd extract of the 108 MB tarball | ~3 s | ~3 s |
| `make ARCH=intel disk.img` | **340 s** | 4–30 s |
| X11 extlibs + server (meson) | ~90 s of that | cached |
| KVM boot check (75 s budget) | passes at ~35–45 s | same |

## Boot check command (copied from the Makefile's x64 `QEMU_CMD`)

```bash
cp ~/.local/share/OVMF/OVMF_CODE_4M.fd oVMF-boot.fd   # per-run copy: -pflash also holds NVRAM
qemu-system-x86_64 -M q35,vmport=on -smp 8 -m 4096M \
  -pflash oVMF-boot.fd -display none -serial file:boot.log -enable-kvm \
  -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
  -drive file=disk.img,format=raw,id=disk0,if=none \
  -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
  -device virtio-tablet-pci -action shutdown=poweroff
```

Pass markers: `Mode: DESKTOP (icewm) - Launching X session`, `ICEWM.BIN`,
`XHOBBY.BIN`; fail markers: `PANIC`, `FATAL`, or a boot that never leaves `BdsDxe`.
Always boot a **copy** of the firmware: with a single `-pflash` file QEMU writes boot
entries back into it, and a firmware file that has accumulated stale entries is how
a good image ends up "not booting".
