# dmftp Examples

## A server with a fixed account

The minimum: decide who may log in, start listening. `on_auth` is the only
required callback - a server that cannot decide who gets in would have to
let everybody in, so `dmftp_server_create()` refuses to build one without it.

```c
#include "dmftp.h"
#include <string.h>

static bool on_auth(dmftp_session_t session, const char* user, const char* password, void* user_data)
{
    (void)user_data;

    if (strcmp(user, "admin") != 0 || strcmp(password, "s3cret") != 0)
        return false;

    /* Applied before the 230 goes out. */
    dmftp_session_set_root(session, "/srv/ftp/admin");
    dmftp_session_set_read_only(session, false);
    return true;
}

static dmftp_server_t g_server;

void start_ftp(void)
{
    dmftp_server_config_t config = { 0 };
    config.port = DMFTP_PORT_CONTROL;
    config.root = "/srv/ftp";     /* nothing outside this is reachable */
    config.max_sessions = 4;

    dmftp_server_callbacks_t callbacks = { 0 };
    callbacks.on_auth = on_auth;

    g_server = dmftp_server_create(&config, &callbacks, NULL);
    dmftp_server_start(g_server);
}
```

`start_ftp()` returns immediately and the server keeps running: dmftp is
callback-driven and owns no thread of its own.

## A read-only anonymous server

```c
static bool allow_anonymous(dmftp_session_t session, const char* user, const char* password, void* user_data)
{
    (void)password; (void)user_data;

    if (strcmp(user, "anonymous") != 0 && strcmp(user, "ftp") != 0)
        return false;

    dmftp_session_set_read_only(session, true);
    return true;
}
```

With `config.read_only = true` the per-session call is redundant - a
read-only server can never be widened by a session - but stating it keeps
the handler honest if the server setting later changes.

## Watching who connects

```c
static void on_open(dmftp_session_t session, void* user_data)
{
    (void)user_data;

    dmip_addr_t peer;
    uint16_t    port;
    if (dmftp_session_get_peer(session, &peer, &port) == 0)
    {
        Dmod_Printf("ftp: %u.%u.%u.%u:%u connected\n",
                     peer.addr.v4[0], peer.addr.v4[1], peer.addr.v4[2], peer.addr.v4[3], port);
    }
}

callbacks.on_session_open = on_open;   /* fires before the 220 greeting */
```

`on_session_close` fires immediately before the session is freed - the
handle must not be used after it returns.

## Downloading a file

The client is a state machine: `dmftp_client_connect()` returns as soon as
the TCP connect is under way, and `on_ready` fires once the login and the
initial `TYPE` are done. Commands may only be issued from that point on.

```c
static void on_ready(dmftp_client_t client, void* user_data)
{
    (void)user_data;
    dmftp_client_get(client, "/pub/firmware.bin", "/tmp/firmware.bin");
}

static void on_done(dmftp_client_t client, int result, uint64_t bytes, void* user_data)
{
    (void)user_data;

    if (result != 0)
    {
        Dmod_Printf("ftp: download failed (%d)\n", result);
    }
    else
    {
        Dmod_Printf("ftp: got %u bytes\n", (unsigned)bytes);
    }
    dmftp_client_quit(client);
}

static void on_closed(dmftp_client_t client, int error, void* user_data)
{
    (void)error; (void)user_data;
    dmftp_client_destroy(client);   /* the handle dies with this callback */
}

void fetch_firmware(void)
{
    dmftp_client_config_t config = { 0 };
    config.host.family = dmip_family_v4;
    config.host.addr.v4[0] = 192; config.host.addr.v4[1] = 168;
    config.host.addr.v4[2] = 1;   config.host.addr.v4[3] = 10;
    config.user = "device";
    config.password = "s3cret";
    /* dmftp_data_passive is the default and the one that works from behind
     * NAT; dmftp_data_active makes the client listen and send PORT. */

    dmftp_client_callbacks_t callbacks = { 0 };
    callbacks.on_ready = on_ready;
    callbacks.on_done = on_done;
    callbacks.on_closed = on_closed;

    dmftp_client_t client = dmftp_client_create(&config, &callbacks, NULL);
    dmftp_client_connect(client);
}
```

`on_done` fires once **both** the data connection has finished and the
server's `226` has arrived - reporting on whichever came first would mean
announcing success before the server had a chance to disagree.

## Listing a remote directory

```c
static void on_list(dmftp_client_t client, const char* line, void* user_data)
{
    (void)client; (void)user_data;
    Dmod_Printf("%s\n", line);   /* one entry, CRLF already stripped */
}

/* From on_ready: */
dmftp_client_list(client, "/pub", true);   /* false for NLST (names only) */
```

The listing ends with the `on_done` that follows the last `on_list`.

## Uploading, and the serial control channel

```c
dmftp_client_put(client, "/var/log/device.log", "/incoming/device.log");
```

FTP's control channel carries one command at a time, so a second call
before `on_done` returns `-EBUSY` rather than queueing:

```c
int result = dmftp_client_put(client, "/a", "/a");
if (result == -EBUSY)
{
    /* still transferring - retry from on_done */
}
```

## Sending something this API does not wrap

```c
dmftp_client_command(client, "SITE", "CHMOD 644 /pub/file");
```

The reply arrives through `on_reply` like any other.
