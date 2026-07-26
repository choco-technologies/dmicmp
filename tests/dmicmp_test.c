/**
 * @file dmicmp_test.c
 * @brief Test steps for dmicmp
 *
 * Checksum steps replicate dmicmp's own message layout by hand (using
 * dmip_checksum() directly, same primitive dmicmp.c itself calls) to
 * compute an expected checksum, then check dmicmp_v4_checksum_valid()/
 * _v6_checksum_valid() agree - same self-verification approach
 * dmip_test.c/dmudp_test.c use for their own checksums.
 *
 * Send steps register a "/dev/null"-backed dmnetif fixture interface (same
 * pattern as dmip_test.c/dmudp_test.c) via dmod_test_setup()/_teardown().
 * No real driver backs it, so a full transmission can't be captured
 * end-to-end here - dmicmp_v4_send_error()/_send_dest_unreachable()/
 * _send_echo_request() are tested up through the point dmip_send()
 * itself can be tested without a driver (route lookup, a hand-seeded ARP
 * cache hit), failing only at the final dmnetif_send() - same limit
 * dmudp_test.c's own send tests document for themselves. This also means
 * an Echo Request's automatic Echo Reply, and the default handler's
 * automatic Destination Unreachable, cannot have their exact wire bytes
 * asserted from outside (the handler that sends them returns void) -
 * those steps only prove the wiring doesn't crash; the reply/error
 * construction itself is covered directly by the checksum/header round-
 * trip steps and the explicit send-function steps.
 *
 * Receiving an Echo Reply, by contrast, *can* be verified end-to-end
 * without any network I/O at all: delivery to a registered listener is a
 * direct in-process call, not something that goes through dmip_send().
 * feed_icmp_packet()/feed_unclaimed_v4_packet() build a complete,
 * checksummed IP packet by hand and drive it straight into dmnetbridge's
 * packet_received DIF implementation via Dmod_GetNextDifModule()/
 * Dmod_GetDifFunction() - the same discovery dmnetbridge_handle_netif_rx()
 * itself uses, see dmip_test.c's/dmudp_test.c's own feed_packet()-style
 * helpers. This requires ENABLE_DIF_REGISTRATIONS and linking
 * dmnetbridge_if - same pattern those files use.
 *
 * Every send-path step uses a distinct destination network, since
 * dmroute's routes and dmarp's cache are both global state that outlives
 * a single step.
 */
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod_test.h"
#include "dmicmp.h"
#include "dmroute.h"
#include "dmarp.h"
#include "dmnetbridge.h"
#include <string.h>
#include <errno.h>

static dmip_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmip_addr_t ip = { 0 };
    ip.family = dmip_family_v4;
    ip.addr.v4[0] = a;
    ip.addr.v4[1] = b;
    ip.addr.v4[2] = c;
    ip.addr.v4[3] = d;
    return ip;
}

static dmip_addr_t make_v6(uint8_t last_byte)
{
    dmip_addr_t ip = { 0 };
    ip.family = dmip_family_v6;
    ip.addr.v6[0] = 0x20;
    ip.addr.v6[1] = 0x01;
    ip.addr.v6[DMIP_IPV6_ADDR_LEN - 1] = last_byte;
    return ip;
}

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

/* dmod modules have no libc memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - a small manual comparison stands in for it. */
static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

#define TEST_ETH_HEADER_LEN 14u
#define TEST_ETHERTYPE_IPV4 0x0800u
#define TEST_ETHERTYPE_IPV6 0x86DDu
#define TEST_MAX_MESSAGE_LEN 64u
#define TEST_UNCLAIMED_PROTOCOL 253u /* IANA "experimentation" - guaranteed not to collide with a real DMIP_PROTO_* */

/**
 * @brief Wrap a complete IP packet in a minimal Ethernet frame and
 *        broadcast it to every packet_received DIF implementor (dmip's
 *        own, in practice) - the same discovery
 *        dmnetbridge_handle_netif_rx() itself uses
 */
