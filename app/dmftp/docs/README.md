# dmftp Documentation

`dmftp` is the FTP protocol library: an RFC 959 server and client over
[`dmtcp`](https://github.com/choco-technologies/dmtcp). For the service that
runs a server from a unit file, see the `ftpd` module in the same repository.

## Contents

- **[dmftp.md](dmftp.md)** - architecture and design rationale: the transfer
  pump, the two loader constraints this module is shaped around, the path
  jail, the security posture, and the complete list of known limitations
- **[api-reference.md](api-reference.md)** - every type, function and error
  code
- **[examples.md](examples.md)** - worked server and client usage

## Quick reference

```c
#include "dmftp.h"

/* Server */
dmftp_server_config_t config = { .port = DMFTP_PORT_CONTROL, .root = "/srv/ftp" };
dmftp_server_callbacks_t callbacks = { .on_auth = my_auth_handler };
dmftp_server_t server = dmftp_server_create(&config, &callbacks, NULL);
dmftp_server_start(server);

/* Client */
dmftp_client_t client = dmftp_client_create(&client_config, &client_callbacks, NULL);
dmftp_client_connect(client);          /* -> on_ready, then _get()/_put()/_list() */
```

View this documentation with `dmf-man`:

```bash
dmf-man dmftp          # main documentation
dmf-man dmftp api      # API reference
```
