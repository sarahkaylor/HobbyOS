#!/usr/bin/env bash
# tools/cp/p0host/batch-main.sh — every game run of the lane after setup, in order.  Each run
# takes ~/cp/measure.lock itself (run.sh).  Logs: runs/<tag>.out, runs/batch.log.
set -u
. "$(dirname "$0")/env.sh"
log() { echo "$(date -Is) $*" >> "$P0/runs/batch.log"; }
go() { local tag=$1; shift; log "start $tag"; "$@" > "$P0/runs/$tag.out" 2>&1; log "end $tag rc=$?"; sleep 5; }
VKL="VK_ADD_LAYER_PATH=$T/vkdump VK_LOADER_LAYERS_ENABLE=VK_LAYER_P0HOST_vkdump"

# usage: batch-main.sh [stage...]   (default: all original stages; later stages by name)
for stage in "${@:-all}"; do
case "$stage" in
warm|all)
  # shader caches are now private ($P0/cache): one benchmark per resolution to fill them
  go warm-720p  env MANGO=1 "$T/run.sh" warm-720p 1280 720 --bench --timeout 900
  go warm-1080p env MANGO=1 "$T/run.sh" warm-1080p 1920 1080 --bench --timeout 900 ;;&
perf|all)
  for rep in a b; do
    go perf-720p-$rep  env MANGO=1 "$T/run.sh" perf-720p-$rep 1280 720 --bench --menu-log 30 --timeout 900
    go perf-1080p-$rep env MANGO=1 "$T/run.sh" perf-1080p-$rep 1920 1080 --bench --menu-log 30 --timeout 900
  done ;;&
cold|all)
  # launch-to-menu with this host's page cache for the game + Proton evicted (inside the lock)
  go cold-720p env PRE_HOOK="python3 $T/evict.py '$GAME' '$PROTON_DIR'" "$T/run.sh" cold-720p 1280 720 --stop-at-menu --timeout 900 ;;&
vk|all)
  go vkfeat env XENV="$VKL P0HOST_VKDUMP=$P0/runs/vkfeat/vkdump.txt" "$T/run.sh" vkfeat 1280 720 --stop-at-menu --menu-settle 15 --timeout 600 ;;&
audio|all)
  go noaudio-wine env XENV="WINEDLLOVERRIDES=winepulse.drv=d;winealsa.drv=d" "$T/run.sh" noaudio-wine 1280 720 --stop-at-menu --menu-settle 15 --timeout 420
  go noaudio-os env NS="snd" XENV="PULSE_SERVER=unix:/nonexistent/p0host" "$T/run.sh" noaudio-os 1280 720 --stop-at-menu --menu-settle 15 --timeout 420 ;;&
audio2)
  # noaudio-os still reached PipeWire through ALSA's pipewire plugin; hide the runtime dir too
  go noaudio-os2 env NS="snd,xdgrun" XENV="PULSE_SERVER=unix:/nonexistent/p0host" "$T/run.sh" noaudio-os2 1280 720 --stop-at-menu --menu-settle 15 --timeout 420 ;;&
warm2)
  # after the read-only-library containment (22:35) the Mesa/fossilize cache is private and empty
  go warm2-720p  env MANGO=1 "$T/run.sh" warm2-720p 1280 720 --bench --timeout 900
  go warm2-1080p env MANGO=1 "$T/run.sh" warm2-1080p 1920 1080 --bench --timeout 900 ;;&
perf2)
  # extra perf repetitions (perf-720p-a and perf-1080p-b overlapped another lane's VM, top snapshots)
  go perf-720p-c  env MANGO=1 "$T/run.sh" perf-720p-c 1280 720 --bench --menu-log 30 --timeout 900
  go perf-1080p-c env MANGO=1 "$T/run.sh" perf-1080p-c 1920 1080 --bench --menu-log 30 --timeout 900 ;;&
steam|all)
  go nosteam env NS="shm,steamhome,nolaunch" "$T/run.sh" nosteam 1280 720 --stop-at-menu --menu-settle 15 --timeout 420 ;;&
steam2)
  # the Steam client unreachable but its library loadable (nosteam hid ~/.steam entirely,
  # which removed steamclient.so itself and failed inside Proton's steam.exe)
  go nosteam2 env NS="shm,steamipc,nolaunch" "$T/run.sh" nosteam2 1280 720 --stop-at-menu --menu-settle 15 --timeout 420 ;;&
steam3)
  # nosteam2 still reached the client (Steam ran "AutoCloud on exit" and set LastPlayed at its
  # exit): its IPC is not only ~/.steam + /dev/shm.  Add a private network namespace (no
  # abstract unix sockets, no network).
  go nosteam3 env NS="shm,steamipc,nolaunch,net" "$T/run.sh" nosteam3 1280 720 --stop-at-menu --menu-settle 15 --timeout 420 ;;&
sync)
  # AD-8: what the game does with Wine's in-server synchronisation (no fsync/esync), which is
  # what upstream Wine on HobbyOS will use: one measured benchmark run + one strace to menu
  go perf-720p-srvsync env MANGO=1 XENV="PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1" "$T/run.sh" perf-720p-srvsync 1280 720 --bench --menu-log 30 --timeout 900
  mkdir -p "$P0/runs/trace-srvsync"
  go trace-srvsync env XENV="PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1" WRAP="strace -f -tt -y -s 96 -o $P0/runs/trace-srvsync/trace.txt" "$T/run.sh" trace-srvsync 1280 720 --stop-at-menu --menu-settle 30 --timeout 2700 ;;&
distro|all)
  go distrowine env RUNNER=wine PFXDIR="$P0/pfx-distro" \
     SETTINGS_PATH="$P0/pfx-distro/pfx/drive_c/users/sarah/AppData/Local/CD Projekt Red/Cyberpunk 2077/UserSettings.json" \
     NS="shm,steamhome,nolaunch" "$T/run.sh" distrowine 1280 720 --stop-at-menu --timeout 300 ;;&
trace|all)
  # item 2+4: one launch-to-menu with every process under strace (separate from perf runs)
  mkdir -p "$P0/runs/trace"
  go trace env WRAP="strace -f -tt -y -s 96 -o $P0/runs/trace/trace.txt" "$T/run.sh" trace 1280 720 --stop-at-menu --menu-settle 30 --timeout 2700 ;;&
esac
done
log "batch ${*:-all} done"
