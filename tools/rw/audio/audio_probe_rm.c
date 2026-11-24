/* audio_probe_rm.c — reproduce RimWorld/FMOD's first two audio probes inside
 * the HobbyOS Linux-ABI guest, against the DSOs the RimWorld harness stages in
 * /LIB64 (src/user/xcompat/pulse_null.c + alsa_null.c).
 *
 * Prints one row per probe so the serial log names the exact call that
 * refuses: pa_simple_new() -> *error, snd_device_name_hint() -> count,
 * snd_pcm_open("default") -> rc.  This measures the *library* half; the raw
 * ALSA-ABI half is src/user/linux_probe_snd.c (probe_snd.elf).
 *
 * Build (host gcc, dynamic; the guest stages the host's glibc loader/libs):
 *   gcc -O1 -o audio_probe_rm.elf audio_probe_rm.c -ldl \
 *     -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 -Wl,-rpath,/LIB64
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

typedef struct { int format; unsigned rate; unsigned char channels; } pa_ss;
typedef void *(*pa_simple_new_t)(const char*, const char*, int, const char*,
                                 const char*, const pa_ss*, const void*,
                                 const void*, int*);
typedef int (*snd_hint_t)(int, const char*, void***);
typedef int (*snd_open_t)(void**, const char*, int, int);
typedef const char* (*snd_strerror_t)(int);

int main(void) {
  /* ---- FMOD's first choice: libpulse-simple.so.0 --------------------- */
  void *ph = dlopen("libpulse-simple.so.0", RTLD_NOW);
  if (!ph) {
    printf("[RMAP] pulse dlopen FAILED: %s\n", dlerror());
  } else {
    printf("[RMAP] pulse dlopen ok handle=%p\n", ph);
    pa_simple_new_t newp = (pa_simple_new_t)dlsym(ph, "pa_simple_new");
    if (!newp) {
      printf("[RMAP] pulse dlsym pa_simple_new FAILED: %s\n", dlerror());
    } else {
      pa_ss ss = { 0 /*PA_SAMPLE_S16LE*/, 48000, 2 };
      int err = -1;
      void *s = newp(0, "rimworld", 1 /*playback*/, 0, "game",
                     &ss, 0, 0, &err);
      printf("[RMAP] pa_simple_new -> stream=%p error=%d\n", s, err);
    }
  }

  /* ---- FMOD's fallback: libasound.so.2 ------------------------------- */
  void *ah = dlopen("libasound.so.2", RTLD_NOW);
  if (!ah) {
    printf("[RMAP] alsa dlopen FAILED: %s\n", dlerror());
    printf("[RMAP] done\n");
    return 0;
  }
  printf("[RMAP] alsa dlopen ok handle=%p\n", ah);
  snd_hint_t hint = (snd_hint_t)dlsym(ah, "snd_device_name_hint");
  snd_open_t openp = (snd_open_t)dlsym(ah, "snd_pcm_open");
  snd_strerror_t strerr = (snd_strerror_t)dlsym(ah, "snd_strerror");
  if (hint) {
    void **h = 0;
    int rc = hint(-1, "pcm", &h);
    int n = 0;
    if (h) while (h[n]) n++;
    printf("[RMAP] snd_device_name_hint -> rc=%d devices=%d\n", rc, n);
  } else {
    printf("[RMAP] alsa has no snd_device_name_hint: %s\n", dlerror());
  }
  if (openp) {
    void *pcm = 0;
    errno = 0;
    int rc = openp(&pcm, "default", 0 /*playback*/, 0);
    printf("[RMAP] snd_pcm_open(\"default\") -> rc=%d pcm=%p errno=%d%s%s\n",
           rc, pcm, errno,
           strerr ? " text=" : "", strerr ? strerr(rc) : "");
  } else {
    printf("[RMAP] alsa has no snd_pcm_open: %s\n", dlerror());
  }
  printf("[RMAP] done\n");
  return 0;
}
