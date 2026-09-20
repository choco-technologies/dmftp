# dmftp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml)

libftp DMOD library module.

## Description

A transport-agnostic FTP (RFC 959) control-line engine: it splits a raw
byte stream into command lines and formats numeric reply lines back out,
the same shape [dmtelnet](https://github.com/choco-technologies/dmtelnet)
uses for Telnet framing. libftp never touches a socket or a filesystem
itself - it is fed raw bytes via `libftp_recv()` and produces raw bytes via
a callback.

See [tools/ftpd](tools/ftpd) for `ftpd`, the actual FTP server built on
this engine (dmtcp connections, authentication, a virtual filesystem root,
PASV/PORT data transfer) - and for why it's a separate Application module
rather than something built into this library.

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

Tests are built automatically alongside the module (see `tests/`) and cover
this engine (command-line parsing, reply formatting) - `tools/ftpd` is
dmtcp/filesystem-backed integration glue, exercised on real hardware
instead, the same split `dmtelnet`/`telnetd` use. Once built, run the
engine tests with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_libftp-local.dmd -y
dmod_loader build/dmf/test_libftp.dmf
```

## Usage

This library module provides functions that can be used by other modules:

```c
#include "libftp.h"
```

See [docs/api-reference.md](docs/api-reference.md) for the full reference.

## API

| Function | Description |
|----------|-------------|
| `libftp_create(callbacks, user_data)` | Create a session. `callbacks->on_send` is required. |
| `libftp_destroy(session)` | Destroy a session created by `_create()`. |
| `libftp_recv(session, data, data_len)` | Feed newly-received raw bytes into the session. |
| `libftp_reply(session, code, text)` | Format and send a numeric FTP reply line. |

See [include/libftp.h](include/libftp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man libftp`.

## Project Structure

```
dmftp/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── libftp.h
├── src/
│   └── libftp.c
├── tests/
│   ├── CMakeLists.txt
│   └── libftp_test.c
├── tools/
│   └── ftpd/          # FTP server (Application, dmtcp + filesystem) - see its own README/docs
├── CMakeLists.txt
├── Makefile
├── libftp.dmr
└── manifest.dmm
```

## Author

Mikolaj Filar

## License

MIT