static void feed_frame(dmnetif_iface_t iface, uint16_t ethertype, const uint8_t* packet, size_t packet_len)
{
    size_t frame_len = TEST_ETH_HEADER_LEN + packet_len;
    uint8_t* frame = Dmod_Malloc(frame_len);
    memset(frame, 0, TEST_ETH_HEADER_LEN);
    write_u16_be(&frame[12], ethertype);
    memcpy(frame + TEST_ETH_HEADER_LEN, packet, packet_len);

    Dmod_Context_t* implementor = NULL;
    while ((implementor = Dmod_GetNextDifModule(dmod_dmnetbridge_packet_received_sig, implementor)) != NULL)
    {
        dmod_dmnetbridge_packet_received_t fn =
            (dmod_dmnetbridge_packet_received_t)Dmod_GetDifFunction(implementor, dmod_dmnetbridge_packet_received_sig);
        if (fn != NULL)
        {
            fn(iface, frame, frame_len);
        }
    }

    Dmod_Free(frame);
}

/**
 * @brief Build a complete, checksummed ICMP message wrapped in an IPv4
 *        or IPv6 packet and feed it in via feed_frame()
 */
static void feed_icmp_packet(dmnetif_iface_t iface, dmip_family_t family, dmip_addr_t src, dmip_addr_t dst, const uint8_t* message, size_t message_len)
{
    if (family == dmip_family_v4)
    {
        dmip_v4_header_t header = { 0 };
        header.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + message_len);
        header.ttl = DMIP_DEFAULT_TTL;
        header.protocol = DMIP_PROTO_ICMP;
        header.src = src;
        header.dst = dst;

        uint8_t packet[DMIP_V4_HEADER_LEN + TEST_MAX_MESSAGE_LEN];
        dmip_v4_build_header(packet, sizeof(packet), &header);
        memcpy(packet + DMIP_V4_HEADER_LEN, message, message_len);

        feed_frame(iface, TEST_ETHERTYPE_IPV4, packet, DMIP_V4_HEADER_LEN + message_len);
    }
    else
    {
        dmip_v6_header_t header = { 0 };
        header.payload_length = (uint16_t)message_len;
        header.next_header = DMIP_PROTO_ICMPV6;
        header.hop_limit = DMIP_DEFAULT_HOP_LIMIT;
        header.src = src;
        header.dst = dst;

        uint8_t packet[DMIP_V6_HEADER_LEN + TEST_MAX_MESSAGE_LEN];
        dmip_v6_build_header(packet, sizeof(packet), &header);
        memcpy(packet + DMIP_V6_HEADER_LEN, message, message_len);

        feed_frame(iface, TEST_ETHERTYPE_IPV6, packet, DMIP_V6_HEADER_LEN + message_len);
    }
}

/**
 * @brief Build an IPv4 packet whose protocol nobody claims (see
 *        TEST_UNCLAIMED_PROTOCOL) and feed it in - drives
 *        dmicmp_handle_unclaimed_protocol() via dmip's default-handler
 *        registration
 */
static void feed_unclaimed_v4_packet(dmnetif_iface_t iface, dmip_addr_t src, dmip_addr_t dst, const uint8_t* payload, size_t payload_len)
{
    dmip_v4_header_t header = { 0 };
    header.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + payload_len);
    header.ttl = DMIP_DEFAULT_TTL;
    header.protocol = TEST_UNCLAIMED_PROTOCOL;
    header.src = src;
    header.dst = dst;

    uint8_t packet[DMIP_V4_HEADER_LEN + TEST_MAX_MESSAGE_LEN];
    dmip_v4_build_header(packet, sizeof(packet), &header);
    if (payload_len > 0)
    {
        memcpy(packet + DMIP_V4_HEADER_LEN, payload, payload_len);
    }

    feed_frame(iface, TEST_ETHERTYPE_IPV4, packet, DMIP_V4_HEADER_LEN + payload_len);
}

/**
 * @brief Build a complete, correctly-checksummed ICMPv4 message into
 *        `out` and return its length
 */
static size_t build_v4_message(uint8_t* out, uint8_t type, uint8_t code, uint16_t identifier, uint16_t sequence, const uint8_t* payload, size_t payload_len)
{
    dmicmp_header_t header = { .type = type, .code = code, .identifier = identifier, .sequence = sequence };
    dmicmp_build_header(out, DMICMP_HEADER_LEN + payload_len, &header);
    if (payload_len > 0)
    {
        memcpy(out + DMICMP_HEADER_LEN, payload, payload_len);
    }
    write_u16_be(&out[2], dmip_checksum(out, DMICMP_HEADER_LEN + payload_len));
    return DMICMP_HEADER_LEN + payload_len;
}

