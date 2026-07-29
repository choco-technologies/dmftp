/**
 * @file dmftp_client.c
 * @brief The client half: a reply-driven state machine over one control
 *        connection, plus the public dmftp_client_* API
 *
 * FTP's control channel is strictly serial - one command outstanding at a
 * time, each answered before the next goes out - so the client is a plain
 * state machine keyed on "what am I waiting for a reply to", rather than a
 * queue. Multi-step operations (login, a transfer's PASV/RETR pair, RNFR
 * followed by RNTO) are just sequences of those states.
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

#define DMFTP_DEFAULT_USER     "anonymous"
#define DMFTP_DEFAULT_PASSWORD "dmftp@"

/* ============================================================================
 *                      Sending commands
 * ========================================================================== */

/**
 * @brief Write "VERB[ arg]\r\n" onto the control connection
 *
 * @return 0 on success, -ENOMEM
 */
static int send_command(struct dmftp_client* client, const char* verb, const char* arg)
{
    char* line = (arg != NULL && arg[0] != '\0') ? dmftp_str_join(verb, " ", arg)
                                                 : dmftp_str_join(verb, NULL, NULL);
    if (line == NULL)
        return -ENOMEM;

    int result = dmftp_net_send(client->control, &client->out, line, strlen(line));
    if (result == 0)
    {
        result = dmftp_net_send(client->control, &client->out, "\r\n", 2u);
    }
    Dmod_Free(line);
    return result;
}

/**
 * @brief Forget whatever a pending or finished transfer was holding
 */
static void clear_pending(struct dmftp_client* client)
{
    Dmod_Free(client->pending_verb);
    Dmod_Free(client->pending_arg);
    Dmod_Free(client->rename_to);
    client->pending_verb = NULL;
    client->pending_arg = NULL;
    client->rename_to = NULL;

    if (client->active_port != 0)
    {
        dmftp_net_unlisten(client->active_port);
        client->active_port = 0;
    }
}

/* ============================================================================
 *                      Transfer completion
 * ========================================================================== */

/**
 * @brief Fire on_done once both the data and control sides have reported
 *
 * See dmftp_internal.h's note on data_done/control_done for why one is not
 * enough.
 */
static void try_report_transfer(struct dmftp_client* client)
{
    if (client->xfer_reported || !client->data_done || !client->control_done)
        return;

    client->xfer_reported = true;
    clear_pending(client);

    if (client->state != dmftp_cstate_closed && client->state != dmftp_cstate_quit)
    {
        client->state = dmftp_cstate_ready;
    }

    if (client->callbacks.on_done != NULL)
    {
        client->callbacks.on_done(client, client->xfer_result, client->xfer_bytes, client->user_data);
    }
}

void dmftp_client_xfer_done(struct dmftp_client* client, int result, uint64_t bytes)
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC || client->xfer == NULL)
        return;

    struct dmftp_xfer* xfer = client->xfer;
    client->xfer = NULL;
    dmftp_xfer_destroy(xfer);

    client->data_done = true;
    client->xfer_bytes = bytes;
    if (client->xfer_result == 0)
    {
        client->xfer_result = result;
    }
    try_report_transfer(client);
}

void dmftp_client_xfer_line(struct dmftp_client* client, const char* line)
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC)
        return;

    if (client->callbacks.on_list != NULL)
    {
        client->callbacks.on_list(client, line, client->user_data);
    }
}

/* ============================================================================
 *                      Data connection setup
 * ========================================================================== */

/**
 * @brief Reserve a local port and send PORT (active mode)
 *
 * @return 0 on success, otherwise a negative errno
 */
static int begin_active_setup(struct dmftp_client* client)
{
    uint16_t port = 0;
    int result = dmftp_net_listen_any(dmftp_listen_client_data, client, &port);
    if (result != 0)
        return result;

    /* The address the server should dial back is the one our control
     * connection is using - the only local address it is known to reach. */
    dmip_addr_t local_addr;
    uint16_t    local_port = 0;
    if (dmtcp_conn_get_local_endpoint(client->control, &local_addr, &local_port) != 0)
    {
        dmftp_net_unlisten(port);
        return -ENOTCONN;
    }

    char tuple[24];
    if (dmftp_format_host_port(tuple, sizeof(tuple), &local_addr, port, NULL) != 0)
    {
        dmftp_net_unlisten(port);
        return -EINVAL;
    }

    client->active_port = port;
    return send_command(client, "PORT", tuple);
}

