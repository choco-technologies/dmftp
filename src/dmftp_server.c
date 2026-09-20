/* DMOD_ENABLE_REGISTRATION is deliberately NOT set here - only src/dmftp.c
 * (which defines dmftp_create()/_destroy()/_recv()/_reply() via the
 * dmod_dmftp_api_declaration() macro) sets it, so dmftp.h's per-function
 * Dmod_ApiRegistration_t entries are DEFINED exactly once across this
 * module's three translation units. Without it, including dmftp.h here
 * only pulls in `extern` declarations - see dmod_defs.h's
 * _DMOD_API_REGISTRATION macro and dmdhcp_registrations.c's own doc
 * comment in https://github.com/choco-technologies/dmdhcp for the same
 * multi-file-module reasoning. */
#include "dmod.h"
#include "dmftp_internal.h"
#include "dmini.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

/**
 * dmftp's dmtcp wiring: the control-connection listener (port 21 by
 * default), the connection table, PASV port bookkeeping, and config
 * loading. RFC 959 command handling and data transfer live in
 * src/dmftp_commands.c - see dmftp_internal.h.
 *
 * dmftp is a Library-type DMOD module (see CMakeLists.txt) with no main():
 * like dmicmp (https://github.com/choco-technologies/dmicmp), it does its
 * whole job from dmod_init()/dmod_deinit() once loaded+enabled, which lets
 * it be started/stopped as a dmsystem "type=library" unit - see
 * configs/ftpd.ini and docs/service.md.
 *
 * Known limitations (deliberately out of scope for this first version,
 * same spirit as telnetd.c's own documented limitations):
 *   - Active mode (PORT) is not implemented - only PASV. Most modern
 *     clients default to passive mode anyway; an explicit PORT gets a
 *     plain "502 Command not implemented".
 *   - TYPE A (ASCII) is accepted but never actually translates line
 *     endings - every transfer is effectively binary. Harmless for the
 *     overwhelming majority of clients, which default to TYPE I anyway.
 *   - No REST (resume), APPE (append), or RNFR/RNTO (rename) support.
 *   - dmtcp_send() on the control connection is best-effort, exactly like
 *     telnetd.c documents for its own Telnet traffic - fine for short
 *     reply lines, which never come close to filling the outbound buffer.
 */

#define DMFTP_CONFIG_PATH "/configs/services/dmftp/ftpd.ini"

struct dmftp_context* g_dmftp_context = NULL;

bool dmftp_is_anonymous_user(const char* user)
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
 * dmftp_commands.c's real-path builder can always just concatenate
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

/**
 * Loads this server's own settings from the same unit file dmsystem starts
 * it from (DMFTP_CONFIG_PATH) - see docs/service.md for why one ini file
 * serves both readers. Tolerant of the file being absent (falls back to
 * built-in defaults) so dmod_init() still succeeds in a host test
 * environment with no /configs at all.
 *
 * @return 0 on success, -ENOMEM if a setting could not be duplicated
 */
static int load_config(struct dmftp_context* ctx)
{
    const char* port_str_default = "21";
    (void)port_str_default;

    int port = DMFTP_DEFAULT_PORT;
    const char* root = "/";
    const char* user = "anonymous";
    const char* pass = "";

    dmini_context_t cfg = dmini_create();
    if (cfg != NULL)
    {
        if (dmini_parse_file(cfg, DMFTP_CONFIG_PATH) == DMINI_OK)
        {
            port = dmini_get_int(cfg, NULL, "port", DMFTP_DEFAULT_PORT);
            root = dmini_get_string(cfg, NULL, "root", root);
            user = dmini_get_string(cfg, NULL, "user", user);
            pass = dmini_get_string(cfg, NULL, "pass", pass);
        }

        ctx->control_port = (uint16_t)port;
        ctx->root = normalize_root(root);
        ctx->user = Dmod_StrDup(user);
        ctx->pass = Dmod_StrDup(pass);

        dmini_destroy(cfg);
    }
    else
    {
        ctx->control_port = (uint16_t)port;
        ctx->root = normalize_root(root);
        ctx->user = Dmod_StrDup(user);
        ctx->pass = Dmod_StrDup(pass);
    }

    if (ctx->root == NULL || ctx->user == NULL || ctx->pass == NULL)
        return -ENOMEM;

    return 0;
}

