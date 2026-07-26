#ifndef DMICMP_H
#define DMICMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmip.h"
#include "dmicmp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmicmp.h
 * @brief DMOD ICMP - Public API
 *
 * dmicmp builds/parses ICMP messages (RFC 792 for ICMPv4, RFC 4443 for
 * ICMPv6) and plugs into dmip's protocol dispatch two ways at once
 * (registered in dmod_init(), see src/dmicmp.c):
 *
 *  - dmip_register_protocol(DMIP_PROTO_ICMP/_ICMPV6, ...) - to receive
 *    genuine ICMP messages. An incoming Echo Request is answered with an
 *    Echo Reply synchronously, inline in that same callback - it already
 *    runs on whatever thread is pumping the interface (see
 *    dmip_protocol_handler_t in dmip.h), so there's no need for an extra
 *    thread or queue just to answer a ping.
 *
 *  - dmip_register_default_protocol(...) - to catch any IP packet whose
 *    protocol nobody else claimed, replying with an ICMPv4 Destination
 *    Unreachable (Protocol Unreachable). See dmip.h's "Protocol
 *    registration" section and docs/dmip.md for why this fallback exists.
 *
 * There is no dmip_v6_send() yet (blocked on a missing NDP module, the
 * same gap dmudp already lives with for sending) - dmicmp still
 * registers for DMIP_PROTO_ICMPV6 and validates/parses incoming ICMPv6
 * messages correctly, but cannot reply to an ICMPv6 Echo Request or send
 * an ICMPv6 error yet; those paths log a warning and drop instead of
 * silently doing nothing. This is a deliberate, temporary limitation -
 * see docs/dmicmp.md.
 *
 * Sending our OWN Echo Request (a "ping") is a separate, caller-driven
 * feature: dmicmp_v4_send_echo_request() just sends, and
 * dmicmp_register_echo_listener()/_unregister_echo_listener() let a
 * caller learn about the matching reply, keyed by the ICMP echo
 * identifier field (the thing that lets multiple concurrent pings tell
 * their replies apart) - a real per-key registry, not a shared queue. An
 * Echo Reply for an identifier nobody registered is simply dropped, the
 * same tiered fallback shape dmip's own protocol dispatch uses (specific
 * match -> default -> drop).
 *
 * dmicmp_v4_send_dest_unreachable() is public (not just internal glue)
 * so another protocol module can report its own delivery failure through
 * dmicmp instead of building the message itself - dmudp is the
 * motivating case: once it gains a port registry, a datagram addressed
 * to a port nobody's listening on should get
 * dmicmp_v4_dest_unreachable_port back, the same way dmicmp's own
 * dmip-default-handler path uses dmicmp_v4_dest_unreachable_protocol. It
 * is itself a thin wrapper over dmicmp_v4_send_error() - the
 * general-purpose primitive for this whole family of RFC 792 "report a
 * problem with this original packet" messages (Destination Unreachable,
 * Time Exceeded, ...): give it a type and a code, and the destination,
 * the RFC 792 quote, and the checksum are all filled in automatically -
 * so a message type without its own dedicated wrapper yet can still be
 * sent without waiting for one.
 *
 * dmicmp depends on dmip (dmip_send()/_v4_send(), dmip_checksum(),
 * dmip_addr_t, protocol registration) and, transitively through it,
 * dmroute/dmnetif - dmicmp itself never talks to those directly. Also
 * depends on dmlist (the echo listener table) and dmosi (the mutex
 * guarding it). Messages are built/parsed as raw byte buffers rather
 * than packed C structs, same reasoning dmip.c/dmarp.c document for
 * themselves (dmod's minimal module runtime gives no struct-packing
 * guarantee).
 */

/* ============================================================================
 *                      ICMPv4 message types (RFC 792)
 * ========================================================================== */

#define DMICMP_V4_TYPE_ECHO_REPLY        0u
#define DMICMP_V4_TYPE_DEST_UNREACHABLE  3u
#define DMICMP_V4_TYPE_ECHO_REQUEST      8u

