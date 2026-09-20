# Running ftpd as a dmsystem service

## Why an Application, not a Library

Compare this to [dmicmp](https://github.com/choco-technologies/dmicmp),
whose `docs/service.md` covers the `type=library` pattern: dmicmp needs no
per-instance settings at all (there is only ever one ICMP echo responder),
so "load and enable it" is the whole story. ftpd is different - it needs
port/root/username/password, and a real deployment often wants *more than
one* independently configured instance (e.g. an anonymous read-only one and
a private authenticated one). Neither of those fits `type=library`:

- `type=library` units have no `args=` effect at all (there is no process
  to hand argv to - see
  [dmsystem's configuration docs](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#typelibrary-services-backed-by-a-library-module-not-a-process)),
  so a Library-type ftpd would have had nowhere to get its settings from
  except hard-coding a path back into `/configs/...` and re-reading its own
  unit file by hand - which is exactly what an earlier version of this
  module did, and it was a hack: brittle (silently falls back to defaults
  if the install layout ever changes), and it still only supported one
  global instance no matter how many unit files you dropped in.
- A Library module is also a *singleton* per loaded instance in the sense
  that matters here: `dmod_init()` runs once and there is one dmtcp control
  connection listener, at one hard-coded/re-read port, for the whole
  system.

Making ftpd a genuine **Application** (`dmod_add_executable`, a real
`main(argc, argv)`) fixes both: `args=` in the unit file becomes ordinary
argv, and every `service start` of a unit spawns an independent *process*
with its own connection table, its own listening port, its own root. This
is the same shape this ecosystem already uses for `dhcpc`/`console`/`getty`
- one small binary, instantiated per `@<name>` unit - just applied to an
FTP server instead of a DHCP lease or a login shell.

## Settings: plain argv, not a re-read config file

```
Usage: ftpd [--port N] [--root PATH] [--user NAME] [--pass SECRET]
```

All four are optional (see `src/ftpd.c` for the defaults: port 21, root
`/`, user `anonymous`, empty password). A unit file supplies them the same
way [dmsystem's own example](https://github.com/choco-technologies/dmsystem)
does for `dmhttpd`:

```ini
exec=ftpd
type=simple
args=--port 21 --root /
```

## Templates: one binary, several instances

`configs/ftpd@.ini` is a template (never started on its own - see
dmsystem's
[template documentation](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#templates)).
`configs/ftpd@public.ini` and `configs/ftpd@private.ini` are two real,
independent instances of it:

```ini
# ftpd@public.ini
args=--port 21 --root /
```

```ini
# ftpd@private.ini
args=--port 2121 --root /dev --user admin --pass secret
```

Both are picked up and started automatically at boot the moment they exist
as real files in the units directory - dropping in a third
`ftpd@<name>.ini` with its own `args=` adds another instance with no code
changes. Verified on an STM32F746G-DISCO board: `ftpd@public` and
`ftpd@private` both start at boot as two separate loaded copies of the
`ftpd` module (distinct load addresses in the boot log), one answering
anonymously on port 21 with `/` as its root, the other requiring
`admin`/`secret` on port 2121 with `/dev` as its root - a `LIST` against
each shows genuinely different, independent directory contents.

## No graceful shutdown hook

Like every other long-running Application module in this ecosystem
(`networkd`, `dmtcpecho`), ftpd does not implement `dmod_signal()` - there
is no working example of that hook anywhere in this codebase to follow, so
`service stop ftpd@<name>` simply ends the process. In-flight transfers on
that instance are dropped, not drained.

## Enabling at boot

Install ftpd with its `.dmr` (see the root [README.md](../../../README.md)),
copy (and if needed, edit) `ftpd@public.ini` into `libsystemd`'s units
directory, then start `systemd` pointed at that directory:

```bash
cp /opt/ftpd/configs/ftpd@public.ini /etc/dmsystem/units/
dmod_loader systemd.dmf --args "/etc/dmsystem/units"
```

On `dmod-boot`, this is done for you by the build once `ftpd
service=ftpd@public.ini` (and, to also ship the template,
`ftpd service=ftpd@.ini`) is added to a board's `flash.dmd` - see
[dmod-boot's dmod-boot skill](https://github.com/choco-technologies/dmod-boot)
and `dmf-get`'s `--config-map` docs for how the `service=` tag routes a
config file to `/configs/services/<module>/<file>`.
