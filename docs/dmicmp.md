# DMICMP - DMOD ICMP

## Overview

DMICMP builds and parses ICMP messages (RFC 792 for ICMPv4, RFC 4443 for
ICMPv6) and plugs into [dmip](../../dmip)'s protocol dispatch two ways at
once:

- `dmip_register_protocol(DMIP_PROTO_ICMP/_ICMPV6, ...)` - to receive
  genuine ICMP messages. An incoming Echo Request is answered with an
  Echo Reply synchronously, inline in that same callback.
- `dmip_register_default_protocol(...)` - to catch any IP packet whose
  protocol nobody else claimed, replying with an ICMPv4 Destination
  Unreachable. See
  [dmip.md](../../dmip/docs/dmip.md#protocol-dispatch) for why this
  fallback exists.

```
┌──────────────────────────────────────────────┐
│                 DMICMP                        │
│   build/parse/checksum an ICMP message,       │
│   auto Echo Reply, dest-unreachable builder,  │
│   echo-reply listener registry                │
├──────────────────────────────────────────────┤
│                  DMIP                         │
│   dmip_send(), dmip_checksum(),               │
│   protocol registration                       │
├──────────────────────────────────────────────┤
│      DMNETBRIDGE / DMROUTE / DMNETIF / DMARP  │
└──────────────────────────────────────────────┘
```

## No extra thread needed to answer a ping

`dmip_register_protocol()`'s callback already runs on whatever thread is
pumping the interface a packet arrived on (see
`dmip_protocol_handler_t` in `dmip.h`) - the same thread
`dmnetbridge_handle_netif_rx()` uses. Answering an Echo Request is
therefore just "build an Echo Reply and call `dmip_send()`, right here,
right now" - no receive queue, no worker thread, no listener registry
needed for this direction. That's also true of the default-handler path:
an unclaimed protocol's Destination Unreachable is sent inline from the
same callback.

## Sending our own Echo Request needs a different shape

The one direction that *does* need a registry is the reverse: when
dmicmp itself sends an Echo Request (a "ping") and something needs to
know whether/when a reply comes back. `dmicmp_register_echo_listener()`/
`_unregister_echo_listener()` keep a small table (one entry per ICMP echo
*identifier* - the field that plays the same role a UDP port would, in
letting multiple concurrent pings tell their replies apart), guarded by
its own mutex. `dmicmp_v4_send_echo_request()` only ever sends; it never
waits. A reply for an identifier nobody registered is silently dropped -
the same tiered fallback shape `dmip`'s own protocol dispatch already
uses (specific match -> default -> drop), just with no "default" tier
here.

## A general-purpose "report a problem" primitive, plus convenience wrappers

`dmicmp_v4_send_error()` is the general-purpose primitive for the whole
family of RFC 792 "report a problem with this original packet" messages
(Destination Unreachable, Time Exceeded, ...): give it a `type` and a
`code`, and everything else - the reply destination (read from the
original packet's own source address), the RFC 792 quote (its header
plus up to 8 bytes of payload), and the checksum - is filled in
automatically. `dmicmp_v4_send_dest_unreachable()` is a thin wrapper over
it with `type` fixed to Destination Unreachable and `code` upgraded to
the type-safe `dmicmp_v4_dest_unreachable_code_t` enum. A message type
that doesn't have its own dedicated wrapper yet (Time Exceeded,
Redirect, ...) can still be sent through `dmicmp_v4_send_error()`
directly, without waiting on a new dmicmp release.

Echo Request/Reply don't fit this "quote a problem packet" shape at all
(there's no original packet to report a problem with) - those go through
a separate internal builder instead, reached publicly via
`dmicmp_v4_send_echo_request()`.

## `dmicmp_v4_send_dest_unreachable()` is a building block, not just internal glue

It's public specifically so another protocol module can report its own
delivery failure through dmicmp instead of building the ICMP message
itself. The motivating case is `dmudp`: once it gains a per-port
registry, a datagram addressed to a port nobody's listening on should
get `dmicmp_v4_dest_unreachable_port` back - the same function dmicmp's
own dmip-default-handler path already calls with
`dmicmp_v4_dest_unreachable_protocol` for a protocol nobody claimed. The
reply destination is read out of the *original* packet's own IP header
(its source is who gets the reply), not passed as a separate parameter -
the caller just hands over the packet it couldn't deliver.

## ICMPv4 checksum vs ICMPv6 checksum

Unlike UDP, an ICMPv4 checksum covers only the ICMP message itself
(header + data) - no pseudo-header, no IP addresses involved
(`dmicmp_v4_checksum_valid()` is a direct `dmip_checksum()` call). ICMPv6
is the opposite: RFC 4443 2.3 requires the same 40-byte IPv6
pseudo-header construction RFC 8200 8.1 defines for UDP over IPv6
(`dmicmp_v6_checksum_valid()` mirrors `dmudp_v6_checksum_valid()`'s own
shape almost exactly, just with `DMIP_PROTO_ICMPV6` in place of
`DMIP_PROTO_UDP`).

## The IPv6 send gap - deliberate and temporary

There is no `dmip_v6_send()` yet (blocked on a missing NDP module, RFC
4861 - the same boundary `dmarp.h`/`dmudp.h` document for their own IPv6
gaps). dmicmp still registers for `DMIP_PROTO_ICMPV6` and validates/
parses incoming ICMPv6 messages correctly - there's real value in
rejecting a malformed or spoofed message even if nothing can be sent back
yet - but an incoming ICMPv6 Echo Request, or an unclaimed IPv6 protocol
hitting the default handler, can't be answered. Both paths log a warning
(`DMOD_LOG_WARN`) and drop, rather than silently doing nothing. Once
`dmip_v6_send()` exists, the ICMPv6 Echo Request path becomes a genuine
Echo Reply the same way the v4 path already works; the unclaimed-protocol
path should become an ICMPv6 Parameter Problem / Unrecognized Next Header
(RFC 4443 3.4) - **not** a Destination Unreachable, which is why that
gap is a `DMOD_LOG_WARN` today rather than a stub that sends the wrong
message type.

## Receiving an Echo Reply needs no `dmip_v6_send()` at all

Delivering an already-arrived Echo Reply to a registered listener is a
direct in-process function call - nothing about it requires sending
anything. So even though dmicmp can't *send* an ICMPv6 Echo Request yet,
a registered listener still receives a matching ICMPv6 Echo Reply if one
arrives (from whatever sent the original request some other way).

## Dependencies

dmicmp depends on [dmip](../../dmip) (`dmip_send()`/`_v4_send()`,
`dmip_checksum()`, `dmip_addr_t`, protocol registration) and,
transitively through it, dmroute (`dmip_addr_t`'s real definition) and
dmnetif (`dmnetif_iface_t`) - dmicmp itself never calls a dmroute/dmnetif
function directly, same as dmudp. Also depends on dmlist (the echo
listener table) and dmosi (the mutex guarding it).