/**
 * @brief Kick off a transfer: remember the command, then ask for a data
 *        connection
 *
 * The command itself is not sent yet - it goes out once the data
 * connection has been arranged (after the 227 or the 200), which is the
 * order every server expects.
 *
 * @return 0 on success, otherwise a negative errno
 */
static int begin_transfer(struct dmftp_client* client, const char* verb, const char* arg, struct dmftp_xfer* xfer)
{
    client->pending_verb = dmftp_str_ndup(verb, strlen(verb));
    client->pending_arg = arg != NULL ? dmftp_str_ndup(arg, strlen(arg)) : NULL;
    if (client->pending_verb == NULL)
    {
        dmftp_xfer_destroy(xfer);
        return -ENOMEM;
    }

    client->xfer = xfer;
    client->data_done = false;
    client->control_done = false;
    client->xfer_reported = false;
    client->xfer_result = 0;
    client->xfer_bytes = 0;
    client->state = dmftp_cstate_setup;

    int result = (client->data_mode == dmftp_data_active) ? begin_active_setup(client)
                                                          : send_command(client, "PASV", NULL);
    if (result != 0)
    {
        client->xfer = NULL;
        dmftp_xfer_destroy(xfer);
        clear_pending(client);
        client->state = dmftp_cstate_ready;
    }
    return result;
}

/**
 * @brief Extract the `h1,h2,h3,h4,p1,p2` tuple out of a 227 reply's prose
 *
 * Servers word the sentence around it differently ("Entering Passive Mode
 * (10,0,0,1,200,1)." and friends), so the tuple is found by scanning for
 * the first digit rather than by matching any particular phrasing.
 */
static int parse_pasv_reply(const char* text, dmip_addr_t* out_addr, uint16_t* out_port)
{
    const char* cursor = text;
    while (*cursor != '\0' && (*cursor < '0' || *cursor > '9'))
    {
        cursor++;
    }

    if (*cursor == '\0')
        return -EPROTO;

    return dmftp_parse_host_port(cursor, out_addr, out_port);
}

/**
 * @brief The server accepted the data-connection setup - open it (passive)
 *        and send the transfer command
 */
static void on_data_setup_ready(struct dmftp_client* client, const char* text, bool passive)
{
    if (passive)
    {
        dmip_addr_t addr;
        uint16_t    port = 0;
        if (parse_pasv_reply(text, &addr, &port) != 0)
        {
            client->xfer_result = -EPROTO;
            client->data_done = true;
            client->control_done = true;
            try_report_transfer(client);
            return;
        }

        dmtcp_conn_t conn = NULL;
        int result = dmftp_net_connect(&addr, port, 0, dmftp_role_client_data, client, &conn);
        if (result != 0)
        {
            client->xfer_result = result;
            client->data_done = true;
            client->control_done = true;
            try_report_transfer(client);
            return;
        }
    }

    if (send_command(client, client->pending_verb, client->pending_arg) != 0)
    {
        client->xfer_result = -ENOMEM;
        client->data_done = true;
        client->control_done = true;
        try_report_transfer(client);
        return;
    }
    client->state = dmftp_cstate_xfer_cmd;
}

void dmftp_client_on_data_accept(struct dmftp_client* client, dmtcp_conn_t conn)
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC || client->xfer == NULL)
    {
        dmtcp_abort(conn);
        return;
    }

    /* One PORT authorizes one connection - release the reservation now so a
     * second, unexpected connection cannot land on the same transfer. */
    if (client->active_port != 0)
    {
        dmftp_net_unlisten(client->active_port);
        client->active_port = 0;
    }
    dmftp_xfer_attach(client->xfer, conn);
}

void dmftp_client_on_data_established(struct dmftp_client* client, dmtcp_conn_t conn)
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC || client->xfer == NULL)
    {
        dmtcp_abort(conn);
        return;
    }
    dmftp_xfer_attach(client->xfer, conn);
}