/**
 * @brief Build a complete, correctly-checksummed ICMPv6 message into
 *        `out` and return its length
 */
static size_t build_v6_message(uint8_t* out, dmip_addr_t src, dmip_addr_t dst, uint8_t type, uint8_t code, uint16_t identifier, uint16_t sequence, const uint8_t* payload, size_t payload_len)
{
    dmicmp_header_t header = { .type = type, .code = code, .identifier = identifier, .sequence = sequence };
    size_t message_len = DMICMP_HEADER_LEN + payload_len;
    dmicmp_build_header(out, message_len, &header);
    if (payload_len > 0)
    {
        memcpy(out + DMICMP_HEADER_LEN, payload, payload_len);
    }

    uint8_t pseudo_and_message[40 + DMICMP_HEADER_LEN + TEST_MAX_MESSAGE_LEN];
    memcpy(&pseudo_and_message[0], src.addr.v6, DMIP_IPV6_ADDR_LEN);
    memcpy(&pseudo_and_message[16], dst.addr.v6, DMIP_IPV6_ADDR_LEN);
    pseudo_and_message[32] = 0; pseudo_and_message[33] = 0; pseudo_and_message[34] = 0;
    pseudo_and_message[35] = (uint8_t)message_len;
    pseudo_and_message[36] = 0; pseudo_and_message[37] = 0; pseudo_and_message[38] = 0;
    pseudo_and_message[39] = DMIP_PROTO_ICMPV6;
    memcpy(&pseudo_and_message[40], out, message_len);

    write_u16_be(&out[2], dmip_checksum(pseudo_and_message, 40 + message_len));
    return message_len;
}

#define TEST_DEVICE_PATH "/dev/null"

static dmnetif_iface_t g_iface = NULL;

void dmod_test_setup(void)
{
    g_iface = dmnetif_register("test0", TEST_DEVICE_PATH);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface);
    g_iface = NULL;
}

/* ---- Header build/parse ---- */

