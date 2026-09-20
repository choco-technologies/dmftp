# FTP as a dmsystem service

## Why this exists

dmftp needs no setup from other modules to do its job - it opens its own
TCP control-connection listener in `dmod_init()` (see `src/dmftp_server.c`)
and from that point on serves FTP with no further calls needed. The catch
is exactly that: `dmod_init()` only runs once something *loads and enables*
dmftp - nothing does that on its own at boot. `configs/ftpd.ini` gives
[dmsystem](https://github.com/choco-technologies/dmsystem)'s `libsystemd` a
way to do it automatically.

## `exec=dmftp`, `type=library`

dmftp is a **Library**-type DMOD module (see `CMakeLists.txt`), not an
Application - it has no `main()`, so nothing can spawn it as a process the
way `libsystemd` starts a `simple`/`oneshot` unit. `type=library` covers
this: `exec`'s module is loaded+enabled (`Dmod_LoadModuleByName` +
`Dmod_EnableModule`) when the unit starts, and disabled+unloaded when it
stops - exactly the mechanism
[dmicmp](https://github.com/choco-technologies/dmicmp)'s own
`configs/icmp.ini` uses, see its `docs/service.md` for the full writeup.

## One file, two readers

`configs/ftpd.ini` is unusual in one respect: it is read by *two* different
things, for two different purposes:

1. **`libsystemd`** reads `exec`/`type`/`description` (and any other unit
   keys - see dmsystem's
   [configuration docs](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md))
   to decide *whether and how* to start dmftp. It ignores every other key.
2. **dmftp itself** re-opens this exact same file, once it has been
   enabled, to read `port`/`root`/`user`/`pass` (see
   `src/dmftp_server.c`'s `load_config()`). It ignores `exec`/`type`/
   `description`.

There is no `type=library` equivalent of `args=` for passing settings
through `libsystemd` itself (`args` is documented as having no effect on a
`library`-type unit - there is no process to pass argv to). Rather than
inventing a second config file and a second install location, dmftp reads
its own settings straight out of the same unit file dmsystem starts it
from, at the path `dmf-get`'s `service` config-map tag installs it to:
`/configs/services/dmftp/ftpd.ini` (see the root
[README.md](../README.md)'s "Enabling at boot" section for how that path
comes about). If dmftp is ever loaded a different way (e.g. a host test
that never places a `/configs` tree), `load_config()` falls back to
built-in defaults (`port=21`, `root=/`, `user=anonymous`, `pass=`) rather
than failing `dmod_init()`.

## `service start`/`service stop` both work

Exactly like `icmp`:

```bash
service start ftpd
service status ftpd
service stop ftpd
```

## Enabling at boot

Install dmftp with its `.dmr` (see the root [README.md](../README.md)),
edit `configs/ftpd.ini` for your desired `port`/`root`/`user`/`pass`, then
drop it into `libsystemd`'s units directory:

```bash
cp /opt/dmftp/configs/ftpd.ini /etc/dmsystem/units/ftpd.ini
dmod_loader systemd.dmf --args "/etc/dmsystem/units"
```

On `dmod-boot`, this is done for you by the build once `dmftp service=ftpd.ini`
is added to a board's `flash.dmd` (see
[dmod-boot's dmod-boot skill](https://github.com/choco-technologies/dmod-boot)
and `dmf-get`'s `--config-map` docs for how the `service=` tag routes
`ftpd.ini` to `/configs/services/dmftp/ftpd.ini`).

## Known limitations

See `src/dmftp_server.c`'s top comment for the full list (PASV-only, no
ASCII translation, no REST/APPE/rename).
