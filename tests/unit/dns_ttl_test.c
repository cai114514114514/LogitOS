/* Cache lifetime belongs to the complete name-to-address result. The address
 * TTL alone cannot keep an expired CNAME alive, and zero is a real lifetime,
 * not an unset field. Use the production parser and async API with the existing
 * byte-packet transport; no public resolver or host clock enters this oracle.
 *
 * RFC 1035 section 3.2.1 permits zero-TTL data for the current transaction only.
 * RFC 2181 section 8 makes TTL a maximum: a local ceiling may shorten it, while
 * a floor may not extend it. The checks below inspect both returned bytes and
 * new outgoing queries, so dropping an answer cannot masquerade as no caching.
 */
#define main dns_base_main
#include "dns_test.c"
#undef main

static const char lookup_name[] = "lifetime.test";
static const uint8_t address4[] = {192, 0, 2, 7};
static const uint8_t address6[] = {
    0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7
};

static struct rr address_record(int type, uint32_t ttl)
{
    struct rr record = {
        .type = type, .ttl = ttl,
        .rdata = type == DNS_T_A ? address4 : address6,
        .rdlen = type == DNS_T_A ? 4 : 16
    };
    return record;
}

static int matches(const ip6_addr *actual, int type)
{
    if (type == DNS_T_A)
        return ip6_is_v4mapped(actual) && ip6_to_v4(actual) == 0xc0000207u;
    return memcmp(actual->b, address6, sizeof address6) == 0;
}

/* The shared builder writes IN records. Only the mixed-class case rewrites
 * those two bytes, deriving offsets from the supplied fixture strings rather
 * than trusting the parser whose filtering is under test. */
static int reply(uint8_t *message, int type, uint16_t txid,
                 const struct rr *records, const uint16_t *classes, int count)
{
    int length = build_resp(message, lookup_name, txid, type, 0, records, count);
    if (!classes) return length;
    uint8_t encoded[DNS_NAME_MAX];
    int offset = 12 + put_qname(encoded, lookup_name) + 4;
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
    CHECK(offset == length, "TTL fixture CLASS offsets reach packet end");
    return length;
}

static void check_parser(int type, const struct rr *records,
                         const uint16_t *classes, int count, int expected_count,
                         uint32_t expected_ttl, const char *label)
{
    uint8_t message[1024];
    ip6_addr addresses[8];
    int found = 0;
    uint32_t ttl = 0xdeadbeefu;
    int length = reply(message, type, 0x3210, records, classes, count);
    int added = collect_answers(message, length, 0x3210, lookup_name,
                                addresses, 8, &found, &ttl);
    CHECK(added == expected_count && found == expected_count,
          "%s retains the accepted address count (type %d)", label, type);
    for (int i = 0; i < found; i++)
        CHECK(matches(&addresses[i], type), "%s retains address bytes (type %d)",
              label, type);
    CHECK(ttl == expected_ttl, "%s keeps TTL %u (type %d, got %u)",
          label, expected_ttl, type, ttl);
}

static void test_alias_lifetime(int type)
{
    struct rr records[] = {
        {.type = DNS_T_CNAME, .ttl = 1, .rdname = "terminal.test"},
        address_record(type, 300)
    };
    records[1].owner = "terminal.test";
    check_parser(type, records, NULL, 2, 1, 1, "short CNAME limits cache lifetime");
    records[0].ttl = 600;
    check_parser(type, records, NULL, 2, 1, 300, "long CNAME preserves address TTL");
    records[0].ttl = 0;
    check_parser(type, records, NULL, 2, 1, 0, "zero CNAME permits only this transaction");

    struct rr chain[] = {
        {.type = DNS_T_CNAME, .ttl = 90, .rdname = "middle.test"},
        {.owner = "middle.test", .type = DNS_T_CNAME, .ttl = 2,
         .rdname = "terminal.test"},
        records[1]
    };
    check_parser(type, chain, NULL, 3, 1, 2, "intermediate CNAME limits lifetime");
    chain[0].ttl = 1;
    check_parser(type, chain, NULL, 3, 1, 1, "first CNAME remains the shortest link");
    chain[1].ttl = 0;
    check_parser(type, chain, NULL, 3, 1, 0, "zero intermediate CNAME remains zero");
    check_parser(type, chain, NULL, 2, 0, 0, "alias without address has no result TTL");
}

