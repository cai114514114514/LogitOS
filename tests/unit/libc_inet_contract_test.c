/* Keep the strict pton contract separate from host differentials: Darwin
 * accepts decimal octets with leading zeroes, while this library deliberately
 * rejects their ambiguity. aton/addr retain the historical BSD radix forms.
 * Guard bytes also check that rejected input never partially commits dst.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;

static void check(int condition, const char *operation, const char *text)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s [%s]\n", operation, text);
    }
}

static void expect_pton(int family, const char *text, const unsigned char *expected)
{
    unsigned char storage[18], untouched[18];
    memset(storage, 0xa5, sizeof storage);
    memcpy(untouched, storage, sizeof storage);
    int result = inet_pton(family, text, storage + 1);
    if (!expected) {
        check(result == 0, "pton reject", text);
        check(!memcmp(storage, untouched, sizeof storage), "pton failure preserves destination", text);
        return;
    }
    size_t size = family == AF_INET ? 4 : 16;
    check(result == 1, "pton accept", text);
    check(!memcmp(storage + 1, expected, size), "pton network bytes", text);
    check(storage[0] == 0xa5 &&
          !memcmp(storage + size + 1, untouched + size + 1, sizeof storage - size - 1),
          "pton destination bounds", text);
}

static void strict_ipv4(void)
{
    const struct {
        const char *text;
        unsigned char bytes[4];
    } valid[] = {
        {"0.0.0.0", {0, 0, 0, 0}},
        {"255.255.255.255", {255, 255, 255, 255}},
        {"127.0.0.1", {127, 0, 0, 1}},
        {"1.2.3.4", {1, 2, 3, 4}},
        {"192.0.2.8", {192, 0, 2, 8}},
    };
    const char *invalid[] = {
        "", "1.2.3", "1.2.3.4.5", "256.2.3.4", "1.2.3.256",
        "1.2.3.04", "01.2.3.4", "1.2.3.08", "00.0.0.0",
        "127.1", "0x7f.0.0.1", "0177.0.0.1", "4294967295",
        "1..3.4", ".1.2.3", "1.2.3.", "1.2.3.4 ", " 1.2.3.4",
        "1.2.3.-1", "+1.2.3.4", "1.2.3.4\t", "1.2.3.4x",
    };
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; i++)
        expect_pton(AF_INET, valid[i].text, valid[i].bytes);
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++)
        expect_pton(AF_INET, invalid[i], NULL);
}

static void legacy_ipv4(void)
{
    const struct {
        const char *text;
        uint32_t address;
    } valid[] = {
        {"0", 0}, {"00", 0}, {"0x0", 0},
        {"127.1", 0x7f000001}, {"0177.1", 0x7f000001},
        {"0x7f.1", 0x7f000001}, {"0X7F.0.0.1", 0x7f000001},
        {"01.2.3.4", 0x01020304}, {"1.2.3.04", 0x01020304},
        {"1.2.3.010", 0x01020308}, {"1.2.65535", 0x0102ffff},
        {"1.16777215", 0x01ffffff}, {"4294967295", 0xffffffff},
        {"037777777777", 0xffffffff}, {"0xffffffff", 0xffffffff},
        {"255.255.255.255", 0xffffffff},
    };
    const char *invalid[] = {
        "", "08.2.3.4", "1.2.3.08", "0x", "0xg", "+1", "-1",
        "4294967296", "040000000000", "0x100000000",
        "256.1", "1.16777216", "1.2.65536", "1.2.3.256",
        "1.2.3.4.5", "1..2", "1.", ".1", "1x",
    };
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; i++) {
        struct in_addr address = {0};
        check(inet_aton(valid[i].text, &address) == 1, "aton accept", valid[i].text);
        check(ntohl(address.s_addr) == valid[i].address, "aton network bytes", valid[i].text);
        check(ntohl(inet_addr(valid[i].text)) == valid[i].address, "addr legacy radix", valid[i].text);
    }
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        struct in_addr address = {htonl(0xa5a5a5a5)};
        check(inet_aton(invalid[i], &address) == 0, "aton reject", invalid[i]);
        check(ntohl(address.s_addr) == 0xa5a5a5a5, "aton failure preserves destination", invalid[i]);
        check(inet_addr(invalid[i]) == INADDR_NONE, "addr rejects malformed input", invalid[i]);
    }
}

static void expect_ipv6(const char *text, const uint16_t groups[8])
{
    unsigned char expected[16];
    for (int i = 0; i < 8; i++) {
        expected[i * 2] = (unsigned char)(groups[i] >> 8);
        expected[i * 2 + 1] = (unsigned char)groups[i];
    }
    expect_pton(AF_INET6, text, expected);
}

static void compressed_ipv6(void)
{
    /* Every nonempty zero run, including all zeroes and either endpoint.
     * Expected groups are set directly; no parser supplies this oracle. */
    for (int start = 0; start < 8; start++) {
        for (int length = 1; length <= 8 - start; length++) {
            uint16_t expected[8];
            char text[64];
            int used = 0;
            for (int i = 0; i < 8; i++)
                expected[i] = i >= start && i < start + length ? 0 : (uint16_t)(i + 1);
            for (int i = 0; i < 8;) {
                if (i == start) {
                    text[used++] = ':';
                    text[used++] = ':';
                    i += length;
                    continue;
                }
                if (used && text[used - 1] != ':') text[used++] = ':';
                used += snprintf(text + used, sizeof text - (size_t)used, "%x", expected[i]);
                i++;
            }
            text[used] = 0;
            expect_ipv6(text, expected);
        }
    }
}

