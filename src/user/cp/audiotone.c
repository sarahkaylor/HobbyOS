/* src/user/cp/audiotone.c -- Windows audio probe for Wine's winealsa.drv
 * (lane C4-audio, docs/cp-audio.md).  An x86_64 PE console program built by
 * tools/cp/p5/build-pe.sh like the other P5 probes (only kernel32/user32/
 * gdi32/ucrtbase are imported; ole32 and winmm are LoadLibrary'd).
 *
 *   audiotone.exe wasapi MS   880 Hz through the default render endpoint:
 *                             IMMDeviceEnumerator -> IAudioClient (shared,
 *                             event-less, 200 ms buffer) -> IAudioRenderClient
 *                             -- the path Wwise takes.
 *   audiotone.exe waveout MS  550 Hz through winmm waveOutOpen(WAVE_MAPPER),
 *                             16-bit 48 kHz stereo, four 100 ms headers.
 *
 * Rows "[AUD] ..." on stdout; the last is "[AUD] <mode> done rc=<n>".  The
 * host grades the guest's audio in the QEMU WAV (tools/cp/audio/wavcheck.py
 * --freq 880 / 550). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("[AUD] ");
  vprintf(fmt, ap);
  printf("\n");
  fflush(stdout);
  va_end(ap);
}

static double ph;
static void fill(void *buf, UINT32 frames, int ch, int is_float, int bits, double hz, double rate) {
  for (UINT32 i = 0; i < frames; i++) {
    double s = 0.5 * sin(ph);
    ph += 2 * 3.14159265358979 * hz / rate;
    if (ph > 2 * 3.14159265358979) ph -= 2 * 3.14159265358979;
    for (int c = 0; c < ch; c++) {
      if (is_float) ((float *)buf)[i * ch + c] = (float)s;
      else if (bits == 16) ((short *)buf)[i * ch + c] = (short)(s * 32767);
      else ((int *)buf)[i * ch + c] = (int)(s * 2147483647.0);
    }
  }
}

typedef HRESULT(WINAPI *pCoInit)(void *, DWORD);
typedef HRESULT(WINAPI *pCoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, void **);
typedef void(WINAPI *pCoFree)(void *);

static int run_wasapi(int ms) {
  HMODULE ole = LoadLibraryA("ole32.dll");
  pCoInit coinit = ole ? (pCoInit)(void *)GetProcAddress(ole, "CoInitializeEx") : 0;
  pCoCreate cocreate = ole ? (pCoCreate)(void *)GetProcAddress(ole, "CoCreateInstance") : 0;
  pCoFree cofree = ole ? (pCoFree)(void *)GetProcAddress(ole, "CoTaskMemFree") : 0;
  if (!coinit || !cocreate || !cofree) { row("wasapi: no ole32"); return 1; }
  HRESULT hr = coinit(NULL, COINIT_MULTITHREADED);
  IMMDeviceEnumerator *en = 0;
  hr = cocreate(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
  row("wasapi: CoCreateInstance(MMDeviceEnumerator) hr=%#lx", (unsigned long)hr);
  if (FAILED(hr)) return 1;
  IMMDeviceCollection *col = 0;
  UINT cnt = 0;
  if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) {
    IMMDeviceCollection_GetCount(col, &cnt);
    IMMDeviceCollection_Release(col);
  }
  row("wasapi: %u active render endpoint(s)", cnt);
  IMMDevice *dev = 0;
  hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
  row("wasapi: GetDefaultAudioEndpoint hr=%#lx", (unsigned long)hr);
  if (FAILED(hr)) return 1;
  WCHAR *id = 0;
  if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) {
    char a[256];
    WideCharToMultiByte(CP_UTF8, 0, id, -1, a, sizeof a, 0, 0);
    row("wasapi: endpoint id %s", a);
    cofree(id);
  }
  IAudioClient *ac = 0;
  hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
  row("wasapi: Activate(IAudioClient) hr=%#lx", (unsigned long)hr);
  if (FAILED(hr)) return 1;
  WAVEFORMATEX *wf = 0;
  hr = IAudioClient_GetMixFormat(ac, &wf);
  if (FAILED(hr)) { row("wasapi: GetMixFormat hr=%#lx", (unsigned long)hr); return 1; }
  int is_float = wf->wFormatTag == 3;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    is_float = ((WAVEFORMATEXTENSIBLE *)wf)->SubFormat.Data1 == 3;
  row("wasapi: mix format tag=%#x ch=%u rate=%lu bits=%u float=%d", wf->wFormatTag, wf->nChannels,
      (unsigned long)wf->nSamplesPerSec, wf->wBitsPerSample, is_float);
  hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, wf, NULL);
  row("wasapi: Initialize(shared, 200 ms) hr=%#lx", (unsigned long)hr);
  if (FAILED(hr)) return 1;
  UINT32 bufsz = 0;
  IAudioClient_GetBufferSize(ac, &bufsz);
  REFERENCE_TIME lat = 0, defp = 0, minp = 0;
  IAudioClient_GetStreamLatency(ac, &lat);
  IAudioClient_GetDevicePeriod(ac, &defp, &minp);
  row("wasapi: buffer %u frames, latency %lld00 ns, period %lld00 ns", bufsz, (long long)lat, (long long)defp);
  IAudioRenderClient *rc = 0;
  hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
  if (FAILED(hr)) { row("wasapi: GetService(render) hr=%#lx", (unsigned long)hr); return 1; }
  UINT64 total = (UINT64)wf->nSamplesPerSec * ms / 1000, done = 0;
  BYTE *p = 0;
  if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, bufsz / 2, &p))) {
    fill(p, bufsz / 2, wf->nChannels, is_float, wf->wBitsPerSample, 880.0, wf->nSamplesPerSec);
    IAudioRenderClient_ReleaseBuffer(rc, bufsz / 2, 0);
    done += bufsz / 2;
  }
  DWORD t0 = GetTickCount();
  hr = IAudioClient_Start(ac);
  row("wasapi: Start hr=%#lx", (unsigned long)hr);
  int errs = 0;
  while (done < total) {
    UINT32 pad = 0;
    if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) { errs++; break; }
    UINT32 n = bufsz - pad;
    if (n > total - done) n = (UINT32)(total - done);
    if (n > 0 && SUCCEEDED(IAudioRenderClient_GetBuffer(rc, n, &p))) {
      fill(p, n, wf->nChannels, is_float, wf->wBitsPerSample, 880.0, wf->nSamplesPerSec);
      IAudioRenderClient_ReleaseBuffer(rc, n, 0);
      done += n;
    }
    Sleep(10);
    if (GetTickCount() - t0 > (DWORD)ms * 4 + 5000) { errs++; row("wasapi: timeout"); break; }
  }
  for (int i = 0; i < 100; i++) {
    UINT32 pad = 0;
    IAudioClient_GetCurrentPadding(ac, &pad);
    if (!pad) break;
    Sleep(10);
  }
  DWORD el = GetTickCount() - t0;
  UINT64 pos = 0, freq = 0;
  IAudioClock *clk = 0;
  if (SUCCEEDED(IAudioClient_GetService(ac, &IID_IAudioClock, (void **)&clk))) {
    IAudioClock_GetFrequency(clk, &freq);
    IAudioClock_GetPosition(clk, &pos, NULL);
    IAudioClock_Release(clk);
  }
  IAudioClient_Stop(ac);
  row("wasapi: wrote %llu frames (%.2f s) in %lu ms; clock %llu/%llu", (unsigned long long)done,
      (double)done / wf->nSamplesPerSec, (unsigned long)el, (unsigned long long)pos, (unsigned long long)freq);
  int bad_pace = el < (DWORD)ms * 8 / 10;
  if (bad_pace) row("wasapi: FAIL played faster than real time");
  IAudioRenderClient_Release(rc);
  IAudioClient_Release(ac);
  IMMDevice_Release(dev);
  IMMDeviceEnumerator_Release(en);
  return errs + bad_pace;
}

typedef MMRESULT(WINAPI *pwOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT(WINAPI *pwHdr)(HWAVEOUT, LPWAVEHDR, UINT);
typedef MMRESULT(WINAPI *pwH)(HWAVEOUT);
typedef UINT(WINAPI *pwNum)(void);

static int run_waveout(int ms) {
  HMODULE wm = LoadLibraryA("winmm.dll");
  if (!wm) { row("waveout: no winmm"); return 1; }
  pwNum num = (pwNum)(void *)GetProcAddress(wm, "waveOutGetNumDevs");
  pwOpen wopen = (pwOpen)(void *)GetProcAddress(wm, "waveOutOpen");
  pwHdr prep = (pwHdr)(void *)GetProcAddress(wm, "waveOutPrepareHeader");
  pwHdr unprep = (pwHdr)(void *)GetProcAddress(wm, "waveOutUnprepareHeader");
  pwHdr wwrite = (pwHdr)(void *)GetProcAddress(wm, "waveOutWrite");
  pwH wclose = (pwH)(void *)GetProcAddress(wm, "waveOutClose");
  if (!num || !wopen || !prep || !unprep || !wwrite || !wclose) { row("waveout: missing exports"); return 1; }
  row("waveout: %u device(s)", num());
  WAVEFORMATEX f;
  memset(&f, 0, sizeof f);
  f.wFormatTag = WAVE_FORMAT_PCM;
  f.nChannels = 2;
  f.nSamplesPerSec = 48000;
  f.wBitsPerSample = 16;
  f.nBlockAlign = 4;
  f.nAvgBytesPerSec = 48000 * 4;
  HWAVEOUT h = 0;
  MMRESULT mr = wopen(&h, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL);
  row("waveout: waveOutOpen(WAVE_MAPPER, 48000/16/2) = %u", mr);
  if (mr) return 1;
  enum { NB = 4, FR = 4800 };
  static short pcm[NB][FR * 2];
  WAVEHDR hd[NB];
  memset(hd, 0, sizeof hd);
  int total = 48 * ms, queued = 0, errs = 0;
  DWORD t0 = GetTickCount();
  for (int i = 0; i < NB; i++) {
    fill(pcm[i], FR, 2, 0, 16, 550.0, 48000);
    hd[i].lpData = (LPSTR)pcm[i];
    hd[i].dwBufferLength = sizeof pcm[i];
    prep(h, &hd[i], sizeof hd[i]);
    if (wwrite(h, &hd[i], sizeof hd[i])) errs++;
    queued += FR;
  }
  while (queued < total) {
    for (int i = 0; i < NB && queued < total; i++) {
      if (!(hd[i].dwFlags & WHDR_DONE)) continue;
      fill(pcm[i], FR, 2, 0, 16, 550.0, 48000);
      hd[i].dwFlags &= ~WHDR_DONE;
      if (wwrite(h, &hd[i], sizeof hd[i])) errs++;
      queued += FR;
    }
    Sleep(5);
    if (GetTickCount() - t0 > (DWORD)ms * 4 + 5000) { errs++; row("waveout: timeout"); break; }
  }
  for (int k = 0; k < 400; k++) {
    int all = 1;
    for (int i = 0; i < NB; i++) all &= (hd[i].dwFlags & WHDR_DONE) != 0;
    if (all) break;
    Sleep(5);
  }
  DWORD el = GetTickCount() - t0;
  for (int i = 0; i < NB; i++) unprep(h, &hd[i], sizeof hd[i]);
  wclose(h);
  row("waveout: queued %d frames (%.2f s) in %lu ms", queued, queued / 48000.0, (unsigned long)el);
  int bad_pace = el < (DWORD)ms * 8 / 10;
  if (bad_pace) row("waveout: FAIL played faster than real time");
  return errs + bad_pace;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "wasapi";
  int ms = argc > 2 ? atoi(argv[2]) : 2000;
  if (ms <= 0) ms = 2000;
  int rc = !strcmp(mode, "waveout") ? run_waveout(ms) : run_wasapi(ms);
  printf("[AUD] %s done rc=%d\n", mode, rc);
  fflush(stdout);
  return rc;
}