DMOD_TEST_STEP(build_header_rejects_bad_arguments)
{
    uint8_t buffer[DMICMP_HEADER_LEN];
    dmicmp_header_t header = { 0 };

    DMOD_TEST_EXPECT_EQ(dmicmp_build_header(NULL, sizeof(buffer), &header), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmicmp_build_header(buffer, sizeof(buffer), NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmicmp_build_header(buffer, DMICMP_HEADER_LEN - 1, &header), -EINVAL);
}

DMOD_TEST_STEP(parse_header_rejects_bad_arguments)
{
    uint8_t buffer[DMICMP_HEADER_LEN] = { 0 };
    dmicmp_header_t header = { 0 };

    DMOD_TEST_EXPECT_EQ(dmicmp_parse_header(NULL, sizeof(buffer), &header), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmicmp_parse_header(buffer, sizeof(buffer), NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmicmp_parse_header(buffer, DMICMP_HEADER_LEN - 1, &header), -EINVAL);
}

DMOD_TEST_STEP(build_and_parse_header_round_trip_echo)
{
    dmicmp_header_t header = { .type = DMICMP_V4_TYPE_ECHO_REQUEST, .code = 0, .identifier = 42, .sequence = 7 };
    uint8_t buffer[DMICMP_HEADER_LEN];

    DMOD_TEST_EXPECT_EQ(dmicmp_build_header(buffer, sizeof(buffer), &header), 0);

    dmicmp_header_t parsed = { 0 };
    DMOD_TEST_EXPECT_EQ(dmicmp_parse_header(buffer, sizeof(buffer), &parsed), 0);
    DMOD_TEST_EXPECT_EQ(parsed.type, DMICMP_V4_TYPE_ECHO_REQUEST);
    DMOD_TEST_EXPECT_EQ(parsed.code, (uint8_t)0);
    DMOD_TEST_EXPECT_EQ(parsed.identifier, (uint16_t)42);
    DMOD_TEST_EXPECT_EQ(parsed.sequence, (uint16_t)7);
}

DMOD_TEST_STEP(build_and_parse_header_round_trip_dest_unreachable)
{
    dmicmp_header_t header = { .type = DMICMP_V4_TYPE_DEST_UNREACHABLE, .code = (uint8_t)dmicmp_v4_dest_unreachable_port };
    uint8_t buffer[DMICMP_HEADER_LEN];

    DMOD_TEST_EXPECT_EQ(dmicmp_build_header(buffer, sizeof(buffer), &header), 0);

    dmicmp_header_t parsed = { 0 };
    DMOD_TEST_EXPECT_EQ(dmicmp_parse_header(buffer, sizeof(buffer), &parsed), 0);
    DMOD_TEST_EXPECT_EQ(parsed.type, DMICMP_V4_TYPE_DEST_UNREACHABLE);
    DMOD_TEST_EXPECT_EQ(parsed.code, (uint8_t)dmicmp_v4_dest_unreachable_port);
    DMOD_TEST_EXPECT_EQ(parsed.identifier, (uint16_t)0);
    DMOD_TEST_EXPECT_EQ(parsed.sequence, (uint16_t)0);
}

/* ---- Checksum ---- */

DMOD_TEST_STEP(v4_checksum_valid_round_trip)
{
    uint8_t payload[4] = { 'p', 'i', 'n', 'g' };
    uint8_t message[DMICMP_HEADER_LEN + sizeof(payload)];
    build_v4_message(message, DMICMP_V4_TYPE_ECHO_REQUEST, 0, 1234, 1, payload, sizeof(payload));

    DMOD_TEST_EXPECT_TRUE(dmicmp_v4_checksum_valid(message, sizeof(message)));

    message[8] ^= 0xFF;
    DMOD_TEST_EXPECT_FALSE(dmicmp_v4_checksum_valid(message, sizeof(message)));
}

DMOD_TEST_STEP(v6_checksum_valid_round_trip)
{
    dmip_addr_t src = make_v6(1);
    dmip_addr_t dst = make_v6(2);
    uint8_t payload[4] = { 'p', 'o', 'n', 'g' };
    uint8_t message[DMICMP_HEADER_LEN + sizeof(payload)];
    build_v6_message(message, src, dst, DMICMP_V6_TYPE_ECHO_REPLY, 0, 4321, 2, payload, sizeof(payload));

    DMOD_TEST_EXPECT_TRUE(dmicmp_v6_checksum_valid(&src, &dst, message, sizeof(message)));

    message[9] ^= 0xFF;
    DMOD_TEST_EXPECT_FALSE(dmicmp_v6_checksum_valid(&src, &dst, message, sizeof(message)));
}

DMOD_TEST_STEP(checksum_valid_rejects_bad_arguments)
{
    dmip_addr_t v4_addr = make_v4(1, 1, 1, 1);
    dmip_addr_t v6_addr = make_v6(1);
    uint8_t message[DMICMP_HEADER_LEN] = { 0 };

    DMOD_TEST_EXPECT_FALSE(dmicmp_v4_checksum_valid(NULL, sizeof(message)));
    DMOD_TEST_EXPECT_FALSE(dmicmp_v4_checksum_valid(message, DMICMP_HEADER_LEN - 1));

    DMOD_TEST_EXPECT_FALSE(dmicmp_v6_checksum_valid(NULL, &v6_addr, message, sizeof(message)));
    DMOD_TEST_EXPECT_FALSE(dmicmp_v6_checksum_valid(&v4_addr, &v6_addr, message, sizeof(message)));
    DMOD_TEST_EXPECT_FALSE(dmicmp_v6_checksum_valid(&v6_addr, &v6_addr, message, DMICMP_HEADER_LEN - 1));
}

/* ---- Sending ---- */

/**
 * @brief Build a minimal valid IPv4 packet whose source is `src` - used
 *        as the "original_packet" dmicmp_v4_send_error() reports a
 *        problem with (the reply destination and route/ARP lookups are
 *        all keyed off `src`, not a `dst` parameter dmicmp_v4_send_error()
 *        doesn't have)
 */
static size_t build_original_v4_packet(uint8_t* out, dmip_addr_t src, const uint8_t* payload, size_t payload_len)
{
    dmip_v4_header_t header = { 0 };
    header.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + payload_len);
    header.ttl = DMIP_DEFAULT_TTL;
    header.protocol = DMIP_PROTO_UDP;
    header.src = src;
    header.dst = make_v4(198, 51, 100, 1); /* TEST-NET-2 - arbitrary, not otherwise used */

    dmip_v4_build_header(out, DMIP_V4_HEADER_LEN + payload_len, &header);
    if (payload_len > 0)
    {
        memcpy(out + DMIP_V4_HEADER_LEN, payload, payload_len);
    }
    return DMIP_V4_HEADER_LEN + payload_len;
}

#define TEST_TIME_EXCEEDED_TYPE 11u /* RFC 792 - no dedicated dmicmp constant yet, exactly the case dmicmp_v4_send_error() exists for */

DMOD_TEST_STEP(v4_send_error_rejects_bad_arguments)
{
    uint8_t not_a_packet[4] = { 0 };

    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_error(TEST_TIME_EXCEEDED_TYPE, 0, NULL, 0, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_error(TEST_TIME_EXCEEDED_TYPE, 0, not_a_packet, sizeof(not_a_packet), DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
}

DMOD_TEST_STEP(v4_send_error_no_route_returns_enetunreach)
{
    dmip_addr_t src = make_v4(203, 0, 113, 3); /* TEST-NET-3 - no route added anywhere in this file */
    uint8_t original_packet[DMIP_V4_HEADER_LEN];
    build_original_v4_packet(original_packet, src, NULL, 0);

    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_error(TEST_TIME_EXCEEDED_TYPE, 0, original_packet, sizeof(original_packet), DMARP_DEFAULT_TIMEOUT_MS), -ENETUNREACH);
}

DMOD_TEST_STEP(v4_send_error_full_path_without_real_driver_returns_eio)
{
    /* type = Time Exceeded here (not Destination Unreachable) to prove
     * dmicmp_v4_send_error() genuinely works for an arbitrary message
     * type, not just the one dmicmp_v4_send_dest_unreachable() wraps. */
    dmip_addr_t dest_net = make_v4(172, 16, 12, 0);
    dmip_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_route_t route = dmroute_add(&dest_net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);

    dmip_addr_t src = make_v4(172, 16, 12, 5);
    dmnetif_mac_addr_t fake_mac = { { 0x02, 0x00, 0x00, 0x00, 0x00, 0x32 } };
    dmarp_cache_insert(g_iface, &src, &fake_mac);

    uint8_t payload[3] = { 'h', 'i', '!' };
    uint8_t original_packet[DMIP_V4_HEADER_LEN + sizeof(payload)];
    build_original_v4_packet(original_packet, src, payload, sizeof(payload));

    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_error(TEST_TIME_EXCEEDED_TYPE, 0, original_packet, sizeof(original_packet), DMARP_DEFAULT_TIMEOUT_MS), -EIO);

    dmarp_cache_remove(g_iface, &src);
    dmroute_remove(route);
}

DMOD_TEST_STEP(v4_send_dest_unreachable_rejects_bad_arguments)
{
    uint8_t not_a_packet[4] = { 0 };

    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_protocol, NULL, 0, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_protocol, not_a_packet, sizeof(not_a_packet), DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
}

DMOD_TEST_STEP(v4_send_dest_unreachable_full_path_without_real_driver_returns_eio)
{
    /* dmudp's own future use case is exactly this: report a delivery
     * failure (here, Port Unreachable) for some original packet it
     * received, without building the ICMP message itself. */
    dmip_addr_t dest_net = make_v4(172, 16, 11, 0);
    dmip_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_route_t route = dmroute_add(&dest_net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);

    dmip_addr_t original_sender = make_v4(172, 16, 11, 6);
    dmnetif_mac_addr_t fake_mac = { { 0x02, 0x00, 0x00, 0x00, 0x00, 0x31 } };
    dmarp_cache_insert(g_iface, &original_sender, &fake_mac);

    dmip_v4_header_t ip_header = { 0 };
    ip_header.ttl = DMIP_DEFAULT_TTL;
    ip_header.protocol = DMIP_PROTO_UDP;
    ip_header.src = original_sender;
    ip_header.dst = make_v4(172, 16, 11, 1);
    uint8_t original_udp_payload[8] = { 0, 53, 0, 12345 >> 8, 12345 & 0xFF, 0, 8, 0 };
    ip_header.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + sizeof(original_udp_payload));

    uint8_t original_packet[DMIP_V4_HEADER_LEN + sizeof(original_udp_payload)];
    dmip_v4_build_header(original_packet, sizeof(original_packet), &ip_header);
    memcpy(original_packet + DMIP_V4_HEADER_LEN, original_udp_payload, sizeof(original_udp_payload));

    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_port, original_packet, sizeof(original_packet), DMARP_DEFAULT_TIMEOUT_MS), -EIO);

    dmarp_cache_remove(g_iface, &original_sender);
    dmroute_remove(route);
}

