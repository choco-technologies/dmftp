/**
 * @file dmftp_server_xfer.c
 * @brief The server commands that need a data connection - PASV/PORT to
 *        set one up, LIST/NLST/RETR/STOR/APPE to use it, ABOR to give up
 *
 * The shape is the same for all four transfer commands: make sure a data
 * connection has been arranged, open the source or sink, build the
 * transfer, answer 150, then hand off. Whether the bytes then flow through
 * an accepted (PASV) or an outgoing (PORT) connection is settled in one
 * place, start_transfer().
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

/**
 * @brief Dmod_FileSeek()'s "from the start" origin
 *
 * The SAL mirrors stdio's fseek() origins; naming it here beats a bare 0
 * at the REST call site.
 */
#define DMFTP_SEEK_SET 0

/* ============================================================================
 *                      PASV / PORT
 * ========================================================================== */

void dmftp_server_cmd_pasv(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;

    if (!session->server->allow_passive)
    {
        dmftp_server_reply(session, 502, "Passive mode is disabled.");
        return;
    }

    /* A second PASV replaces the first: the client changed its mind, and
     * the old reservation would otherwise stay held for the session. */
    dmftp_server_reset_data(session);

    uint16_t port = 0;
    int result = dmftp_net_listen_any(dmftp_listen_server_data, session, &port);
    if (result != 0)
    {
        dmftp_server_reply(session, 425, "Cannot open a passive data port.");
        return;
    }

    session->pasv_port = port;
    session->data_mode = dmftp_data_passive;

    /* The address is the one the client reached us on, not a routing
     * lookup: it is the address this client demonstrably has a path to. */
    char tuple[24];
    if (dmftp_format_host_port(tuple, sizeof(tuple), &session->local_addr, port, NULL) != 0)
    {
        dmftp_net_unlisten(port);
        session->pasv_port = 0;
        dmftp_server_reply(session, 425, "Cannot open a passive data port.");
        return;
    }

    char* text = dmftp_str_join("Entering Passive Mode (", tuple, ").");
    dmftp_server_reply(session, 227, text != NULL ? text : "Entering Passive Mode.");
    Dmod_Free(text);
}

/** @brief Whether two addresses are the same IPv4 host */
static bool same_v4_host(const dmip_addr_t* a, const dmip_addr_t* b)
{
    if (a->family != dmip_family_v4 || b->family != dmip_family_v4)
        return false;

    for (size_t i = 0; i < DMIP_IPV4_ADDR_LEN; i++)
    {
        if (a->addr.v4[i] != b->addr.v4[i])
            return false;
    }
    return true;
}

void dmftp_server_cmd_port(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    if (!session->server->allow_active)
    {
        dmftp_server_reply(session, 502, "Active mode is disabled.");
        return;
    }

    char* arg = dmftp_str_ndup(cmd->arg, cmd->arg_len);
    if (arg == NULL)
    {
        dmftp_server_reply(session, 550, "Out of memory.");
        return;
    }

    dmip_addr_t addr;
    uint16_t    port = 0;
    int         parsed = dmftp_parse_host_port(arg, &addr, &port);
    Dmod_Free(arg);

    if (parsed != 0 || port == 0)
    {
        dmftp_server_reply(session, 501, "Syntax error in PORT arguments.");
        return;
    }

    /* The classic FTP bounce attack (CERT CA-1997-27) is a PORT naming a
     * third party, turning the server into a relay that opens connections
     * on the attacker's behalf. Requiring the data address to match the
     * control connection's peer closes it, and costs a legitimate client
     * nothing. */
    if (!same_v4_host(&addr, &session->peer_addr))
    {
        dmftp_server_reply(session, 501, "PORT address must match the control connection.");
        return;
    }

    dmftp_server_reset_data(session);
    session->active_addr = addr;
    session->active_port = port;
    session->data_mode = dmftp_data_active;
    dmftp_server_reply(session, 200, "PORT command successful.");
}

/* ============================================================================
 *                      Starting a transfer
 * ========================================================================== */

/**
 * @brief Whether the client has arranged a data connection at all
 */
static bool data_ready(const struct dmftp_session* session)
{
    if (session->data_mode == dmftp_data_active)
        return session->active_port != 0;

    return session->pasv_port != 0 || session->pending_data_conn != NULL;
}

