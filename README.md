# dmftp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml)

dmftp DMOD library module.

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
dmf-get install -d ${DMOD_DMF_DIR}/test_dmftp-local.dmd -y
dmod_loader build/dmf/test_dmftp.dmf
```

## Usage

<TBD>

This library module provides functions that can be used by other modules:

```c
#include "dmftp.h"
```

## API

| Function | Description |
|----------|-------------|
| `dmftp_create()` | Create a new `dmftp_t` instance. |
| `dmftp_destroy()` | Destroy an instance created by `_create()`. |
| `dmftp_is_valid()` | Check whether a handle is a valid instance. |

See [include/dmftp.h](include/dmftp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmftp`.
## Project Structure

```
dmftp/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmftp.h
├── src/
│   └── dmftp.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmftp_test.c
├── CMakeLists.txt
├── Makefile
├── dmftp.dmr
└── manifest.dmm
```

## Author

Mikolaj Filar

## License

MIT