static void test_ignored_lifetimes(int type)
{
    struct rr records[] = {
        {.type = DNS_T_CNAME, .ttl = 0, .rdname = "foreign.test"},
        {.owner = "unrelated.test", .type = DNS_T_CNAME, .ttl = 0,
         .rdname = "other.test"},
        {.type = DNS_T_CNAME, .ttl = 40, .rdname = "terminal.test"},
        address_record(type, 0), address_record(type, 90),
        address_record(type, 0)
    };
    const uint16_t classes[] = {3, 1, 1, 0x0101, 1, 1};
    records[3].owner = "terminal.test";
    records[4].owner = "terminal.test";
    records[5].owner = "unrelated.test";
    check_parser(type, records, classes, 6, 1, 40,
                 "foreign and unrelated records cannot shorten lifetime");

    struct rr direct[] = {address_record(type, 60), address_record(type, 1)};
    check_parser(type, direct, NULL, 2, 2, 1, "shortest accepted address limits lifetime");
    direct[0].ttl = 0;
    check_parser(type, direct, NULL, 2, 2, 0, "zero address stays zero within a packet");
}

static int start_query(int dual_stack)
{
    reset_dns();
    fixture_ip6_state = dual_stack ? 2 : 0;
    int id = dns_query_start(lookup_name);
    CHECK(id >= 0 && nsent == (dual_stack ? 2 : 1),
          "query reaches the modeled transport with requested families");
    if (id < 0) return -1;
    CHECK(sent_qtype(0) == DNS_T_A, "first outgoing transaction asks for A");
    if (dual_stack)
        CHECK(sent_qtype(1) == DNS_T_AAAA, "second outgoing transaction asks for AAAA");
    return id;
}

static void answer(int type, uint32_t ttl)
{
    uint8_t message[512];
    struct rr record = address_record(type, ttl);
    int length = reply(message, type, sent_txid(type == DNS_T_A ? 0 : 1),
                       &record, NULL, 1);
    deliver(message, length, RESOLVER, 53);
    dns_poll();
}

static void check_result(int id, int expected_count, const char *label)
{
    ip6_addr addresses[8];
    int count = dns_query_addrs(id, addresses, 8);
    CHECK(count == expected_count, "%s returns current transaction addresses", label);
    int seen4 = 0, seen6 = 0;
    for (int i = 0; i < count; i++) {
        seen4 += matches(&addresses[i], DNS_T_A);
        seen6 += matches(&addresses[i], DNS_T_AAAA);
    }
    CHECK(seen4 == 1 && seen6 == expected_count - 1,
          "%s preserves current transaction address bytes", label);
}

static void check_requery(int cached, int expected_count, const char *label)
{
    int sent_before = nsent;
    int id = dns_query_start(lookup_name);
    int query_count = fixture_ip6_state == 2 ? 2 : 1;
    CHECK(id >= 0, "%s allocates a subsequent query", label);
    if (id < 0) return;
    CHECK(nsent == sent_before + (cached ? 0 : query_count),
          "%s %s", label, cached ? "uses the cache" : "sends a new query");
    if (cached) check_result(id, expected_count, label);
    else {
        ip6_addr addresses[8];
        CHECK(dns_query_addrs(id, addresses, 8) == 0,
              "%s waits for a new response", label);
    }
    dns_query_free(id);
}

static void test_zero_lifetime(void)
{
    int id = start_query(0);
    if (id < 0) return;
    answer(DNS_T_A, 0);
    check_result(id, 1, "zero TTL");
    dns_query_free(id);
    check_requery(0, 1, "zero TTL next transaction");
}

static void test_one_second_lifetime(void)
{
    int id = start_query(0);
    if (id < 0) return;
    uint64_t received = ticks;
    answer(DNS_T_A, 1);
    check_result(id, 1, "one second TTL");
    dns_query_free(id);
    ticks = received + TIMER_HZ - 1;
    check_requery(1, 1, "one second TTL before expiry");
    ticks = received + TIMER_HZ;
    check_requery(0, 1, "one second TTL at expiry");
}

