/**
 * @file dmftp_server.c
 * @brief Server and session lifecycle, the control-channel line assembler,
 *        and the public server/session API
 *
 * The command implementations live next door (dmftp_server_cmd.c for
 * everything that needs no data connection, dmftp_server_xfer.c for
 * everything that does); this file owns the objects those commands act on.
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

#define DMFTP_DEFAULT_BANNER "dmftp ready"

/* ============================================================================
 *                      Replies
 * ========================================================================== */

void dmftp_server_reply_raw(struct dmftp_session* session, const char* line)
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC || line == NULL)
        return;

    dmftp_net_send(session->control, &session->out, line, strlen(line));
    dmftp_net_send(session->control, &session->out, "\r\n", 2u);
}

void dmftp_server_reply(struct dmftp_session* session, int code, const char* text)
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC)
        return;

    /* A reply is a three-digit code, a space, the text and CRLF. The text
     * is bounded by what this module itself writes, except for the handful
     * of replies that echo a path back - those go through the heap join
     * below rather than a fixed buffer. */
    size_t needed = 3u + 1u + (text != NULL ? strlen(text) : 0u) + 2u;
    char*  buffer = Dmod_Malloc(needed);
    if (buffer == NULL)
        return;

    size_t written = 0;
    if (dmftp_format_reply(buffer, needed, code, text != NULL ? text : "", &written) == 0)
    {
        dmftp_net_send(session->control, &session->out, buffer, written);
    }
    Dmod_Free(buffer);
}

/* ============================================================================
 *                      Session lifecycle
 * ========================================================================== */

static int compare_pointer(const void* data, const void* user_data)
{
    return data == user_data ? 0 : -1;
}

void dmftp_server_reset_data(struct dmftp_session* session)
{
    if (session->pasv_port != 0)
    {
        dmftp_net_unlisten(session->pasv_port);
        session->pasv_port = 0;
    }
    if (session->pending_data_conn != NULL)
    {
        dmftp_net_detach(session->pending_data_conn);
        dmtcp_abort(session->pending_data_conn);
        session->pending_data_conn = NULL;
    }
    if (session->xfer != NULL)
    {
        struct dmftp_xfer* xfer = session->xfer;
        session->xfer = NULL; /* first, so the completion hook is a no-op */
        dmftp_xfer_finish(xfer, -ECONNABORTED);
        dmftp_xfer_destroy(xfer);
    }
    session->rest_offset = 0;
}

static void session_free(struct dmftp_session* session)
{
    dmftp_buf_free(&session->out);
    dmftp_buf_free(&session->in);
    Dmod_Free(session->user);
    Dmod_Free(session->root);
    Dmod_Free(session->cwd);
    Dmod_Free(session->rename_from);
    session->magic = 0;
    Dmod_Free(session);
}

void dmftp_server_session_close(struct dmftp_session* session)
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC || session->closing)
        return;

    session->closing = true;

    dmftp_server_reset_data(session);
    dmftp_net_unlisten_owner(session);

    struct dmftp_server* server = session->server;
    if (server != NULL && server->sessions != NULL)
    {
        dmlist_remove(server->sessions, session, compare_pointer);
    }

    if (server != NULL && server->callbacks.on_session_close != NULL)
    {
        server->callbacks.on_session_close(session, server->user_data);
    }

    if (session->control != NULL)
    {
        dmtcp_conn_t control = session->control;
        session->control = NULL;
        dmftp_net_detach(control); /* before the free below - see dmftp_net_detach() */
        dmtcp_close(control);
    }

    session_free(session);
}

dmod_dmftp_api_declaration(1.0, void, _session_close, ( dmftp_session_t session ))
{
    dmftp_lock();
    if (session != NULL && session->magic == DMFTP_SESSION_MAGIC && !session->closing)
    {
        dmftp_server_reply(session, 421, "Service not available, closing control connection.");
        dmftp_server_session_close(session);
    }
    dmftp_unlock();
}

/**
 * @brief Build the session object for a freshly accepted control connection
 *
 * @return The session, or NULL on allocation failure (nothing left behind)
 */
