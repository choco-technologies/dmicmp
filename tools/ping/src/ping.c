/**
 * @file ping.c
 * @brief ping - send ICMPv4 Echo Requests to a host and report the replies
 *
 * A thin CLI over dmicmp's Echo Request/Reply API
 * (dmicmp_v4_send_echo_request(), dmicmp_register_echo_listener()/
 * _unregister_echo_listener() - see include/dmicmp.h). Registration is
 * re-done for every request: the identifier is fixed for the whole run,
 * but each iteration sets `g_expected_sequence` before sending and drops
 * (via echo_reply_handler()'s own sequence check) any reply that doesn't
 * match it - guards against a stale reply for an earlier request arriving
 * after that request already timed out.
 *
 * <address> must be a literal dotted-decimal IPv4 address - dmicmp has no
 * hostname resolution, and no ICMPv6 send path yet (see dmicmp.h's top
 * comment), so this tool cannot ping an IPv6 destination either.
 */
#include "dmod.h"
#include "dmicmp.h"
#include "dmroute.h"
#include "dmarp.h"
#include "dmosi.h"
#include <string.h>

#define PING_DEFAULT_COUNT       4u
#define PING_DEFAULT_INTERVAL_MS 1000u
#define PING_DEFAULT_TIMEOUT_MS  1000u
#define PING_DEFAULT_PAYLOAD_LEN 32u
#define PING_MAX_PAYLOAD_LEN     1024u

static dmosi_semaphore_t g_reply_sem;
static uint16_t g_expected_sequence;
static bool g_reply_matched;

/* Parses a plain decimal uint32_t (dmod's minimal module runtime has no
 * strtol()/atoi() - see dmod/src/module/string.c's replacement set). */
static bool parse_uint32(const char* s, uint32_t* out)
{
    if (s == NULL || *s == '\0')
        return false;

    uint64_t value = 0;
    for (const char* p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;

        value = value * 10 + (uint64_t)(*p - '0');
        if (value > UINT32_MAX)
            return false;
    }

    *out = (uint32_t)value;
    return true;
}

/* Parses "A.B.C.D" (each octet 0-255) into an IPv4 dmroute_addr_t. */
static bool parse_ipv4_address(const char* s, dmroute_addr_t* ip)
{
    if (s == NULL)
        return false;

    ip->family = dmroute_family_v4;

    for (int i = 0; i < DMROUTE_IPV4_ADDR_LEN; i++)
    {
        if (*s < '0' || *s > '9')
            return false;

        uint32_t octet = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            octet = octet * 10 + (uint32_t)(*s - '0');
            if (octet > 255 || ++digits > 3)
                return false;
            s++;
        }
        ip->addr.v4[i] = (uint8_t)octet;

        if (i < DMROUTE_IPV4_ADDR_LEN - 1)
        {
            if (*s != '.')
                return false;
            s++;
        }
    }

    return *s == '\0';
}

/* Only ever called while a single dmicmp_register_echo_listener() /
 * dmicmp_v4_send_echo_request() pair is outstanding (send_one_ping()
 * sends requests one at a time, never concurrently) - g_expected_sequence
 * and g_reply_matched are safe as plain statics under that single-writer
 * invariant. */
static void echo_reply_handler(const dmip_addr_t* src, uint16_t identifier, uint16_t sequence, const uint8_t* payload, size_t payload_len)
{
    (void)src;
    (void)identifier;
    (void)payload;
    (void)payload_len;

    if (sequence != g_expected_sequence)
        return;

    g_reply_matched = true;
    dmosi_semaphore_post(g_reply_sem, 1);
}

/* Sends one Echo Request and blocks (up to wait_timeout_ms) for its
 * reply. Returns 0 and prints a "bytes from ..." line if a matching reply
 * arrived in time, -1 (and prints why) otherwise. */
static int send_one_ping(const dmroute_addr_t* dst, const char* address_str, uint16_t identifier, uint16_t sequence,
                          const uint8_t* payload, size_t payload_len, uint32_t wait_timeout_ms)
{
    g_reply_matched = false;
    g_expected_sequence = sequence;

    int reg_ret = dmicmp_register_echo_listener(identifier, echo_reply_handler);
    if (reg_ret != 0)
    {
        Dmod_Printf("ping: failed to register reply listener for icmp_seq=%u (error %d)\n", (unsigned)sequence, reg_ret);
        return -1;
    }

    uint32_t start_tick = dmosi_get_tick_count();
    int send_ret = dmicmp_v4_send_echo_request(dst, identifier, sequence, payload, payload_len, DMARP_DEFAULT_TIMEOUT_MS);
    if (send_ret != 0)
    {
        dmicmp_unregister_echo_listener(identifier);
        Dmod_Printf("ping: send failed for icmp_seq=%u (error %d)\n", (unsigned)sequence, send_ret);
        return -1;
    }

    int wait_ret = dmosi_semaphore_wait(g_reply_sem, 1, (int32_t)wait_timeout_ms);
    dmicmp_unregister_echo_listener(identifier);

    if (wait_ret == 0 && g_reply_matched)
    {
        uint32_t rtt_ms = dmosi_get_tick_count() - start_tick;
        Dmod_Printf("%u bytes from %s: icmp_seq=%u time=%u ms\n",
                    (unsigned)payload_len, address_str, (unsigned)sequence, (unsigned)rtt_ms);
        return 0;
    }

    Dmod_Printf("Request timeout for icmp_seq %u\n", (unsigned)sequence);
    return -1;
}

