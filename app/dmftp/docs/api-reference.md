# dmftp API Reference

See [dmftp.md](dmftp.md) for the design rationale behind this API, and
[examples.md](examples.md) for worked usage.

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `DMFTP_PORT_CONTROL` | 21 | RFC 959 control port; `dmftp_server_config_t::port` defaults to it |
| `DMFTP_PORT_DATA_DEFAULT` | 20 | RFC 959 §3.2 server-side data port, for `active_data_port` |
| `DMFTP_VERB_MAX` | 4 | Longest command verb RFC 959 defines |
| `DMFTP_LINE_MAX` | 512 | Longest control line accepted; a longer one gets `500` and is discarded |

## Shared types

| Type | Description |
|------|-------------|
| `dmftp_type_t` | `dmftp_type_ascii` / `dmftp_type_image`. ASCII really translates: LF↔CRLF in both directions |
| `dmftp_data_mode_t` | `dmftp_data_passive` (PASV) / `dmftp_data_active` (PORT) |
| `dmftp_command_t` | One parsed control line: `verb` (upper-cased, NUL-terminated), `arg` (borrowed, may be NULL), `arg_len` |

## Control-line codec

Pure functions - no connection, no session, no allocation.

| Function | Description |
|----------|-------------|
| `dmftp_parse_command()` | Parse a CRLF-stripped line into verb + argument; strips any Telnet IAC prefix |
| `dmftp_format_reply()` | Format one single-line reply (`"226 Transfer complete.\r\n"`) |
| `dmftp_parse_reply()` | Parse a reply line into code + final/continuation + text |
| `dmftp_format_host_port()` | Encode an IPv4 address + port as `h1,h2,h3,h4,p1,p2` |
| `dmftp_parse_host_port()` | Decode the same, tolerating spaces after the commas |

### Errors

| Code | Meaning |
|---|---|
| `-EINVAL` | NULL argument, out-of-range reply code, or a non-IPv4 address |
| `-EPROTO` | Malformed input: empty line, verb over `DMFTP_VERB_MAX`, reply not starting with three digits, tuple not six decimal fields in range |
| `-ENOSPC` | The output buffer is too small (24 bytes always suffices for a host-port tuple) |

## Server

| Type | Description |
|------|-------------|
| `dmftp_server_t` | Opaque handle to one server (one listening control port) |
| `dmftp_session_t` | Opaque handle to one client's control connection |
| `dmftp_auth_handler_t` | **Required.** Decides whether a USER/PASS pair may log in; may call `_session_set_root()`/`_set_read_only()` from inside |
| `dmftp_session_handler_t` | Announces a session opening (before the 220) or closing (immediately before it is freed) |
| `dmftp_server_callbacks_t` | `on_auth` (required), `on_session_open`, `on_session_close` |
| `dmftp_server_config_t` | `port`, `root`, `read_only`, `allow_passive`, `allow_active`, `active_data_port`, `max_sessions`, `banner` - all copied |

### Lifecycle

| Function | Description |
|----------|-------------|
| `dmftp_server_create()` | Build a server; NULL config means every default. Returns NULL without an `on_auth` |
| `dmftp_server_start()` | Listen on the control port. `-EALREADY` if running, `-EEXIST` if the port is taken |
| `dmftp_server_stop()` | Stop listening and close every live session. Idempotent |
| `dmftp_server_destroy()` | Stop (if running) and free |
| `dmftp_server_get_port()` / `_get_session_count()` | Accessors; 0 for an invalid handle |

### Per-session

| Function | Description |
|----------|-------------|
| `dmftp_session_get_user()` | The name given by USER; NULL before USER arrives. Borrowed |
| `dmftp_session_get_peer()` | The address/port the client connected from |
| `dmftp_session_set_root()` | Confine this session further (resolved inside the server root - can only narrow). Resets the working directory to `/` |
| `dmftp_session_set_read_only()` | Restrict write access; cannot widen past the server's own setting |
| `dmftp_session_set_user_data()` / `_get_user_data()` | An opaque per-session pointer |
| `dmftp_session_close()` | Close from the outside: sends `421`, then FIN |