static void free_config(struct dmftp_context* ctx)
{
    Dmod_Free(ctx->root);
    Dmod_Free(ctx->user);
    Dmod_Free(ctx->pass);
    ctx->root = NULL;
    ctx->user = NULL;
    ctx->pass = NULL;
}

static dmftp_connection_t* find_free_slot_locked(struct dmftp_context* ctx)
{
    for (size_t i = 0; i < DMFTP_MAX_CONNECTIONS; i++)
    {
        if (!ctx->connections[i].in_use)
            return &ctx->connections[i];
    }
    return NULL;
}

static dmftp_connection_t* find_connection_by_pasv_port_locked(struct dmftp_context* ctx, uint16_t port)
{
    for (size_t i = 0; i < DMFTP_MAX_CONNECTIONS; i++)
    {
        if (ctx->connections[i].in_use && ctx->connections[i].pasv_pending && ctx->connections[i].pasv_port == port)
            return &ctx->connections[i];
    }
    return NULL;
}

void dmftp_connection_release(dmftp_connection_t* c)
{
    if (c == NULL || !c->in_use)
        return;

    dmftp_server_stop_pasv(c);

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
    c->data_op = dmftp_data_op_none;

    if (c->engine != NULL)
    {
        dmftp_destroy(c->engine);
        c->engine = NULL;
    }
    Dmod_Free(c->username);
    c->username = NULL;
    Dmod_Free(c->cwd);
    c->cwd = NULL;

    c->control_conn = NULL;
    c->in_use = false;
}

/* ---- PASV port reservation - see dmftp_internal.h ---- */

static void pasv_on_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)peer;
    (void)peer_port;
    (void)iface;

    struct dmftp_context* ctx = g_dmftp_context;
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
    dmftp_connection_t* c = find_connection_by_pasv_port_locked(ctx, local_port);
    dmosi_mutex_unlock(ctx->mutex);

    if (c == NULL)
    {
        /* No session is waiting on this port (e.g. it already got a
         * different data connection, or its control connection ended) -
         * a stray connect we have nothing to do with. */
        dmtcp_abort(conn);
        return;
    }

    dmftp_server_stop_pasv(c); /* One-shot - the ephemeral port is spent now that it's used. */

    c->data_conn = conn;
    c->transfer_ok = false;

    dmtcp_conn_callbacks_t callbacks = {
        .on_data     = dmftp_data_on_data,
        .on_writable = dmftp_data_on_writable,
        .on_closed   = dmftp_data_on_closed,
        .on_reset    = dmftp_data_on_reset,
        .on_error    = dmftp_data_on_error,
    };
    dmtcp_conn_set_callbacks(conn, &callbacks, c);

    dmftp_data_begin(c);
}

int dmftp_server_start_pasv(dmftp_connection_t* c, uint16_t* out_port)
{
    if (c == NULL || out_port == NULL || g_dmftp_context == NULL)
        return -EINVAL;

    dmftp_server_stop_pasv(c);

    uint16_t port;
    int ret = dmtcp_listen_any(pasv_on_accept, &port);
    if (ret != 0)
        return ret;

    dmosi_mutex_lock(g_dmftp_context->mutex);
    c->pasv_pending = true;
    c->pasv_port = port;
    dmosi_mutex_unlock(g_dmftp_context->mutex);

    *out_port = port;
    return 0;
}

void dmftp_server_stop_pasv(dmftp_connection_t* c)
{
    if (c == NULL || !c->pasv_pending)
        return;

    dmtcp_unlisten(c->pasv_port);

    if (g_dmftp_context != NULL)
        dmosi_mutex_lock(g_dmftp_context->mutex);
    c->pasv_pending = false;
    c->pasv_port = 0;
    if (g_dmftp_context != NULL)
        dmosi_mutex_unlock(g_dmftp_context->mutex);
}

/* ---- Control connection ---- */

void dmftp_handle_engine_send(dmftp_t engine, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)engine;
    dmftp_connection_t* c = user_data;
    if (c->control_conn != NULL)
    {
        /* Best-effort - see this file's top comment. */
        dmtcp_send(c->control_conn, data, data_len);
    }
}

