#include "oracle.h"
#include "security/crypto.h"
#include "station.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
struct radio {
    uint64_t clock_ms;
    uint8_t channel, transmit[2400], rx[1600];
    size_t tx_bytes, rx_bytes;
    unsigned sent, received;
    int tx_fail;
};
static int radio_set_channel(void *opaque, uint8_t channel)
{
    ((struct radio *)opaque)->channel = channel;
    return 0;
}
static int transmit(void *opaque, const uint8_t *frame, size_t bytes)
{
    struct radio *radio = opaque;
    if (bytes > sizeof(radio->transmit)) {
        return -1;
    }
    memcpy(radio->transmit, frame, bytes);
    radio->tx_bytes = bytes;
    ++radio->sent;
    return radio->tx_fail ? -1 : 0;
}
static int radio_random(void *opaque, uint8_t *buffer, size_t bytes)
{
    (void)opaque;
    for (size_t i = 0; i < bytes; ++i) {
        buffer[i] = (uint8_t)(i + 1);
    }
    return 0;
}
static uint64_t clock_ms(void *opaque)
{
    return ((struct radio *)opaque)->clock_ms;
}
static void radio_receive_ethernet(void *opaque, const uint8_t *frame, size_t bytes)
{
    struct radio *radio = opaque;
    if (bytes <= sizeof(radio->rx)) {
        memcpy(radio->rx, frame, bytes);
        radio->rx_bytes = bytes;
        ++radio->received;
    }
}
static void check_crypto_oracles(void)
{
    uint8_t key[16], nonce[13], aad[22], plain[47], out[100], before[100];
    for (unsigned i = 0; i < 16; ++i) {
        key[i] = (uint8_t)i;
    }
    for (unsigned i = 0; i < 13; ++i) {
        nonce[i] = (uint8_t)i;
    }
    for (unsigned i = 0; i < 22; ++i) {
        aad[i] = (uint8_t)i;
    }
    for (unsigned i = 0; i < 47; ++i) {
        plain[i] = (uint8_t)i;
    }
    CHECK(wifi_psk((const uint8_t *)"password", 8, (const uint8_t *)"IEEE", 4, out) == 0 &&
          memcmp(out, oracle_pmk, 32) == 0);
    CHECK(wifi_ccm_seal(key, nonce, aad, 22, plain, 47, out, sizeof(out)) == 55 &&
          memcmp(out, oracle_ccm, 55) == 0);
    CHECK(wifi_ccm_open(key, nonce, aad, 22, out, 55, out, sizeof(out)) == 47 &&
          memcmp(out, plain, 47) == 0);
    CHECK(wifi_key_unwrap(key, oracle_wrapped, sizeof(oracle_wrapped), out, sizeof(out)) == 32);
    for (unsigned i = 0; i < 32; ++i) {
        CHECK(out[i] == i);
    }
    uint8_t bad[55];
    memcpy(bad, oracle_ccm, 55);
    bad[54] ^= 1;
    memset(out, 0xa6, sizeof(out));
    memcpy(before, out, sizeof(out));
    CHECK(wifi_ccm_open(key, nonce, aad, 22, bad, 55, out, sizeof(out)) == -1);
    CHECK(memcmp(out, before, sizeof(out)) == 0);
    for (size_t bytes = 0; bytes < 8; ++bytes) {
        CHECK(wifi_ccm_open(key, nonce, aad, 22, bad, bytes, out, sizeof(out)) == -1);
    }
}
static void connect_station(struct wifi_station *station, struct radio *radio)
{
    memset(radio, 0, sizeof(*radio));
    struct wifi_station_ops ops = {radio,        radio_set_channel, transmit,
                                   radio_random, clock_ms,          radio_receive_ethernet};
    CHECK(wifi_station_init(station, &ops, (const uint8_t[]){2, 0, 0, 0, 0, 1}) == 0);
    CHECK(wifi_station_scan(station, (const uint8_t[]){1, 6, 11}, 3) == 0);
    CHECK(wifi_station_receive(station, oracle_beacon, sizeof(oracle_beacon), -40) == 0 &&
          station->bss_count == 1);
    CHECK(wifi_station_join(station, 0, (const uint8_t *)"password", 8) == 0 &&
          station->state == WIFI_AUTHENTICATING);
    CHECK(radio->tx_bytes == 30 && radio->transmit[0] == 0xb0 && radio->transmit[26] == 1);
    CHECK(wifi_station_receive(station, oracle_auth, sizeof(oracle_auth), -40) == 0 &&
          station->state == WIFI_ASSOCIATING);
    CHECK(wifi_station_receive(station, oracle_assoc, sizeof(oracle_assoc), -40) == 0 &&
          station->state == WIFI_KEY_EXCHANGE);
    CHECK(wifi_station_transmit(station, oracle_ethernet, sizeof(oracle_ethernet)) == -1);
    CHECK(wifi_station_receive(station, oracle_m1, sizeof(oracle_m1), -40) == 0 &&
          station->state == WIFI_WAIT_M3);
    CHECK(memcmp(station->pending_ptk, oracle_ptk, 48) == 0);
    CHECK(radio->tx_bytes == sizeof(oracle_m2) + 32 &&
          memcmp(radio->transmit + 32, oracle_m2, sizeof(oracle_m2)) == 0);
    CHECK(wifi_station_receive(station, oracle_m3, sizeof(oracle_m3), -40) == 0 &&
          station->state == WIFI_CONNECTED);
    CHECK(radio->tx_bytes == sizeof(oracle_m4) + 32 &&
          memcmp(radio->transmit + 32, oracle_m4, sizeof(oracle_m4)) == 0);
}
/* Header sequence numbers belong to the client; the independent oracle fixes
 * every other wire byte, including EAPOL MIC and CCMP ciphertext/tag. */
