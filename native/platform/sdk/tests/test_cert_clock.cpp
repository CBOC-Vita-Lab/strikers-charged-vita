// np_https.h: a certificate refused only for its dates is reported as a wrong date and time (NP_HTTPS_E_CLOCK), every other certificate fault is not.
//   c++ -std=c++11 -Wall -Wextra -I<native> native/platform/sdk/tests/test_cert_clock.cpp -o /tmp/test_cert_clock && /tmp/test_cert_clock
#include "platform/sdk/net/np_https.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    CHECK(np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED));
    CHECK(np_https_cert_is_date_fault(NP_HTTPS_CERT_NOT_YET_VALID));
    CHECK(np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_NOT_YET_VALID));
    // negative controls: another fault next to the dates, or no date fault at all, is not a clock problem
    CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_HOST_MISMATCH));
    CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_UNTRUSTED));
    CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_NOT_YET_VALID | NP_HTTPS_CERT_BAD_SIGNATURE));
    CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_UNTRUSTED));
    CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_HOST_MISMATCH));
    CHECK(!np_https_cert_is_date_fault(NP_HTTPS_CERT_OTHER));
    CHECK(!np_https_cert_is_date_fault(0));
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