### Commands the server answers

| Group | Commands |
|---|---|
| Access | `USER` `PASS` `ACCT` `QUIT` |
| Session | `NOOP` `SYST` `FEAT` `OPTS` `HELP` `STAT` `TYPE` `MODE` `STRU` `ALLO` `REST` |
| Navigation | `PWD`/`XPWD` `CWD`/`XCWD` `CDUP`/`XCUP` |
| Data setup | `PASV` `PORT` |
| Transfer | `LIST` `NLST` `RETR` `STOR` `APPE` `ABOR` |
| Mutation | `DELE` `MKD`/`XMKD` `RMD`/`XRMD` `RNFR` `RNTO` |
| Metadata | `SIZE` `MDTM` |

Anything else - `EPSV`, `EPRT`, `AUTH`, ... - is answered `502`.
`FEAT` advertises `SIZE`, `REST STREAM` and `UTF8`; `MDTM` is deliberately
not advertised (see [dmftp.md](dmftp.md)).

## Client

| Type | Description |
|------|-------------|
| `dmftp_client_t` | Opaque handle to one connection to a remote server |
| `dmftp_client_ready_handler_t` | Connected **and** logged in - commands may now be issued |
| `dmftp_client_reply_handler_t` | Every final (non-continuation) reply, including the 220 greeting |
| `dmftp_client_list_handler_t` | One line of a LIST/NLST response, CRLF stripped |
| `dmftp_client_done_handler_t` | A transfer finished: `result` and the byte count. Fires once both the data connection and the server's `226` are in |
| `dmftp_client_closed_handler_t` | TERMINAL - the control connection is gone; the handle must not be used afterwards |
| `dmftp_client_config_t` | `host` (IPv4), `port`, `user`, `password`, `data_mode`, `type` - all copied |

| Function | Description |
|----------|-------------|
| `dmftp_client_create()` | Build a client. NULL config, or a non-IPv4 host, returns NULL |
| `dmftp_client_connect()` | Open the control connection and log in; outcome via `on_ready` / `on_closed` |
| `dmftp_client_get()` | RETR a remote path into a local file |
| `dmftp_client_put()` / `_append()` | STOR / APPE a local file to a remote path |
| `dmftp_client_list()` | LIST (`long_format` true) or NLST; lines arrive via `on_list` |
| `dmftp_client_cwd()` / `_delete()` / `_mkdir()` / `_rmdir()` / `_size()` | One-shot commands; the outcome arrives via `on_reply` |
| `dmftp_client_rename()` | RNFR followed automatically by RNTO once the 350 arrives |
| `dmftp_client_command()` | Send an arbitrary verb + argument - the escape hatch |
| `dmftp_client_quit()` | Send QUIT and close; `on_closed` fires when it is really gone |
| `dmftp_client_destroy()` | Abandon any connection and free. Does not deliver `on_closed` |
| `dmftp_client_get_user_data()` / `_is_ready()` | Accessors |

### Errors

| Code | Meaning |
|---|---|
| `-EINVAL` | NULL/invalid handle, or a verb over `DMFTP_VERB_MAX` |
| `-ENOTCONN` | Not logged in yet (`on_ready` has not fired) |
| `-EBUSY` | Another command is still outstanding - FTP's control channel is serial |
| `-EIO` | The local file could not be opened |
| `-EALREADY` | `dmftp_client_connect()` on an already-connected client |

`on_done`'s `result` is `0`, or `-EPERM` (the server refused the command),
`-ECONNRESET` (the data connection died mid-transfer), `-EIO` (local file
error), or `-EPROTO` (an unparseable `227`).

## What's not here yet

- IPv4 only (no `dmip_v6_send()` below us); `EPSV`/`EPRT` answer `502`.
- No FTPS/TLS.
- `MDTM` has no timestamp to report - the SAL exposes no mtime.
- `LIST` requires a directory; a single file cannot be listed by name.
- One transfer at a time per session, one command in flight per client.

See [dmftp.md](dmftp.md)'s "Known limitations" section for the complete,
explained list.