/* ============================================================================
 *                      The reply state machine
 * ========================================================================== */

/** @brief RFC 959 §4.2: the leading digit is the whole verdict */
static bool reply_ok(int code)
{
    return code >= 200 && code < 400;
}

static void finish_login(struct dmftp_client* client)
{
    client->state = dmftp_cstate_ready;
    if (client->callbacks.on_ready != NULL)
    {
        client->callbacks.on_ready(client, client->user_data);
    }
}

/**
 * @brief Drive the connect/login sequence: 220 -> USER -> PASS -> TYPE
 *
 * @return true if the reply was consumed by the login sequence
 */
static bool handle_login_reply(struct dmftp_client* client, int code)
{
    switch (client->state)
    {
        case dmftp_cstate_greeting:
            if (code != 220)
            {
                dmftp_client_on_control_closed(client, -ECONNREFUSED);
                return true;
            }
            client->state = dmftp_cstate_user;
            send_command(client, "USER", client->user);
            return true;

        case dmftp_cstate_user:
            if (code == 230)
            {
                /* Some servers skip the password step entirely for
                 * anonymous logins - honour that instead of sending a PASS
                 * they never asked for. */
                client->state = dmftp_cstate_type;
                send_command(client, "TYPE", client->type == dmftp_type_ascii ? "A" : "I");
                return true;
            }
            if (code != 331)
            {
                dmftp_client_on_control_closed(client, -EACCES);
                return true;
            }
            client->state = dmftp_cstate_pass;
            send_command(client, "PASS", client->password);
            return true;

        case dmftp_cstate_pass:
            if (code != 230)
            {
                dmftp_client_on_control_closed(client, -EACCES);
                return true;
            }
            client->state = dmftp_cstate_type;
            send_command(client, "TYPE", client->type == dmftp_type_ascii ? "A" : "I");
            return true;

        case dmftp_cstate_type:
            /* A server that refuses TYPE still works for everything else,
             * so a rejection here is not worth failing the login over. */
            finish_login(client);
            return true;

        default:
            return false;
    }
}

/**
 * @brief Mark the control side of a transfer as finished with `result`
 */
static void fail_transfer(struct dmftp_client* client, int result)
{
    client->control_done = true;
    if (client->xfer_result == 0)
    {
        client->xfer_result = result;
    }

    if (client->xfer != NULL)
    {
        struct dmftp_xfer* xfer = client->xfer;
        dmftp_xfer_finish(xfer, result); /* clears client->xfer and sets data_done */
    }
    else
    {
        client->data_done = true;
    }
    try_report_transfer(client);
}

/**
 * @brief Drive a transfer's control-channel half
 *
 * @return true if the reply was consumed
 */
static bool handle_transfer_reply(struct dmftp_client* client, int code, const char* text)
{
    switch (client->state)
    {
        case dmftp_cstate_setup:
            if (!reply_ok(code))
            {
                fail_transfer(client, -EPERM);
                return true;
            }
            on_data_setup_ready(client, text, client->data_mode == dmftp_data_passive);
            return true;

        case dmftp_cstate_xfer_cmd:
            if (code >= 100 && code < 200)
            {
                client->state = dmftp_cstate_xfer; /* 150: the data is on its way */
                return true;
            }
            fail_transfer(client, -EPERM);
            return true;

        case dmftp_cstate_xfer:
            if (reply_ok(code))
            {
                client->control_done = true;
                try_report_transfer(client);
            }
            else
            {
                fail_transfer(client, -EPERM);
            }
            return true;

        default:
            return false;
    }
}

/**
 * @brief Everything that is not login and not a transfer
 */
static void handle_simple_reply(struct dmftp_client* client, int code)
{
    switch (client->state)
    {
        case dmftp_cstate_rnfr:
            if (code == 350 && client->rename_to != NULL)
            {
                client->state = dmftp_cstate_simple;
                send_command(client, "RNTO", client->rename_to);
                return;
            }
            clear_pending(client);
            client->state = dmftp_cstate_ready;
            return;

        case dmftp_cstate_simple:
            clear_pending(client);
            client->state = dmftp_cstate_ready;
            return;

        case dmftp_cstate_quit:
            dmftp_client_on_control_closed(client, 0);
            return;

        default:
            return; /* an unsolicited reply while idle - reported, not acted on */
    }
}

