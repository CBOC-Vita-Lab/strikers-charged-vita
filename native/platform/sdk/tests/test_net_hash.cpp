// MD5 and HMAC-MD5 of the SDK's net module (the game signs its ranking submissions with them) against
// the published test vectors: RFC 1321 and RFC 2202.
#include <revolution/net/NETDigest.h>

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static void hex(const unsigned char* d, char* out) {
    for (int i = 0; i < 16; ++i) sprintf(out + 2 * i, "%02x", d[i]);
}

static void md5_of(const char* text, char* out) {
    NETMD5Context c;
    unsigned char digest[16];
    NETMD5Init(&c);
    NETMD5Update(&c, (void*)text, (u32)strlen(text));
    NETMD5GetDigest(&c, digest);
    hex(digest, out);
}

int main() {
    char h[40];
    md5_of("", h);
    CHECK(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"));
    md5_of("abc", h);
    CHECK(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"));
    md5_of("12345678901234567890123456789012345678901234567890123456789012345678901234567890", h);
    CHECK(!strcmp(h, "57edf4a22be3c955ac49da2e2107b67a"));
    // HMAC-MD5, RFC 2202 test case 2
    NETHMACContext ctx;
    unsigned char digest[16];
    char key[] = "Jefe";
    const char data[] = "what do ya want for nothing?";
    NETHMACInit(&ctx, NETGetMD5Interface(), key, 4);
    NETHMACUpdate(&ctx, (void*)data, (u32)strlen(data));
    NETHMACGetDigest(&ctx, digest);
    hex(digest, h);
    printf("hmac: %s\n", h);
    CHECK(!strcmp(h, "750c783e6ab0b503eaa86e310a5db738"));
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_net_hash: ok\n");
    return 0;
}