/**
 * @brief Answer 150 and get the bytes moving
 *
 * Takes ownership of `xfer`: on any failure it is finished, destroyed and
 * detached from the session before returning.
 */
static void start_transfer(struct dmftp_session* session, struct dmftp_xfer* xfer)
{
    session->xfer = xfer;
    dmftp_server_reply(session, 150, "Opening data connection.");

    if (session->data_mode == dmftp_data_active)
    {
        dmtcp_conn_t conn = NULL;
        int result = dmftp_net_connect(&session->active_addr, session->active_port,
                                        session->server->active_data_port,
                                        dmftp_role_server_data, session, &conn);
        if (result != 0)
        {
            /* The transfer never started, so report the connection failure
             * itself rather than letting dmftp_server_xfer_done() turn it
             * into a generic 451. */
            session->xfer = NULL;
            dmftp_xfer_destroy(xfer);
            session->active_port = 0;
            dmftp_server_reply(session, 425, "Cannot open the data connection.");
            return;
        }
        session->active_port = 0; /* one PORT authorizes exactly one transfer */
        return;                   /* on_established attaches it */
    }

    if (session->pending_data_conn != NULL)
    {
        dmtcp_conn_t conn = session->pending_data_conn;
        session->pending_data_conn = NULL;
        dmftp_xfer_attach(xfer, conn);
    }
    /* Otherwise the PASV listener is still armed and the accept handler
     * will attach the connection as soon as the client opens it. */
}

/**
 * @brief Common prologue for RETR/STOR/APPE: check the data connection,
 *        resolve the path, open the file
 *
 * @param mode Mode string for Dmod_FileOpen()
 *
 * @return The open file, or NULL after replying itself
 */
static void* open_transfer_file(struct dmftp_session* session, const dmftp_command_t* cmd, const char* mode)
{
    if (!data_ready(session))
    {
        dmftp_server_reply(session, 425, "Use PASV or PORT first.");
        return NULL;
    }

    char* arg = dmftp_str_ndup(cmd->arg, cmd->arg_len);
    char* os_path = dmftp_path_resolve(session->root, session->cwd, arg, NULL);
    Dmod_Free(arg);

    if (os_path == NULL)
    {
        dmftp_server_reply(session, 550, "Out of memory.");
        return NULL;
    }

    void* file = Dmod_FileOpen(os_path, mode);
    Dmod_Free(os_path);

    if (file == NULL)
    {
        dmftp_server_reply(session, 550, "Cannot open file.");
    }
    return file;
}

/**
 * @brief Apply a pending REST offset to a just-opened file
 *
 * The offset is consumed either way (RFC 3659 §5: a restart marker applies
 * to the next transfer only), so a failed seek cannot silently carry over
 * into the transfer after this one.
 *
 * @return true if the file is positioned where the client asked
 */
static bool apply_restart(struct dmftp_session* session, void* file)
{
    uint32_t offset = session->rest_offset;
    session->rest_offset = 0;

    if (offset == 0)
        return true;

    return Dmod_FileSeek(file, (long)offset, DMFTP_SEEK_SET) == 0;
}

/* ============================================================================
 *                      LIST / NLST
 * ========================================================================== */

/**
 * @brief Skip the `-l`, `-a`, `-la` option words clients put in front of a
 *        LIST path
 *
 * RFC 959 says the argument is a pathname; every real client sends `ls`
 * flags anyway, and a server that treated "-la" as a directory name would
 * answer 550 to a perfectly ordinary listing request.
 *
 * @return A new string holding the real path (possibly empty), or NULL on
 *         allocation failure
 */
static char* strip_list_options(const dmftp_command_t* cmd)
{
    if (cmd->arg == NULL || cmd->arg_len == 0)
        return dmftp_str_ndup("", 0);

    size_t start = 0;
    while (start < cmd->arg_len)
    {
        if (cmd->arg[start] != '-')
            break;

        while (start < cmd->arg_len && cmd->arg[start] != ' ')
        {
            start++;
        }
        while (start < cmd->arg_len && cmd->arg[start] == ' ')
        {
            start++;
        }
    }
    return dmftp_str_ndup(cmd->arg + start, cmd->arg_len - start);
}

