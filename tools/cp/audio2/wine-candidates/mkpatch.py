import sys, shutil, os
base = "/tmp/a2wp"
a = f"{base}/a/dlls/winealsa.drv/alsa.c"
for d in ("b", "c"):
    os.makedirs(f"{base}/{d}/dlls/winealsa.drv", exist_ok=True)
s = open(a).read()
GATE = int(sys.argv[1]) if len(sys.argv) > 1 else 2

def rep(s, old, new):
    assert s.count(old) == 1, old[:80]
    return s.replace(old, new, 1)

# ---------- 0001: lead-in gate (candidate) ----------
f = s
f = rep(f, "#include <stdio.h>\n#include <pthread.h>\n", "#include <stdio.h>\n#include <pthread.h>\n#include <time.h>\n")
f = rep(f, """    UINT32 data_in_alsa_frames;
""", """    UINT32 data_in_alsa_frames;
    /* frames still queued for playback by a play clock: everything written,
     * minus elapsed time x rate, never below what ALSA says it holds
     * (buffer - avail).  On a device whose hw_ptr runs ahead of what has
     * really played (QEMU's virtio-sound hands a period back as soon as its
     * mixer has copied it), avail alone says "empty" while 20-36 ms are
     * still playing. */
    INT64 queued_frames;
    ULONGLONG queued_ns;
""")
f = rep(f, """    if (written > 0)
        stream->vol_adjusted_frames -= written;
    return written;
}""", """    if (written > 0){
        stream->vol_adjusted_frames -= written;
        stream->queued_frames += written;
    }
    return written;
}

static ULONGLONG alsa_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Age the play clock to now; avail is this period's snd_pcm_avail_update(). */
static void alsa_update_queued(struct alsa_stream *stream, snd_pcm_sframes_t avail)
{
    ULONGLONG now = alsa_now_ns();
    INT64 in_alsa;

    if(stream->queued_ns)
        stream->queued_frames -= (INT64)((now - stream->queued_ns) *
                stream->fmt->nSamplesPerSec / 1000000000);
    stream->queued_ns = now;
    in_alsa = avail >= 0 && avail < stream->alsa_bufsize_frames ?
        stream->alsa_bufsize_frames - avail : 0;
    if(stream->queued_frames < in_alsa)
        stream->queued_frames = in_alsa;
}""")
f = rep(f, """    stream->data_in_alsa_frames = 0;

    return len;
}""", """    stream->data_in_alsa_frames = 0;
    if(stream->queued_frames > (INT64)leave)
        stream->queued_frames = leave;

    return len;
}""")
f = rep(f, """    /* this call seems to be required to get an accurate snd_pcm_state() */
    avail = snd_pcm_avail_update(stream->pcm_handle);
""", """    /* this call seems to be required to get an accurate snd_pcm_state() */
    avail = snd_pcm_avail_update(stream->pcm_handle);
    alsa_update_queued(stream, avail);
""")
f = rep(f, """        avail = stream->alsa_bufsize_frames;

        if((err = snd_pcm_recover(stream->pcm_handle, -EPIPE, 1)) < 0)""", """        avail = stream->alsa_bufsize_frames;
        stream->queued_frames = 0;

        if((err = snd_pcm_recover(stream->pcm_handle, -EPIPE, 1)) < 0)""")
f = rep(f, """    /* Add a lead-in when starting with too few frames to ensure
     * continuous rendering.  Additional benefit: Force ALSA to start. */
    if(stream->data_in_alsa_frames == 0 && stream->held_frames < stream->alsa_period_frames)
    {""", """    /* Add a lead-in when starting with too few frames to ensure
     * continuous rendering.  Additional benefit: Force ALSA to start.
     * Only when the device is really close to empty by the play clock: the
     * data_in_alsa_frames estimate counts all free space as played, so it
     * reaches 0 while ALSA still holds data, and the silence would then be
     * played in the middle of the stream. */
    if(stream->data_in_alsa_frames == 0 && stream->held_frames < stream->alsa_period_frames &&
            stream->queued_frames < (INT64)(%d * stream->alsa_period_frames))
    {""" % GATE)
open(f"{base}/b/dlls/winealsa.drv/alsa.c", "w").write(f)