static struct dmftp_session* session_create(struct dmftp_server* server, dmtcp_conn_t conn,
                                             const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    struct dmftp_session* session = Dmod_Malloc(sizeof(*session));
    if (session == NULL)
        return NULL;

    memset(session, 0, sizeof(*session));
    session->magic = DMFTP_SESSION_MAGIC;
    session->server = server;
    session->control = conn;
    session->peer_addr = *peer;
    session->peer_port = peer_port;
    session->iface = iface;
    session->read_only = server->read_only;
    session->type = dmftp_type_image;
    session->data_mode = dmftp_data_passive;
    dmftp_buf_init(&session->out);
    dmftp_buf_init(&session->in);

    session->root = dmftp_str_ndup(server->root, strlen(server->root));
    session->cwd = dmftp_str_ndup("/", 1u);

    uint16_t local_port = 0;
    (void)dmtcp_conn_get_local_endpoint(conn, &session->local_addr, &local_port);

    if (session->root == NULL || session->cwd == NULL)
    {
        session_free(session);
        return NULL;
    }
    return session;
}

void dmftp_server_on_accept(struct dmftp_server* server, dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    if (server == NULL || server->magic != DMFTP_SERVER_MAGIC || !server->running)
    {
        dmtcp_abort(conn);
        return;
    }

    if (server->max_sessions != 0 && dmlist_size(server->sessions) >= server->max_sessions)
    {
        /* RFC 959's "421 too many users" - answered on the connection and
         * then dropped, rather than silently refused, so the client gets a
         * diagnosable reason. */
        dmtcp_send(conn, "421 Too many connections.\r\n", 27u);
        dmtcp_close(conn);
        return;
    }

    struct dmftp_session* session = session_create(server, conn, peer, peer_port, iface);
    if (session == NULL)
    {
        dmtcp_abort(conn);
        return;
    }

    if (!dmlist_push_back(server->sessions, session))
    {
        session_free(session);
        dmtcp_abort(conn);
        return;
    }

    dmftp_net_attach(conn, dmftp_role_server_control, session);

    if (server->callbacks.on_session_open != NULL)
    {
        server->callbacks.on_session_open(session, server->user_data);
    }

    dmftp_server_reply(session, 220, server->banner);
}

/* ============================================================================
 *                      Control line assembly
 * ========================================================================== */

/**
 * @brief Hand one complete line (CRLF already removed) to the dispatcher
 */
static void deliver_line(struct dmftp_session* session, const char* line, size_t len)
{
    if (len == 0)
        return; /* a bare CRLF is not a command - ignore it silently */

    dmftp_server_dispatch(session, line, len);
}

void dmftp_server_on_control_data(struct dmftp_session* session, const uint8_t* data, size_t len)
{
    if (session->closing)
        return;

    if (dmftp_buf_append(&session->in, data, len) != 0)
    {
        dmftp_server_reply(session, 421, "Out of memory, closing connection.");
        dmftp_server_session_close(session);
        return;
    }

    for (;;)
    {
        size_t text_len = 0;
        char*  line = dmftp_buf_take_line(&session->in, &text_len);
        if (line == NULL)
        {
            /* Nothing complete yet. A peer that never sends a newline is the
             * one way it could drive this module's heap use, so cap the
             * accumulator instead of growing forever. */
            if (session->in.len > DMFTP_LINE_MAX)
            {
                session->in.len = 0;
                dmftp_server_reply(session, 500, "Command line too long.");
            }
            return;
        }

        deliver_line(session, line, text_len);
        Dmod_Free(line);

        if (session->closing)
            return; /* the command closed the session - stop touching it */
    }
}

void dmftp_server_on_control_closed(struct dmftp_session* session)
{
    dmftp_server_session_close(session);
}

/* ============================================================================
 *                      Data connection handoff
 * ========================================================================== */

void dmftp_server_on_data_accept(struct dmftp_session* session, dmtcp_conn_t conn)
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC || session->closing)
    {
        dmtcp_abort(conn);
        return;
    }

    /* One PASV reservation serves exactly one connection (RFC 959 §3.2). */
    if (session->pasv_port != 0)
    {
        dmftp_net_unlisten(session->pasv_port);
        session->pasv_port = 0;
    }

    if (session->xfer != NULL)
    {
        dmftp_xfer_attach(session->xfer, conn);
    }
    else
    {
        session->pending_data_conn = conn; /* RETR/STOR has not arrived yet */
    }
}

