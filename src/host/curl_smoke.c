/*
 * curl_smoke.c - host smoke for the vendored libcurl 8.22.0 built against
 * the vendored mbedTLS 3.6.7 (L6, browser plan AD-7).
 *
 * Two checks:
 *   1. capability check via curl_version()/curl_version_info(): the TLS
 *      backend must be mbedTLS and the protocol set must include
 *      http + https + file.
 *   2. one HTTPS HEAD request (real TLS handshake + HTTP round trip).
 *
 * Built and run by src/host/build_curl_host.sh; exits 0 only when every
 * check passes.  Host-only tooling: no HobbyOS code here.
 */
#include <stdio.h>
#include <string.h>

#include <curl/curl.h>

static int failures = 0;

static int proto_listed(const char *const *protocols, const char *want) {
  const char *const *p;

  for (p = protocols; *p != NULL; p++) {
    if (strcmp(*p, want) == 0) {
      return 1;
    }
  }
  return 0;
}

static void check_capabilities(void) {
  const curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
  const char *const *p;
  int protocol_count = 0;

  for (p = vi->protocols; *p != NULL; p++) {
    protocol_count++;
  }
  printf("curl_version() = %s\n", curl_version());
  printf("version=%s ssl_version=%s\n", vi->version,
         vi->ssl_version != NULL ? vi->ssl_version : "(none)");
  printf("zlib=%s brotli=%s zstd=%s\n",
         vi->libz_version != NULL ? vi->libz_version : "(none)",
         vi->brotli_version != NULL ? vi->brotli_version : "(none)",
         vi->zstd_version != NULL ? vi->zstd_version : "(none)");
  printf("features=0x%lx protocol_count=%d\n", (unsigned long)vi->features,
         protocol_count);
  printf("protocols =");
  for (p = vi->protocols; *p != NULL; p++) {
    printf(" %s", *p);
  }
  printf("\n");

  if (vi->ssl_version == NULL || strncmp(vi->ssl_version, "mbedTLS", 7) != 0) {
    printf("FAIL: TLS backend is not mbedTLS\n");
    failures++;
  } else {
    printf("PASS: TLS backend = %s\n", vi->ssl_version);
  }
  if (!proto_listed(vi->protocols, "http") ||
      !proto_listed(vi->protocols, "https") ||
      !proto_listed(vi->protocols, "file")) {
    printf("FAIL: http/https/file not all present in protocol set\n");
    failures++;
  } else {
    printf("PASS: http + https + file present\n");
  }
  if ((vi->features & CURL_VERSION_SSL) == 0) {
    printf("FAIL: CURL_VERSION_SSL not set\n");
    failures++;
  } else {
    printf("PASS: CURL_VERSION_SSL set\n");
  }
}

static void check_https_head(const char *url) {
  CURL *h;
  CURLcode res;
  long http_code = 0;
  long verify_result = -1;

  h = curl_easy_init();
  if (h == NULL) {
    printf("FAIL: curl_easy_init\n");
    failures++;
    return;
  }
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "hobbyos-l6-curl-smoke/1.0");

  res = curl_easy_perform(h);
  if (res == CURLE_OK) {
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(h, CURLINFO_SSL_VERIFYRESULT, &verify_result);
  }
  printf("HEAD %s -> res=%d (%s) http=%ld ssl_verify=%ld\n", url, (int)res,
         curl_easy_strerror(res), http_code, verify_result);
  if (res != CURLE_OK) {
    printf("FAIL: HTTPS HEAD request did not complete\n");
    failures++;
  } else if (http_code < 200 || http_code >= 400) {
    printf("FAIL: HTTPS HEAD returned HTTP %ld\n", http_code);
    failures++;
  } else if (verify_result != 0) {
    printf("FAIL: certificate verification result %ld\n", verify_result);
    failures++;
  } else {
    printf("PASS: HTTPS HEAD request (TLS handshake + HTTP round trip)\n");
  }
  curl_easy_cleanup(h);
}

int main(void) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    printf("FAIL: curl_global_init\n");
    return 1;
  }
  check_capabilities();
  check_https_head("https://example.com/");
  curl_global_cleanup();

  if (failures != 0) {
    printf("SMOKE FAIL: %d check(s) failed\n", failures);
    return 1;
  }
  printf("SMOKE PASS: libcurl capabilities + HTTPS HEAD OK\n");
  return 0;
}