static void ipv6_boundaries(void)
{
    const struct {
        const char *text;
        uint16_t groups[8];
    } valid[] = {
        {"1:2:3:4:5:6:7:8", {1, 2, 3, 4, 5, 6, 7, 8}},
        {"0:0:0:0:0:0:0:0", {0}},
        {"FFFF:abcd:0001:2:3:4:5:6", {0xffff, 0xabcd, 1, 2, 3, 4, 5, 6}},
        {"::ffff:192.0.2.1", {0, 0, 0, 0, 0, 0xffff, 0xc000, 0x201}},
        {"::192.0.2.1", {0, 0, 0, 0, 0, 0, 0xc000, 0x201}},
        {"1:2:3:4:5:6:192.0.2.1", {1, 2, 3, 4, 5, 6, 0xc000, 0x201}},
        {"1:2:3:4:5::192.0.2.1", {1, 2, 3, 4, 5, 0, 0xc000, 0x201}},
        {"::ffff:0.0.0.0", {0, 0, 0, 0, 0, 0xffff, 0, 0}},
        {"::ffff:255.255.255.255", {0, 0, 0, 0, 0, 0xffff, 0xffff, 0xffff}},
    };
    const char *invalid[] = {
        "", ":", ":::", ":1", "1:", "::1:", "1::2:",
        "1:2:3:4:5:6:7:8:", "1:2:3:4:5:6:7:",
        "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9",
        "1:2:3:4:5:6:7:8::", "::1:2:3:4:5:6:7:8",
        "1:2:3:4::5:6:7:8", "1::2::3", "1:::2", "1:::",
        "12345::", "::12345", "::g", "::+1", "::-1", " ::1", "::1 ",
        "::ffff:192.0.2.1:", "::ffff:192.0.2.1:1", "::ffff:192.0.2",
        "::ffff:192.0.2.256", "::ffff:192.0.2.01", "::ffff:0xc0.0.2.1",
        "1:2:3:4:5:6:7:192.0.2.1", "1:2:3:4:5::6:192.0.2.1",
        "1:2:3:4:5:6::192.0.2.1", "192.0.2.1", "::ffff:192.0.2.1.2",
    };
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; i++)
        expect_ipv6(valid[i].text, valid[i].groups);
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++)
        expect_pton(AF_INET6, invalid[i], NULL);
}

int main(void)
{
    strict_ipv4();
    legacy_ipv4();
    compressed_ipv6();
    ipv6_boundaries();
    printf("inet contract: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
