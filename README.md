# dmftp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmftp/actions/workflows/ci.yml)

FTP for DMOD - an RFC 959 server and client over
[dmtcp](https://github.com/choco-technologies/dmtcp).

## Description

This repository builds **two modules**:

| Module | Type | What it is |
|---|---|---|
| `dmftp` | Library | The protocol itself: an FTP server and an FTP client, sharing one control-line codec, one jail-safe path resolver and one transfer engine |
| `ftpd` | Application | A service front-end - turns a unit file's `args=` and an INI file into a running server |

The split follows the usual rule: only a Library module can be enabled as a
dependency of another module, so anything embedding an FTP server or client
of its own links `dmftp` directly rather than going through `ftpd`.

Both PASV and PORT data connections work in both roles. Files are reached
through the DMOD SAL (`Dmod_FileOpen()`, `Dmod_OpenDir()`, ...) - the same
calls the `dmell` commands use - so whatever `dmvfs` has mounted is served
with no dependency on `dmvfs` itself.

Nothing here owns a thread. Control lines arrive inline on `dmtcp`'s
delivery thread, and outbound file data is pushed from `dmtcp`'s
`on_writable` callback as the peer's ACKs free buffer space - so a multi-
megabyte `RETR` costs no thread, no timer and no polling loop. See
[app/dmftp/docs/dmftp.md](app/dmftp/docs/dmftp.md) for the full architecture
and the complete list of known limitations.

> **Requires dmtcp with `on_writable`.** The transfer pump is built on
> `dmtcp_writable_handler_t` / `dmtcp_send_space()`. Building against an
> older dmtcp fails at compile time on `dmtcp_conn_callbacks_t` having no
> `on_writable` member.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Both modules are built; the artifacts land in `build/dmf/` as `dmftp.dmf`
and `ftpd.dmf`.

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

Each module has its own Makefile:

```bash
make -C app/dmftp DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
make -C app/ftpd  DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built alongside the modules (see each module's `tests/`). Once
built, run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs each test module's dependencies with `dmf-get` and then
runs it through `dmod_loader`. To run one manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmftp-local.dmd -y
dmod_loader build/dmf/test_dmftp.dmf
```

`test_dmftp` covers the control-line codec directly and drives a real client
session end to end - a hand-built IPv4/TCP frame through `dmnetbridge` →
`dmip` → `dmtcp` → `dmftp`'s command dispatcher. `test_ftpd` covers the
configuration contract and the server object the service builds.

## Usage

```c
#include "dmftp.h"

static bool on_auth(dmftp_session_t session, const char* user, const char* password, void* user_data)
{
    if (strcmp(user, "admin") != 0 || strcmp(password, "s3cret") != 0)
        return false;

    dmftp_session_set_root(session, "/srv/ftp/admin");  /* applied before the 230 */
    return true;
}

dmftp_server_config_t config = { .port = DMFTP_PORT_CONTROL, .root = "/srv/ftp" };
dmftp_server_callbacks_t callbacks = { .on_auth = on_auth };

dmftp_server_t server = dmftp_server_create(&config, &callbacks, NULL);
dmftp_server_start(server);   /* returns immediately; the server keeps serving */
```

Or run the service instead of embedding one:

```bash
dmod_loader ftpd.dmf --args "--root /srv/ftp --user admin:s3cret"
```

See [app/dmftp/docs/examples.md](app/dmftp/docs/examples.md) for client
usage and more server recipes.

## API

| Function | Description |
|----------|-------------|
| `dmftp_server_create()` / `_start()` / `_stop()` / `_destroy()` | Server lifecycle |
| `dmftp_session_get_user()` / `_get_peer()` / `_set_root()` / `_set_read_only()` / `_close()` | Per-session accessors, usable from the auth callback |
| `dmftp_client_create()` / `_connect()` / `_quit()` / `_destroy()` | Client lifecycle |
| `dmftp_client_get()` / `_put()` / `_append()` / `_list()` | Transfers |
| `dmftp_client_cwd()` / `_delete()` / `_mkdir()` / `_rmdir()` / `_size()` / `_rename()` / `_command()` | One-shot commands |
| `dmftp_parse_command()` / `_format_reply()` / `_parse_reply()` / `_format_host_port()` / `_parse_host_port()` | The control-line codec |

See [app/dmftp/include/dmftp.h](app/dmftp/include/dmftp.h) for the full
declarations and
[app/dmftp/docs/api-reference.md](app/dmftp/docs/api-reference.md) for the
complete reference.

## Documentation

- **[app/dmftp/docs/dmftp.md](app/dmftp/docs/dmftp.md)** - architecture and
  design rationale
- **[app/dmftp/docs/api-reference.md](app/dmftp/docs/api-reference.md)** -
  complete API documentation
- **[app/dmftp/docs/examples.md](app/dmftp/docs/examples.md)** - worked
  server and client usage
- **[app/ftpd/docs/configuration.md](app/ftpd/docs/configuration.md)** -
  every `ftpd` INI key and command-line flag

View documentation using `dmf-man dmftp` / `dmf-man ftpd`.

## Project Structure

```
dmftp/
├── app/
│   ├── dmftp/                    # Library module: the protocol
│   │   ├── include/dmftp.h
│   │   ├── src/
│   │   │   ├── dmftp.c               # dmod_init()/_deinit(), lock, buffers
│   │   │   ├── dmftp_internal.h      # private state and the file map
│   │   │   ├── dmftp_registrations.c # Built-in API registration (own TU)
│   │   │   ├── dmftp_wire.c          # control-line codec
│   │   │   ├── dmftp_path.c          # jail-safe path resolution
│   │   │   ├── dmftp_net.c           # every dmtcp callback + listener registry
│   │   │   ├── dmftp_xfer.c          # transfer engine and the on_writable pump
│   │   │   ├── dmftp_server.c        # server/session lifecycle
│   │   │   ├── dmftp_server_cmd.c    # command dispatch table
│   │   │   ├── dmftp_server_xfer.c   # PASV/PORT, LIST/RETR/STOR/APPE
│   │   │   └── dmftp_client.c        # the client half
│   │   ├── docs/
│   │   ├── tests/
│   │   ├── CMakeLists.txt
│   │   ├── Makefile
│   │   └── dmftp.dmr
│   ├── ftpd/                     # Application module: the service
│   │   ├── src/ftpd.c
│   │   ├── examples/             # ftpd.ini and the systemd unit
│   │   ├── docs/
│   │   ├── tests/
│   │   ├── CMakeLists.txt
│   │   ├── Makefile
│   │   └── ftpd.dmr
│   └── CMakeLists.txt
├── CMakeLists.txt
└── manifest.dmm
```

## Author

Mikolaj Filar

## License

MIT
