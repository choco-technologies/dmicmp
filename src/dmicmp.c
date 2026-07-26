/**
 * @file dmicmp.c
 * @brief DMOD ICMP - Implementation
 *
 * Three loosely-coupled pieces live here:
 *
 *  - Header build/parse and checksum (ICMPv4: plain dmip_checksum() over
 *    the message; ICMPv6: the same primitive over an IPv6 pseudo-header
 *    + message, RFC 4443 2.3 - see dmicmp.h).
 *
 *  - Sending: send_v4_message() is the one place that actually builds and
 *    transmits an ICMPv4 message (via dmip_send()) - dmicmp_v4_send_error()
 *    (the general-purpose "report a problem with this original packet"
 *    primitive), dmicmp_v4_send_dest_unreachable(), and
 *    dmicmp_v4_send_echo_request() are all thin wrappers over it.
 *
 *  - Receiving: dmicmp registers with dmip two ways at once (see
 *    dmod_init()) - dmip_register_protocol() for DMIP_PROTO_ICMP/
 *    _ICMPV6, to answer an incoming Echo Request inline and deliver an
 *    incoming Echo Reply to whichever dmicmp_register_echo_listener()
 *    call matches its identifier (a small dmlist-backed table, guarded
 *    by g_echo_mutex - the same shape dmip.c's own protocol dispatch
 *    table uses, just keyed by identifier instead of protocol number);
 *    and dmip_register_default_protocol(), to answer any IP packet whose
 *    protocol nobody claimed with an ICMPv4 Destination Unreachable.
 *
 * There is no dmip_v6_send() yet - an incoming ICMPv6 Echo Request or an
 * unclaimed IPv6 protocol can be parsed and checksum-validated correctly,
 * but not answered; those paths log a warning and drop rather than
 * silently doing nothing. See docs/dmicmp.md.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmicmp.h"
#include "dmlist.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

/**
 * @brief Default ARP-resolution timeout for a reply/error dmicmp sends
 *        on its own initiative (auto Echo Reply, dmip's default-handler
 *        path) - mirrors dmarp's own DMARP_DEFAULT_TIMEOUT_MS, duplicated
 *        here rather than linking dmarp just for one constant (dmicmp
 *        only depends on dmip - see CMakeLists.txt)
 */
#define DMICMP_DEFAULT_ARP_TIMEOUT_MS 1000u

#define DMICMP_V6_PSEUDO_HEADER_LEN 40u

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static uint16_t read_u16_be(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* ============================================================================
 *                      Common header
 * ========================================================================== */

/**
 * @brief Implementation of dmicmp_build_header() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, int, _build_header, ( uint8_t* buffer, size_t buffer_len, const dmicmp_header_t* header ))
{
    if (buffer == NULL || header == NULL || buffer_len < DMICMP_HEADER_LEN)
        return -EINVAL;

    buffer[0] = header->type;
    buffer[1] = header->code;
    write_u16_be(&buffer[2], 0); /* checksum - filled in by the caller once the payload is known, see send_v4_message() */
    write_u16_be(&buffer[4], header->identifier);
    write_u16_be(&buffer[6], header->sequence);
    return 0;
}

/**
 * @brief Implementation of dmicmp_parse_header() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, int, _parse_header, ( const uint8_t* buffer, size_t length, dmicmp_header_t* header ))
{
    if (buffer == NULL || header == NULL || length < DMICMP_HEADER_LEN)
        return -EINVAL;

    header->type = buffer[0];
    header->code = buffer[1];
    header->checksum = read_u16_be(&buffer[2]);
    header->identifier = read_u16_be(&buffer[4]);
    header->sequence = read_u16_be(&buffer[6]);
    return 0;
}

/* ============================================================================
 *                      Checksum
 * ========================================================================== */

/**
 * @brief Implementation of dmicmp_v4_checksum_valid() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, bool, _v4_checksum_valid, ( const uint8_t* message, size_t length ))
{
    if (message == NULL || length < DMICMP_HEADER_LEN)
        return false;

    return dmip_checksum(message, length) == 0;
}

/**
 * @brief Write the 40-byte IPv6 pseudo-header (RFC 4443 2.3) into `buf`
 */
