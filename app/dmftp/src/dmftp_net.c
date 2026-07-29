/**
 * @file dmftp_net.c
 * @brief Every dmtcp interaction that passes a function pointer, plus the
 *        listener registry that gives an accepted connection an owner
 *
 * @par Why all of it is in one file
 * dmtcp's own docs (docs/dmtcp.md, "A loader constraint") record that this
 * loader mis-resolves a callback whose address is taken in one .c file and
 * handed to another module's registration API from a different .c file of
 * the same module - the call ends up at an unrelocated address. So every
 * dmtcp_listen()/_listen_any()/_connect()/_conn_set_callbacks() in dmftp
 * is issued here, right next to the handlers being registered, and the
 * rest of the module goes through the dmftp_net_*() helpers instead.
 *
 * @par Why there is a listener registry
 * dmtcp_accept_handler_t carries no user_data - a listener is identified
 * only by the port it reserved. dmftp needs three different owners behind
 * an accepted connection (a server's control port, a session's PASV data
 * port, a client's active-mode data port), so one table maps local port ->
 * {kind, owner} and the single accept handler below dispatches on it.
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

/** @brief One reserved port and who owns it */
struct dmftp_listener
{
    uint16_t            port;
    dmftp_listen_kind_t kind;
    void*               owner;
};

static dmlist_context_t* g_listeners = NULL;

int dmftp_net_init(void)
{
    g_listeners = dmlist_create(DMFTP_ALLOCATOR_NAME);
    return g_listeners != NULL ? 0 : -ENOMEM;
}

void dmftp_net_deinit(void)
{
    if (g_listeners == NULL)
        return;

    /* Whoever still holds a port at module teardown is gone by now, so
     * release it at the dmtcp level too rather than leaving a listener
     * pointing at freed memory. */
    struct dmftp_listener* entry;
    while ((entry = dmlist_pop_front(g_listeners)) != NULL)
    {
        dmtcp_unlisten(entry->port);
        Dmod_Free(entry);
    }

    dmlist_destroy(g_listeners);
    g_listeners = NULL;
}

static int compare_listener_port(const void* data, const void* user_data)
{
    const struct dmftp_listener* entry = (const struct dmftp_listener*)data;
    uint16_t port = *(const uint16_t*)user_data;
    return entry->port == port ? 0 : -1;
}

static struct dmftp_listener* find_listener(uint16_t port)
{
    if (g_listeners == NULL)
        return NULL;

    return (struct dmftp_listener*)dmlist_find(g_listeners, &port, compare_listener_port);
}

/* ============================================================================
 *                      Data-connection callbacks
 * ========================================================================== */

/**
 * @brief Resolve the transfer a data connection belongs to
 *
 * The registered owner of a data connection is the session or client, not
 * the transfer: the connection often exists before the command that starts
 * the transfer does (clients open a PASV connection the instant they get
 * the 227, well before sending RETR), so the transfer is looked up at
 * callback time rather than captured at registration time.
 */
static struct dmftp_xfer* data_xfer(dmftp_role_t role, void* owner)
{
    if (owner == NULL)
        return NULL;

    if (role == dmftp_role_server_data)
    {
        struct dmftp_session* session = (struct dmftp_session*)owner;
        return session->magic == DMFTP_SESSION_MAGIC ? session->xfer : NULL;
    }

    struct dmftp_client* client = (struct dmftp_client*)owner;
    return client->magic == DMFTP_CLIENT_MAGIC ? client->xfer : NULL;
}

static void data_on_established(dmtcp_conn_t conn, void* user_data, dmftp_role_t role)
{
    dmftp_lock();
    if (role == dmftp_role_server_data)
    {
        dmftp_server_on_data_established((struct dmftp_session*)user_data, conn);
    }
    else
    {
        dmftp_client_on_data_established((struct dmftp_client*)user_data, conn);
    }
    dmftp_unlock();
}

static void data_on_data(dmtcp_conn_t conn, const uint8_t* data, size_t len, void* user_data, dmftp_role_t role)
{
    (void)conn;
    dmftp_lock();
    struct dmftp_xfer* xfer = data_xfer(role, user_data);
    if (xfer != NULL)
    {
        if (data == NULL)
        {
            /* The peer's FIN: for an upload that is the only end-of-file
             * marker FTP has (RFC 959 §3.4 stream mode). */
            dmftp_xfer_finish(xfer, 0);
        }
        else
        {
            dmftp_xfer_receive(xfer, data, len);
        }
    }
    dmftp_unlock();
}

