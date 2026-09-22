/* Reuse the existing resolver/transport fixture, but keep this suite separate:
 * its owner-name control intentionally fails exactly one original assertion.
 * These additional CNAME cases would change that unrelated control's count. */
#define main dns_base_main
#include "dns_test.c"
#undef main

static const char query_name[] = "class.test";
static const uint8_t address4[] = {192, 0, 2, 7};
static const uint8_t address6[] = {
    0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7
};

/* The shared packet builder emits IN records. Locate CLASS using the lengths
 * that builder actually writes, independently of the resolver's name parser.
 * All records here have truthful RDLENGTHs; malformed-packet tests live in
 * dns_test.c. The offset includes both bytes, catching low-byte-only checks. */
static int class_reply(uint8_t *message, int qtype, uint16_t txid,
                       const struct rr *records, const uint16_t *classes,
                       int count)
{
    int length = build_resp(message, query_name, txid, qtype, 0, records, count);
    uint8_t encoded[DNS_NAME_MAX];
    int offset = 12 + put_qname(encoded, query_name) + 4;

    for (int i = 0; i < count; i++) {
        int owner_length = records[i].owner
            ? put_qname(encoded, records[i].owner) : 2;
        int data_length = records[i].rdname
            ? put_qname(encoded, records[i].rdname) : records[i].rdlen;
        int class_offset = offset + owner_length + 2;
        message[class_offset] = (uint8_t)(classes[i] >> 8);
        message[class_offset + 1] = (uint8_t)classes[i];
        offset += owner_length + 10 + data_length;
    }
    CHECK(offset == length, "fixture CLASS offsets reach the packet end");
    return length;
}

struct answers {
    ip6_addr addresses[8];
    int count;
    uint32_t ttl;
};

/* Exercise the same collector for real A and AAAA question packets. The shared
 * transport fixture models a v4-only interface, so pretending it sends AAAA
 * would hide a capability gap. Async delivery/cache is checked separately. */
static struct answers parse_reply(int qtype, const struct rr *records,
                                  const uint16_t *classes, int count)
{
    reset_dns();
    uint8_t query[512], message[1024], encoded[DNS_NAME_MAX];
    uint16_t txid;
    int query_length = build_query_type(query, query_name, qtype, &txid);
    int class_offset = 12 + put_qname(encoded, query_name) + 2;
    CHECK(query_length > class_offset + 1 && query[class_offset] == 0 &&
          query[class_offset + 1] == 1, "outgoing question requests class IN");

    int length = class_reply(message, qtype, txid, records, classes, count);
    struct answers result = {0};
    int added = collect_answers(message, length, txid, query_name,
                                result.addresses, 8, &result.count, &result.ttl);
    CHECK(added == result.count, "collector reports its actual address count");
    return result;
}

static int matches(const ip6_addr *actual, int type, const uint8_t *bytes)
{
    if (type == DNS_T_A)
        return ip6_is_v4mapped(actual) &&
               ip6_to_v4(actual) == IPV4(bytes[0], bytes[1], bytes[2], bytes[3]);
    return memcmp(actual->b, bytes, 16) == 0;
}

static struct rr address_record(int type)
{
    struct rr record = {
        .type = type, .ttl = 120,
        .rdata = type == DNS_T_A ? address4 : address6,
        .rdlen = type == DNS_T_A ? 4 : 16
    };
    return record;
}

static void test_direct(int type)
{
    struct rr record = address_record(type);
    const uint16_t classes[] = {0, 1, 3, 4, 255, 0x0101};

    for (unsigned i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        struct answers result = parse_reply(type, &record, &classes[i], 1);
        if (classes[i] == 1) {
            CHECK(result.count == 1 && matches(&result.addresses[0], type,
                  record.rdata), "IN address remains usable (type %d)", type);
            CHECK(result.ttl == 120, "IN address retains its TTL");
        } else {
            CHECK(result.count == 0, "non-IN address refused (type %d class %u)",
                  type, classes[i]);
            CHECK(result.ttl == 0, "non-IN address contributes no cache TTL");
        }
    }
}