void dmftp_server_on_data_established(struct dmftp_session* session, dmtcp_conn_t conn)
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC || session->closing)
    {
        dmtcp_abort(conn);
        return;
    }

    if (session->xfer != NULL)
    {
        dmftp_xfer_attach(session->xfer, conn);
    }
    else
    {
        dmtcp_abort(conn);
    }
}

void dmftp_server_xfer_done(struct dmftp_session* session, int result, uint64_t bytes)
{
    (void)bytes;

    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC || session->xfer == NULL)
        return;

    struct dmftp_xfer* xfer = session->xfer;
    session->xfer = NULL;
    dmftp_xfer_destroy(xfer);

    if (session->closing)
        return;

    if (result == 0)
    {
        dmftp_server_reply(session, 226, "Transfer complete.");
    }
    else if (result == -ECONNABORTED)
    {
        dmftp_server_reply(session, 426, "Transfer aborted.");
    }
    else
    {
        dmftp_server_reply(session, 451, "Transfer failed.");
    }
}

/* ============================================================================
 *                      Public server API
 * ========================================================================== */

static void server_free(struct dmftp_server* server)
{
    if (server->sessions != NULL)
    {
        dmlist_destroy(server->sessions);
    }
    Dmod_Free(server->root);
    Dmod_Free(server->banner);
    server->magic = 0;
    Dmod_Free(server);
}

/**
 * @brief Copy the caller's configuration into the server, applying defaults
 *
 * @return 0 on success, -ENOMEM
 */
static int apply_config(struct dmftp_server* server, const dmftp_server_config_t* config)
{
    const char* root = "/";
    const char* banner = DMFTP_DEFAULT_BANNER;

    server->port = DMFTP_PORT_CONTROL;
    server->allow_passive = true;
    server->allow_active = true;

    if (config != NULL)
    {
        if (config->port != 0)
        {
            server->port = config->port;
        }
        if (config->root != NULL && config->root[0] != '\0')
        {
            root = config->root;
        }
        if (config->banner != NULL)
        {
            banner = config->banner;
        }

        /* Neither flag set means "no preference", which is both modes -
         * a config that disabled both by accident would be a server no
         * client could transfer anything through. */
        if (config->allow_passive || config->allow_active)
        {
            server->allow_passive = config->allow_passive;
            server->allow_active = config->allow_active;
        }

        server->read_only = config->read_only;
        server->active_data_port = config->active_data_port;
        server->max_sessions = config->max_sessions;
    }

    server->root = dmftp_str_ndup(root, strlen(root));
    server->banner = dmftp_str_ndup(banner, strlen(banner));
    return (server->root != NULL && server->banner != NULL) ? 0 : -ENOMEM;
}

dmod_dmftp_api_declaration(1.0, dmftp_server_t, _server_create, ( const dmftp_server_config_t* config, const dmftp_server_callbacks_t* callbacks, void* user_data ))
{
    if (callbacks == NULL || callbacks->on_auth == NULL)
    {
        DMOD_LOG_ERROR("dmftp: a server needs an on_auth callback\n");
        return NULL;
    }

    struct dmftp_server* server = Dmod_Malloc(sizeof(*server));
    if (server == NULL)
        return NULL;

    memset(server, 0, sizeof(*server));
    server->magic = DMFTP_SERVER_MAGIC;
    server->callbacks = *callbacks;
    server->user_data = user_data;

    if (apply_config(server, config) != 0)
    {
        server_free(server);
        return NULL;
    }

    server->sessions = dmlist_create(DMFTP_ALLOCATOR_NAME);
    if (server->sessions == NULL)
    {
        server_free(server);
        return NULL;
    }
    return server;
}

dmod_dmftp_api_declaration(1.0, int, _server_start, ( dmftp_server_t server ))
{
    if (server == NULL || server->magic != DMFTP_SERVER_MAGIC)
        return -EINVAL;

    dmftp_lock();
    int result;
    if (server->running)
    {
        result = -EALREADY;
    }
    else
    {
        result = dmftp_net_listen(server->port, dmftp_listen_server_control, server);
        server->running = (result == 0);
    }
    dmftp_unlock();
    return result;
}