static void handle_reply(struct dmftp_client* client, int code, const char* text)
{
    if (client->callbacks.on_reply != NULL)
    {
        client->callbacks.on_reply(client, code, text, client->user_data);
    }
    if (client->state == dmftp_cstate_closed)
        return;

    if (handle_login_reply(client, code))
        return;
    if (handle_transfer_reply(client, code, text))
        return;

    handle_simple_reply(client, code);
}

void dmftp_client_on_established(struct dmftp_client* client)
{
    /* Nothing to send yet - the server speaks first, with its 220. */
    if (client->state == dmftp_cstate_idle)
    {
        client->state = dmftp_cstate_greeting;
    }
}

void dmftp_client_on_control_data(struct dmftp_client* client, const uint8_t* data, size_t len)
{
    if (client->state == dmftp_cstate_closed)
        return;

    if (dmftp_buf_append(&client->in, data, len) != 0)
    {
        dmftp_client_on_control_closed(client, -ENOMEM);
        return;
    }

    for (;;)
    {
        size_t text_len = 0;
        char*  line = dmftp_buf_take_line(&client->in, &text_len);
        if (line == NULL)
        {
            if (client->in.len > DMFTP_LINE_MAX)
            {
                client->in.len = 0; /* a server that never terminates a line */
            }
            return;
        }

        int         code = 0;
        bool        final = false;
        const char* text = NULL;
        /* Continuation lines (`230-`) are part of a banner, not a verdict -
         * only the final line drives the state machine. */
        if (dmftp_parse_reply(line, text_len, &code, &final, &text) == 0 && final)
        {
            handle_reply(client, code, text);
        }
        Dmod_Free(line);

        if (client->state == dmftp_cstate_closed)
            return;
    }
}

/* ============================================================================
 *                      Teardown
 * ========================================================================== */

void dmftp_client_on_control_closed(struct dmftp_client* client, int error)
{
    if (client->state == dmftp_cstate_closed)
        return;

    client->state = dmftp_cstate_closed;

    if (client->xfer != NULL)
    {
        struct dmftp_xfer* xfer = client->xfer;
        client->xfer = NULL;
        dmftp_xfer_finish(xfer, -ECONNRESET);
        dmftp_xfer_destroy(xfer);
    }
    clear_pending(client);

    if (client->control != NULL)
    {
        dmtcp_conn_t control = client->control;
        client->control = NULL;
        dmftp_net_detach(control); /* dmftp_client_destroy() may free us next */
        dmtcp_close(control);
    }

    if (client->callbacks.on_closed != NULL)
    {
        client->callbacks.on_closed(client, error, client->user_data);
    }
}

/* ============================================================================
 *                      Public API
 * ========================================================================== */

dmod_dmftp_api_declaration(1.0, dmftp_client_t, _client_create, ( const dmftp_client_config_t* config, const dmftp_client_callbacks_t* callbacks, void* user_data ))
{
    if (config == NULL || config->host.family != dmip_family_v4)
        return NULL;

    struct dmftp_client* client = Dmod_Malloc(sizeof(*client));
    if (client == NULL)
        return NULL;

    memset(client, 0, sizeof(*client));
    client->magic = DMFTP_CLIENT_MAGIC;
    client->host = config->host;
    client->port = config->port != 0 ? config->port : DMFTP_PORT_CONTROL;
    client->data_mode = config->data_mode;
    client->type = config->type;
    client->state = dmftp_cstate_idle;
    client->user_data = user_data;
    dmftp_buf_init(&client->out);
    dmftp_buf_init(&client->in);

    if (callbacks != NULL)
    {
        client->callbacks = *callbacks;
    }

    const char* user = (config->user != NULL) ? config->user : DMFTP_DEFAULT_USER;
    const char* password = (config->password != NULL) ? config->password : DMFTP_DEFAULT_PASSWORD;
    client->user = dmftp_str_ndup(user, strlen(user));
    client->password = dmftp_str_ndup(password, strlen(password));

    if (client->user == NULL || client->password == NULL)
    {
        Dmod_Free(client->user);
        Dmod_Free(client->password);
        Dmod_Free(client);
        return NULL;
    }
    return client;
}