static void test_mixed_addresses(int type)
{
    struct rr records[3] = {
        address_record(type), address_record(type), address_record(type)
    };
    const uint16_t classes[] = {3, 1, 0x0101};
    records[0].ttl = 1;
    records[2].ttl = 2;
    struct answers result = parse_reply(type, records, classes, 3);
    CHECK(result.count == 1 && matches(&result.addresses[0], type, records[1].rdata),
          "mixed classes retain only the IN address (type %d)", type);
    CHECK(result.ttl == 120, "ignored classes cannot shorten the IN cache TTL");
}

static void test_cname(int type)
{
    /* Keep the valid alias TTL above the address TTL: this gate defines class
     * acceptance, not the separate policy for combining CNAME/address TTLs. */
    struct rr records[] = {
        {.type = DNS_T_CNAME, .ttl = 180, .rdname = "alias.test"},
        address_record(type)
    };
    records[1].owner = "alias.test";
    uint16_t classes[] = {1, 1};
    struct answers result = parse_reply(type, records, classes, 2);
    CHECK(result.count == 1, "IN CNAME and IN address resolve (type %d)", type);

    classes[0] = 3;
    result = parse_reply(type, records, classes, 2);
    CHECK(result.count == 0, "non-IN CNAME cannot redirect an IN query (type %d)", type);

    classes[0] = 1;
    classes[1] = 4;
    result = parse_reply(type, records, classes, 2);
    CHECK(result.count == 0, "IN CNAME cannot admit a non-IN address (type %d)", type);

    /* Ignoring a foreign-class alias must not mutate the chain's current name:
     * otherwise the following legitimate alias silently becomes unreachable. */
    struct rr mixed[] = {
        {.type = DNS_T_CNAME, .ttl = 1, .rdname = "wrong.test"},
        records[0], records[1], records[1]
    };
    const uint16_t mixed_classes[] = {3, 1, 4, 1};
    mixed[2].ttl = 1;
    result = parse_reply(type, mixed, mixed_classes, 4);
    CHECK(result.count == 1 && matches(&result.addresses[0], type, records[1].rdata),
          "foreign CNAME does not block the later IN chain (type %d)", type);
    CHECK(result.ttl == 120, "foreign records leave the accepted address TTL intact");
}

static void test_async_cache(void)
{
    reset_dns();
    int id = dns_query_start(query_name);
    CHECK(id >= 0 && nsent == 1, "async query reaches the modeled UDP transport");
    if (id < 0 || nsent != 1) return;

    struct rr record = address_record(DNS_T_A);
    uint16_t record_class = 3;
    uint8_t message[512];
    int length = class_reply(message, DNS_T_A, sent_txid(0), &record, &record_class, 1);
    deliver(message, length, RESOLVER, 53);
    dns_poll();
    ip6_addr addresses[8];
    CHECK(dns_query_addrs(id, addresses, 8) < 0,
          "async IN query rejects a CH answer");
    dns_query_free(id);

    id = dns_query_start(query_name);
    CHECK(id >= 0 && nsent == 2, "refused class was not inserted into the cache");
    if (id < 0 || nsent != 2) {
        if (id >= 0) dns_query_free(id);
        return;
    }
    record_class = 1;
    length = class_reply(message, DNS_T_A, sent_txid(1), &record, &record_class, 1);
    deliver(message, length, RESOLVER, 53);
    dns_poll();
    CHECK(dns_query_addrs(id, addresses, 8) == 1 &&
          matches(&addresses[0], DNS_T_A, address4), "later IN answer resolves normally");
    dns_query_free(id);

    id = dns_query_start(query_name);
    CHECK(id >= 0 && nsent == 2 && dns_query_addrs(id, addresses, 8) == 1,
          "accepted IN answer remains cacheable");
    if (id >= 0) dns_query_free(id);
}

int main(void)
{
    test_direct(DNS_T_A);
    test_direct(DNS_T_AAAA);
    test_mixed_addresses(DNS_T_A);
    test_mixed_addresses(DNS_T_AAAA);
    test_cname(DNS_T_A);
    test_cname(DNS_T_AAAA);
    test_async_cache();
    printf("dns class: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