DMOD_TEST_STEP(v4_send_echo_request_rejects_bad_arguments)
{
    dmip_addr_t dst = make_v4(10, 0, 0, 1);
    dmip_addr_t bad_family = dst;
    bad_family.family = dmip_family_v6;

    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_echo_request(NULL, 1, 1, NULL, 0, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmicmp_v4_send_echo_request(&bad_family, 1, 1, NULL, 0, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
}

/* ---- Echo listener registry ---- */

DMOD_TEST_STEP(register_echo_listener_rejects_null_handler)
{
    DMOD_TEST_EXPECT_EQ(dmicmp_register_echo_listener(1, NULL), -EINVAL);
}

static void unused_echo_handler(const dmip_addr_t* src, uint16_t identifier, uint16_t sequence, const uint8_t* payload, size_t payload_len)
{
    (void)src; (void)identifier; (void)sequence; (void)payload; (void)payload_len;
}

DMOD_TEST_STEP(register_echo_listener_twice_returns_eexist)
{
    DMOD_TEST_EXPECT_EQ(dmicmp_register_echo_listener(100, unused_echo_handler), 0);
    DMOD_TEST_EXPECT_EQ(dmicmp_register_echo_listener(100, unused_echo_handler), -EEXIST);

    dmicmp_unregister_echo_listener(100);
}

DMOD_TEST_STEP(unregister_echo_listener_unregistered_is_safe)
{
    dmicmp_unregister_echo_listener(999); /* must not crash */
}

/* ---- Receiving an Echo Reply ---- */

static bool     g_listener_called;
static dmip_addr_t g_listener_src;
static uint16_t g_listener_identifier;
static uint16_t g_listener_sequence;
static uint8_t  g_listener_payload[TEST_MAX_MESSAGE_LEN];
static size_t   g_listener_payload_len;

static void recording_echo_handler(const dmip_addr_t* src, uint16_t identifier, uint16_t sequence, const uint8_t* payload, size_t payload_len)
{
    g_listener_called = true;
    g_listener_src = *src;
    g_listener_identifier = identifier;
    g_listener_sequence = sequence;
    g_listener_payload_len = payload_len;
    if (payload_len > 0)
    {
        memcpy(g_listener_payload, payload, payload_len);
    }
}

static void reset_listener_recording(void)
{
    g_listener_called = false;
    memset(&g_listener_src, 0, sizeof(g_listener_src));
    g_listener_identifier = 0;
    g_listener_sequence = 0;
    g_listener_payload_len = 0;
}

DMOD_TEST_STEP(echo_reply_delivered_to_registered_listener)
{
    reset_listener_recording();
    DMOD_TEST_EXPECT_EQ(dmicmp_register_echo_listener(2222, recording_echo_handler), 0);

    dmip_addr_t src = make_v4(10, 5, 0, 1);
    dmip_addr_t dst = make_v4(10, 5, 0, 2);
    uint8_t payload[4] = { 'p', 'i', 'n', 'g' };
    uint8_t message[DMICMP_HEADER_LEN + sizeof(payload)];
    size_t message_len = build_v4_message(message, DMICMP_V4_TYPE_ECHO_REPLY, 0, 2222, 5, payload, sizeof(payload));

    feed_icmp_packet(g_iface, dmip_family_v4, src, dst, message, message_len);

    DMOD_TEST_EXPECT_TRUE(g_listener_called);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_listener_src.addr.v4, src.addr.v4, DMIP_IPV4_ADDR_LEN));
    DMOD_TEST_EXPECT_EQ(g_listener_identifier, (uint16_t)2222);
    DMOD_TEST_EXPECT_EQ(g_listener_sequence, (uint16_t)5);
    DMOD_TEST_EXPECT_EQ(g_listener_payload_len, sizeof(payload));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_listener_payload, payload, sizeof(payload)));

    dmicmp_unregister_echo_listener(2222);
}