dmod_dmftp_api_declaration(1.0, int, _client_connect, ( dmftp_client_t client ))
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC)
        return -EINVAL;

    dmftp_lock();
    int result;
    if (client->state != dmftp_cstate_idle)
    {
        result = -EALREADY;
    }
    else
    {
        client->state = dmftp_cstate_greeting;
        result = dmftp_net_connect(&client->host, client->port, 0, dmftp_role_client_control, client, &client->control);
        if (result != 0)
        {
            client->state = dmftp_cstate_idle;
        }
    }
    dmftp_unlock();
    return result;
}

/**
 * @brief Shared entry check for every command-issuing API
 *
 * @return 0 if a command may be sent right now
 */
static int check_ready(struct dmftp_client* client)
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC)
        return -EINVAL;
    if (client->state == dmftp_cstate_closed || client->control == NULL)
        return -ENOTCONN;
    if (client->state != dmftp_cstate_ready)
        return client->state < dmftp_cstate_ready ? -ENOTCONN : -EBUSY;

    return 0;
}

/**
 * @brief Open a local file and build the transfer around it
 *
 * @return The transfer, or NULL if the file could not be opened
 */
static struct dmftp_xfer* make_file_transfer(struct dmftp_client* client, const char* local_path, bool upload, const char* mode)
{
    void* file = Dmod_FileOpen(local_path, mode);
    if (file == NULL)
        return NULL;

    struct dmftp_xfer* xfer = dmftp_xfer_create(client, dmftp_owner_client, upload);
    if (xfer == NULL)
    {
        Dmod_FileClose(file);
        return NULL;
    }

    dmftp_xfer_set_file(xfer, file, client->type == dmftp_type_ascii);
    return xfer;
}

dmod_dmftp_api_declaration(1.0, int, _client_get, ( dmftp_client_t client, const char* remote_path, const char* local_path ))
{
    dmftp_lock();
    int result = check_ready(client);
    if (result == 0 && (remote_path == NULL || local_path == NULL))
    {
        result = -EINVAL;
    }

    if (result == 0)
    {
        struct dmftp_xfer* xfer = make_file_transfer(client, local_path, false, "wb");
        result = (xfer != NULL) ? begin_transfer(client, "RETR", remote_path, xfer) : -EIO;
    }
    dmftp_unlock();
    return result;
}

/**
 * @brief STOR and APPE - identical but for the verb they send
 */
static int client_upload(struct dmftp_client* client, const char* local_path, const char* remote_path, const char* verb)
{
    dmftp_lock();
    int result = check_ready(client);
    if (result == 0 && (remote_path == NULL || local_path == NULL))
    {
        result = -EINVAL;
    }

    if (result == 0)
    {
        struct dmftp_xfer* xfer = make_file_transfer(client, local_path, true, "rb");
        result = (xfer != NULL) ? begin_transfer(client, verb, remote_path, xfer) : -EIO;
    }
    dmftp_unlock();
    return result;
}

dmod_dmftp_api_declaration(1.0, int, _client_put, ( dmftp_client_t client, const char* local_path, const char* remote_path ))
{
    return client_upload(client, local_path, remote_path, "STOR");
}

dmod_dmftp_api_declaration(1.0, int, _client_append, ( dmftp_client_t client, const char* local_path, const char* remote_path ))
{
    return client_upload(client, local_path, remote_path, "APPE");
}

dmod_dmftp_api_declaration(1.0, int, _client_list, ( dmftp_client_t client, const char* remote_path, bool long_format ))
{
    dmftp_lock();
    int result = check_ready(client);

    if (result == 0)
    {
        struct dmftp_xfer* xfer = dmftp_xfer_create(client, dmftp_owner_client, false);
        if (xfer == NULL)
        {
            result = -ENOMEM;
        }
        else
        {
            dmftp_xfer_set_lines(xfer);
            result = begin_transfer(client, long_format ? "LIST" : "NLST", remote_path, xfer);
        }
    }
    dmftp_unlock();
    return result;
}