static void data_on_writable(dmtcp_conn_t conn, size_t space, void* user_data, dmftp_role_t role)
{
    (void)conn;
    (void)space;
    dmftp_lock();
    struct dmftp_xfer* xfer = data_xfer(role, user_data);
    if (xfer != NULL)
    {
        dmftp_xfer_pump(xfer);
    }
    dmftp_unlock();
}

static void data_on_terminal(void* user_data, dmftp_role_t role, int error)
{
    dmftp_lock();
    struct dmftp_xfer* xfer = data_xfer(role, user_data);
    if (xfer != NULL)
    {
        /* The TCB is being freed right now, so the transfer must not keep
         * using the handle - dmftp_xfer_finish() drops it. */
        xfer->conn = NULL;
        dmftp_xfer_finish(xfer, error);
    }
    dmftp_unlock();
}

/* ---- Server data connection ---- */

static void server_data_established(dmtcp_conn_t conn, void* user_data)
{
    data_on_established(conn, user_data, dmftp_role_server_data);
}

static void server_data_data(dmtcp_conn_t conn, const uint8_t* data, size_t len, void* user_data)
{
    data_on_data(conn, data, len, user_data, dmftp_role_server_data);
}

static void server_data_writable(dmtcp_conn_t conn, size_t space, void* user_data)
{
    data_on_writable(conn, space, user_data, dmftp_role_server_data);
}

static void server_data_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    data_on_terminal(user_data, dmftp_role_server_data, 0);
}

static void server_data_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    data_on_terminal(user_data, dmftp_role_server_data, -ECONNRESET);
}

static void server_data_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn;
    data_on_terminal(user_data, dmftp_role_server_data, error);
}

/* ---- Client data connection ---- */

static void client_data_established(dmtcp_conn_t conn, void* user_data)
{
    data_on_established(conn, user_data, dmftp_role_client_data);
}

static void client_data_data(dmtcp_conn_t conn, const uint8_t* data, size_t len, void* user_data)
{
    data_on_data(conn, data, len, user_data, dmftp_role_client_data);
}

static void client_data_writable(dmtcp_conn_t conn, size_t space, void* user_data)
{
    data_on_writable(conn, space, user_data, dmftp_role_client_data);
}

static void client_data_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    data_on_terminal(user_data, dmftp_role_client_data, 0);
}

static void client_data_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    data_on_terminal(user_data, dmftp_role_client_data, -ECONNRESET);
}

static void client_data_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn;
    data_on_terminal(user_data, dmftp_role_client_data, error);
}

/* ============================================================================
 *                      Control-connection callbacks
 * ========================================================================== */

static void server_control_data(dmtcp_conn_t conn, const uint8_t* data, size_t len, void* user_data)
{
    (void)conn;
    struct dmftp_session* session = (struct dmftp_session*)user_data;

    dmftp_lock();
    if (session != NULL && session->magic == DMFTP_SESSION_MAGIC)
    {
        if (data == NULL)
        {
            /* The client half-closed its control connection: nothing more
             * will ever be commanded, so finish the session rather than
             * hold a half-open TCB open indefinitely. */
            dmftp_server_on_control_closed(session);
        }
        else
        {
            dmftp_server_on_control_data(session, data, len);
        }
    }
    dmftp_unlock();
}

static void server_control_writable(dmtcp_conn_t conn, size_t space, void* user_data)
{
    (void)space;
    struct dmftp_session* session = (struct dmftp_session*)user_data;

    dmftp_lock();
    if (session != NULL && session->magic == DMFTP_SESSION_MAGIC)
    {
        dmftp_net_flush(conn, &session->out);
    }
    dmftp_unlock();
}

static void server_control_terminal(void* user_data)
{
    struct dmftp_session* session = (struct dmftp_session*)user_data;

    dmftp_lock();
    if (session != NULL && session->magic == DMFTP_SESSION_MAGIC)
    {
        /* dmtcp is about to free the TCB - drop it before anything can try
         * to send a farewell reply down a dead handle. */
        session->control = NULL;
        dmftp_server_session_close(session);
    }
    dmftp_unlock();
}

static void server_control_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    server_control_terminal(user_data);
}

static void server_control_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    server_control_terminal(user_data);
}

static void server_control_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn;
    (void)error;
    server_control_terminal(user_data);
}

static void client_control_established(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    struct dmftp_client* client = (struct dmftp_client*)user_data;

    dmftp_lock();
    if (client != NULL && client->magic == DMFTP_CLIENT_MAGIC)
    {
        dmftp_client_on_established(client);
    }
    dmftp_unlock();
}

