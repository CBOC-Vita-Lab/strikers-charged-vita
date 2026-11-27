#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ssl_misc.h"
#include "ssl_tls13_keys.h"
#include "mbedtls/ssl_ciphersuites.h"

#if defined(MBEDTLS_USE_PSA_CRYPTO) || !defined(MBEDTLS_SSL_PROTO_TLS1_3)
#error This test requires the shipped non-PSA TLS 1.3 profile.
#endif
#if defined(MBEDTLS_PLATFORM_MEMORY) || defined(MBEDTLS_DEBUG_C)
#error The calloc wrapper and null debug context require the shipped profile.
#endif

struct allocation { void *ptr; size_t bytes; };
static struct allocation live[32];
static int armed, fail_nth, attempts, harness_errors, checks, failures;

void *__real_calloc(size_t count, size_t size);
void __real_free(void *ptr);

void *__wrap_calloc(size_t count, size_t size)
{
    void *ptr;
    unsigned i;
    if (armed && ++attempts == fail_nth)
        return NULL;
    ptr = __real_calloc(count, size);
    if (armed && ptr != NULL) {
        for (i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
            if (live[i].ptr == NULL) {
                live[i].ptr = ptr;
                live[i].bytes = count * size;
                return ptr;
            }
        }
        ++harness_errors;
    }
    return ptr;
}

void __wrap_free(void *ptr)
{
    unsigned i;
    for (i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
        if (ptr != NULL && live[i].ptr == ptr) {
            live[i].ptr = NULL;
            live[i].bytes = 0;
            break;
        }
    }
    __real_free(ptr);
}

static unsigned live_count(size_t *bytes)
{
    unsigned i, count = 0;
    *bytes = 0;
    for (i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
        if (live[i].ptr != NULL) {
            ++count;
            *bytes += live[i].bytes;
        }
    }
    return count;
}

static void check(int pass, const char *label)
{
    ++checks;
    if (!pass) {
        ++failures;
        printf("FAIL %s\n", label);
    }
}

static void run_case(const char *label, int nth, int endpoint,
                     int ciphersuite, int expect_error)
{
    mbedtls_ssl_transform transform;
    mbedtls_ssl_key_set keys;
    size_t bytes;
    unsigned count;
    int ret;
    memset(&keys, 0, sizeof(keys));
    keys.key_len = 16;
    keys.iv_len = 12;
    mbedtls_ssl_transform_init(&transform);
    fail_nth = nth;
    attempts = 0;
    armed = 1;
    ret = mbedtls_ssl_tls13_populate_transform(
        &transform, endpoint, ciphersuite, &keys, NULL);
    armed = 0;
    count = live_count(&bytes);
    printf("%s fail_nth=%d ret=%d attempts=%d live=%u bytes=%lu\n",
           label, nth, ret, attempts, count, (unsigned long) bytes);
    check(expect_error ? ret < 0 : ret == 0, "expected result");
    if (expect_error) {
        /* Real callers free only the outer transform on this failure path. */
        check(count == 0, "error leaves no nested allocation before fixture cleanup");
    } else {
        check(count == 4 && bytes == 1336 && attempts == 4,
              "success uses the real host32 AES-GCM allocation path");
        check(transform.tls_version == MBEDTLS_SSL_VERSION_TLS1_3 &&
              transform.ivlen == 12 && transform.taglen == 16,
              "success transform fields");
    }
    mbedtls_ssl_transform_free(&transform);
    check(live_count(&bytes) == 0, "fixture cleanup releases all allocations");
    check(harness_errors == 0, "allocation tracker capacity");
}

int main(void)
{
    int nth;
    run_case("success", 0, MBEDTLS_SSL_IS_CLIENT,
             MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 0);
    for (nth = 1; nth <= 4; ++nth)
        run_case("allocation_failure", nth, MBEDTLS_SSL_IS_CLIENT,
                 MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 1);
    run_case("invalid_endpoint", 0, -1,
             MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 1);
    run_case("unknown_ciphersuite", 0, MBEDTLS_SSL_IS_CLIENT, -1, 1);
    printf("populate_transform allocation checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