/**
 * @brief ICMPv4 Destination Unreachable codes (RFC 792)
 *
 * dmicmp_v4_dest_unreachable_port is the one dmudp is expected to use
 * once it gains a port registry, to report a datagram addressed to a
 * port nobody's listening on - dmicmp itself only ever sends
 * dmicmp_v4_dest_unreachable_protocol, as dmip's default handler (see
 * this file's top comment). A real enum (not a raw uint8_t + #define
 * constants) so dmicmp_v4_send_dest_unreachable() gives the compiler a
 * chance to catch a bad value, not just dmicmp's own runtime check.
 */
typedef enum
{
    dmicmp_v4_dest_unreachable_net      = 0,
    dmicmp_v4_dest_unreachable_host     = 1,
    dmicmp_v4_dest_unreachable_protocol = 2,
    dmicmp_v4_dest_unreachable_port     = 3,
} dmicmp_v4_dest_unreachable_code_t;

/* ============================================================================
 *                      ICMPv6 message types (RFC 4443)
 *
 * Recognized on receipt only - see this file's top comment for why there
 * is no ICMPv6 send path yet.
 * ========================================================================== */

#define DMICMP_V6_TYPE_DEST_UNREACHABLE  1u
#define DMICMP_V6_TYPE_ECHO_REQUEST      128u
#define DMICMP_V6_TYPE_ECHO_REPLY        129u

/* ============================================================================
 *                      Common header
 * ========================================================================== */

/**
 * @brief Length in bytes of the common ICMP header: type, code, checksum,
 *        and 4 bytes whose meaning depends on `type` (identifier +
 *        sequence for Echo Request/Reply, unused/zero for Destination
 *        Unreachable) - identical layout for ICMPv4 and ICMPv6
 */
#define DMICMP_HEADER_LEN 8u

/**
 * @brief Parsed common ICMP header fields
 *
 * dmicmp_build_header() ignores `checksum` (the caller computes and
 * writes it separately, over the header + whatever payload follows -
 * see dmicmp_v4_checksum_valid()/_v6_checksum_valid()); `identifier`/
 * `sequence` are only meaningful for Echo Request/Reply, and are written
 * as zero for any other `type`. dmicmp_parse_header() always fills every
 * field.
 */
typedef struct
{
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;    /**< Parse output only - see above */
    uint16_t identifier;  /**< Meaningful only for Echo Request/Reply */
    uint16_t sequence;    /**< Meaningful only for Echo Request/Reply */
} dmicmp_header_t;

/**
 * @brief Build an 8-byte ICMP header (no payload) into `buffer`
 *
 * `header->checksum` is ignored - compute the real checksum over the
 * whole message (header + payload) with dmip_checksum() and write it
 * into `buffer[2..3]` yourself once the payload is in place, the same
 * two-step dmip_v4_build_header() avoids needing by computing the
 * checksum internally; dmicmp can't do that here since, unlike an IP
 * header, an ICMP message's checksum covers a payload of the caller's
 * choosing that doesn't exist yet at build_header() time.
 *
 * @param buffer     Output buffer, must be at least DMICMP_HEADER_LEN bytes
 * @param buffer_len Size of `buffer` in bytes
 * @param header     Fields to encode
 *
 * @return 0 on success, -EINVAL on a NULL argument or too-small buffer
 */
dmod_dmicmp_api(1.0, int, _build_header, ( uint8_t* buffer, size_t buffer_len, const dmicmp_header_t* header ));

/**
 * @brief Parse an 8-byte ICMP header from `buffer`
 *
 * @param buffer Received bytes, header first
 * @param length Number of valid bytes in `buffer`, must be at least
 *                DMICMP_HEADER_LEN
 * @param header Output: parsed fields
 *
 * @return 0 on success, -EINVAL on a NULL argument or `length` shorter
 *         than DMICMP_HEADER_LEN
 */
dmod_dmicmp_api(1.0, int, _parse_header, ( const uint8_t* buffer, size_t length, dmicmp_header_t* header ));

/* ============================================================================
 *                      Checksum
 * ========================================================================== */