DMOD_TEST_STEP(echo_reply_v6_delivered_to_registered_listener)
{
    reset_listener_recording();
    DMOD_TEST_EXPECT_EQ(dmicmp_register_echo_listener(3333, recording_echo_handler), 0);

    dmip_addr_t src = make_v6(10);
    dmip_addr_t dst = make_v6(11);
    uint8_t payload[4] = { 'p', 'o', 'n', 'g' };
    uint8_t message[DMICMP_HEADER_LEN + sizeof(payload)];
    size_t message_len = build_v6_message(message, src, dst, DMICMP_V6_TYPE_ECHO_REPLY, 0, 3333, 9, payload, sizeof(payload));

    feed_icmp_packet(g_iface, dmip_family_v6, src, dst, message, message_len);

    DMOD_TEST_EXPECT_TRUE(g_listener_called);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_listener_src.addr.v6, src.addr.v6, DMIP_IPV6_ADDR_LEN));
    DMOD_TEST_EXPECT_EQ(g_listener_identifier, (uint16_t)3333);
    DMOD_TEST_EXPECT_EQ(g_listener_sequence, (uint16_t)9);
    DMOD_TEST_EXPECT_EQ(g_listener_payload_len, sizeof(payload));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_listener_payload, payload, sizeof(payload)));

    dmicmp_unregister_echo_listener(3333);
}

