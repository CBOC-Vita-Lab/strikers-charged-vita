// np_json.h limits (host test): text over 256 KiB is refused without parsing; a normal document and one just under the cap still parse.
// g++ -std=c++17 -I. tools/net/json_cap_test.cpp -o /tmp/json_cap_test && /tmp/json_cap_test
#include "platform/sdk/net/online/np_json.h"
#include <cstdio>
int main() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++bad; } };
    NpNet::Json j;
    const char* small = "{\"a\":1,\"b\":[1,2,3]}";
    check(j.Parse(small, strlen(small)), "small document parses");
    std::string under = "{\"s\":\"" + std::string(256 * 1024 - 20, 'x') + "\"}";
    check(under.size() <= 256 * 1024 && j.Parse(under.data(), under.size()), "document just under the cap parses");
    std::string over = "{\"s\":\"" + std::string(256 * 1024, 'x') + "\"}";
    check(!j.Parse(over.data(), over.size()), "document over the cap is refused");
    check(j.Root() < 0, "a refused parse leaves no root");
    check(!j.Parse(nullptr, 5), "null text is refused");
    std::printf(bad ? "json_cap_test: FAILED\n" : "json_cap_test: PASS\n");
    return bad ? 1 : 0;
}
