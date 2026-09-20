# ftpd

An FTP (RFC 959) server built on [libftp](../../README.md). Unlike
[telnetd](https://github.com/choco-technologies/dmtelnet/tree/main/tools/telnetd)
(a dmdrvi driver), ftpd is a plain **Application** module with a real
`main()` - see [docs/service.md](docs/service.md) for why that split
matters here: it's what lets more than one independently-configured ftpd
run at once (different port, root, and credentials each), the same way
this ecosystem runs one `dhcpc`/`console`/`getty` process per interface or
tty rather than one process juggling all of them.

```
dmtcp (TCP) <-> libftp (protocol) <-> ftpd (dmtcp glue, RFC 959 commands, filesystem access)
```

## Supported commands

`USER`, `PASS`, `QUIT`, `NOOP`, `SYST`, `PWD`/`XPWD`, `CWD`, `CDUP`, `TYPE`,
`PASV`, `PORT`, `LIST`, `NLST`, `RETR`, `STOR`, `DELE`, `MKD`/`XMKD`,
`RMD`/`XRMD`, `SIZE`, `ABOR`.

`PORT` only accepts an address matching the control connection's own peer
(refused otherwise) - this is what stops the server being abused as an
"FTP bounce" to reach a third host on the client's behalf (RFC 2577 §3.2).

## Configuration

Plain command-line arguments, not a config file - see
[docs/service.md](docs/service.md) for why:

```
ftpd [--port N] [--root PATH] [--user NAME] [--pass SECRET]
```

| Flag     | Default     | Meaning |
|----------|-------------|---------|
| `--port` | `21`        | TCP port the control connection listens on |
| `--root` | `/`         | Virtual filesystem root every FTP path is resolved (and jailed) under |
| `--user` | `anonymous` | Required username - `anonymous` (case-insensitive) accepts any USER/PASS |
| `--pass` | (empty)     | Required password when `--user` isn't `anonymous` - empty accepts any password |

On dmod-boot, add `service=` entries to `flash.dmd` (see
[configs/ftpd@.ini](configs/ftpd@.ini)/[configs/ftpd@public.ini](configs/ftpd@public.ini)
for the actual unit files, and [docs/service.md](docs/service.md) for the
full walkthrough, including running a second, differently-configured
instance):

```
ftpd service=ftpd@.ini
ftpd service=ftpd@public.ini
```

## Known limitations (deliberately out of scope for this first version)

- **No ASCII translation** - `TYPE A` is accepted but every transfer is
  effectively binary (no CRLF translation). Harmless for the overwhelming
  majority of clients, which default to `TYPE I` anyway.
- **No resume/append/rename** - `REST`, `APPE`, and `RNFR`/`RNTO` are not
  implemented.
- **No per-directory-entry timestamps** - the Dmod SAL has no mtime
  accessor, so every `LIST` line reports the same fixed placeholder date.
  Real clients tolerate this (they parse the fixed-width fields, not the
  date's value).
- **No graceful shutdown hook** - see [docs/service.md](docs/service.md);
  `service stop` simply ends the process, dropping any in-flight transfer.

See `src/ftpd_server.c`'s top comment for the full rationale, including the
compound-literal/loader-relocation pitfall that cost real debugging time on
real hardware - worth reading before touching the callback-wiring code.

## Author

Mikolaj Filar

## License

MIT (see the repository's [LICENSE](../../LICENSE))