static void client_control_data(dmtcp_conn_t conn, const uint8_t* data, size_t len, void* user_data)
{
    (void)conn;
    struct dmftp_client* client = (struct dmftp_client*)user_data;

    dmftp_lock();
    if (client != NULL && client->magic == DMFTP_CLIENT_MAGIC)
    {
        if (data == NULL)
        {
            dmftp_client_on_control_closed(client, 0);
        }
        else
        {
            dmftp_client_on_control_data(client, data, len);
        }
    }
    dmftp_unlock();
}

static void client_control_writable(dmtcp_conn_t conn, size_t space, void* user_data)
{
    (void)space;
    struct dmftp_client* client = (struct dmftp_client*)user_data;

    dmftp_lock();
    if (client != NULL && client->magic == DMFTP_CLIENT_MAGIC)
    {
        dmftp_net_flush(conn, &client->out);
    }
    dmftp_unlock();
}

static void client_control_terminal(void* user_data, int error)
{
    struct dmftp_client* client = (struct dmftp_client*)user_data;

    dmftp_lock();
    if (client != NULL && client->magic == DMFTP_CLIENT_MAGIC)
    {
        client->control = NULL;
        dmftp_client_on_control_closed(client, error);
    }
    dmftp_unlock();
}

static void client_control_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    client_control_terminal(user_data, 0);
}

static void client_control_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    client_control_terminal(user_data, -ECONNRESET);
}

static void client_control_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn;
    client_control_terminal(user_data, error);
}

/**
 * @brief Fill in the callback set for `role`
 *
 * One switch instead of four near-identical dmtcp_conn_callbacks_t
 * literals scattered through this file.
 */
static void callbacks_for_role(dmftp_role_t role, dmtcp_conn_callbacks_t* out)
{
    memset(out, 0, sizeof(*out));

    switch (role)
    {
        case dmftp_role_server_control:
            out->on_data = server_control_data;
            out->on_writable = server_control_writable;
            out->on_closed = server_control_closed;
            out->on_reset = server_control_reset;
            out->on_error = server_control_error;
            break;

        case dmftp_role_server_data:
            out->on_established = server_data_established;
            out->on_data = server_data_data;
            out->on_writable = server_data_writable;
            out->on_closed = server_data_closed;
            out->on_reset = server_data_reset;
            out->on_error = server_data_error;
            break;

        case dmftp_role_client_control:
            out->on_established = client_control_established;
            out->on_data = client_control_data;
            out->on_writable = client_control_writable;
            out->on_closed = client_control_closed;
            out->on_reset = client_control_reset;
            out->on_error = client_control_error;
            break;

        case dmftp_role_client_data:
        default:
            out->on_established = client_data_established;
            out->on_data = client_data_data;
            out->on_writable = client_data_writable;
            out->on_closed = client_data_closed;
            out->on_reset = client_data_reset;
            out->on_error = client_data_error;
            break;
    }
}

int dmftp_net_attach(dmtcp_conn_t conn, dmftp_role_t role, void* owner)
{
    dmtcp_conn_callbacks_t callbacks;
    callbacks_for_role(role, &callbacks);
    return dmtcp_conn_set_callbacks(conn, &callbacks, owner);
}

void dmftp_net_detach(dmtcp_conn_t conn)
{
    if (conn == NULL)
        return;

    /* dmtcp_close() is graceful: the TCB outlives this call by a FIN
     * exchange and then a TIME_WAIT, and dmtcp fires on_closed at the end of
     * that - by which time the session or client that was this connection's
     * user_data has long been freed. Clearing the callbacks first is what
     * makes closing a connection and freeing its owner in the same breath
     * safe; without it the terminal callback reads a dangling pointer and
     * the magic check is left guessing at recycled heap. */
    dmtcp_conn_callbacks_t none;
    memset(&none, 0, sizeof(none));
    dmtcp_conn_set_callbacks(conn, &none, NULL);
}

/* ============================================================================
 *                      Accepting connections
 * ========================================================================== */

/**
 * @brief The module's single dmtcp accept handler
 *
 * Runs inline on the interface's thread. Callbacks are installed here,
 * synchronously, before returning - dmtcp explicitly does not hold back
 * segments that arrive for a freshly accepted connection, so a client that
 * connects and immediately sends would otherwise lose those bytes.
 */
