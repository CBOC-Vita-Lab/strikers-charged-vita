// Host test of platform/sdk/dwc/np_pow.h (the registration proof of work solver, wave 14 / gxnet14). The expected nonces are the service's reference solver's (server/src/core/pow.js
// solve(): base 36 from 0, SHA-256(challenge ":" nonce) with `bits` leading zero bits); SHA-256 itself is checked against the FIPS vectors.
//   clang++ -std=gnu++11 -I. -o /tmp/pow_test tools/net/pow_test.cpp && /tmp/pow_test
#include <stdio.h>
#include <string.h>
#include <string>
#include "platform/sdk/dwc/np_pow.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static std::string Hex(const uint8_t* d, int n) { char b[3]; std::string s; for (int i = 0; i < n; ++i) { snprintf(b, sizeof b, "%02x", d[i]); s += b; } return s; }
static std::string Sha(const std::string& m) { uint8_t d[32]; NpPow::Sha256(d, m.data(), m.size()); return Hex(d, 32); }

static std::string Solve(const char* challenge, int bits, int* rc = nullptr) {
    NpPow::Solver s;
    if (!s.Start(challenge, bits)) { if (rc) *rc = -2; return ""; }
    int r = 0;
    while ((r = s.Step(1000)) == 0) {}
    if (rc) *rc = r;
    return r == 1 ? s.nonce : "";
}

int main() {
    // FIPS 180-4 / NIST vectors (one block, two blocks, the million-a is skipped)
    CHECK(Sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Sha(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");   // the edge of one block
    CHECK(Sha(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");   // needs a second block
    // the same nonces as the service's reference solver
    CHECK(Solve("1790000000000.0123456789abcdef.8.fedcba9876543210", 8) == "48");
    CHECK(Solve("1790000000000.0123456789abcdef.12.fedcba9876543210", 12) == "1qh");
    CHECK(Solve("1790000000000.a1b2c3d4e5f60718.16.00112233445566ff", 16) == "1mol");
    // the answer really has the zero bits (an independent check by hand)
    {
        const std::string ch = "1790000000000.a1b2c3d4e5f60718.16.00112233445566ff", n = Solve(ch.c_str(), 16);
        uint8_t d[32]; const std::string m = ch + ":" + n; NpPow::Sha256(d, m.data(), m.size());
        CHECK(NpPow::LeadingZeroBits(d, 32) >= 16);
        CHECK(d[0] == 0 && d[1] == 0);
    }
    // limits: unusable challenges and difficulties are refused; a difficulty above the client's limit is not attempted
    { NpPow::Solver s; CHECK(!s.Start("", 8)); CHECK(!s.Start(nullptr, 8)); CHECK(!s.Start(std::string(81, '1').c_str(), 8)); CHECK(!s.Start("1.2.3.4", NpPow::kMaxBits + 1)); CHECK(!s.Start("1.2.3.4", -1)); CHECK(s.Start("1.2.3.4", NpPow::kMaxBits)); }
    CHECK(NpPow::kMaxBits == 17);
    // a solver with a try limit gives up (-1) instead of grinding for ever
    { int rc = 0; Solve("1790000000000.0123456789abcdef.8.fedcba9876543210", 8, &rc); CHECK(rc == 1); NpPow::Solver s; s.Start("1790000000000.0123456789abcdef.20.fedcba9876543210", 17, 50); int r; while ((r = s.Step(10)) == 0) {} CHECK(r == -1); }
    // slices: the same answer however the work is cut
    { NpPow::Solver s; s.Start("1790000000000.0123456789abcdef.12.fedcba9876543210", 12); int r; int calls = 0; while ((r = s.Step(7)) == 0) ++calls; CHECK(r == 1 && std::string(s.nonce) == "1qh" && calls > 10); }
    printf(fails ? "pow_test: %d FAILED\n" : "pow_test: all passed\n", fails);
    return fails ? 1 : 0;
}