/**
 * @brief Issue a one-shot command whose whole result is its reply
 */
static int client_simple(struct dmftp_client* client, const char* verb, const char* arg)
{
    dmftp_lock();
    int result = check_ready(client);
    if (result == 0)
    {
        result = send_command(client, verb, arg);
        if (result == 0)
        {
            client->state = dmftp_cstate_simple;
        }
    }
    dmftp_unlock();
    return result;
}

dmod_dmftp_api_declaration(1.0, int, _client_cwd, ( dmftp_client_t client, const char* path ))
{
    return client_simple(client, "CWD", path);
}

dmod_dmftp_api_declaration(1.0, int, _client_delete, ( dmftp_client_t client, const char* path ))
{
    return client_simple(client, "DELE", path);
}

dmod_dmftp_api_declaration(1.0, int, _client_mkdir, ( dmftp_client_t client, const char* path ))
{
    return client_simple(client, "MKD", path);
}

dmod_dmftp_api_declaration(1.0, int, _client_rmdir, ( dmftp_client_t client, const char* path ))
{
    return client_simple(client, "RMD", path);
}

dmod_dmftp_api_declaration(1.0, int, _client_size, ( dmftp_client_t client, const char* path ))
{
    return client_simple(client, "SIZE", path);
}

dmod_dmftp_api_declaration(1.0, int, _client_command, ( dmftp_client_t client, const char* verb, const char* arg ))
{
    if (verb == NULL || verb[0] == '\0' || strlen(verb) > DMFTP_VERB_MAX)
        return -EINVAL;

    return client_simple(client, verb, arg);
}

dmod_dmftp_api_declaration(1.0, int, _client_rename, ( dmftp_client_t client, const char* from, const char* to ))
{
    dmftp_lock();
    int result = check_ready(client);
    if (result == 0 && (from == NULL || to == NULL))
    {
        result = -EINVAL;
    }

    if (result == 0)
    {
        /* The RNTO is held here and sent by handle_simple_reply() once the
         * server has answered 350 - RFC 959 §4.1.3 requires the pair. */
        client->rename_to = dmftp_str_ndup(to, strlen(to));
        result = (client->rename_to != NULL) ? send_command(client, "RNFR", from) : -ENOMEM;
        if (result == 0)
        {
            client->state = dmftp_cstate_rnfr;
        }
        else
        {
            clear_pending(client);
        }
    }
    dmftp_unlock();
    return result;
}

dmod_dmftp_api_declaration(1.0, int, _client_quit, ( dmftp_client_t client ))
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC)
        return -EINVAL;

    dmftp_lock();
    if (client->state != dmftp_cstate_closed)
    {
        send_command(client, "QUIT", NULL);
        client->state = dmftp_cstate_quit;
    }
    dmftp_unlock();
    return 0;
}

dmod_dmftp_api_declaration(1.0, void, _client_destroy, ( dmftp_client_t client ))
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC)
        return;

    dmftp_lock();

    /* Suppress on_closed: the caller asked for this teardown and would be
     * getting a callback about a handle it is in the middle of destroying. */
    client->callbacks.on_closed = NULL;
    dmftp_client_on_control_closed(client, 0);
    dmftp_net_unlisten_owner(client);

    dmftp_buf_free(&client->out);
    dmftp_buf_free(&client->in);
    Dmod_Free(client->user);
    Dmod_Free(client->password);
    client->magic = 0;
    Dmod_Free(client);

    dmftp_unlock();
}

dmod_dmftp_api_declaration(1.0, void*, _client_get_user_data, ( dmftp_client_t client ))
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC)
        return NULL;
    return client->user_data;
}

dmod_dmftp_api_declaration(1.0, bool, _client_is_ready, ( dmftp_client_t client ))
{
    if (client == NULL || client->magic != DMFTP_CLIENT_MAGIC)
        return false;
    return client->state == dmftp_cstate_ready;
}