static void control_tcp_on_data(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data)
{
    dmftp_connection_t* c = user_data;
    if (data == NULL)
    {
        /* Peer's FIN (dmtcp_data_handler_t's read()-returns-0 convention) -
         * a client that disconnects without QUIT. Close our side too;
         * the real cleanup happens once the terminal callback fires. */
        dmtcp_close(conn);
        return;
    }
    dmftp_recv(c->engine, data, data_len);
}

static void control_tcp_on_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    dmftp_connection_release((dmftp_connection_t*)user_data);
}

static void control_tcp_on_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    dmftp_connection_release((dmftp_connection_t*)user_data);
}

static void control_tcp_on_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn;
    (void)error;
    dmftp_connection_release((dmftp_connection_t*)user_data);
}

static void control_on_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)peer;
    (void)peer_port;
    (void)iface;

    struct dmftp_context* ctx = g_dmftp_context;
    if (ctx == NULL)
    {
        dmtcp_abort(conn);
        return;
    }

    dmosi_mutex_lock(ctx->mutex);
    dmftp_connection_t* c = find_free_slot_locked(ctx);
    if (c != NULL)
    {
        memset(c, 0, sizeof(*c));
        c->in_use = true;
    }
    dmosi_mutex_unlock(ctx->mutex);

    if (c == NULL)
    {
        DMOD_LOG_WARN("dmftp: too many connections (max %u), rejecting\n", (unsigned)DMFTP_MAX_CONNECTIONS);
        dmtcp_abort(conn);
        return;
    }

    c->cwd = Dmod_StrDup("/");
    dmftp_callbacks_t engine_callbacks = {
        .on_command = dmftp_handle_command,
        .on_send    = dmftp_handle_engine_send,
    };
    c->engine = dmftp_create(&engine_callbacks, c);
    if (c->cwd == NULL || c->engine == NULL)
    {
        Dmod_Free(c->cwd);
        c->cwd = NULL;
        if (c->engine != NULL) { dmftp_destroy(c->engine); c->engine = NULL; }
        c->in_use = false;
        dmtcp_abort(conn);
        return;
    }

    c->control_conn = conn;
    c->binary_mode = true;

    dmtcp_conn_callbacks_t tcp_callbacks = {
        .on_data   = control_tcp_on_data,
        .on_closed = control_tcp_on_closed,
        .on_reset  = control_tcp_on_reset,
        .on_error  = control_tcp_on_error,
    };
    dmtcp_conn_set_callbacks(conn, &tcp_callbacks, c);

    dmftp_reply(c->engine, 220, "dmftp ready");
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    struct dmftp_context* ctx = Dmod_Malloc(sizeof(*ctx));
    if (ctx == NULL)
        return -1;
    memset(ctx, 0, sizeof(*ctx));

    if (load_config(ctx) != 0)
    {
        DMOD_LOG_ERROR("dmftp: failed to load configuration\n");
        free_config(ctx);
        Dmod_Free(ctx);
        return -1;
    }

    ctx->mutex = dmosi_mutex_create(false);
    if (ctx->mutex == NULL)
    {
        free_config(ctx);
        Dmod_Free(ctx);
        return -1;
    }

    int ret = dmtcp_listen(ctx->control_port, control_on_accept);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("dmftp: failed to listen on TCP port %u (error %d)\n", (unsigned)ctx->control_port, ret);
        dmosi_mutex_destroy(ctx->mutex);
        free_config(ctx);
        Dmod_Free(ctx);
        return -1;
    }

    g_dmftp_context = ctx;
    DMOD_LOG_INFO("dmftp: listening on TCP port %u, root=\"%s\"\n", (unsigned)ctx->control_port, ctx->root);
    return 0;
}

int dmod_deinit(void)
{
    struct dmftp_context* ctx = g_dmftp_context;
    if (ctx == NULL)
        return 0;

    dmtcp_unlisten(ctx->control_port);

    for (size_t i = 0; i < DMFTP_MAX_CONNECTIONS; i++)
    {
        if (ctx->connections[i].in_use)
            dmftp_connection_release(&ctx->connections[i]);
    }

    dmosi_mutex_destroy(ctx->mutex);
    g_dmftp_context = NULL;

    free_config(ctx);
    Dmod_Free(ctx);
    return 0;
}
