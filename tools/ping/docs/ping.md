# ping

## Overview

`ping` is a CLI over [dmicmp](../../..)'s Echo Request/Reply API - it sends
one or more ICMPv4 Echo Requests to a literal IPv4 address and reports
each matching Echo Reply, or that the request was lost. It never builds
or parses an ICMP message itself; everything goes through
`dmicmp_v4_send_echo_request()` to send and
`dmicmp_register_echo_listener()`/`dmicmp_unregister_echo_listener()` to
learn about the reply (see `include/dmicmp.h`).

`<address>` must be a literal dotted-decimal IPv4 address (e.g.
`192.168.1.1`) - dmicmp has no hostname resolution, and no ICMPv6 send
path yet (see `dmicmp.h`'s top comment), so `ping` cannot resolve a name
or reach an IPv6 destination either.

## Usage

```
ping <address>       Send 4 ICMPv4 Echo Requests to <address>
ping --help | -h     Show this help

Options:
  -c <count>         Number of Echo Requests to send (default: 4)
  -i <interval_ms>   Delay between requests, in milliseconds (default: 1000)
  -W <timeout_ms>    Time to wait for a reply before it's lost, in milliseconds (default: 1000)
  -s <size>          Number of payload bytes to send, 0-1024 (default: 32)
```

Example output:

```
$ ping 192.168.1.1
PING 192.168.1.1: 32 data bytes
32 bytes from 192.168.1.1: icmp_seq=0 time=3 ms
32 bytes from 192.168.1.1: icmp_seq=1 time=2 ms
32 bytes from 192.168.1.1: icmp_seq=2 time=2 ms
32 bytes from 192.168.1.1: icmp_seq=3 time=3 ms

--- 192.168.1.1 ping statistics ---
4 packets transmitted, 4 packets received, 0% packet loss
```

`time=` is measured locally, from just before
`dmicmp_v4_send_echo_request()` is called to the moment the matching
reply's listener callback posts the semaphore `ping` is waiting on
(`dmosi_get_tick_count()` before/after) - it includes ARP resolution time
for the very first request to a given host, the same way a real ping's
first RTT sample often does.

## How a request/reply round works

Each Echo Request reuses the same ICMP identifier for the whole `ping`
run (derived from the tick count at startup, so two concurrent `ping`
invocations are unlikely to collide) but a fresh sequence number per
request. Before sending, `ping`:

1. Calls `dmicmp_register_echo_listener(identifier, echo_reply_handler)`.
2. Calls `dmicmp_v4_send_echo_request(...)`.
3. Blocks on a semaphore (created with `dmosi_semaphore_create(0, 1)`) for
   up to `-W`'s timeout, via `dmosi_semaphore_wait()`.
4. Calls `dmicmp_unregister_echo_listener(identifier)` unconditionally,
   whether or not a reply arrived in time.

`echo_reply_handler()` only posts the semaphore if the reply's sequence
number matches the request currently outstanding - a reply that arrives
late (after its own request already timed out and got unregistered) is
simply never delivered at all, since `dmicmp` drops a reply for an
identifier with no registered listener; the sequence check is an extra
guard against the much narrower race where a *new* registration for the
next request is already in place by the time a very late reply for the
*previous* one arrives.

## Exit codes

- `0` - at least one Echo Request got a matching reply, or `--help`/`-h`
  was given
- `1` - bad arguments (missing/invalid address, unknown flag, an
  out-of-range `-c`/`-W`/`-s` value), or every Echo Request was lost
  (no reply within `-W`'s timeout, or `dmicmp_v4_send_echo_request()`
  itself failed - e.g. no route to the destination)

## Testing

`tests/ping_test.c` drives this module through `Dmod_RunModule("ping",
argc, argv)` (loads it, runs `main()`, unloads it again - see
`Dmod_RunModule` in `dmod.h`) rather than depending on `ping` as a linked
module: an Application-type module can't be loaded as another module's
dependency (only a Library-type module can), and `Dmod_RunModule`
sidesteps that entirely by loading/running/unloading it on demand instead
of holding it resident for the test's lifetime.

A "/dev/null"-backed `dmnetif` fixture, with a `dmroute` route and a
`dmarp` cache entry pointed at it, lets a send be exercised up through
`dmip_send()` itself - same pattern `dmicmp`'s own
`tests/dmicmp_test.c` uses for `dmicmp_v4_send_error()`/
`_send_dest_unreachable()`. No real driver backs the fixture, so
`dmicmp_v4_send_echo_request()` still fails there (`dmnetif_send()` ->
`-EIO`) and no real Echo Reply can ever arrive - every test step expects
`ping` to report loss, not success; they only prove argument handling and
a full send attempt complete without crashing or hanging, and report the
honest outcome.

## Dependencies

- `dmicmp` - builds/sends the Echo Request and delivers the matching Echo
  Reply (see the top-level [dmicmp](../../..) module)
- `dmroute` - the address type (`dmroute_addr_t`, re-exported by `dmicmp`
  as `dmip_addr_t`) `ping` parses `<address>` into
- `dmarp` - `DMARP_DEFAULT_TIMEOUT_MS`, the ARP-resolution timeout
  forwarded to `dmicmp_v4_send_echo_request()`
- `dmosi` - the semaphore `ping` waits for a reply on, the tick count used
  to measure round-trip time, and the sleep between requests
