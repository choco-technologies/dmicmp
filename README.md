# dmicmp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmicmp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmicmp/actions/workflows/ci.yml)

dmicmp DMOD library module.

## Description

TODO: describe what this module does.

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

<TBD>

This library module provides functions that can be used by other modules:

```c
#include "dmicmp.h"
```

## API

| Function | Description |
|----------|-------------|
| `dmicmp_create()` | Create a new `dmicmp_t` instance. |
| `dmicmp_destroy()` | Destroy an instance created by `_create()`. |
| `dmicmp_is_valid()` | Check whether a handle is a valid instance. |

See [include/dmicmp.h](include/dmicmp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

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