static int same_wire(const struct radio *radio, const uint8_t *expected, size_t bytes)
{
    return radio->tx_bytes == bytes && !memcmp(radio->transmit, expected, 22) &&
           !memcmp(radio->transmit + 24, expected + 24, bytes - 24);
}

static void check_pairwise_rekey(struct wifi_station *station, struct radio *radio)
{
    connect_station(station, radio);
    CHECK(wifi_station_receive(station, oracle_rekey_m1, sizeof(oracle_rekey_m1), -40) == 0);
    CHECK(station->state == WIFI_REKEYING && wifi_station_link_up(station));
    CHECK(!memcmp(station->ptk, oracle_ptk, 48) &&
          !memcmp(station->pending_ptk, oracle_rekey_ptk, 48));
    CHECK(same_wire(radio, oracle_rekey_m2_wire, sizeof(oracle_rekey_m2_wire)));
    uint8_t bad[sizeof(oracle_rekey_m3)];
    memcpy(bad, oracle_rekey_m3, sizeof(bad));
    bad[32 + 81] ^= 1;
    CHECK(wifi_station_receive(station, bad, sizeof(bad), -40) == -1);
    CHECK(!memcmp(station->ptk, oracle_ptk, 48) && wifi_station_link_up(station));
    CHECK(wifi_station_receive(station, oracle_rx, sizeof(oracle_rx), -40) == 0);
    CHECK(wifi_station_receive(station, oracle_rekey_m3, sizeof(oracle_rekey_m3), -40) == 0);
    CHECK(same_wire(radio, oracle_rekey_m4_wire, sizeof(oracle_rekey_m4_wire)));
    CHECK(station->state == WIFI_CONNECTED && !memcmp(station->ptk, oracle_rekey_ptk, 48));
    CHECK(station->tx_pn == 0 && station->rx_pn[16] == 0);
    CHECK(wifi_station_receive(station, oracle_new_pair_rx, sizeof(oracle_new_pair_rx), -40) == 0);
    CHECK(wifi_station_receive(station, oracle_rekey_m3, sizeof(oracle_rekey_m3), -40) == 0);
    CHECK(station->tx_pn == 1 && station->rx_pn[16] == 1);
    CHECK(wifi_station_receive(station, oracle_new_pair_rx, sizeof(oracle_new_pair_rx), -40) == -1);
    CHECK(wifi_station_receive(station, oracle_rx, sizeof(oracle_rx), -40) == -1);
}

