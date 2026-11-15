#!/usr/bin/env bash
# tools/cp/p0host/ns.sh <spec> <cmd...> — run <cmd> as sarah inside a private mount (and
# optionally network) namespace that hides some host endpoints, WITHOUT touching them for
# anyone else (the user's Steam client, PipeWire, sound devices keep running untouched).
#   spec (comma list):
#     rolib      the whole Steam library /mnt/fast/games/steamapps read-only, with OUR dir
#                $P0/cache/steam-shadercache bound over shadercache/1091500.  Every game launch
#                uses this (launch.sh): measured 2026-11-14, steamclient.so — loaded into the
#                game by lsteamclient — setenv()s MESA_SHADER_CACHE_DIR & the fossilize dump path
#                to <library>/shadercache/1091500, overriding ours, so without this a run writes
#                into the user's Steam library.
#     snd        tmpfs over /dev/snd               (no ALSA hw devices)
#     xdgrun     tmpfs over /run/user/1000         (no PipeWire/Pulse/D-Bus sockets: Ubuntu's ALSA
#                "default" PCM is the pipewire plugin, so hiding /dev/snd alone is not enough —
#                measured: winealsa.drv initialised through it in run noaudio-os)
#     shm        tmpfs over /dev/shm               (no Steam client shared-memory IPC)
#     steamhome  tmpfs over ~/.steam               (no steam.pid / steam.pipe / sdk links)
#     steamipc   tmpfs over ~/.steam holding ONLY the library symlinks (sdk64, sdk32, bin*, root,
#                steam) — steamclient.so stays loadable, but steam.pid / steam.pipe / steam.token /
#                registry.vdf (how it finds the running client) are gone; use with shm
#     net        new network namespace, lo only    (no network, no abstract-socket IPC)
#     nolaunch   /bin/false bind-mounted over every way to START a Steam client or a URL
#                handler (steam.sh, steamrt64/steam, ubuntu12_32/steam, /usr/lib/steam/bin_steam.sh,
#                xdg-open) and DBUS_SESSION_BUS_ADDRESS cleared — so a "Steam is not running"
#                code path can never start or message the user's real client.
# The caller's environment is preserved exactly (saved to a file, restored after setpriv).
set -eu
spec=$1; shift
envf=$(mktemp /home/sarah/cp/p0host/run/nsenv.XXXXXX)
env -0 > "$envf"
inner='
set -eu
spec=$1; envf=$2; shift 2
case ",$spec," in *,rolib,*)
  L=/mnt/fast/games/steamapps
  python3 /home/sarah/cp/p0host/tools/mountas.py robind "$L"
  python3 /home/sarah/cp/p0host/tools/mountas.py bind /home/sarah/cp/p0host/cache/steam-shadercache "$L/shadercache/1091500"
  # the Proton script filelock opens dist.lock with O_RDWR+O_CREAT+O_TRUNC even when nothing changes
  : > /home/sarah/cp/p0host/run/dist.lock; chown 1000:1000 /home/sarah/cp/p0host/run/dist.lock
  python3 /home/sarah/cp/p0host/tools/mountas.py bind /home/sarah/cp/p0host/run/dist.lock "$L/common/Proton - Experimental/dist.lock" ;;
esac
case ",$spec," in *,snd,*) mount -t tmpfs -o mode=755 p0host-nosnd /dev/snd ;; esac
case ",$spec," in *,xdgrun,*) mount -t tmpfs -o mode=700,uid=1000,gid=1000 p0host-noxdg /run/user/1000 ;; esac
case ",$spec," in *,shm,*) mount -t tmpfs -o mode=1777 p0host-noshm /dev/shm ;; esac
case ",$spec," in *,steamhome,*) mount -t tmpfs -o mode=755,uid=1000,gid=1000 p0host-nosteam /home/sarah/.steam ;; esac
case ",$spec," in *,steamipc,*)
  mount -t tmpfs -o mode=755,uid=1000,gid=1000 p0host-steamipc /home/sarah/.steam
  for l in bin bin32 bin64 root sdk32 sdk64 steam; do
    ln -s "$(readlink /proc/1/root/home/sarah/.steam/$l 2>/dev/null || true)" /home/sarah/.steam/$l 2>/dev/null || true
  done
  chown -h 1000:1000 /home/sarah/.steam/* ;;
esac
case ",$spec," in *,net,*) ip link set lo up ;; esac
case ",$spec," in *,nolaunch,*)
  for f in /home/sarah/.local/share/Steam/steam.sh /home/sarah/.local/share/Steam/steamrt64/steam \
           /home/sarah/.local/share/Steam/ubuntu12_32/steam /usr/lib/steam/bin_steam.sh /usr/bin/xdg-open; do
    [ -e "$f" ] && mount --bind /bin/false "$f"
  done ;;
esac
mapfile -d "" E < "$envf"; rm -f "$envf"
case ",$spec," in *,nolaunch,*) E+=("DBUS_SESSION_BUS_ADDRESS=disabled:" "BROWSER=/bin/false") ;; esac
exec setpriv --reuid=1000 --regid=1000 --init-groups -- env -i "${E[@]}" "$@"
'
uflags="-m --propagation private"
case ",$spec," in *,net,*) uflags="$uflags -n" ;; esac
# shellcheck disable=SC2086
exec sudo -n unshare $uflags -- bash -c "$inner" ns-inner "$spec" "$envf" "$@"
