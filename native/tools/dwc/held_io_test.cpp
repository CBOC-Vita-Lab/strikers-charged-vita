// The friends list's held poll must carry its hold to the HTTPS layer as the no-data limit (HttpRequest::ioTimeoutMs = hold + 10 s), and an
// ordinary request must not (0 = the transport's short default). A recording transport stands in for the network.
#include <stdio.h>
#include <string>
#include <vector>

#include "platform/sdk/dwc/np_online_api.h"
#include "platform/sdk/dwc/np_online_http.h"

using namespace NpOnline;
static int g_bad = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: %s -- ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_bad; } } while (0)

struct Rec : HttpTransport {
    std::vector<HttpRequest> seen;
    uint32_t Start(const HttpRequest& r) override { seen.push_back(r); return uint32_t(seen.size()); }
    bool Poll(uint32_t, HttpResponse*) override { return false; }
    void Cancel(uint32_t) override {}
};

int main() {
    Rec* rec = new Rec;
    Api* api = CreateHttpApi(rec);
    FriendsRequest fr;
    fr.waitSec = 0;
    CHECK(api->BeginFriends(fr) != 0, "plain friends request starts");
    const HttpRequest plain = rec->seen.back();
    fr.waitSec = 20;
    snprintf(fr.rev, sizeof fr.rev, "%s", "abc");
    CHECK(api->BeginFriends(fr) != 0, "held friends request starts");
    const HttpRequest held = rec->seen.back();
    printf("plain: %s total %u io %u\nheld:  %s total %u io %u\n", plain.path.c_str(), plain.timeoutMs, plain.ioTimeoutMs, held.path.c_str(), held.timeoutMs, held.ioTimeoutMs);
    CHECK(plain.ioTimeoutMs == 0, "an ordinary request keeps the transport's short no-data limit");
    CHECK(held.path.find("wait=20") != std::string::npos, "the hold is asked for");
    CHECK(held.timeoutMs == plain.timeoutMs + 20000, "the hold is added to the total limit");
    CHECK(held.ioTimeoutMs == 30000, "no data for the hold plus 10 s is allowed: %u", held.ioTimeoutMs);
    fr.waitSec = 99;   // the service holds 55 s at most
    CHECK(api->BeginFriends(fr) != 0, "capped hold starts");
    CHECK(rec->seen.back().ioTimeoutMs == 65000, "hold capped at 55 s: %u", rec->seen.back().ioTimeoutMs);
    if (!g_bad) printf("held_io_test: ok\n");
    return g_bad;
}