/**
 * @brief Check whether a received ICMPv4 message's checksum is valid
 *
 * Unlike UDP, an ICMPv4 checksum covers only the ICMP message itself
 * (header + data) - no pseudo-header, no IP addresses involved. Plain
 * dmip_checksum() is enough.
 *
 * @param message Received ICMP message, header first
 * @param length  Number of valid bytes in `message`, must be at least
 *                 DMICMP_HEADER_LEN
 *
 * @return true if the checksum matches the message's contents
 */
dmod_dmicmp_api(1.0, bool, _v4_checksum_valid, ( const uint8_t* message, size_t length ));

/**
 * @brief Check whether a received ICMPv6 message's checksum is valid
 *
 * Unlike ICMPv4, an ICMPv6 checksum DOES need the IPv6 pseudo-header
 * (RFC 4443 2.3, same construction RFC 8200 8.1 defines for UDP over
 * IPv6) - exposed even though dmicmp can't send ICMPv6 yet, so an
 * incoming ICMPv6 message can still be validated correctly rather than
 * trusted blindly.
 *
 * @param src_ip  Source IP address the message arrived from (family
 *                 must be dmip_family_v6)
 * @param dst_ip  Destination IP address the message arrived on (family
 *                 must be dmip_family_v6)
 * @param message Received ICMP message, header first
 * @param length  Number of valid bytes in `message`, must be at least
 *                 DMICMP_HEADER_LEN
 *
 * @return true if the checksum matches the message's contents
 */
dmod_dmicmp_api(1.0, bool, _v6_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* message, size_t length ));

/* ============================================================================
 *                      Sending
 * ========================================================================== */

/**
 * @brief Send an arbitrary ICMPv4 error message quoting `original_packet`
 *        back to its sender
 *
 * The general-purpose primitive for the whole family of RFC 792 "report
 * a problem with this original packet" messages (Destination
 * Unreachable, Time Exceeded, ...) - dmicmp_v4_send_dest_unreachable()
 * is itself a thin wrapper over this. Only `type`/`code` need to be
 * supplied: the destination (`original_packet`'s own source address),
 * the RFC 792 quote (its header plus up to the first 8 bytes of
 * payload), and the checksum are all filled in automatically - so a
 * message type without its own dedicated wrapper yet can still be sent
 * without waiting for one.
 *
 * @param type                 ICMP message type
 * @param code                 ICMP code - meaning depends on `type`
 * @param original_packet      The complete original wire-format IPv4
 *                              packet (header + payload) this message
 *                              reports a problem with. Only its header
 *                              and the first 8 bytes of payload are
 *                              actually sent (RFC 792) - pass the whole
 *                              packet, dmicmp reads only what it needs
 * @param original_packet_len  Length of `original_packet` in bytes
 * @param arp_timeout_ms       Forwarded to dmip_send()
 *
 * @return 0 on success, -EINVAL (NULL/too-short original_packet, or not
 *         a well-formed IPv4 header), otherwise whatever dmip_send()
 *         itself returns
 */
dmod_dmicmp_api(1.0, int, _v4_send_error, ( uint8_t type, uint8_t code, const uint8_t* original_packet, size_t original_packet_len, uint32_t arp_timeout_ms ));

/**
 * @brief Send an ICMPv4 Destination Unreachable back to the sender of
 *        `original_packet`
 *
 * A thin wrapper over dmicmp_v4_send_error() with `type` fixed to
 * DMICMP_V4_TYPE_DEST_UNREACHABLE - kept as its own function (rather
 * than making every caller spell that out) both for the common case and
 * so `code` gets the compiler-checked
 * dmicmp_v4_dest_unreachable_code_t enum instead of a raw uint8_t.
 * Public so another protocol module can report its own delivery failure
 * through dmicmp instead of building the message itself - see this
 * file's top comment for the motivating dmudp case. Also used
 * internally: dmicmp's own dmip default-handler registration (see
 * src/dmicmp.c) calls this with dmicmp_v4_dest_unreachable_protocol for
 * any IP packet whose protocol nobody claimed.
 *
 * @param code                 Which "unreachable" this is
 * @param original_packet      See dmicmp_v4_send_error()
 * @param original_packet_len  Length of `original_packet` in bytes
 * @param arp_timeout_ms       Forwarded to dmip_send()
 *
 * @return See dmicmp_v4_send_error()
 */
