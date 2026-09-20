#include "dmod.h"
#include "ftpd_internal.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

/**
 * ftpd's dmtcp wiring: the control-connection listener, the connection
 * table, and PASV port bookkeeping/PORT active-open. RFC 959 command
 * handling and data transfer live in src/ftpd_commands.c - see
 * ftpd_internal.h. Argument parsing and process entry are in src/ftpd.c.
 *
 * ftpd is an Application-type DMOD module (see CMakeLists.txt) with a
 * real main() - unlike a Library module (e.g. dmicmp, or this repo's own
 * libftp), it is spawned as a process by dmsystem's "type=simple" unit
 * mechanism, with its settings (port/root/user/pass) passed as ordinary
 * argv via the unit's `args=` key - see configs/ftpd.ini and
 * docs/service.md. This is deliberate: it is what lets more than one
 * independently-configured ftpd run at once (a public/anonymous instance
 * on one port serving one root, a private/authenticated instance on
 * another port serving a different root), the same way this ecosystem
 * runs one `networkd`/`dhcpc` process per interface rather than one
 * global instance juggling all of them.
 *
 * Callback structs (libftp_callbacks_t, dmtcp_conn_callbacks_t) are always
 * built field-by-field here, never as a `{ .field = fn, ... }` compound
 * literal, even though every other module in this ecosystem (telnetd.c
 * included) uses that shorthand freely. Confirmed on real STM32F746G-DISCO
 * hardware: GCC compiles a compound literal whose fields are function
 * addresses as a small anonymous constant-data blob (loaded via a single
 * PC-relative `ldmia`), and the dmod loader's relocation pass does not
 * patch function-address entries living in a data blob like that for a
 * position-independent .dmf - every function pointer built that way comes
 * out as its raw, unrelocated link-time offset (a small integer, not a
 * valid code address), and the first call through it hard-faults. Plain
 * `x.field = fn;` assignments compile to ordinary PC-relative code that the
 * loader does relocate correctly.
 *
 * PORT (active mode): cmd_port() in ftpd_commands.c refuses any address
 * that doesn't match the control connection's own peer, before ever
 * storing it - without that check, PORT would let any client point this
 * server's outbound data connection at an arbitrary third host/port (the
 * classic "FTP bounce" abuse, RFC 2577 §3.2), effectively turning it into
 * an anonymous port scanner. ftpd_server_connect_port() then opens that
 * connection from FTPD_ACTIVE_SRC_PORT (20, ftp-data, per RFC 959 §3.2) -
 * the same source port for every active-mode transfer, on every
 * connection, which is fine since dmtcp keys connections by the full
 * 4-tuple, not source port alone.
 *
 * Known limitations (deliberately out of scope for this first version,
 * same spirit as telnetd.c's own documented limitations):
 *   - TYPE A (ASCII) is accepted but never actually translates line
 *     endings - every transfer is effectively binary. Harmless for the
 *     overwhelming majority of clients, which default to TYPE I anyway.
 *   - No REST (resume), APPE (append), or RNFR/RNTO (rename) support.
 *   - dmtcp_send() on the control connection is best-effort, exactly like
 *     telnetd.c documents for its own Telnet traffic - fine for short
 *     reply lines, which never come close to filling the outbound buffer.
 *   - No graceful shutdown hook (`dmod_signal()`) - none of this
 *     ecosystem's other long-running Application modules (networkd,
 *     dmtcpecho) implement one either; `service stop` simply ends the
 *     process.
 */

struct ftpd_context* g_ftpd_context = NULL;

