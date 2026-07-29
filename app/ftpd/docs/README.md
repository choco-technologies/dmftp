# ftpd Documentation

`ftpd` is the FTP service: a thin Application module that turns a unit
file's `args=` line and an INI file into a running FTP server. All the
protocol logic lives in the `dmftp` Library next door - anything embedding a
server or client of its own should link that directly rather than going
through this.

## Contents

- **[configuration.md](configuration.md)** - every INI key and command-line
  flag, how they override each other, and the security note

## Quick start

```bash
# With a configuration file
dmod_loader ftpd.dmf --args "--config /etc/ftpd.ini"

# Or entirely from flags
dmod_loader ftpd.dmf --args "--root /srv/ftp --user admin:s3cret"
```

As a unit for dmsystem's `systemd`, copy
[`../examples/ftpd-unit.ini`](../examples/ftpd-unit.ini) into the units
directory it scans.

## Testing it against a real client

The automated tests cover the configuration contract and the server object
`ftpd` builds ([`tests/ftpd_test.c`](../tests/ftpd_test.c)); driving the
service end to end means pointing a real client at it:

```bash
# Anything that speaks RFC 959 works - it advertises UNIX Type: L8 listings
lftp -u admin,s3cret ftp://<device-ip>
curl -u admin:s3cret ftp://<device-ip>/pub/
```

Expect `SIZE`, `REST STREAM` and `UTF8` in the `FEAT` reply, and both `PASV`
and `PORT` to work. `MDTM` answers `550` on purpose - the SAL exposes no
modification time, which is also why it is not advertised. See
[dmftp's documentation](../../dmftp/docs/dmftp.md) for the full list.

View this documentation with `dmf-man`:

```bash
dmf-man ftpd
```