dmod_dmftp_api_declaration(1.0, int, _server_stop, ( dmftp_server_t server ))
{
    if (server == NULL || server->magic != DMFTP_SERVER_MAGIC)
        return -EINVAL;

    dmftp_lock();
    if (server->running)
    {
        dmftp_net_unlisten(server->port);
        server->running = false;
    }

    /* Each close removes the session from this very list, so keep taking
     * the front rather than walking an index across a shrinking list. */
    struct dmftp_session* session;
    while ((session = (struct dmftp_session*)dmlist_front(server->sessions)) != NULL)
    {
        dmftp_server_reply(session, 421, "Service closing control connection.");
        dmftp_server_session_close(session);
    }
    dmftp_unlock();
    return 0;
}

dmod_dmftp_api_declaration(1.0, void, _server_destroy, ( dmftp_server_t server ))
{
    if (server == NULL || server->magic != DMFTP_SERVER_MAGIC)
        return;

    dmftp_server_stop(server);

    dmftp_lock();
    server_free(server);
    dmftp_unlock();
}

dmod_dmftp_api_declaration(1.0, uint16_t, _server_get_port, ( dmftp_server_t server ))
{
    if (server == NULL || server->magic != DMFTP_SERVER_MAGIC)
        return 0;
    return server->port;
}

dmod_dmftp_api_declaration(1.0, size_t, _server_get_session_count, ( dmftp_server_t server ))
{
    if (server == NULL || server->magic != DMFTP_SERVER_MAGIC)
        return 0;

    dmftp_lock();
    size_t count = dmlist_size(server->sessions);
    dmftp_unlock();
    return count;
}

/* ============================================================================
 *                      Public session API
 * ========================================================================== */

dmod_dmftp_api_declaration(1.0, const char*, _session_get_user, ( dmftp_session_t session ))
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC)
        return NULL;
    return session->user;
}

dmod_dmftp_api_declaration(1.0, int, _session_get_peer, ( dmftp_session_t session, dmip_addr_t* out_addr, uint16_t* out_port ))
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC || out_addr == NULL || out_port == NULL)
        return -EINVAL;

    *out_addr = session->peer_addr;
    *out_port = session->peer_port;
    return 0;
}

dmod_dmftp_api_declaration(1.0, int, _session_set_root, ( dmftp_session_t session, const char* root ))
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC || root == NULL)
        return -EINVAL;

    dmftp_lock();

    /* Resolved against the SERVER's root, not the session's current one, so
     * repeated calls cannot walk outward one step at a time - and a `..`
     * inside `root` is clamped by dmftp_path_virtual() exactly as a
     * client's would be. */
    char* new_root = dmftp_path_resolve(session->server->root, "/", root, NULL);
    char* new_cwd = dmftp_str_ndup("/", 1u);
    int   result = -ENOMEM;

    if (new_root != NULL && new_cwd != NULL)
    {
        Dmod_Free(session->root);
        Dmod_Free(session->cwd);
        session->root = new_root;
        session->cwd = new_cwd;
        result = 0;
    }
    else
    {
        Dmod_Free(new_root);
        Dmod_Free(new_cwd);
    }

    dmftp_unlock();
    return result;
}

dmod_dmftp_api_declaration(1.0, int, _session_set_read_only, ( dmftp_session_t session, bool read_only ))
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC)
        return -EINVAL;

    /* A read-only server stays read-only: this can restrict, never widen. */
    session->read_only = read_only || session->server->read_only;
    return 0;
}

dmod_dmftp_api_declaration(1.0, int, _session_set_user_data, ( dmftp_session_t session, void* user_data ))
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC)
        return -EINVAL;

    session->user_data = user_data;
    return 0;
}

dmod_dmftp_api_declaration(1.0, void*, _session_get_user_data, ( dmftp_session_t session ))
{
    if (session == NULL || session->magic != DMFTP_SESSION_MAGIC)
        return NULL;
    return session->user_data;
}
