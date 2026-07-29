# ftpd Configuration

`ftpd` reads an INI file for its defaults and its account table, then lets
command-line flags override anything the file set. That order is deliberate:
several units can share one configuration file and still differ in a setting
or two.

The default file is `/etc/ftpd.ini`; `--config PATH` points somewhere else.
A missing file is **not** fatal - a unit that configures everything through
flags is a perfectly good way to run this.

A ready-to-edit copy of everything below ships as
[`examples/ftpd.ini`](../examples/ftpd.ini), and the matching unit file as
[`examples/ftpd-unit.ini`](../examples/ftpd-unit.ini).

## `[server]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `port` | integer | `21` | Control port to listen on |
| `root` | path | `/` | The directory every session is confined to |
| `readonly` | boolean | `false` | Refuse every mutating command (`STOR`, `DELE`, `MKD`, `RMD`, `RNFR`/`RNTO`, `APPE`) |
| `banner` | text | `dmftp ready` | Text of the `220` greeting |
| `max_sessions` | integer | `0` | Concurrent session limit; `0` is unlimited |
| `passive` | boolean | `true` | Answer `PASV` |
| `active` | boolean | `true` | Answer `PORT` |
| `active_data_port` | integer | `0` | Local port for a `PORT` data connection; `0` picks an ephemeral one, `20` is RFC 959's classic value |
| `anonymous` | boolean | `false` | Accept `anonymous`/`ftp` with any password, always read-only |

Booleans accept `1`/`true`/`yes`/`on` and `0`/`false`/`no`/`off`.

`root` is a real confinement, not a starting directory: a client can never
resolve a path outside it. A `..` that would climb past it is clamped to the
root, which is what a chrooted FTP server does and what clients expect when
they blindly send `CWD ../..`.

Setting both `passive` and `active` to `false` would leave a server no client
could transfer anything through, so that combination is treated as "no
preference" and enables both.

## `[users]`, `[home]`, `[readonly]`

Three flat sections keyed by user name, rather than one `[user.name]`
section each - it keeps the common case (a name and a password) to a single
readable line.

```ini
[users]
admin = s3cret
guest =

[home]
guest = /pub

[readonly]
guest = true
```

| Section | Meaning |
|---|---|
| `[users]` | `name = password`. **An empty password accepts any password for that name** |
| `[home]` | Optional per-user directory, resolved inside `[server] root`. No entry means the server root itself |
| `[readonly]` | Optional per-user write permission. No entry means whatever `[server] readonly` says |

A user can only ever be restricted further, never granted more than the
server allows: on a `readonly = true` server, a `[readonly] admin = false`
entry is ignored.

## Command-line flags

Every flag overrides the file.

| Flag | Equivalent key |
|---|---|
| `-c`, `--config PATH` | (which file to read; default `/etc/ftpd.ini`) |
| `-p`, `--port N` | `[server] port` |
| `-r`, `--root PATH` | `[server] root` |
| `--banner TEXT` | `[server] banner` |
| `--max-sessions N` | `[server] max_sessions` |
| `--active-data-port N` | `[server] active_data_port` |
| `--read-only` | `[server] readonly = true` |
| `--no-passive` | `[server] passive = false` |
| `--no-active` | `[server] active = false` |
| `--anonymous` | `[server] anonymous = true` |
| `--user NAME:PASS[:HOME[:ro]]` | one `[users]` entry, with its `[home]`/`[readonly]` |
| `-h`, `--help` | print usage and exit |

`--user` defines (or replaces) one account inline:

```bash
ftpd --root /srv/ftp --user admin:s3cret --user guest::/pub:ro
```

A later definition wins, which is what lets a flag override an account of
the same name coming out of the INI file.

## Refusing to start

`ftpd` exits with `-EINVAL` if there are **no accounts and anonymous access
is off** - a server nobody could log into is a configuration mistake, not a
lockdown, and failing loudly beats listening on port 21 to no purpose.

## Running it

As a unit (see [`examples/ftpd-unit.ini`](../examples/ftpd-unit.ini)):

```ini
description=FTP server
exec=ftpd
args=--config /etc/ftpd.ini
after=networking
requires=networking
type=simple
restart=always
```

Or straight from the loader:

```bash
dmod_loader ftpd.dmf --args "--root /srv/ftp --user admin:s3cret"
```

`main()` configures the server, starts it and returns - dmftp is
callback-driven, so there is nothing for a thread to sit in. The service
keeps serving until the module is unloaded, at which point `dmod_deinit()`
closes the port and every live session.

## Security note

Read this before putting an `ftpd` anywhere interesting.

- **FTP's control channel is plaintext.** Usernames and passwords cross the
  network in the clear, and there is no TLS module in this tree to wrap it
  in. Treat `ftpd` as something for a trusted network segment.
- **An empty password in `[users]` accepts any password.** It is a
  convenience for a guest account behind that trusted segment, not an
  access-control mechanism.
- **There is no rate limiting or lockout.** `max_sessions` caps concurrency,
  not attempts. Password comparison is not timing-hardened either - a
  smaller problem than the plaintext channel it travels over, but worth
  knowing.
- **Anonymous access is always read-only** and off unless you turn it on.
- The FTP bounce attack (CERT CA-1997-27) *is* closed: `dmftp` requires a
  `PORT` command's address to match the control connection's peer.