bool ftpd_is_anonymous_user(const char* user)
{
    static const char anon[] = "anonymous";
    size_t i = 0;
    for (; anon[i] != '\0'; i++)
    {
        char a = anon[i];
        char b = user[i];
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return user[i] == '\0';
}

/**
 * Strips a configured root's trailing slash (e.g. "/ftp/" -> "/ftp"), so
 * ftpd_commands.c's real-path builder can always just concatenate
 * root + virtual_path without worrying about a doubled slash - except for
 * the root "/" itself, which is left alone (dmvfs is mounted there, and
 * stripping it would leave an empty string).
 */
static char* normalize_root(const char* raw)
{
    size_t len = strlen(raw);
    while (len > 1 && raw[len - 1] == '/')
        len--;

    char* copy = Dmod_Malloc(len + 1);
    if (copy == NULL)
        return NULL;

    for (size_t i = 0; i < len; i++)
        copy[i] = raw[i];
    copy[len] = '\0';
    return copy;
}

static ftpd_connection_t* find_free_slot_locked(struct ftpd_context* ctx)
{
    for (size_t i = 0; i < FTPD_MAX_CONNECTIONS; i++)
    {
        if (!ctx->connections[i].in_use)
            return &ctx->connections[i];
    }
    return NULL;
}

static ftpd_connection_t* find_connection_by_pasv_port_locked(struct ftpd_context* ctx, uint16_t port)
{
    for (size_t i = 0; i < FTPD_MAX_CONNECTIONS; i++)
    {
        if (ctx->connections[i].in_use && ctx->connections[i].pasv_pending && ctx->connections[i].pasv_port == port)
            return &ctx->connections[i];
    }
    return NULL;
}

void ftpd_connection_release(ftpd_connection_t* c)
{
    if (c == NULL || !c->in_use)
        return;

    ftpd_server_stop_pasv(c);

    if (c->data_conn != NULL)
    {
        dmtcp_abort(c->data_conn);
        c->data_conn = NULL;
    }
    if (c->transfer_file != NULL)
    {
        Dmod_FileClose(c->transfer_file);
        c->transfer_file = NULL;
    }
    Dmod_Free(c->list_buffer);
    c->list_buffer = NULL;
    c->list_len = 0;
    c->list_sent = 0;
    c->data_op = ftpd_data_op_none;

    if (c->engine != NULL)
    {
        libftp_destroy(c->engine);
        c->engine = NULL;
    }
    Dmod_Free(c->username);
    c->username = NULL;
    Dmod_Free(c->cwd);
    c->cwd = NULL;

    c->control_conn = NULL;
    c->in_use = false;
}

/* ---- PASV port reservation / PORT active connect - see ftpd_internal.h ---- */

/**
 * Fills in the callbacks shared by both a PASV-accepted and a PORT-
 * connected data connection. Field-by-field, not a compound literal - see
 * the warning in this file's top comment. `on_established` is left NULL
 * (PASV-accepted connections are already established by the time they're
 * handed to us - see dmtcp_accept_handler_t's own doc comment); callers
 * that need it (ftpd_server_connect_port()) set it themselves afterward.
 */
static void fill_data_callbacks(dmtcp_conn_callbacks_t* callbacks)
{
    memset(callbacks, 0, sizeof(*callbacks));
    callbacks->on_data     = ftpd_data_on_data;
    callbacks->on_writable = ftpd_data_on_writable;
    callbacks->on_closed   = ftpd_data_on_closed;
    callbacks->on_reset    = ftpd_data_on_reset;
    callbacks->on_error    = ftpd_data_on_error;
}

static void pasv_on_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)peer;
    (void)peer_port;
    (void)iface;

    struct ftpd_context* ctx = g_ftpd_context;
    if (ctx == NULL)
    {
        dmtcp_abort(conn);
        return;
    }

    dmip_addr_t local_addr;
    uint16_t local_port;
    if (dmtcp_conn_get_local_endpoint(conn, &local_addr, &local_port) != 0)
    {
        dmtcp_abort(conn);
        return;
    }

    dmosi_mutex_lock(ctx->mutex);
    ftpd_connection_t* c = find_connection_by_pasv_port_locked(ctx, local_port);
    dmosi_mutex_unlock(ctx->mutex);

    if (c == NULL)
    {
        /* No session is waiting on this port (e.g. it already got a
         * different data connection, or its control connection ended) -
         * a stray connect we have nothing to do with. */
        dmtcp_abort(conn);
        return;
    }

    ftpd_server_stop_pasv(c); /* One-shot - the ephemeral port is spent now that it's used. */

    c->data_conn = conn;
    c->transfer_ok = false;

    dmtcp_conn_callbacks_t callbacks;
    fill_data_callbacks(&callbacks);
    dmtcp_conn_set_callbacks(conn, &callbacks, c);

    ftpd_data_begin(c);
}

