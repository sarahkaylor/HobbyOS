tools/cp/audio2/wine-candidates/ -- Wine 11.0 patches proposed by lane H5-audio2
(docs/cp-audio2.md §9).  NOT in tools/cp/wine-patches/: build-wine.sh applies
every *.patch there, so a candidate moves there only after the integrator
approves it.  Both apply after wine-patches/0001 and 0002 (they touch only
dlls/winealsa.drv/alsa.c).

0101-winealsa-leadin-gate.patch  (candidate)
    alsa_write_data's silence lead-in fires when data_in_alsa_frames == 0 &&
    held_frames < period.  data_in_alsa_frames is decremented by the whole
    free space each period, so it reaches 0 while ALSA still holds audio, and
    on QEMU's virtio-sound avail says "empty" while 20-36 ms are still in
    QEMU's mixer (the kernel's [SND] zeros rows: q_hw ~ 0, q_vis 1298-1741).
    The patch keeps a play clock of what is still queued (frames written minus
    elapsed time x rate, never below buffer - avail) and writes the lead-in only
    when that is under 2 periods.  The played-frames accounting and the padding
    the client sees are unchanged (round 1's accounting change raised real
    xruns in the model).

0102-winealsa-h5-diag.patch  (diagnostic only, never for a release build)
    Rate-limited stderr rows ([WALSA] stat every 10 s, [WALSA] leadin for the
    first 64 lead-ins then every 64th): period events, the client's wake
    latency (first GetCurrentPadding/GetBuffer after the event), ticks with no
    ReleaseBuffer, lead-ins split by whether the client had come back since
    the last event, AUDCLNT_BUFFERFLAGS_SILENT frames, gated lead-ins.

mkpatch.py regenerates both from a pristine alsa.c (/tmp/a2wp/a/... on
strix): python3 mkpatch.py GATE_PERIODS.
