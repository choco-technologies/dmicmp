# ping

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](../../LICENSE)

`ping` DMOD application module - a CLI to send ICMPv4 Echo Requests to a
host and report the matching Echo Replies, built on top of
[dmicmp](../../)'s own Echo Request/Reply API. It builds/parses no ICMP
bytes itself, only calls `dmicmp_v4_send_echo_request()` and
`dmicmp_register_echo_listener()`/`_unregister_echo_listener()`.

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

`<address>` is a dotted-decimal IPv4 address or a host name (for example,
`ping one.one.one.one -c 4`). Names are resolved once through `dmdns`,
using its hosts table, cache and configured DNS servers (including DHCP).
The first IPv4 address is used and printed in the banner and reply lines.
IPv6 destinations are not supported. DNS uses the resolver's default
per-server timeout; `-W` controls only the wait for an ICMP reply.

Example output:

```
$ ping 192.168.1.1
PING 192.168.1.1 (192.168.1.1): 32 data bytes
32 bytes from 192.168.1.1: icmp_seq=0 time=3 ms
32 bytes from 192.168.1.1: icmp_seq=1 time=2 ms
32 bytes from 192.168.1.1: icmp_seq=2 time=2 ms
32 bytes from 192.168.1.1: icmp_seq=3 time=3 ms

--- 192.168.1.1 ping statistics ---
4 packets transmitted, 4 packets received, 0% packet loss
```

## Building

This module lives under `tools/ping` inside the `dmicmp` repository and is
built as part of the parent's CMake configure (the top-level
`CMakeLists.txt` calls `add_subdirectory(tools)`, whose own
`CMakeLists.txt` calls `add_subdirectory(ping)`) - it is not built
standalone.

```bash
mkdir -p build
cd build
cmake ..
cmake --build . --target ping
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

It is also released as its own separate module-type application: any
tagged release of this repository packages `ping` alongside `dmicmp`
itself (see `build/packages/ping/` after a build with `DMOD_DMR_PATH` set,
and the repository's release workflow).

## Testing

Run the deterministic DNS/ICMP regression tests on the host without module
downloads or a network connection:

```bash
cmake -S tools/ping/tests/native -B build/native-tests
cmake --build build/native-tests
ctest --test-dir build/native-tests --output-on-failure
```

These compile the real CLI with substitutes for DNS and ICMP, checking
first-IPv4 selection, a single lookup per run, literal input, resolution
errors, cleanup, and argument validation before resolution. Real network
resolution and replies should also be checked on a networked target with
`ping one.one.one.one -c 3`, `ping 1.1.1.1 -c 2`, and
`ping codex-ping-test.invalid -c 1`.

`tests/ping_test.c` drives this module through `Dmod_RunModule("ping",
argc, argv)` (loads it, runs `main()`, unloads it again - see
`Dmod_RunModule` in `dmod.h`) rather than depending on `ping` as a linked
module: an Application-type module can't be loaded as another module's
dependency (only a Library-type module can), and `Dmod_RunModule`
sidesteps that entirely by loading/running/unloading it on demand instead
of holding it resident for the test's lifetime.

## Documentation

See the `docs/` directory:

- **[ping.md](docs/ping.md)** - Usage, exit codes, and how this module is
  tested

View documentation using `dmf-man ping`.

## Project Structure

```
ping/
├── docs/
│   ├── README.md
│   └── ping.md
├── src/
│   └── ping.c
├── tests/
│   ├── CMakeLists.txt
│   └── ping_test.c
├── CMakeLists.txt
└── ping.dmr
```

LICENSE is shared with the rest of the `dmicmp` repository (`../../LICENSE`).

## Author

Patryk Kubiak

## License

MIT