dmod_dmicmp_api(1.0, int, _v4_send_dest_unreachable, ( dmicmp_v4_dest_unreachable_code_t code, const uint8_t* original_packet, size_t original_packet_len, uint32_t arp_timeout_ms ));

/**
 * @brief Callback registered via dmicmp_register_echo_listener() to
 *        receive one matching Echo Reply
 *
 * Called from whatever thread is pumping the interface the reply
 * arrived on (same delivery context as dmip_protocol_handler_t - see
 * dmip.h) - `payload` is only valid for the duration of the call, copy
 * it out if you need it afterward. Fires at most once per
 * dmicmp_register_echo_listener() call - register again (with a fresh
 * sequence, if you're sending more than one Echo Request under the same
 * identifier) for another.
 *
 * @param src        Address the reply came from
 * @param identifier The identifier this listener was registered with
 * @param sequence   Whatever sequence dmicmp_v4_send_echo_request() was
 *                    called with
 * @param payload    Data echoed back unchanged from the original request
 * @param payload_len Length of `payload` in bytes
 */
typedef void (*dmicmp_echo_reply_handler_t)( const dmip_addr_t* src, uint16_t identifier, uint16_t sequence, const uint8_t* payload, size_t payload_len );

/**
 * @brief Register `handler` to be called the next time an Echo Reply
 *        carrying `identifier` arrives (either family)
 *
 * One registration per identifier, like dmip_register_protocol() is one
 * registration per protocol number - register a fresh one per
 * outstanding ping, not a long-lived subscription. An Echo Reply for an
 * identifier with no registered listener is silently dropped, the same
 * tiered fallback shape dmip's own protocol dispatch uses (specific
 * match -> default -> drop) - here there is no "default" tier, just
 * match-or-drop.
 *
 * @param identifier ICMP echo identifier to match
 * @param handler    Callback to invoke for the first Echo Reply carrying
 *                    `identifier`
 *
 * @return 0 on success, -EINVAL if `handler` is NULL, -EEXIST if
 *         `identifier` already has a registered listener (call
 *         dmicmp_unregister_echo_listener() first if you mean to
 *         replace it), -ENOMEM if the registration itself could not be
 *         allocated
 */
dmod_dmicmp_api(1.0, int, _register_echo_listener, ( uint16_t identifier, dmicmp_echo_reply_handler_t handler ));

/**
 * @brief Undo dmicmp_register_echo_listener() - safe to call for an
 *        identifier with no registered listener (a no-op)
 *
 * @param identifier ICMP echo identifier to release
 */
dmod_dmicmp_api(1.0, void, _unregister_echo_listener, ( uint16_t identifier ));

/**
 * @brief Build, checksum and send an ICMPv4 Echo Request
 *
 * Register a listener for `identifier` first
 * (dmicmp_register_echo_listener()) if you want to know when/whether a
 * reply comes back - this call only sends, it never waits.
 *
 * @param dst            Destination (family must be dmip_family_v4)
 * @param identifier     Caller-chosen - whatever a matching
 *                        dmicmp_register_echo_listener() was registered
 *                        with
 * @param sequence       Caller-chosen, echoed back unchanged in the reply
 * @param payload        Arbitrary data echoed back unchanged in the reply
 * @param payload_len    Length of `payload` in bytes
 * @param arp_timeout_ms Forwarded to dmip_send()
 *
 * @return 0 on success, -EINVAL (bad argument/family), otherwise
 *         whatever dmip_send() returns. No dmicmp_v6_send_echo_request()
 *         yet - same gap as dmip_v6_send()/dmudp_send()'s IPv6 case (no
 *         NDP module to resolve a next-hop MAC through yet)
 */
dmod_dmicmp_api(1.0, int, _v4_send_echo_request, ( const dmip_addr_t* dst, uint16_t identifier, uint16_t sequence, const void* payload, size_t payload_len, uint32_t arp_timeout_ms ));

#ifdef __cplusplus
}
#endif

#endif // DMICMP_H