# ---------- 0002: H5 diagnostics (rate-limited stderr rows) ----------
g = f
g = rep(g, """    INT64 queued_frames;
    ULONGLONG queued_ns;
""", """    INT64 queued_frames;
    ULONGLONG queued_ns;
    /* H5-audio2 diagnostics (0002, never for a release build) */
    ULONGLONG d_event_ns, d_call_ns, d_rel_ns, d_sum_ns, d_lat_max;
    UINT32 d_rel_tick, d_silent, d_ticks, d_events, d_woke, d_nowake, d_late5, d_late10, d_late20;
    UINT32 d_empty, d_lead, d_lead_nowake, d_lead_woke, d_gated, d_rows;
""")
g = rep(g, """static ULONGLONG alsa_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
""", """static ULONGLONG alsa_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* the client's first call after the period event: its wake latency */
static void diag_client_call(struct alsa_stream *stream)
{
    ULONGLONG now, lat;
    if(!stream->d_event_ns || stream->d_call_ns) return;
    now = alsa_now_ns();
    stream->d_call_ns = now;
    lat = now - stream->d_event_ns;
    stream->d_woke++;
    if(lat > stream->d_lat_max) stream->d_lat_max = lat;
    if(lat > 5000000) stream->d_late5++;
    if(lat > 10000000) stream->d_late10++;
    if(lat > 20000000) stream->d_late20++;
}
""")
# per-tick accounting at the start of alsa_write_data
g = rep(g, """    avail = snd_pcm_avail_update(stream->pcm_handle);
    alsa_update_queued(stream, avail);
""", """    avail = snd_pcm_avail_update(stream->pcm_handle);
    alsa_update_queued(stream, avail);
    if(stream->flow == eRender){
        ULONGLONG now = stream->queued_ns;
        stream->d_ticks++;
        if(stream->d_event_ns && !stream->d_call_ns) stream->d_nowake++;
        if(!stream->d_rel_tick) stream->d_empty++;
        if(!stream->d_sum_ns) stream->d_sum_ns = now;
        if(now - stream->d_sum_ns >= 10000000000ULL){
            fprintf(stderr, "[WALSA] stat ticks=%u events=%u woke=%u nowake=%u lat_max_us=%llu late5=%u late10=%u late20=%u "
                    "empty_ticks=%u leadins=%u leadin_nowake=%u leadin_woke=%u gated=%u silent_frames=%u evcb=%d\\n",
                    stream->d_ticks, stream->d_events, stream->d_woke, stream->d_nowake,
                    (unsigned long long)(stream->d_lat_max / 1000), stream->d_late5, stream->d_late10, stream->d_late20,
                    stream->d_empty, stream->d_lead, stream->d_lead_nowake, stream->d_lead_woke, stream->d_gated,
                    stream->d_silent, stream->event != NULL);
            stream->d_ticks = stream->d_events = stream->d_woke = stream->d_nowake = 0;
            stream->d_late5 = stream->d_late10 = stream->d_late20 = stream->d_empty = 0;
            stream->d_lead = stream->d_lead_nowake = stream->d_lead_woke = stream->d_gated = stream->d_silent = 0;
            stream->d_lat_max = 0;
            stream->d_sum_ns = now;
        }
    }
""")
g = rep(g, """            stream->queued_frames < (INT64)(%d * stream->alsa_period_frames))
    {""" % GATE, ("""            stream->queued_frames < (INT64)(%d * stream->alsa_period_frames))
    {""" % GATE) + """
        {
            ULONGLONG now = stream->queued_ns;
            int woke = stream->d_call_ns != 0;
            stream->d_lead++;
            if(woke) stream->d_lead_woke++; else stream->d_lead_nowake++;
            if(++stream->d_rows <= 64 || !(stream->d_rows & 63))
                fprintf(stderr, "[WALSA] leadin n=%u held=%u in_alsa=%u avail=%ld queued=%lld woke=%d lat_us=%lld "
                        "since_event_us=%lld rel_tick=%u since_rel_us=%lld\\n",
                        stream->d_rows, stream->held_frames, stream->data_in_alsa_frames, (long)avail,
                        (long long)stream->queued_frames, woke,
                        woke ? (long long)((stream->d_call_ns - stream->d_event_ns) / 1000) : -1LL,
                        stream->d_event_ns ? (long long)((now - stream->d_event_ns) / 1000) : -1LL,
                        stream->d_rel_tick,
                        stream->d_rel_ns ? (long long)((now - stream->d_rel_ns) / 1000) : -1LL);
        }""")
# gated count: else-branch after the lead-in block is hard; count where gate blocks
g = rep(g, """    if(stream->data_in_alsa_frames == 0 && stream->held_frames < stream->alsa_period_frames &&
            stream->queued_frames < (INT64)(%d * stream->alsa_period_frames))""" % GATE, """    if(stream->data_in_alsa_frames == 0 && stream->held_frames < stream->alsa_period_frames &&
            stream->queued_frames >= (INT64)(%d * stream->alsa_period_frames))
        stream->d_gated++;
    if(stream->data_in_alsa_frames == 0 && stream->held_frames < stream->alsa_period_frames &&
            stream->queued_frames < (INT64)(%d * stream->alsa_period_frames))""" % (GATE, GATE))
# the event
g = rep(g, """        max_copy_frames -= written;
    }

    if(stream->event)
        NtSetEvent(stream->event, NULL);
}""", """        max_copy_frames -= written;
    }

    stream->d_rel_tick = 0;
    stream->d_event_ns = alsa_now_ns();
    stream->d_call_ns = 0;
    stream->d_events++;
    if(stream->event)
        NtSetEvent(stream->event, NULL);
}""")
g = rep(g, """    /* padding is solely updated at callback time in shared mode */
    *params->padding = stream->held_frames;""", """    /* padding is solely updated at callback time in shared mode */
    *params->padding = stream->held_frames;
    if(stream->flow == eRender) diag_client_call(stream);""")
g = rep(g, """    if(stream->getbuf_last)
        return alsa_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);

    if(!frames)
        return alsa_unlock_result(stream, &params->result, S_OK);

    /* held_frames == GetCurrentPadding_nolock(); */""", """    if(stream->getbuf_last)
        return alsa_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);

    diag_client_call(stream);
    if(!frames)
        return alsa_unlock_result(stream, &params->result, S_OK);

    /* held_frames == GetCurrentPadding_nolock(); */""")
g = rep(g, """    if(params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer(stream, buffer, written_frames);
""", """    if(params->flags & AUDCLNT_BUFFERFLAGS_SILENT){
        silence_buffer(stream, buffer, written_frames);
        stream->d_silent += written_frames;
    }
    stream->d_rel_tick += written_frames;
    stream->d_rel_ns = alsa_now_ns();
""")
open(f"{base}/c/dlls/winealsa.drv/alsa.c", "w").write(g)
print("ok gate", GATE)