DMOD_TEST_STEP(echo_reply_with_no_listener_is_dropped)
{
    reset_listener_recording();

    dmip_addr_t src = make_v4(10, 6, 0, 1);
    dmip_addr_t dst = make_v4(10, 6, 0, 2);
    uint8_t message[DMICMP_HEADER_LEN];
    size_t message_len = build_v4_message(message, DMICMP_V4_TYPE_ECHO_REPLY, 0, 4444, 1, NULL, 0);

    feed_icmp_packet(g_iface, dmip_family_v4, src, dst, message, message_len); /* must not crash */

    DMOD_TEST_EXPECT_FALSE(g_listener_called);
}

DMOD_TEST_STEP(corrupted_checksum_echo_reply_produces_no_listener_call)
{
    reset_listener_recording();
    DMOD_TEST_EXPECT_EQ(dmicmp_register_echo_listener(5555, recording_echo_handler), 0);

    dmip_addr_t src = make_v4(10, 7, 0, 1);
    dmip_addr_t dst = make_v4(10, 7, 0, 2);
    uint8_t message[DMICMP_HEADER_LEN];
    size_t message_len = build_v4_message(message, DMICMP_V4_TYPE_ECHO_REPLY, 0, 5555, 1, NULL, 0);
    message[2] ^= 0xFF; /* corrupt the checksum */

    feed_icmp_packet(g_iface, dmip_family_v4, src, dst, message, message_len);

    DMOD_TEST_EXPECT_FALSE(g_listener_called);

    dmicmp_unregister_echo_listener(5555);
}

/* ---- Auto-reply / default-handler wiring (smoke tests - see this
 * file's top comment for why the actual reply/error bytes can't be
 * captured here) ---- */

DMOD_TEST_STEP(echo_request_v4_received_does_not_crash)
{
    dmip_addr_t src = make_v4(10, 8, 0, 1);
    dmip_addr_t dst = make_v4(10, 8, 0, 2);
    uint8_t message[DMICMP_HEADER_LEN];
    size_t message_len = build_v4_message(message, DMICMP_V4_TYPE_ECHO_REQUEST, 0, 1, 1, NULL, 0);

    feed_icmp_packet(g_iface, dmip_family_v4, src, dst, message, message_len);
}

DMOD_TEST_STEP(echo_request_v6_received_does_not_crash)
{
    dmip_addr_t src = make_v6(20);
    dmip_addr_t dst = make_v6(21);
    uint8_t message[DMICMP_HEADER_LEN];
    size_t message_len = build_v6_message(message, src, dst, DMICMP_V6_TYPE_ECHO_REQUEST, 0, 1, 1, NULL, 0);

    feed_icmp_packet(g_iface, dmip_family_v6, src, dst, message, message_len); /* logged and dropped - no dmip_v6_send() yet */
}

DMOD_TEST_STEP(unclaimed_protocol_packet_dispatches_without_crashing)
{
    dmip_addr_t src = make_v4(10, 9, 0, 1);
    dmip_addr_t dst = make_v4(10, 9, 0, 2);
    uint8_t payload[8] = { 0 };

    feed_unclaimed_v4_packet(g_iface, src, dst, payload, sizeof(payload));
}