static void test_zero_replaces_concurrent_cache(void)
{
    int zero_id = start_query(0);
    if (zero_id < 0) return;
    int cached_id = dns_query_start(lookup_name);
    CHECK(cached_id >= 0 && nsent == 2,
          "concurrent same-name query sends its own transaction");
    if (cached_id < 0) {
        dns_query_free(zero_id);
        return;
    }

    /* deliver() targets the newest live modeled socket. Complete that query
     * first, then release it before replying to the older outstanding socket. */
    uint8_t message[512];
    struct rr record = address_record(DNS_T_A, 60);
    int length = reply(message, DNS_T_A, sent_txid(1), &record, NULL, 1);
    deliver(message, length, RESOLVER, 53);
    dns_poll();
    check_result(cached_id, 1, "concurrent positive TTL");
    dns_query_free(cached_id);
    check_requery(1, 1, "concurrent positive TTL before replacement");

    answer(DNS_T_A, 0);
    check_result(zero_id, 1, "concurrent zero TTL");
    dns_query_free(zero_id);
    check_requery(0, 1, "zero TTL replaces a concurrent cache entry");
}

static void test_async_alias(void)
{
    int id = start_query(0);
    if (id < 0) return;
    struct rr records[] = {
        {.type = DNS_T_CNAME, .ttl = 1, .rdname = "terminal.test"},
        address_record(DNS_T_A, 300)
    };
    records[1].owner = "terminal.test";
    uint8_t message[512];
    int length = reply(message, DNS_T_A, sent_txid(0), records, NULL, 2);
    uint64_t received = ticks;
    deliver(message, length, RESOLVER, 53);
    dns_poll();
    check_result(id, 1, "async alias");
    dns_query_free(id);
    ticks = received + TIMER_HZ;
    check_requery(0, 1, "short CNAME at expiry");
}

static void test_family_zero(int first_type, int zero_first)
{
    int id = start_query(1);
    if (id < 0) return;
    int second_type = first_type == DNS_T_A ? DNS_T_AAAA : DNS_T_A;
    answer(first_type, zero_first ? 0 : 60);
    answer(second_type, zero_first ? 60 : 0);
    check_result(id, 2, "dual-family zero TTL");
    CHECK(dq[id].ttl == 0, "zero TTL survives family merge (first type %d, zero first %d)",
          first_type, zero_first);
    dns_query_free(id);
    check_requery(0, 2, "dual-family zero TTL next transaction");
}

static void test_receive_age(int second_type, int wait_past_expiry)
{
    int id = start_query(1);
    if (id < 0) return;
    uint64_t received = ticks;
    answer(DNS_T_A, 1);
    /* Move modeled time directly: a busy kernel need not poll DNS at every
     * tick. Neither a late poll nor a late second family renews the first TTL. */
    ticks += wait_past_expiry ? TIMER_HZ + 1 : DNS_2ND_GRACE / 2;
    if (second_type) answer(second_type, 60);
    else {
        ticks = received + DNS_2ND_GRACE;
        dns_poll();
    }
    int count = second_type ? 2 : 1;
    check_result(id, count, "delayed completion");
    dns_query_free(id);
    if (!wait_past_expiry) {
        ticks = received + TIMER_HZ - 1;
        check_requery(1, count, "received TTL before expiry");
        ticks = received + TIMER_HZ;
    }
    check_requery(0, count, "received TTL at expiry");
}

int main(void)
{
    test_alias_lifetime(DNS_T_A);
    test_alias_lifetime(DNS_T_AAAA);
    test_ignored_lifetimes(DNS_T_A);
    test_ignored_lifetimes(DNS_T_AAAA);
    test_zero_lifetime();
    test_one_second_lifetime();
    test_zero_replaces_concurrent_cache();
    test_async_alias();
    test_family_zero(DNS_T_A, 1);
    test_family_zero(DNS_T_AAAA, 1);
    test_family_zero(DNS_T_A, 0);
    test_family_zero(DNS_T_AAAA, 0);
    test_receive_age(DNS_T_AAAA, 0);
    test_receive_age(0, 0);
    test_receive_age(DNS_T_AAAA, 1);
    printf("dns TTL: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
