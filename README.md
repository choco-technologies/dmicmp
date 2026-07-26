# dmicmp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmicmp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmicmp/actions/workflows/ci.yml)

dmicmp DMOD library module.

## Description

dmicmp builds and parses ICMP messages (RFC 792 for ICMPv4, RFC 4443 for
ICMPv6) and plugs into [dmip](https://github.com/choco-technologies/dmip)'s
protocol dispatch: it registers as the handler for ICMP's own IP protocol
number (answering an incoming Echo Request with an Echo Reply inline, no
extra thread needed) and as dmip's *default* handler, replying with an
ICMPv4 Destination Unreachable to any IP packet whose protocol nobody
else claimed. See [docs/dmicmp.md](docs/dmicmp.md) for the full design
rationale, including the deliberate, temporary IPv6-send gap (no
`dmip_v6_send()` yet) and the echo-reply listener registry used for
sending our own Echo Request ("ping").

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmicmp-local.dmd -y
dmod_loader build/dmf/test_dmicmp.dmf
```

## Usage

dmicmp needs no setup from other modules to do its core job (answering
pings, reporting an unclaimed protocol) - it registers itself with dmip
automatically in `dmod_init()`. A module that wants to send its own ping
or report its own delivery failure calls it directly:

```c
#include "dmicmp.h"

/* Report "port unreachable" for a UDP datagram nobody's listening for */
dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_port,
                                 original_ip_packet, original_ip_packet_len,
                                 DMARP_DEFAULT_TIMEOUT_MS);

/* Send a ping and find out about the reply */
dmicmp_register_echo_listener(my_identifier, my_echo_reply_handler);
dmicmp_v4_send_echo_request(&dst, my_identifier, 1, NULL, 0, DMARP_DEFAULT_TIMEOUT_MS);
```

## API

| Function | Description |
|----------|-------------|
| `dmicmp_build_header()` / `_parse_header()` | Build/parse the 8-byte common ICMP header |
| `dmicmp_v4_checksum_valid()` / `_v6_checksum_valid()` | Verify a received ICMP message's checksum |
| `dmicmp_v4_send_error()` | General-purpose: send an arbitrary ICMPv4 error message (any type/code) about some undeliverable original packet |
| `dmicmp_v4_send_dest_unreachable()` | Thin wrapper over `dmicmp_v4_send_error()` for the common Destination Unreachable case |
| `dmicmp_v4_send_echo_request()` | Send an ICMPv4 Echo Request ("ping") |
| `dmicmp_register_echo_listener()` / `_unregister_echo_listener()` | Learn about the Echo Reply matching a ping's identifier |

See [include/dmicmp.h](include/dmicmp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[dmicmp.md](docs/dmicmp.md)** - Design overview and rationale
- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmicmp`.
## Project Structure

```
dmicmp/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmicmp.h
├── src/
│   └── dmicmp.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmicmp_test.c
├── CMakeLists.txt
├── Makefile
├── dmicmp.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