/**
 * @brief LIST and NLST, which differ only in the format of each line
 */
static void listing_command(struct dmftp_session* session, const dmftp_command_t* cmd, bool long_format)
{
    if (!data_ready(session))
    {
        dmftp_server_reply(session, 425, "Use PASV or PORT first.");
        return;
    }

    char* arg = strip_list_options(cmd);
    char* os_path = dmftp_path_resolve(session->root, session->cwd, arg, NULL);
    Dmod_Free(arg);

    if (os_path == NULL)
    {
        dmftp_server_reply(session, 550, "Out of memory.");
        return;
    }

    void* dir = Dmod_OpenDir(os_path);
    if (dir == NULL)
    {
        Dmod_Free(os_path);
        dmftp_server_reply(session, 550, "No such directory.");
        return;
    }

    struct dmftp_xfer* xfer = dmftp_xfer_create(session, dmftp_owner_session, true);
    if (xfer == NULL)
    {
        Dmod_CloseDir(dir);
        Dmod_Free(os_path);
        dmftp_server_reply(session, 451, "Out of memory.");
        return;
    }

    dmftp_xfer_set_dir(xfer, dir, os_path, long_format); /* both handles move into the transfer */
    start_transfer(session, xfer);
}

void dmftp_server_cmd_list(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    listing_command(session, cmd, true);
}

void dmftp_server_cmd_nlst(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    listing_command(session, cmd, false);
}

/* ============================================================================
 *                      RETR / STOR / APPE
 * ========================================================================== */

void dmftp_server_cmd_retr(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    void* file = open_transfer_file(session, cmd, "rb");
    if (file == NULL)
        return;

    if (!apply_restart(session, file))
    {
        Dmod_FileClose(file);
        dmftp_server_reply(session, 550, "Cannot seek to the restart position.");
        return;
    }

    struct dmftp_xfer* xfer = dmftp_xfer_create(session, dmftp_owner_session, true);
    if (xfer == NULL)
    {
        Dmod_FileClose(file);
        dmftp_server_reply(session, 451, "Out of memory.");
        return;
    }

    dmftp_xfer_set_file(xfer, file, session->type == dmftp_type_ascii);
    start_transfer(session, xfer);
}

/**
 * @brief STOR and APPE, which differ only in the file open mode
 */
static void store_command(struct dmftp_session* session, const dmftp_command_t* cmd, bool append)
{
    void* file = open_transfer_file(session, cmd, append ? "ab" : "wb");
    if (file == NULL)
        return;

    /* A restart on an upload positions the file the client is writing into;
     * on an append it is meaningless, and RFC 3659 says to ignore it. */
    if (!append && !apply_restart(session, file))
    {
        Dmod_FileClose(file);
        dmftp_server_reply(session, 550, "Cannot seek to the restart position.");
        return;
    }
    session->rest_offset = 0;

    struct dmftp_xfer* xfer = dmftp_xfer_create(session, dmftp_owner_session, false);
    if (xfer == NULL)
    {
        Dmod_FileClose(file);
        dmftp_server_reply(session, 451, "Out of memory.");
        return;
    }

    dmftp_xfer_set_file(xfer, file, session->type == dmftp_type_ascii);
    start_transfer(session, xfer);
}

void dmftp_server_cmd_stor(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    store_command(session, cmd, false);
}

void dmftp_server_cmd_appe(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    store_command(session, cmd, true);
}

/* ============================================================================
 *                      ABOR
 * ========================================================================== */

void dmftp_server_cmd_abor(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;

    if (session->xfer == NULL)
    {
        dmftp_server_reply(session, 226, "No transfer to abort.");
        return;
    }

    /* dmftp_xfer_finish() runs the completion hook, which answers 426 for
     * the aborted transfer and clears session->xfer; RFC 959 §4.1.3.5 wants
     * the 226 for the ABOR command itself right after. The reset then
     * releases whatever data-connection setup was still reserved. */
    dmftp_xfer_finish(session->xfer, -ECONNABORTED);
    dmftp_server_reset_data(session);
    dmftp_server_reply(session, 226, "Abort successful.");
}
