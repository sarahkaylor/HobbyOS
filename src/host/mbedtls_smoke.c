/*
 * mbedtls_smoke.c - host smoke for the vendored mbedTLS 3.6.7 (L6, browser
 * plan AD-7): the RNG path curl's mbedTLS backend uses (entropy -> CTR_DRBG)
 * plus SHA-256 against FIPS 180-4 known-answer vectors.
 *
 * Built and run by src/host/build_mbedtls_host.sh; exits 0 only when every
 * check passes.  Host-only tooling: no HobbyOS code here.
 */
#include <stdio.h>
#include <string.h>

#include <mbedtls/build_info.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>

static int failures = 0;

static void to_hex(const unsigned char *in, size_t len, char *out) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < len; i++) {
    out[i * 2] = hex[in[i] >> 4];
    out[i * 2 + 1] = hex[in[i] & 15];
  }
  out[len * 2] = '\0';
}

static void sha256_kat(const char *what, const unsigned char *in, size_t len,
                       const char *want) {
  unsigned char out[32];
  char got[65];

  if (mbedtls_sha256(in, len, out, 0) != 0) {
    printf("FAIL: sha256(%s) call failed\n", what);
    failures++;
    return;
  }
  to_hex(out, 32, got);
  printf("sha256(%s) = %s\n", what, got);
  if (strcmp(got, want) != 0) {
    printf("FAIL: sha256(%s) want %s\n", what, want);
    failures++;
    return;
  }
  printf("PASS: sha256(%s)\n", what);
}

int main(void) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  unsigned char rnd1[32];
  unsigned char rnd2[32];
  const char *pers = "hobbyos-l6-mbedtls-smoke";
  char hex1[65];
  char hex2[65];
  int ret;
  int i;
  int all_zero;

  printf("mbedTLS %s (MBEDTLS_VERSION_NUMBER 0x%08x)\n",
         MBEDTLS_VERSION_STRING, MBEDTLS_VERSION_NUMBER);

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);

  ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers));
  if (ret != 0) {
    printf("FAIL: ctr_drbg_seed ret=%d\n", ret);
    return 1;
  }
  ret = mbedtls_ctr_drbg_random(&drbg, rnd1, sizeof(rnd1));
  if (ret == 0) {
    ret = mbedtls_ctr_drbg_random(&drbg, rnd2, sizeof(rnd2));
  }
  if (ret != 0) {
    printf("FAIL: ctr_drbg_random ret=%d\n", ret);
    return 1;
  }
  to_hex(rnd1, sizeof(rnd1), hex1);
  to_hex(rnd2, sizeof(rnd2), hex2);
  printf("rng block 1 = %s\n", hex1);
  printf("rng block 2 = %s\n", hex2);
  all_zero = 1;
  for (i = 0; i < (int)sizeof(rnd1); i++) {
    if (rnd1[i] != 0) {
      all_zero = 0;
      break;
    }
  }
  if (all_zero || memcmp(rnd1, rnd2, sizeof(rnd1)) == 0) {
    printf("FAIL: rng blocks identical or all-zero\n");
    failures++;
  } else {
    printf("PASS: rng (two 32-byte draws differ, non-zero)\n");
  }

  sha256_kat("empty", (const unsigned char *)"", 0,
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  sha256_kat("abc", (const unsigned char *)"abc", 3,
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);

  if (failures != 0) {
    printf("SMOKE FAIL: %d check(s) failed\n", failures);
    return 1;
  }
  printf("SMOKE PASS: mbedTLS RNG + SHA-256 vectors OK\n");
  return 0;
}