static void check_group_rekey(struct wifi_station *station, struct radio *radio)
{
    connect_station(station, radio);
    CHECK(wifi_station_receive(station, oracle_group_one, sizeof(oracle_group_one), -40) == 0);
    CHECK(same_wire(radio, oracle_group_two_wire, sizeof(oracle_group_two_wire)));
    CHECK(station->group_keys[1].installed && station->group_keys[2].installed);
    CHECK(!memcmp(station->group_keys[2].key, oracle_group_key, 16));
    CHECK(wifi_station_receive(station, oracle_group_rx, sizeof(oracle_group_rx), -40) == 0);
    CHECK(wifi_station_receive(station, oracle_group_one, sizeof(oracle_group_one), -40) == 0);
    CHECK(station->group_keys[2].replay[16] == 1);
    CHECK(wifi_station_receive(station, oracle_group_again, sizeof(oracle_group_again), -40) == 0);
    CHECK(station->group_keys[2].replay[16] == 1);
    CHECK(wifi_station_receive(station, oracle_group_rx, sizeof(oracle_group_rx), -40) == -1);
    CHECK(!memcmp(station->ptk, oracle_ptk, 48) && station->tx_pn == 3);
}

static void check_rekey_retry_counters(struct wifi_station *station, struct radio *radio)
{
    connect_station(station, radio);
    CHECK(wifi_station_receive(station, oracle_rekey_m1, sizeof(oracle_rekey_m1), -40) == 0);
    CHECK(wifi_station_receive(station, oracle_rekey_m1_retry, sizeof(oracle_rekey_m1_retry),
                               -40) == 0);
    CHECK(!memcmp(station->pending_ptk, oracle_rekey_ptk, 48) && station->pending_replay == 4);
    CHECK(wifi_station_receive(station, oracle_rekey_m3_retry, sizeof(oracle_rekey_m3_retry),
                               -40) == 0);
    CHECK(wifi_station_receive(station, oracle_new_pair_rx, sizeof(oracle_new_pair_rx), -40) == 0);
    CHECK(wifi_station_receive(station, oracle_rekey_m3_later_retry,
                               sizeof(oracle_rekey_m3_later_retry), -40) == 0);
    CHECK(station->rx_pn[16] == 1 && station->tx_pn == 1 && station->replay == 7);
    CHECK(wifi_station_receive(station, oracle_new_pair_rx, sizeof(oracle_new_pair_rx), -40) == -1);
}

static void check_rekey_submission_failure(struct wifi_station *station, struct radio *radio)
{
    connect_station(station, radio);
    radio->tx_fail = 1;
    CHECK(wifi_station_receive(station, oracle_group_one, sizeof(oracle_group_one), -40) == -1);
    CHECK(station->group_keys[1].installed && !station->group_keys[2].installed);
    CHECK(wifi_station_link_up(station) && station->replay == 2 && station->tx_pn == 1);
    radio->tx_fail = 0;
    CHECK(wifi_station_receive(station, oracle_group_one, sizeof(oracle_group_one), -40) == 0);
    CHECK(station->group_keys[2].installed && station->tx_pn == 2);
    connect_station(station, radio);
    CHECK(wifi_station_receive(station, oracle_rekey_m1, sizeof(oracle_rekey_m1), -40) == 0);
    radio->tx_fail = 1;
    CHECK(wifi_station_receive(station, oracle_rekey_m3, sizeof(oracle_rekey_m3), -40) == -1);
    CHECK(wifi_station_link_up(station) && !memcmp(station->ptk, oracle_ptk, 48));
    CHECK(station->state == WIFI_REKEYING && station->tx_pn == 2 && station->replay == 2);
    radio->tx_fail = 0;
    CHECK(wifi_station_receive(station, oracle_rekey_m3, sizeof(oracle_rekey_m3), -40) == 0);
    CHECK(station->tx_pn == 0 && !memcmp(station->ptk, oracle_rekey_ptk, 48));
}

