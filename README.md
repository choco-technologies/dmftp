# dmftp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml)

An FTP (RFC 959) server for DMOD boards.

## Description

dmftp is two things layered on top of each other, in one module:

- A transport-agnostic FTP **control-line engine** (`include/dmftp.h`,
  `src/dmftp.c`) - splits a raw byte stream into command lines and formats
  reply lines back out, never touching a socket or a filesystem itself.
  Shaped the same way [dmtelnet](https://github.com/choco-technologies/dmtelnet)
  frames Telnet.
- The actual **FTP server** built on top of it (`src/dmftp_server.c`,
  `src/dmftp_commands.c`): a [dmtcp](https://github.com/choco-technologies/dmtcp)
  control-connection listener, RFC 959 command handling, a configurable
  virtual filesystem root, and PASV-mode data transfer against the board's
  own filesystem via the Dmod SAL (`Dmod_FileOpen`/`Dmod_OpenDir`/...).

dmftp is a **Library**-type DMOD module with no `main()` - like
[dmicmp](https://github.com/choco-technologies/dmicmp), it does its whole
job from `dmod_init()`/`dmod_deinit()` once loaded+enabled, which lets it
be started automatically at boot as a
[dmsystem](https://github.com/choco-technologies/dmsystem) service - see
[docs/service.md](docs/service.md).

### Supported commands

`USER`, `PASS`, `QUIT`, `NOOP`, `SYST`, `PWD`/`XPWD`, `CWD`, `CDUP`, `TYPE`,
`PASV`, `LIST`, `NLST`, `RETR`, `STOR`, `DELE`, `MKD`/`XMKD`, `RMD`/`XRMD`,
`SIZE`, `ABOR`.

### Known limitations (deliberately out of scope for this first version)

- **PASV only** - active mode (`PORT`) replies `502 Command not implemented`.
  Every mainstream FTP client defaults to passive mode already.
- **No ASCII translation** - `TYPE A` is accepted but every transfer is
  effectively binary (no CRLF translation). Harmless for the overwhelming
  majority of clients, which default to `TYPE I` anyway.
- **No resume/append/rename** - `REST`, `APPE`, and `RNFR`/`RNTO` are not
  implemented.
- **No per-directory-entry timestamps** - the Dmod SAL has no mtime
  accessor, so every `LIST` line reports the same fixed placeholder date.
  Real clients tolerate this (they parse the fixed-width fields, not the
  date's value).

See `src/dmftp_server.c`'s top comment for the full rationale.

## Configuration

dmftp reads its own settings from the same unit file that starts it as a
service - see [configs/ftpd.ini](configs/ftpd.ini) and
[docs/service.md](docs/service.md) for why one file serves both readers:

| Key    | Default     | Meaning |
|--------|-------------|---------|
| `port` | `21`        | TCP port the control connection listens on |
| `root` | `/`         | Virtual filesystem root every FTP path is resolved (and jailed) under |
| `user` | `anonymous` | Required username - `anonymous` (case-insensitive) accepts any USER/PASS |
| `pass` | (empty)     | Required password when `user` isn't `anonymous` - empty accepts any password |

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
the transport-agnostic engine in `src/dmftp.c` (command-line parsing, reply
formatting) - the dmtcp/filesystem-backed server in `src/dmftp_server.c`/
`src/dmftp_commands.c` is integration glue exercised on real hardware
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
dmf-get install -d ${DMOD_DMF_DIR}/test_dmftp-local.dmd -y
dmod_loader build/dmf/test_dmftp.dmf
```

## Usage

Enable it as a service (see [docs/service.md](docs/service.md) for the full
walkthrough), then connect with any FTP client:

```bash
ftp <board-ip>
lftp <board-ip>
```

Other modules that need the control-line engine directly can use it too:

```c
#include "dmftp.h"
```

## API

| Function | Description |
|----------|-------------|
| `dmftp_create()` | Create a new control-connection session. |
| `dmftp_destroy()` | Destroy a session created by `_create()`. |
| `dmftp_recv()` | Feed raw bytes in, get `on_command` callbacks out. |
| `dmftp_reply()` | Format and send a numeric FTP reply line. |

See [include/dmftp.h](include/dmftp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation
- **[service.md](docs/service.md)** - Running dmftp as a dmsystem service

View documentation using `dmf-man dmftp`.

## Project Structure

```
dmftp/
├── configs/
│   └── ftpd.ini            # dmsystem unit + dmftp's own port/root/user/pass settings
├── docs/                    # Documentation (markdown format)
│   └── service.md
├── include/                 # Public headers
│   └── dmftp.h
├── src/
│   ├── dmftp.c              # Transport-agnostic RFC 959 control-line engine
│   ├── dmftp_internal.h     # Shared server-side state (not installed)
│   ├── dmftp_server.c       # dmtcp wiring, config loading, connection table
│   └── dmftp_commands.c     # Command handling, path resolution, PASV transfer
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