static void print_usage(const char* prog)
{
    Dmod_Printf("Usage:\n");
    Dmod_Printf("  %s <address>       Send %u ICMPv4 Echo Requests to <address>\n", prog, (unsigned)PING_DEFAULT_COUNT);
    Dmod_Printf("  %s --help | -h     Show this help\n", prog);
    Dmod_Printf("\n");
    Dmod_Printf("Options:\n");
    Dmod_Printf("  -c <count>         Number of Echo Requests to send (default: %u)\n", (unsigned)PING_DEFAULT_COUNT);
    Dmod_Printf("  -i <interval_ms>   Delay between requests, in milliseconds (default: %u)\n", (unsigned)PING_DEFAULT_INTERVAL_MS);
    Dmod_Printf("  -W <timeout_ms>    Time to wait for a reply before it's lost, in milliseconds (default: %u)\n", (unsigned)PING_DEFAULT_TIMEOUT_MS);
    Dmod_Printf("  -s <size>          Number of payload bytes to send, 0-%u (default: %u)\n", (unsigned)PING_MAX_PAYLOAD_LEN, (unsigned)PING_DEFAULT_PAYLOAD_LEN);
    Dmod_Printf("\n");
    Dmod_Printf("<address> must be a literal IPv4 address (e.g. 192.168.1.1) - dmicmp has no\n");
    Dmod_Printf("hostname resolution and no ICMPv6 send support yet.\n");
}

int main(int argc, char* argv[])
{
    const char* prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "ping";

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        print_usage(prog);
        return 0;
    }

    if (argc < 2)
    {
        print_usage(prog);
        return 1;
    }

    dmroute_addr_t dst = { 0 };
    if (!parse_ipv4_address(argv[1], &dst))
    {
        Dmod_Printf("%s: invalid IPv4 address '%s'\n", prog, argv[1]);
        return 1;
    }

    uint32_t count = PING_DEFAULT_COUNT;
    uint32_t interval_ms = PING_DEFAULT_INTERVAL_MS;
    uint32_t wait_timeout_ms = PING_DEFAULT_TIMEOUT_MS;
    uint32_t payload_len = PING_DEFAULT_PAYLOAD_LEN;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
        {
            if (!parse_uint32(argv[++i], &count) || count == 0)
            {
                Dmod_Printf("%s: invalid count '%s'\n", prog, argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
        {
            if (!parse_uint32(argv[++i], &interval_ms))
            {
                Dmod_Printf("%s: invalid interval '%s'\n", prog, argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc)
        {
            if (!parse_uint32(argv[++i], &wait_timeout_ms) || wait_timeout_ms == 0)
            {
                Dmod_Printf("%s: invalid timeout '%s'\n", prog, argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
        {
            if (!parse_uint32(argv[++i], &payload_len) || payload_len > PING_MAX_PAYLOAD_LEN)
            {
                Dmod_Printf("%s: invalid size '%s'\n", prog, argv[i]);
                return 1;
            }
        }
        else
        {
            print_usage(prog);
            return 1;
        }
    }

    uint8_t* payload = NULL;
    if (payload_len > 0)
    {
        payload = Dmod_Malloc(payload_len);
        if (payload == NULL)
        {
            Dmod_Printf("%s: out of memory allocating a %u-byte payload\n", prog, (unsigned)payload_len);
            return 1;
        }
        for (uint32_t i = 0; i < payload_len; i++)
            payload[i] = (uint8_t)i;
    }

    g_reply_sem = dmosi_semaphore_create(0, 1);
    if (g_reply_sem == NULL)
    {
        Dmod_Printf("%s: failed to create the reply semaphore\n", prog);
        if (payload != NULL)
            Dmod_Free(payload);
        return 1;
    }

    /* Ticks-derived rather than a fixed constant, so two `ping` runs in
     * flight at once (e.g. against different hosts) are unlikely to share
     * an identifier and cross-deliver each other's replies. */
    uint16_t identifier = (uint16_t)(dmosi_get_tick_count() & 0xFFFFu);

    Dmod_Printf("PING %s: %u data bytes\n", argv[1], (unsigned)payload_len);

    uint32_t transmitted = 0;
    uint32_t received = 0;

    for (uint32_t seq = 0; seq < count; seq++)
    {
        transmitted++;
        if (send_one_ping(&dst, argv[1], identifier, (uint16_t)seq, payload, payload_len, wait_timeout_ms) == 0)
            received++;

        if (seq + 1 < count)
            dmosi_thread_sleep(interval_ms);
    }

    dmosi_semaphore_destroy(g_reply_sem);
    g_reply_sem = NULL;
    if (payload != NULL)
        Dmod_Free(payload);

    uint32_t loss_percent = (transmitted > 0) ? ((transmitted - received) * 100u) / transmitted : 0u;
    Dmod_Printf("\n--- %s ping statistics ---\n", argv[1]);
    Dmod_Printf("%u packets transmitted, %u packets received, %u%% packet loss\n",
                (unsigned)transmitted, (unsigned)received, (unsigned)loss_percent);

    return (received > 0) ? 0 : 1;
}
