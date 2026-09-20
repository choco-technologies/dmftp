# libftp API Reference

`include/libftp.h` is a transport-agnostic FTP (RFC 959) control-connection
engine: it turns a raw byte stream into command-line events and formats
reply lines back out, the same shape
[dmtelnet](https://github.com/choco-technologies/dmtelnet) uses for Telnet
framing. It never touches a socket or a filesystem, and it has no config
and no `main()` - the actual FTP *server* is a separate Application module
in this same repo, [tools/ftpd](../tools/ftpd) (see its own
`docs/service.md` for why that split exists and how it's started).

## Types

### `libftp_t`

Opaque handle to one control-connection session.

### `libftp_callbacks_t`

```c
typedef struct
{
    libftp_command_handler_t on_command;
    libftp_send_handler_t    on_send;
} libftp_callbacks_t;
```

- `on_command` - fires once per complete command line: `(session, verb, arg, user_data)`.
  `verb` is upper-cased and NUL-terminated; `arg` is the rest of the line
  with leading spaces stripped (`""` if there was none). May be left NULL.
- `on_send` - the only way the engine ever produces wire bytes. **Required**.

## Functions

### `libftp_create`

```c
libftp_t libftp_create(const libftp_callbacks_t* callbacks, void* user_data);
```

Creates a new session. Returns NULL if `callbacks` is NULL, `callbacks->on_send`
is NULL, or on allocation failure.

### `libftp_destroy`

```c
void libftp_destroy(libftp_t session);
```

Destroys a session. Safe to call with NULL.

### `libftp_recv`

```c
int libftp_recv(libftp_t session, const uint8_t* data, size_t data_len);
```

Feeds newly-received raw bytes into the session, invoking `on_command` once
per complete (CRLF- or bare-LF-terminated) line. A line longer than
`LIBFTP_MAX_LINE_LEN` has its overflow silently dropped rather than merged
into the next line. Returns 0 on success, `-EINVAL` on a bad argument.

### `libftp_reply`

```c
int libftp_reply(libftp_t session, int code, const char* text);
```

Formats and sends a `"CODE text\r\n"` reply line (RFC 959 §4.2), e.g.
`libftp_reply(session, 230, "Login successful")`. Returns 0 on success,
`-EINVAL` if `session`/`text` is NULL or `code` is not in `100..559`.

## Constants

- `LIBFTP_MAX_LINE_LEN` (512) - the maximum buffered length of one
  not-yet-terminated command line.