static void dmftp_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    dmip_addr_t local_addr;
    uint16_t    local_port = 0;
    if (dmtcp_conn_get_local_endpoint(conn, &local_addr, &local_port) != 0)
    {
        dmtcp_abort(conn);
        return;
    }

    dmftp_lock();

    struct dmftp_listener* listener = find_listener(local_port);
    if (listener == NULL)
    {
        /* The port was released between the SYN and this callback - refuse
         * rather than leave an unowned connection alive. */
        dmftp_unlock();
        dmtcp_abort(conn);
        return;
    }

    switch (listener->kind)
    {
        case dmftp_listen_server_control:
            dmftp_server_on_accept((struct dmftp_server*)listener->owner, conn, peer, peer_port, iface);
            break;

        case dmftp_listen_server_data:
            dmftp_net_attach(conn, dmftp_role_server_data, listener->owner);
            dmftp_server_on_data_accept((struct dmftp_session*)listener->owner, conn);
            break;

        case dmftp_listen_client_data:
        default:
            dmftp_net_attach(conn, dmftp_role_client_data, listener->owner);
            dmftp_client_on_data_accept((struct dmftp_client*)listener->owner, conn);
            break;
    }

    dmftp_unlock();
}

/**
 * @brief Record a successful reservation, undoing it if the table cannot
 *        take it
 */
static int remember_listener(uint16_t port, dmftp_listen_kind_t kind, void* owner)
{
    struct dmftp_listener* entry = Dmod_Malloc(sizeof(*entry));
    if (entry == NULL)
    {
        dmtcp_unlisten(port);
        return -ENOMEM;
    }

    entry->port = port;
    entry->kind = kind;
    entry->owner = owner;

    if (!dmlist_push_back(g_listeners, entry))
    {
        Dmod_Free(entry);
        dmtcp_unlisten(port);
        return -ENOMEM;
    }
    return 0;
}

int dmftp_net_listen(uint16_t port, dmftp_listen_kind_t kind, void* owner)
{
    if (g_listeners == NULL)
        return -EINVAL;

    int result = dmtcp_listen(port, dmftp_accept);
    if (result != 0)
        return result;

    return remember_listener(port, kind, owner);
}

int dmftp_net_listen_any(dmftp_listen_kind_t kind, void* owner, uint16_t* out_port)
{
    if (g_listeners == NULL || out_port == NULL)
        return -EINVAL;

    uint16_t port = 0;
    int result = dmtcp_listen_any(dmftp_accept, &port);
    if (result != 0)
        return result;

    result = remember_listener(port, kind, owner);
    if (result != 0)
        return result;

    *out_port = port;
    return 0;
}

void dmftp_net_unlisten(uint16_t port)
{
    struct dmftp_listener* entry = find_listener(port);
    if (entry != NULL)
    {
        dmlist_remove(g_listeners, entry, NULL);
        Dmod_Free(entry);
    }
    dmtcp_unlisten(port);
}

void dmftp_net_unlisten_owner(void* owner)
{
    if (g_listeners == NULL)
        return;

    /* Indexed rather than dmlist_foreach()'d: the body removes entries, and
     * mutating a list from inside its own iterator is not something dmlist
     * promises to survive. */
    size_t index = 0;
    while (index < dmlist_size(g_listeners))
    {
        struct dmftp_listener* entry = (struct dmftp_listener*)dmlist_get(g_listeners, index);
        if (entry != NULL && entry->owner == owner)
        {
            uint16_t port = entry->port;
            dmlist_remove_at(g_listeners, index);
            Dmod_Free(entry);
            dmtcp_unlisten(port);
            continue; /* the list shifted down - re-check this index */
        }
        index++;
    }
}

int dmftp_net_connect(const dmip_addr_t* dst, uint16_t dst_port, uint16_t src_port,
                       dmftp_role_t role, void* owner, dmtcp_conn_t* out_conn)
{
    dmtcp_conn_callbacks_t callbacks;
    callbacks_for_role(role, &callbacks);
    return dmtcp_connect(dst, dst_port, src_port, &callbacks, owner, out_conn);
}

/* ============================================================================
 *                      Buffered sending
 * ========================================================================== */

int dmftp_net_flush(dmtcp_conn_t conn, dmftp_buf_t* out)
{
    if (conn == NULL || out == NULL)
        return -EINVAL;

    while (out->len > 0)
    {
        int sent = dmtcp_send(conn, out->data, out->len);
        if (sent < 0)
            return sent;

        size_t taken = (size_t)sent;
        bool   short_write = taken < out->len;
        dmftp_buf_consume(out, taken);

        /* dmtcp armed its edge-triggered on_writable latch on that short
         * write, so the rest goes out from the writable callback rather
         * than from a spin here. */
        if (short_write)
            break;
    }
    return 0;
}

int dmftp_net_send(dmtcp_conn_t conn, dmftp_buf_t* out, const void* data, size_t len)
{
    if (out == NULL)
        return -EINVAL;

    int result = dmftp_buf_append(out, data, len);
    if (result != 0)
        return result;

    if (conn == NULL)
        return 0; /* the connection is gone; the bytes die with the buffer */

    return dmftp_net_flush(conn, out);
}