static void write_v6_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint32_t message_len)
{
    memcpy(&buf[0], src_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    memcpy(&buf[16], dst_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    buf[32] = (uint8_t)(message_len >> 24);
    buf[33] = (uint8_t)(message_len >> 16);
    buf[34] = (uint8_t)(message_len >> 8);
    buf[35] = (uint8_t)(message_len & 0xFFu);
    buf[36] = 0;
    buf[37] = 0;
    buf[38] = 0;
    buf[39] = DMIP_PROTO_ICMPV6;
}

/**
 * @brief Implementation of dmicmp_v6_checksum_valid() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, bool, _v6_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* message, size_t length ))
{
    if (src_ip == NULL || dst_ip == NULL || message == NULL || length < DMICMP_HEADER_LEN)
        return false;
    if (src_ip->family != dmip_family_v6 || dst_ip->family != dmip_family_v6)
        return false;

    size_t total = DMICMP_V6_PSEUDO_HEADER_LEN + length;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return false;

    write_v6_pseudo_header(buf, src_ip, dst_ip, (uint32_t)length);
    memcpy(buf + DMICMP_V6_PSEUDO_HEADER_LEN, message, length);

    bool valid = dmip_checksum(buf, total) == 0;
    Dmod_Free(buf);
    return valid;
}

/* ============================================================================
 *                      Sending
 * ========================================================================== */

/**
 * @brief Build, checksum and transmit an ICMPv4 message - the one place
 *        that actually calls dmip_send() for one
 *
 * Shared by dmicmp_v4_send_error() (and, through it,
 * dmicmp_v4_send_dest_unreachable()), dmicmp_v4_send_echo_request(), and
 * the inline Echo Reply auto-response in handle_v4_icmp_message().
 * `header->src` is left unset in the dmip_header_t built here -
 * dmip_v4_send() fills it in itself (unlike UDP, an ICMPv4 checksum
 * doesn't need the source address up front, so there's no need to call
 * dmip_v4_get_source_address() first).
 */
static int send_v4_message(const dmip_addr_t* dst, const dmicmp_header_t* header, const void* payload, size_t payload_len, uint32_t arp_timeout_ms)
{
    if (dst == NULL || header == NULL || (payload == NULL && payload_len > 0))
        return -EINVAL;
    if (dst->family != dmip_family_v4)
        return -EINVAL;

    size_t message_len = DMICMP_HEADER_LEN + payload_len;
    uint8_t* message = Dmod_Malloc(message_len);
    if (message == NULL)
        return -ENOMEM;

    dmicmp_build_header(message, message_len, header);
    if (payload_len > 0)
        memcpy(message + DMICMP_HEADER_LEN, payload, payload_len);

    write_u16_be(&message[2], dmip_checksum(message, message_len));

    dmip_header_t ip_header = { 0 };
    ip_header.family = dmip_family_v4;
    ip_header.header.v4.ttl = DMIP_DEFAULT_TTL;
    ip_header.header.v4.protocol = DMIP_PROTO_ICMP;
    ip_header.header.v4.identification = dmip_v4_next_identification();
    ip_header.header.v4.dst = *dst;

    int result = dmip_send(&ip_header, message, message_len, arp_timeout_ms);

    Dmod_Free(message);
    return result;
}

/**
 * @brief Implementation of dmicmp_v4_send_error() - see dmicmp.h
 *
 * Reads the reply destination and the RFC 792 quote (original header +
 * up to 8 bytes of payload) straight out of `original_packet` itself -
 * only `type`/`code` come from the caller.
 */
dmod_dmicmp_api_declaration(1.0, int, _v4_send_error, ( uint8_t type, uint8_t code, const uint8_t* original_packet, size_t original_packet_len, uint32_t arp_timeout_ms ))
{
    if (original_packet == NULL)
        return -EINVAL;

    dmip_v4_header_t ip_header = { 0 };
    size_t header_len = 0;
    if (dmip_v4_parse_header(original_packet, original_packet_len, &ip_header, &header_len) != 0)
        return -EINVAL;

    /* RFC 792: quote the original header plus (up to) the first 8 bytes
     * of its payload - never more, even if the original packet is longer. */
    size_t remaining = original_packet_len - header_len;
    size_t quote_len = header_len + ((remaining < 8u) ? remaining : 8u);

    dmicmp_header_t header = { .type = type, .code = code };
    return send_v4_message(&ip_header.src, &header, original_packet, quote_len, arp_timeout_ms);
}

/**
 * @brief Implementation of dmicmp_v4_send_dest_unreachable() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, int, _v4_send_dest_unreachable, ( dmicmp_v4_dest_unreachable_code_t code, const uint8_t* original_packet, size_t original_packet_len, uint32_t arp_timeout_ms ))
{
    return dmicmp_v4_send_error(DMICMP_V4_TYPE_DEST_UNREACHABLE, (uint8_t)code, original_packet, original_packet_len, arp_timeout_ms);
}

/* ============================================================================
 *                      Echo listener registry
 *
 * A dmlist of struct dmicmp_echo_listener*, guarded by g_echo_mutex - the
 * same shape dmip.c's own protocol-handler table uses (see dmip.c's
 * "Protocol dispatch" section), just keyed by the ICMP echo identifier
 * instead of an IP protocol number, and with no "default" tier: an Echo
 * Reply for an identifier nobody registered is simply dropped.
 * ========================================================================== */

struct dmicmp_echo_listener
{
    uint16_t                    identifier;
    dmicmp_echo_reply_handler_t handler;
};

static dmlist_context_t* g_echo_listeners = NULL;
static dmosi_mutex_t     g_echo_mutex = NULL;

/**
 * @brief dmlist_compare_func_t matching a struct dmicmp_echo_listener
 *        against a `const uint16_t*` identifier needle
 */
static int compare_echo_identifier(const void* data, const void* user_data)
{
    const struct dmicmp_echo_listener* entry = (const struct dmicmp_echo_listener*)data;
    uint16_t identifier = *(const uint16_t*)user_data;
    return (entry->identifier == identifier) ? 0 : -1;
}

static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

/**
 * @brief Implementation of dmicmp_register_echo_listener() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, int, _register_echo_listener, ( uint16_t identifier, dmicmp_echo_reply_handler_t handler ))
{
    if (handler == NULL)
        return -EINVAL;

    dmosi_mutex_lock(g_echo_mutex);

    int result;
    if (dmlist_find(g_echo_listeners, &identifier, compare_echo_identifier) != NULL)
    {
        result = -EEXIST;
    }
    else
    {
        struct dmicmp_echo_listener* entry = Dmod_Malloc(sizeof(*entry));
        if (entry == NULL)
        {
            result = -ENOMEM;
        }
        else
        {
            entry->identifier = identifier;
            entry->handler = handler;
            if (dmlist_push_back(g_echo_listeners, entry))
            {
                result = 0;
            }
            else
            {
                Dmod_Free(entry);
                result = -ENOMEM;
            }
        }
    }

    dmosi_mutex_unlock(g_echo_mutex);
    return result;
}

/**
 * @brief Implementation of dmicmp_unregister_echo_listener() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, void, _unregister_echo_listener, ( uint16_t identifier ))
{
    dmosi_mutex_lock(g_echo_mutex);

    struct dmicmp_echo_listener* entry = (struct dmicmp_echo_listener*)dmlist_find(g_echo_listeners, &identifier, compare_echo_identifier);
    if (entry != NULL)
    {
        dmlist_remove(g_echo_listeners, entry, compare_pointer);
        Dmod_Free(entry);
    }

    dmosi_mutex_unlock(g_echo_mutex);
}

/**
 * @brief Implementation of dmicmp_v4_send_echo_request() - see dmicmp.h
 */
dmod_dmicmp_api_declaration(1.0, int, _v4_send_echo_request, ( const dmip_addr_t* dst, uint16_t identifier, uint16_t sequence, const void* payload, size_t payload_len, uint32_t arp_timeout_ms ))
{
    dmicmp_header_t header = { .type = DMICMP_V4_TYPE_ECHO_REQUEST, .identifier = identifier, .sequence = sequence };
    return send_v4_message(dst, &header, payload, payload_len, arp_timeout_ms);
}

/**
 * @brief Look up `identifier` in g_echo_listeners and call its handler if
 *        one is registered - shared by the v4 and v6 Echo Reply paths
 *
 * Mutex is held only for the lookup, released before calling out - same
 * "don't hold the lock into another module's code" reasoning dmip.c's
 * own dispatch_packet() documents for itself, so a handler is free to
 * call dmicmp_register_echo_listener() again (e.g. for its next ping)
 * from inside the callback.
 */
static void deliver_echo_reply(const dmip_addr_t* src, uint16_t identifier, uint16_t sequence, const uint8_t* payload, size_t payload_len)
{
    dmosi_mutex_lock(g_echo_mutex);
    struct dmicmp_echo_listener* entry = (struct dmicmp_echo_listener*)dmlist_find(g_echo_listeners, &identifier, compare_echo_identifier);
    dmicmp_echo_reply_handler_t handler = (entry != NULL) ? entry->handler : NULL;
    dmosi_mutex_unlock(g_echo_mutex);

    if (handler != NULL)
    {
        handler(src, identifier, sequence, payload, payload_len);
    }
}

/* ============================================================================
 *                      Receiving
 * ========================================================================== */

/**
 * @brief Handle one checksum-verified, parsed ICMPv4 message
 *
 * An Echo Request is answered inline, right here - this function is
 * already running on whatever thread is pumping the interface (see
 * dmip_protocol_handler_t in dmip.h), so there's no need for an extra
 * thread or queue just to answer a ping. An Echo Reply is delivered to
 * whichever dmicmp_register_echo_listener() call matches its identifier,
 * if any. Anything else (Destination Unreachable, any other type/code)
 * has no consumer yet - a documented extension point, not silently
 * papered over.
 */
static void handle_v4_icmp_message(const dmip_addr_t* src, const uint8_t* message, size_t message_len)
{
    if (!dmicmp_v4_checksum_valid(message, message_len))
        return;

    dmicmp_header_t header = { 0 };
    if (dmicmp_parse_header(message, message_len, &header) != 0)
        return;

    const uint8_t* payload = message + DMICMP_HEADER_LEN;
    size_t payload_len = message_len - DMICMP_HEADER_LEN;

    if (header.type == DMICMP_V4_TYPE_ECHO_REQUEST)
    {
        dmicmp_header_t reply = { .type = DMICMP_V4_TYPE_ECHO_REPLY, .identifier = header.identifier, .sequence = header.sequence };
        send_v4_message(src, &reply, payload, payload_len, DMICMP_DEFAULT_ARP_TIMEOUT_MS);
    }
    else if (header.type == DMICMP_V4_TYPE_ECHO_REPLY)
    {
        deliver_echo_reply(src, header.identifier, header.sequence, payload, payload_len);
    }
}

/**
 * @brief Handle one checksum-verified, parsed ICMPv6 message
 *
 * Receive-only: an Echo Request here cannot be answered (no
 * dmip_v6_send() yet), so it's logged and dropped instead of silently
 * doing nothing. An Echo Reply, by contrast, can still reach a
 * registered listener - nothing about *delivering* a reply needs
 * dmip_v6_send(), only *sending* our own Echo Request would.
 */
static void handle_v6_icmp_message(const dmip_addr_t* src, const dmip_addr_t* dst, const uint8_t* message, size_t message_len)
{
    if (!dmicmp_v6_checksum_valid(src, dst, message, message_len))
        return;

    dmicmp_header_t header = { 0 };
    if (dmicmp_parse_header(message, message_len, &header) != 0)
        return;

    if (header.type == DMICMP_V6_TYPE_ECHO_REQUEST)
    {
        DMOD_LOG_WARN("dmicmp: received ICMPv6 Echo Request, cannot reply - no dmip_v6_send() yet\n");
    }
    else if (header.type == DMICMP_V6_TYPE_ECHO_REPLY)
    {
        deliver_echo_reply(src, header.identifier, header.sequence, message + DMICMP_HEADER_LEN, message_len - DMICMP_HEADER_LEN);
    }
}

/**
 * @brief dmip_protocol_handler_t registered for DMIP_PROTO_ICMP and
 *        DMIP_PROTO_ICMPV6 - see dmod_init()
 *
 * Parses `packet`'s IP header to locate the ICMP message and read
 * `src`/`dst`, then hands off to handle_v4_icmp_message()/
 * _v6_icmp_message(). `packet` is borrowed (see dmip_protocol_handler_t's
 * own doc comment in dmip.h) - nothing here keeps a pointer into it past
 * the call.
 */
static void dmicmp_handle_ip_packet(dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len)
{
    (void)iface;

    if (family == dmip_family_v4)
    {
        dmip_v4_header_t ip_header = { 0 };
        size_t header_len = 0;
        if (dmip_v4_parse_header(packet, packet_len, &ip_header, &header_len) != 0)
            return;

        handle_v4_icmp_message(&ip_header.src, packet + header_len, packet_len - header_len);
    }
    else /* dmip_family_v6 */
    {
        dmip_v6_header_t ip_header = { 0 };
        if (dmip_v6_parse_header(packet, packet_len, &ip_header) != 0)
            return;

        handle_v6_icmp_message(&ip_header.src, &ip_header.dst, packet + DMIP_V6_HEADER_LEN, packet_len - DMIP_V6_HEADER_LEN);
    }
}

/**
 * @brief dmip_protocol_handler_t registered via
 *        dmip_register_default_protocol() - see dmod_init()
 *
 * Called for any IP packet whose protocol nobody claimed. Never actually
 * reached for a genuine ICMP packet - dmicmp itself claims both
 * DMIP_PROTO_ICMP and DMIP_PROTO_ICMPV6 via dmip_register_protocol(), so
 * dmip's dispatcher routes those to dmicmp_handle_ip_packet() instead,
 * never here.
 */
static void dmicmp_handle_unclaimed_protocol(dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len)
{
    (void)iface;

    if (family == dmip_family_v4)
    {
        dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_protocol, packet, packet_len, DMICMP_DEFAULT_ARP_TIMEOUT_MS);
    }
    else
    {
        /* The technically-correct v6 reply here would be a Parameter
         * Problem / Unrecognized Next Header (RFC 4443 3.4), not a
         * Destination Unreachable - noted so nobody "fixes" this to send
         * the wrong message type once dmip_v6_send() exists. */
        DMOD_LOG_WARN("dmicmp: received unclaimed IPv6 protocol, cannot send ICMPv6 error - no dmip_v6_send() yet\n");
    }
}

/* ---- DMOD lifecycle ---- */

/**
 * @brief Module initialization - allocates the echo listener table and
 *        its guarding mutex, then registers with dmip
 */
int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    g_echo_listeners = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_echo_mutex = dmosi_mutex_create(false);
    if (g_echo_listeners == NULL || g_echo_mutex == NULL)
    {
        DMOD_LOG_ERROR("Failed to allocate dmicmp state\n");
        return -1;
    }

    int result = dmip_register_protocol(DMIP_PROTO_ICMP, dmicmp_handle_ip_packet);
    if (result != 0)
    {
        DMOD_LOG_ERROR("dmicmp: cannot register as the ICMP protocol handler (%d)\n", result);
        return -1;
    }

    result = dmip_register_protocol(DMIP_PROTO_ICMPV6, dmicmp_handle_ip_packet);
    if (result != 0)
    {
        DMOD_LOG_ERROR("dmicmp: cannot register as the ICMPv6 protocol handler (%d)\n", result);
        return -1;
    }

    result = dmip_register_default_protocol(dmicmp_handle_unclaimed_protocol);
    if (result != 0)
    {
        DMOD_LOG_ERROR("dmicmp: cannot register as dmip's default protocol handler (%d)\n", result);
        return -1;
    }

    DMOD_LOG_INFO("DMICMP initialized\n");
    return 0;
}

/**
 * @brief Module deinitialization - unregisters from dmip, frees every
 *        remaining echo listener registration, then the table and mutex
 */
int dmod_deinit(void)
{
    dmip_unregister_default_protocol();
    dmip_unregister_protocol(DMIP_PROTO_ICMPV6);
    dmip_unregister_protocol(DMIP_PROTO_ICMP);

    size_t count = dmlist_size(g_echo_listeners);
    for (size_t i = 0; i < count; i++)
    {
        Dmod_Free(dmlist_pop_front(g_echo_listeners));
    }
    dmlist_destroy(g_echo_listeners);
    g_echo_listeners = NULL;

    dmosi_mutex_destroy(g_echo_mutex);
    g_echo_mutex = NULL;

    DMOD_LOG_INFO("DMICMP deinitialized\n");
    return 0;
}