int ftpd_server_connect_port(ftpd_connection_t* c)
{
    if (c == NULL || !c->port_pending)
        return -EINVAL;

    dmtcp_conn_callbacks_t callbacks;
    fill_data_callbacks(&callbacks);
    callbacks.on_established = ftpd_data_on_established;

    dmtcp_conn_t conn;
    int ret = dmtcp_connect(&c->port_addr, c->port_port, (uint16_t)FTPD_ACTIVE_SRC_PORT, &callbacks, c, &conn);
    if (ret != 0)
        return ret;

    c->port_pending = false;
    c->data_conn = conn;
    c->transfer_ok = false;
    return 0;
}

int ftpd_server_start_pasv(ftpd_connection_t* c, uint16_t* out_port)
{
    if (c == NULL || out_port == NULL || g_ftpd_context == NULL)
        return -EINVAL;

    ftpd_server_stop_pasv(c);

    uint16_t port;
    int ret = dmtcp_listen_any(pasv_on_accept, &port);
    if (ret != 0)
        return ret;

    dmosi_mutex_lock(g_ftpd_context->mutex);
    c->pasv_pending = true;
    c->pasv_port = port;
    dmosi_mutex_unlock(g_ftpd_context->mutex);

    *out_port = port;
    return 0;
}

void ftpd_server_stop_pasv(ftpd_connection_t* c)
{
    if (c == NULL || !c->pasv_pending)
        return;

    dmtcp_unlisten(c->pasv_port);

    if (g_ftpd_context != NULL)
        dmosi_mutex_lock(g_ftpd_context->mutex);
    c->pasv_pending = false;
    c->pasv_port = 0;
    if (g_ftpd_context != NULL)
        dmosi_mutex_unlock(g_ftpd_context->mutex);
}

/* ---- Control connection ---- */

void ftpd_handle_engine_send(libftp_t engine, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)engine;
    ftpd_connection_t* c = user_data;
    if (c->control_conn != NULL)
    {
        /* Best-effort - see this file's top comment. */
        dmtcp_send(c->control_conn, data, data_len);
    }
}

static void control_tcp_on_data(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data)
{
    ftpd_connection_t* c = user_data;
    if (data == NULL)
    {
        /* Peer's FIN (dmtcp_data_handler_t's read()-returns-0 convention) -
         * a client that disconnects without QUIT. Close our side too;
         * the real cleanup happens once the terminal callback fires. */
        dmtcp_close(conn);
        return;
    }
    libftp_recv(c->engine, data, data_len);
}

static void control_tcp_on_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    DMOD_LOG_INFO("ftpd: <%p> control connection closed\n", user_data);
    ftpd_connection_release((ftpd_connection_t*)user_data);
}

static void control_tcp_on_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    DMOD_LOG_INFO("ftpd: <%p> control connection reset\n", user_data);
    ftpd_connection_release((ftpd_connection_t*)user_data);
}

static void control_tcp_on_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn;
    DMOD_LOG_INFO("ftpd: <%p> control connection error %d\n", user_data, error);
    ftpd_connection_release((ftpd_connection_t*)user_data);
}