static void check_connection_loss(struct wifi_station *station, struct radio *radio)
{
    connect_station(station, radio);
    radio->clock_ms = 4500;
    CHECK(wifi_station_receive(station, oracle_beacon, sizeof(oracle_beacon), -40) == 0);
    radio->clock_ms = 5001;
    CHECK(wifi_station_tick(station) == 0 && wifi_station_link_up(station));
    radio->clock_ms = 9500;
    CHECK(wifi_station_tick(station) == -1 && !wifi_station_link_up(station));
    CHECK(station->disconnect_reason == WIFI_DISCONNECT_BEACON_LOSS);
    CHECK(wifi_station_transmit(station, oracle_ethernet, sizeof(oracle_ethernet)) == -1);
    CHECK(!station->group_keys[1].installed && station->pmk[0] == 0);
    connect_station(station, radio);
    CHECK(wifi_station_receive(station, oracle_rekey_m1, sizeof(oracle_rekey_m1), -40) == 0);
    radio->clock_ms = 4500;
    CHECK(wifi_station_receive(station, oracle_beacon, sizeof(oracle_beacon), -40) == 0);
    radio->clock_ms = 5001;
    CHECK(wifi_station_tick(station) == -1 && !wifi_station_link_up(station));
    CHECK(station->disconnect_reason == WIFI_DISCONNECT_HANDSHAKE_TIMEOUT);
    CHECK(!memcmp(station->ptk, (uint8_t[48]){0}, 48));
    CHECK(!memcmp(station->pending_ptk, (uint8_t[48]){0}, 48));
    CHECK(!memcmp(station->pmk, (uint8_t[32]){0}, 32));
    connect_station(station, radio);
    wifi_station_transport_lost(station);
    CHECK(!wifi_station_link_up(station) &&
          station->disconnect_reason == WIFI_DISCONNECT_TRANSPORT);
}

int main(void)
{
    check_crypto_oracles();
    struct wifi_station *station = calloc(1, sizeof(*station));
    struct radio radio;
    if (!station) {
        return 2;
    }
    connect_station(station, &radio);
    uint8_t bad[sizeof(oracle_rx)];
    memcpy(bad, oracle_rx, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 1;
    CHECK(wifi_station_receive(station, bad, sizeof(bad), -40) == -1 && radio.received == 0 &&
          station->rx_pn[16] == 0);
    CHECK(wifi_station_receive(station, oracle_rx, sizeof(oracle_rx), -40) == 0 &&
          radio.received == 1);
    CHECK(radio.rx_bytes == sizeof(oracle_ethernet) &&
          memcmp(radio.rx, oracle_ethernet, sizeof(oracle_ethernet)) == 0);
    CHECK(wifi_station_receive(station, oracle_rx, sizeof(oracle_rx), -40) == -1 &&
          radio.received == 1);
    uint8_t outbound[sizeof(oracle_ethernet)];
    memcpy(outbound, oracle_ethernet, sizeof(outbound));
    memcpy(outbound + 6, station->mac, 6);
    CHECK(wifi_station_transmit(station, outbound, sizeof(outbound)) == 0 && station->tx_pn == 1 &&
          radio.transmit[1] == 0x41 && radio.transmit[24] == 1);
    CHECK(wifi_station_receive(station, oracle_m3, sizeof(oracle_m3), -40) == 0 &&
          station->tx_pn == 2 && station->rx_pn[16] == 1);
    CHECK(wifi_station_receive(station, oracle_rx, sizeof(oracle_rx), -40) == -1);
    radio.tx_fail = 1;
    CHECK(wifi_station_transmit(station, outbound, sizeof(outbound)) == -1 && station->tx_pn == 3);
    radio.tx_fail = 0;
    station->tx_pn = UINT64_C(0xffffffffffff);
    CHECK(wifi_station_transmit(station, outbound, sizeof(outbound)) == -1);
    unsigned sent = radio.sent;
    station->lock = 1;
    CHECK(wifi_station_transmit(station, outbound, sizeof(outbound)) == -1 && radio.sent == sent);
    station->lock = 0;
    wifi_station_disconnect(station);
    CHECK(station->state == WIFI_IDLE);
    for (unsigned i = 0; i < 48; ++i) {
        CHECK(station->ptk[i] == 0);
    }
    connect_station(station, &radio);
    wifi_station_disconnect(station);
    CHECK(wifi_station_scan(station, (const uint8_t[]){1, 6}, 2) == 0);
    radio.clock_ms = 150;
    CHECK(wifi_station_tick(station) == 0 && radio.channel == 6);
    radio.clock_ms = 300;
    CHECK(wifi_station_tick(station) == 0 && station->state == WIFI_IDLE);
    connect_station(station, &radio);
    station->state = WIFI_WAIT_M3;
    station->deadline = 1;
    radio.clock_ms = 2;
    CHECK(wifi_station_tick(station) == -1 && station->state == WIFI_FAILED);
    check_pairwise_rekey(station, &radio);
    check_group_rekey(station, &radio);
    check_rekey_retry_counters(station, &radio);
    check_rekey_submission_failure(station, &radio);
    check_connection_loss(station, &radio);
    free(station);
    printf("WIFI_STATION: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
