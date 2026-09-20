# dmftp API Reference

`include/dmftp.h` is a transport-agnostic FTP (RFC 959) control-connection
engine: it turns a raw byte stream into command-line events and formats
reply lines back out, the same shape
[dmtelnet](https://github.com/choco-technologies/dmtelnet) uses for Telnet
framing. It never touches a socket or a filesystem - the actual FTP
*server* (dmtcp control/data connections, authentication, the virtual
filesystem root, PASV transfer) is this same module's own
`src/dmftp_server.c`/`src/dmftp_commands.c`, started automatically as a
dmsystem service - see [service.md](service.md).

## Types

### `dmftp_t`

Opaque handle to one control-connection session.

### `dmftp_callbacks_t`

```c
typedef struct
{
    dmftp_command_handler_t on_command;
    dmftp_send_handler_t    on_send;
} dmftp_callbacks_t;
```

- `on_command` - fires once per complete command line: `(session, verb, arg, user_data)`.
  `verb` is upper-cased and NUL-terminated; `arg` is the rest of the line
  with leading spaces stripped (`""` if there was none). May be left NULL.
- `on_send` - the only way the engine ever produces wire bytes. **Required**.

## Functions

### `dmftp_create`

```c
dmftp_t dmftp_create(const dmftp_callbacks_t* callbacks, void* user_data);
```

Creates a new session. Returns NULL if `callbacks` is NULL, `callbacks->on_send`
is NULL, or on allocation failure.

### `dmftp_destroy`

```c
void dmftp_destroy(dmftp_t session);
```

Destroys a session. Safe to call with NULL.

### `dmftp_recv`

```c
int dmftp_recv(dmftp_t session, const uint8_t* data, size_t data_len);
```

Feeds newly-received raw bytes into the session, invoking `on_command` once
per complete (CRLF- or bare-LF-terminated) line. A line longer than
`DMFTP_MAX_LINE_LEN` has its overflow silently dropped rather than merged
into the next line. Returns 0 on success, `-EINVAL` on a bad argument.

### `dmftp_reply`

```c
int dmftp_reply(dmftp_t session, int code, const char* text);
```

Formats and sends a `"CODE text\r\n"` reply line (RFC 959 §4.2), e.g.
`dmftp_reply(session, 230, "Login successful")`. Returns 0 on success,
`-EINVAL` if `session`/`text` is NULL or `code` is not in `100..559`.

## Constants

- `DMFTP_MAX_LINE_LEN` (512) - the maximum buffered length of one
  not-yet-terminated command line.

## The FTP server itself

Everything that makes this module an actual FTP *server* - not just a
line-framing engine - lives in `src/dmftp_server.c` (dmtcp control-
connection listener, connection table, PASV port bookkeeping, config
loading) and `src/dmftp_commands.c` (RFC 959 command handling, virtual-root
path resolution, PASV data-connection transfer). None of it is public
Built-in API (nothing outside this module needs to call it), so it is not
listed here - see those files' own doc comments, `docs/service.md` for how
it is started, and the "Supported commands" / "Known limitations" sections
in the root [README.md](../README.md).