static void control_on_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)iface;

    if (peer != NULL && peer->family == dmip_family_v4)
    {
        DMOD_LOG_INFO("ftpd: accepted control connection from %u.%u.%u.%u:%u\n",
            (unsigned)peer->addr.v4[0], (unsigned)peer->addr.v4[1],
            (unsigned)peer->addr.v4[2], (unsigned)peer->addr.v4[3], (unsigned)peer_port);
    }

    struct ftpd_context* ctx = g_ftpd_context;
    if (ctx == NULL)
    {
        dmtcp_abort(conn);
        return;
    }

    dmosi_mutex_lock(ctx->mutex);
    ftpd_connection_t* c = find_free_slot_locked(ctx);
    if (c != NULL)
    {
        memset(c, 0, sizeof(*c));
        c->in_use = true;
    }
    dmosi_mutex_unlock(ctx->mutex);

    if (c == NULL)
    {
        DMOD_LOG_WARN("ftpd: too many connections (max %u), rejecting\n", (unsigned)FTPD_MAX_CONNECTIONS);
        dmtcp_abort(conn);
        return;
    }

    c->cwd = Dmod_StrDup("/");
    /* Field-by-field, not a compound literal: see this file's top comment -
     * a struct literal whose fields are function addresses gets compiled
     * as a constant-data template that the loader's relocator does not
     * patch for a position-independent .dmf, so every function pointer in
     * it silently ends up as its unrelocated link-time offset instead of
     * a real runtime address. Individual assignments avoid that codegen
     * shape entirely. */
    libftp_callbacks_t engine_callbacks;
    engine_callbacks.on_command = ftpd_handle_command;
    engine_callbacks.on_send    = ftpd_handle_engine_send;
    c->engine = libftp_create(&engine_callbacks, c);
    if (c->cwd == NULL || c->engine == NULL)
    {
        Dmod_Free(c->cwd);
        c->cwd = NULL;
        if (c->engine != NULL) { libftp_destroy(c->engine); c->engine = NULL; }
        c->in_use = false;
        dmtcp_abort(conn);
        return;
    }

    c->control_conn = conn;
    c->binary_mode = true;

    /* Field-by-field - see the compound-literal warning above. */
    dmtcp_conn_callbacks_t tcp_callbacks;
    memset(&tcp_callbacks, 0, sizeof(tcp_callbacks));
    tcp_callbacks.on_data   = control_tcp_on_data;
    tcp_callbacks.on_closed = control_tcp_on_closed;
    tcp_callbacks.on_reset  = control_tcp_on_reset;
    tcp_callbacks.on_error  = control_tcp_on_error;
    dmtcp_conn_set_callbacks(conn, &tcp_callbacks, c);

    libftp_reply(c->engine, 220, "ftpd ready");
}

/**
 * Sets up this process's one server instance and starts listening -
 * called once from main() with the settings it parsed out of argv. Never
 * torn back down: an Application module has no dmod_deinit() equivalent
 * to call it from - see this file's top comment.
 *
 * @return 0 on success, -ENOMEM on allocation failure, -EADDRINUSE (or
 *         another negative dmtcp_listen() error) if `port` is already in
 *         use
 */
int ftpd_server_start(uint16_t port, const char* root, const char* user, const char* pass)
{
    struct ftpd_context* ctx = Dmod_Malloc(sizeof(*ctx));
    if (ctx == NULL)
        return -ENOMEM;
    memset(ctx, 0, sizeof(*ctx));

    ctx->control_port = port;
    ctx->root = normalize_root(root);
    ctx->user = Dmod_StrDup(user);
    ctx->pass = Dmod_StrDup(pass);
    if (ctx->root == NULL || ctx->user == NULL || ctx->pass == NULL)
    {
        Dmod_Free(ctx->root);
        Dmod_Free(ctx->user);
        Dmod_Free(ctx->pass);
        Dmod_Free(ctx);
        return -ENOMEM;
    }

    ctx->mutex = dmosi_mutex_create(false);
    if (ctx->mutex == NULL)
    {
        Dmod_Free(ctx->root);
        Dmod_Free(ctx->user);
        Dmod_Free(ctx->pass);
        Dmod_Free(ctx);
        return -ENOMEM;
    }

    int ret = dmtcp_listen(ctx->control_port, control_on_accept);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("ftpd: failed to listen on TCP port %u (error %d)\n", (unsigned)ctx->control_port, ret);
        dmosi_mutex_destroy(ctx->mutex);
        Dmod_Free(ctx->root);
        Dmod_Free(ctx->user);
        Dmod_Free(ctx->pass);
        Dmod_Free(ctx);
        return ret;
    }

    g_ftpd_context = ctx;
    DMOD_LOG_INFO("ftpd: listening on TCP port %u, root=\"%s\"\n", (unsigned)ctx->control_port, ctx->root);
    return 0;
}
