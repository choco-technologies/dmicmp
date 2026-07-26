# dmicmp API Reference

See [dmicmp.md](dmicmp.md) for the design rationale behind this API.

## Types

| Type | Description |
|------|-------------|
| `dmicmp_header_t` | Parsed common ICMP header fields: `type`, `code`, `checksum` (parse output only), `identifier`/`sequence` (Echo Request/Reply only) |
| `dmicmp_v4_dest_unreachable_code_t` | Enum of ICMPv4 Destination Unreachable codes: `dmicmp_v4_dest_unreachable_net`/`_host`/`_protocol`/`_port` |
| `dmicmp_echo_reply_handler_t` | Callback registered via `dmicmp_register_echo_listener()` |

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `DMICMP_HEADER_LEN` | 8 | Length of the common ICMP header (identical layout for v4 and v6) |
| `DMICMP_V4_TYPE_ECHO_REPLY` | 0 | ICMPv4 Echo Reply |
| `DMICMP_V4_TYPE_DEST_UNREACHABLE` | 3 | ICMPv4 Destination Unreachable |
| `DMICMP_V4_TYPE_ECHO_REQUEST` | 8 | ICMPv4 Echo Request |
| `DMICMP_V6_TYPE_DEST_UNREACHABLE` | 1 | ICMPv6 Destination Unreachable (receive-only) |
| `DMICMP_V6_TYPE_ECHO_REQUEST` | 128 | ICMPv6 Echo Request (receive-only) |
| `DMICMP_V6_TYPE_ECHO_REPLY` | 129 | ICMPv6 Echo Reply (receive-only) |

## Functions

### Header build/parse

| Function | Description |
|----------|-------------|
| `dmicmp_build_header()` | Build the 8-byte common ICMP header into a buffer (checksum field left 0) |
| `dmicmp_parse_header()` | Parse the 8-byte common ICMP header from a buffer |

### Checksum

| Function | Description |
|----------|-------------|
| `dmicmp_v4_checksum_valid()` | Verify an ICMPv4 message's checksum (no pseudo-header) |
| `dmicmp_v6_checksum_valid()` | Verify an ICMPv6 message's checksum (IPv6 pseudo-header required, RFC 4443 2.3) |

### Sending

| Function | Description |
|----------|-------------|
| `dmicmp_v4_send_error()` | General-purpose: send an arbitrary ICMPv4 error message (any type/code) quoting a given original packet - destination, RFC 792 quote, and checksum all filled in automatically |
| `dmicmp_v4_send_dest_unreachable()` | Thin wrapper over `dmicmp_v4_send_error()` fixed to Destination Unreachable, with a type-safe code enum - usable by other protocol modules (e.g. a future dmudp Port Unreachable), not just internally |
| `dmicmp_v4_send_echo_request()` | Send an ICMPv4 Echo Request (a "ping") - register a listener first if you want to know about the reply |

### Echo reply listener registry

| Function | Description |
|----------|-------------|
| `dmicmp_register_echo_listener()` | Register a callback for the next Echo Reply carrying a given identifier (either family) |
| `dmicmp_unregister_echo_listener()` | Undo `dmicmp_register_echo_listener()` - safe no-op if unregistered |

## What's not here yet

- No `dmicmp_v6_send_*()` - blocked on `dmip_v6_send()` not existing yet
  (no NDP module). Incoming ICMPv6 is still parsed and checksum-validated
  correctly; an Echo Request or unclaimed protocol that would need a v6
  reply is logged and dropped instead.
- No dedicated wrapper for Time Exceeded, Redirect, or other RFC 792
  message types - reachable today via the general-purpose
  `dmicmp_v4_send_error()` primitive, without a dedicated function.
