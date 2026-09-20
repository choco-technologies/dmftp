#include "dmod.h"
#include "ftpd_internal.h"
#include <string.h>
#include <stdarg.h>

/**
 * RFC 959 command handling and PASV/PORT data transfer for ftpd - the
 * "policy" layer on top of libftp's transport-agnostic engine and the
 * dmtcp/argv wiring in src/ftpd_server.c/ftpd.c. See ftpd_internal.h for
 * the shared connection/context structs, and ftpd_server.c's top comment
 * for this server's documented scope (binary transfers only, no
 * REST/APPE/rename).
 */

static void reply(ftpd_connection_t* c, int code, const char* text)
{
    libftp_reply(c->engine, code, text);
}

static void replyf(ftpd_connection_t* c, int code, const char* fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    Dmod_VSnPrintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    libftp_reply(c->engine, code, buffer);
}

/* ============================================================================
 *                      Virtual path resolution
 * ========================================================================== */

/**
 * Joins `arg` onto `cwd` (or treats it as absolute if it starts with '/'),
 * then resolves "." / ".." components - a "." is dropped, a ".." pops the
 * last component (clamped at the virtual root, never escaping above it).
 * The result always starts with '/' and never ends with one (except the
 * root itself, "/").
 *
 * @return A newly heap-allocated virtual path, or NULL on allocation failure
 */
static char* normalize_virtual_path(const char* cwd, const char* arg)
{
    char raw[LIBFTP_MAX_LINE_LEN];

    if (arg[0] == '/')
        Dmod_SnPrintf(raw, sizeof(raw), "%s", arg);
    else if (strcmp(cwd, "/") == 0)
        Dmod_SnPrintf(raw, sizeof(raw), "/%s", arg);
    else
        Dmod_SnPrintf(raw, sizeof(raw), "%s/%s", cwd, arg);

    char* components[64];
    size_t depth = 0;

    char* p = raw;
    while (*p != '\0')
    {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        char* start = p;
        while (*p != '/' && *p != '\0')
            p++;
        if (*p == '/')
        {
            *p = '\0';
            p++;
        }

        if (start[0] == '\0' || (start[0] == '.' && start[1] == '\0'))
        {
            /* Empty component (shouldn't happen after the leading-slash
             * skip above) or "." - drop it. */
        }
        else if (start[0] == '.' && start[1] == '.' && start[2] == '\0')
        {
            if (depth > 0)
                depth--;
        }
        else if (depth < (sizeof(components) / sizeof(components[0])))
        {
            components[depth++] = start;
        }
    }

    size_t total = 1; /* Leading '/' */
    for (size_t i = 0; i < depth; i++)
        total += strlen(components[i]) + 1;

    char* result = Dmod_Malloc(total + 1);
    if (result == NULL)
        return NULL;

    size_t pos = 0;
    result[pos++] = '/';
    for (size_t i = 0; i < depth; i++)
    {
        size_t component_len = strlen(components[i]);
        memcpy(result + pos, components[i], component_len);
        pos += component_len;
        if (i + 1 < depth)
            result[pos++] = '/';
    }
    result[pos] = '\0';

    return result;
}

/**
 * Prefixes a normalized virtual path with the configured root to get the
 * real dmvfs-backed path to pass to the Dmod_File.../Dmod_...Dir... SAL
 * functions - e.g. root "/ftp" + virtual "/sub" -> "/ftp/sub". No-op
 * concatenation when root is
 * "/" itself (dmvfs is already mounted there - see
 * src/ftpd_server.c's normalize_root()).
 *
 * @return A newly heap-allocated real path, or NULL on allocation failure
 */
static char* build_real_path(const char* virtual_path)
{
    const char* root = g_ftpd_context->root;
    if (root[0] == '/' && root[1] == '\0')
        return Dmod_StrDup(virtual_path);

    size_t root_len = strlen(root);
    size_t virtual_len = strlen(virtual_path);

    char* result = Dmod_Malloc(root_len + virtual_len + 1);
    if (result == NULL)
        return NULL;

    memcpy(result, root, root_len);
    memcpy(result + root_len, virtual_path, virtual_len);
    result[root_len + virtual_len] = '\0';
    return result;
}

/**
 * Resolves `arg` (relative to `c->cwd`, or absolute) to both a virtual and
 * a real path in one call - the shape every path-taking command needs.
 * Frees nothing on success; on failure, replies 451 and both out params
 * are left NULL.
 */
static void resolve_paths(ftpd_connection_t* c, const char* arg, char** out_virtual, char** out_real)
{
    *out_virtual = NULL;
    *out_real = NULL;

    char* virtual_path = normalize_virtual_path(c->cwd, arg);
    if (virtual_path == NULL)
    {
        reply(c, 451, "Local error in processing");
        return;
    }

    char* real_path = build_real_path(virtual_path);
    if (real_path == NULL)
    {
        Dmod_Free(virtual_path);
        reply(c, 451, "Local error in processing");
        return;
    }

    *out_virtual = virtual_path;
    *out_real = real_path;
}

/* ============================================================================
 *                      Directory listing rendering
 * ========================================================================== */

static bool buffer_append(uint8_t** buf, size_t* len, size_t* cap, const void* data, size_t data_len)
{
    if (*len + data_len > *cap)
    {
        size_t new_cap = (*cap == 0) ? 256 : *cap;
        while (new_cap < *len + data_len)
            new_cap *= 2;

        uint8_t* grown = Dmod_Realloc(*buf, new_cap);
        if (grown == NULL)
            return false;
        *buf = grown;
        *cap = new_cap;
    }

    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    return true;
}

/**
 * Renders `real_dir`'s contents as either a bare filename-per-line list
 * (NLST) or a Unix "ls -l"-style listing (LIST). Dmod_ReadDirEx() reports
 * only a name/type per entry (no size - see Dmod_DirEntry_t), so a
 * regular file's size is fetched by opening it - acceptable for the small
 * embedded directories this targets, not for one with thousands of files.
 * There is likewise no mtime available anywhere in the Dmod SAL, so every
 * LIST line reports the same fixed placeholder timestamp; real FTP clients
 * tolerate this (they parse the fixed-width fields, not the date's value).
 *
 * @return true on success (`*out_buf`/`*out_len` set - `*out_buf` may be
 *         NULL if the directory is empty), false if `real_dir` could not
 *         be opened as a directory or an allocation failed
 */
static bool render_listing(const char* real_dir, bool name_only, uint8_t** out_buf, size_t* out_len)
{
    void* dir = Dmod_OpenDir(real_dir);
    if (dir == NULL)
        return false;

    uint8_t* buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    bool ok = true;

    const Dmod_DirEntry_t* entry;
    while (ok && (entry = Dmod_ReadDirEx(dir)) != NULL)
    {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0)
            continue;

        char line[320];
        int line_len;

        if (name_only)
        {
            line_len = Dmod_SnPrintf(line, sizeof(line), "%s\r\n", entry->name);
        }
        else
        {
            unsigned long size = 0;
            if (entry->type != Dmod_DirEntryType_Dir)
            {
                char child[320];
                if (real_dir[0] == '/' && real_dir[1] == '\0')
                    Dmod_SnPrintf(child, sizeof(child), "/%s", entry->name);
                else
                    Dmod_SnPrintf(child, sizeof(child), "%s/%s", real_dir, entry->name);

                void* f = Dmod_FileOpen(child, "r");
                if (f != NULL)
                {
                    size = (unsigned long)Dmod_FileSize(f);
                    Dmod_FileClose(f);
                }
            }

            char type_char = (entry->type == Dmod_DirEntryType_Dir) ? 'd' : '-';
            line_len = Dmod_SnPrintf(line, sizeof(line),
                "%crw-r--r--   1 owner    group    %10lu Jan 01 00:00 %s\r\n",
                type_char, size, entry->name);
        }

        if (line_len > 0)
        {
            if ((size_t)line_len >= sizeof(line))
                line_len = (int)sizeof(line) - 1;
            ok = buffer_append(&buf, &len, &cap, line, (size_t)line_len);
        }
    }

    Dmod_CloseDir(dir);

    if (!ok)
    {
        Dmod_Free(buf);
        return false;
    }

    *out_buf = buf;
    *out_len = len;
    return true;
}

/* ============================================================================
 *                      Data connection sending
 * ========================================================================== */

/** Releases everything a LIST/NLST/RETR/STOR (or a stray idle data
 * connection) held, without sending any reply - callers decide whether a
 * reply is owed (see ftpd_data_on_closed()/_reset()/_error() vs.
 * finish_transfer() below). */
static void cleanup_transfer_state(ftpd_connection_t* c)
{
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
    c->data_conn = NULL;
    c->transfer_ok = false;
}

/** Cleans up and always replies - used where a reply is unconditionally
 * owed (cmd_abor()). See ftpd_data_on_closed()/_reset()/_error() for the
 * data-connection-ended paths, which reply only if a transfer was
 * actually in progress. */
static void finish_transfer(ftpd_connection_t* c, int code, const char* text)
{
    cleanup_transfer_state(c);
    reply(c, code, text);
}

static void send_list_chunk(ftpd_connection_t* c)
{
    for (;;)
    {
        if (c->list_sent >= c->list_len)
        {
            c->transfer_ok = true;
            dmtcp_close(c->data_conn);
            return;
        }

        int space = dmtcp_send_space(c->data_conn);
        if (space <= 0)
            return; /* Wait for ftpd_data_on_writable() */

        size_t remaining = c->list_len - c->list_sent;
        size_t want = ((size_t)space < remaining) ? (size_t)space : remaining;

        int sent = dmtcp_send(c->data_conn, c->list_buffer + c->list_sent, want);
        if (sent <= 0)
            return;

        c->list_sent += (size_t)sent;
        if ((size_t)sent < want)
            return; /* Short write - wait for ftpd_data_on_writable() */
    }
}

static void send_retr_chunk(ftpd_connection_t* c)
{
    uint8_t chunk[FTPD_DATA_CHUNK_SIZE];

    for (;;)
    {
        int space = dmtcp_send_space(c->data_conn);
        if (space <= 0)
            return; /* Wait for ftpd_data_on_writable() */

        size_t want = ((size_t)space < sizeof(chunk)) ? (size_t)space : sizeof(chunk);
        size_t got = Dmod_FileRead(chunk, 1, want, c->transfer_file);
        if (got == 0)
        {
            c->transfer_ok = true;
            dmtcp_close(c->data_conn);
            return;
        }

        int sent = dmtcp_send(c->data_conn, chunk, got);
        if (sent < 0)
            sent = 0;

        if ((size_t)sent < got)
        {
            /* dmtcp_send_space() only guarantees a full accept "from this
             * thread" (see dmtcp.h) - defensively rewind the unsent tail
             * so it is re-read on the next attempt instead of being lost. */
            Dmod_FileSeek(c->transfer_file, -(long)(got - (size_t)sent), DMOD_SEEK_CUR);
            return;
        }
    }
}

void ftpd_data_begin(ftpd_connection_t* c)
{
    switch (c->data_op)
    {
    case ftpd_data_op_list:
    case ftpd_data_op_nlst:
        send_list_chunk(c);
        break;
    case ftpd_data_op_retr:
        send_retr_chunk(c);
        break;
    case ftpd_data_op_stor:
        break; /* Nothing to do yet - wait for ftpd_data_on_data() */
    case ftpd_data_op_none:
    default:
        /* The data connection arrived before the LIST/RETR/STOR that will
         * use it - real clients commonly connect PASV's data channel
         * immediately after the 227 reply, before sending the command
         * that actually needs it (this is the common case, not the
         * exception). Leave it open and idle: cmd_list()/_retr()/_stor()
         * call this same function again, with data_op now set, once that
         * command arrives - see their own "c->data_conn != NULL" check. */
        break;
    }
}

void ftpd_data_on_data(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data)
{
    ftpd_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    if (data == NULL)
    {
        /* Peer's FIN. For STOR (the client is the sender), this is the
         * normal, successful end of the upload. For LIST/NLST/RETR (the
         * client is only ever the receiver) or an idle connection nothing
         * ever claimed (data_op still none - e.g. LIST failed before
         * reaching the data phase, and the client dropped the now-useless
         * PASV/PORT connection), a FIN from the client is not a success
         * signal - transfer_ok stays false, and ftpd_data_on_closed()
         * only replies at all if data_op says a transfer was actually
         * requested. */
        if (c->data_op == ftpd_data_op_stor)
            c->transfer_ok = true;
        dmtcp_close(conn);
        return;
    }

    if (c->data_op == ftpd_data_op_stor && c->transfer_file != NULL)
    {
        Dmod_FileWrite(data, 1, data_len, c->transfer_file);
    }
}

void ftpd_data_on_writable(dmtcp_conn_t conn, size_t space, void* user_data)
{
    (void)space;
    ftpd_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    if (c->data_op == ftpd_data_op_list || c->data_op == ftpd_data_op_nlst)
        send_list_chunk(c);
    else if (c->data_op == ftpd_data_op_retr)
        send_retr_chunk(c);
}

/**
 * Common tail for the three data-connection-ended callbacks below: cleans
 * up always, but only replies if a LIST/NLST/RETR/STOR was actually in
 * progress (data_op != none). A connection that arrived (PASV) or was
 * opened (PORT) but never got claimed by a command - e.g. the command
 * that would have claimed it failed for its own reason and already sent
 * its own reply, same as any other client-visible error - ending is not
 * newsworthy and must not produce a second, spurious reply on the control
 * connection.
 */
static void end_data_connection(ftpd_connection_t* c, int code, const char* text)
{
    bool was_active = (c->data_op != ftpd_data_op_none);
    cleanup_transfer_state(c);
    if (was_active)
        reply(c, code, text);
}

void ftpd_data_on_closed(dmtcp_conn_t conn, void* user_data)
{
    ftpd_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    if (c->transfer_ok)
        end_data_connection(c, 226, "Transfer complete");
    else
        end_data_connection(c, 426, "Connection closed; transfer aborted");
}

void ftpd_data_on_reset(dmtcp_conn_t conn, void* user_data)
{
    ftpd_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    end_data_connection(c, 426, "Connection reset; transfer aborted");
}

void ftpd_data_on_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)error;
    ftpd_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    end_data_connection(c, 451, "Local error; transfer aborted");
}

void ftpd_data_on_established(dmtcp_conn_t conn, void* user_data)
{
    ftpd_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    /* PORT-mode equivalent of pasv_on_accept()'s ftpd_data_begin() call -
     * the command that triggered ftpd_server_connect_port() already set
     * data_op before connecting, so this is the exact same dispatch. */
    ftpd_data_begin(c);
}

/**
 * Arranges (or confirms) the data connection a just-prepared LIST/NLST/
 * RETR/STOR needs, once `c->data_op` is already set:
 *   - PASV, already connected (c->data_conn set): kick off sending now.
 *   - PASV, still waiting for the client to connect: nothing to do here -
 *     pasv_on_accept() calls ftpd_data_begin() once it arrives.
 *   - PORT: actively connect now.
 *
 * @return true if a connection is in hand or successfully underway, false
 *         if PORT's dmtcp_connect() failed synchronously (the caller
 *         should discard what it just prepared and reply with an error)
 */
static bool start_data_transfer(ftpd_connection_t* c)
{
    if (c->data_conn != NULL)
    {
        ftpd_data_begin(c);
        return true;
    }
    if (c->port_pending)
        return ftpd_server_connect_port(c) == 0;

    return true; /* c->pasv_pending - nothing to do yet. */
}

/** Undoes whatever a LIST/NLST/RETR/STOR just set up, without touching any
 * data connection - used when start_data_transfer() fails synchronously,
 * before any reply has gone out for this command. */
static void discard_prepared_transfer(ftpd_connection_t* c)
{
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
}

/* ============================================================================
 *                      RFC 959 commands
 * ========================================================================== */

static void cmd_user(ftpd_connection_t* c, const char* arg)
{
    if (arg[0] == '\0')
    {
        reply(c, 501, "USER requires a name");
        return;
    }

    Dmod_Free(c->username);
    c->username = Dmod_StrDup(arg);
    c->logged_in = false;
    c->user_received = (c->username != NULL);

    if (!c->user_received)
    {
        reply(c, 451, "Local error in processing");
        return;
    }

    replyf(c, 331, "Password required for %s", arg);
}

static void cmd_pass(ftpd_connection_t* c, const char* arg)
{
    if (!c->user_received)
    {
        reply(c, 503, "Login with USER first");
        return;
    }

    const struct ftpd_context* ctx = g_ftpd_context;
    bool ok;
    if (ftpd_is_anonymous_user(ctx->user))
        ok = true;
    else
        ok = (strcmp(c->username, ctx->user) == 0) && (ctx->pass[0] == '\0' || strcmp(arg, ctx->pass) == 0);

    if (ok)
    {
        c->logged_in = true;
        reply(c, 230, "Login successful");
    }
    else
    {
        c->logged_in = false;
        c->user_received = false;
        reply(c, 530, "Login incorrect");
    }
}

static void cmd_quit(ftpd_connection_t* c)
{
    reply(c, 221, "Goodbye");
    if (c->control_conn != NULL)
        dmtcp_close(c->control_conn);
}

static void cmd_pwd(ftpd_connection_t* c)
{
    replyf(c, 257, "\"%s\" is the current directory", c->cwd);
}

static void cmd_type(ftpd_connection_t* c, const char* arg)
{
    if (strcmp(arg, "I") == 0 || strcmp(arg, "L8") == 0)
    {
        c->binary_mode = true;
        reply(c, 200, "Type set to I");
    }
    else if (strcmp(arg, "A") == 0)
    {
        /* Accepted but not actually translated - see this file's top
         * comment / ftpd_server.c's known-limitations list. */
        c->binary_mode = false;
        reply(c, 200, "Type set to A");
    }
    else
    {
        reply(c, 504, "Type not supported");
    }
}

static void cmd_cwd(ftpd_connection_t* c, const char* arg)
{
    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;

    void* dir = Dmod_OpenDir(real_path);
    bool ok = (dir != NULL) || strcmp(virtual_path, "/") == 0;
    if (dir != NULL)
        Dmod_CloseDir(dir);
    Dmod_Free(real_path);

    if (!ok)
    {
        Dmod_Free(virtual_path);
        reply(c, 550, "Directory not found");
        return;
    }

    Dmod_Free(c->cwd);
    c->cwd = virtual_path;
    replyf(c, 250, "Directory changed to %s", c->cwd);
}

/** Drops a previous PASV/PORT's data connection that arrived (or was
 * opened) but never got claimed by a LIST/RETR/STOR - called before
 * starting a fresh PASV or PORT so a session can't leak one per re-try. */
static void clear_stale_data_conn(ftpd_connection_t* c)
{
    if (c->data_conn != NULL && c->data_op == ftpd_data_op_none)
    {
        dmtcp_conn_t stale = c->data_conn;
        c->data_conn = NULL;
        dmtcp_abort(stale);
    }
}

static void cmd_pasv(ftpd_connection_t* c)
{
    clear_stale_data_conn(c);
    c->port_pending = false; /* A fresh PASV supersedes any pending PORT. */

    uint16_t port;
    if (ftpd_server_start_pasv(c, &port) != 0)
    {
        reply(c, 425, "Cannot open passive connection");
        return;
    }

    dmip_addr_t local_addr;
    uint16_t local_port;
    if (dmtcp_conn_get_local_endpoint(c->control_conn, &local_addr, &local_port) != 0 ||
        local_addr.family != dmip_family_v4)
    {
        ftpd_server_stop_pasv(c);
        reply(c, 425, "Cannot open passive connection");
        return;
    }

    replyf(c, 227, "Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
        (unsigned)local_addr.addr.v4[0], (unsigned)local_addr.addr.v4[1],
        (unsigned)local_addr.addr.v4[2], (unsigned)local_addr.addr.v4[3],
        (unsigned)(port >> 8), (unsigned)(port & 0xFFu));
}

/**
 * PORT h1,h2,h3,h4,p1,p2 (RFC 959 §4.1.2) - the client tells us where to
 * actively connect for its *next* data transfer.
 *
 * The given address is required to match the control connection's own
 * peer exactly - see this file's top comment (ftpd_server.c) for why:
 * without that check, PORT would let any client point our outbound data
 * connection at an arbitrary third host, the classic "FTP bounce" abuse.
 * The actual dmtcp_connect() happens later, from whichever LIST/RETR/STOR
 * uses this - see start_data_transfer().
 */
static void cmd_port(ftpd_connection_t* c, const char* arg)
{
    unsigned int h1, h2, h3, h4, p1, p2;
    int fields = Dmod_Sscanf(arg, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2);
    if (fields != 6 || h1 > 255 || h2 > 255 || h3 > 255 || h4 > 255 || p1 > 255 || p2 > 255)
    {
        reply(c, 501, "Invalid PORT argument");
        return;
    }

    dmip_addr_t peer_addr;
    uint16_t peer_port;
    if (dmtcp_conn_get_peer_endpoint(c->control_conn, &peer_addr, &peer_port) != 0 ||
        peer_addr.family != dmip_family_v4 ||
        peer_addr.addr.v4[0] != (uint8_t)h1 || peer_addr.addr.v4[1] != (uint8_t)h2 ||
        peer_addr.addr.v4[2] != (uint8_t)h3 || peer_addr.addr.v4[3] != (uint8_t)h4)
    {
        reply(c, 501, "PORT address must match the control connection's peer");
        return;
    }

    clear_stale_data_conn(c);
    ftpd_server_stop_pasv(c); /* A fresh PORT supersedes any pending PASV. */

    c->port_addr = peer_addr;
    c->port_port = (uint16_t)(p1 * 256u + p2);
    c->port_pending = true;

    reply(c, 200, "PORT command successful");
}

/**
 * Real FTP clients commonly send LIST/NLST with `ls`-style flags instead
 * of (or before) a path - observed on the wire from Nautilus/GVFS as
 * literally "LIST -a". A real path is never a `-`-prefixed token, so any
 * number of leading `-flag` tokens are skipped; whatever remains (if
 * anything) is treated as the actual path argument.
 */
static const char* strip_list_flags(const char* arg)
{
    while (arg[0] == '-')
    {
        while (*arg != '\0' && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }
    return arg;
}

static void cmd_list(ftpd_connection_t* c, const char* arg, ftpd_data_op_t op)
{
    if (c->data_conn == NULL && !c->pasv_pending && !c->port_pending)
    {
        reply(c, 425, "Use PASV or PORT first");
        return;
    }
    if (c->data_op != ftpd_data_op_none)
    {
        reply(c, 450, "Another transfer is already in progress");
        return;
    }

    arg = strip_list_flags(arg);

    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;
    Dmod_Free(virtual_path);

    uint8_t* buffer = NULL;
    size_t len = 0;
    bool ok = render_listing(real_path, op == ftpd_data_op_nlst, &buffer, &len);
    Dmod_Free(real_path);

    if (!ok)
    {
        reply(c, 550, "Failed to list directory");
        return;
    }

    c->data_op = op;
    c->list_buffer = buffer;
    c->list_len = len;
    c->list_sent = 0;

    if (!start_data_transfer(c))
    {
        discard_prepared_transfer(c);
        reply(c, 425, "Cannot open data connection");
        return;
    }
    reply(c, 150, "Here comes the directory listing");
}

static void cmd_retr(ftpd_connection_t* c, const char* arg)
{
    if (c->data_conn == NULL && !c->pasv_pending && !c->port_pending)
    {
        reply(c, 425, "Use PASV or PORT first");
        return;
    }
    if (c->data_op != ftpd_data_op_none)
    {
        reply(c, 450, "Another transfer is already in progress");
        return;
    }

    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;
    Dmod_Free(virtual_path);

    void* file = Dmod_FileOpen(real_path, "r");
    Dmod_Free(real_path);
    if (file == NULL)
    {
        reply(c, 550, "File not found");
        return;
    }

    c->data_op = ftpd_data_op_retr;
    c->transfer_file = file;

    if (!start_data_transfer(c))
    {
        discard_prepared_transfer(c);
        reply(c, 425, "Cannot open data connection");
        return;
    }
    reply(c, 150, "Opening binary mode data connection for file transfer");
}

static void cmd_stor(ftpd_connection_t* c, const char* arg)
{
    if (c->data_conn == NULL && !c->pasv_pending && !c->port_pending)
    {
        reply(c, 425, "Use PASV or PORT first");
        return;
    }
    if (c->data_op != ftpd_data_op_none)
    {
        reply(c, 450, "Another transfer is already in progress");
        return;
    }

    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;
    Dmod_Free(virtual_path);

    void* file = Dmod_FileOpen(real_path, "w");
    Dmod_Free(real_path);
    if (file == NULL)
    {
        reply(c, 550, "Cannot create file");
        return;
    }

    c->data_op = ftpd_data_op_stor;
    c->transfer_file = file;

    if (!start_data_transfer(c))
    {
        discard_prepared_transfer(c);
        reply(c, 425, "Cannot open data connection");
        return;
    }
    reply(c, 150, "Ready to receive file");
}

static void cmd_dele(ftpd_connection_t* c, const char* arg)
{
    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;
    Dmod_Free(virtual_path);

    bool ok = (Dmod_FileRemove(real_path) == 0);
    Dmod_Free(real_path);
    reply(c, ok ? 250 : 550, ok ? "File deleted" : "Delete failed");
}

static void cmd_mkd(ftpd_connection_t* c, const char* arg)
{
    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;

    bool ok = (Dmod_MakeDir(real_path, 0755) == 0);
    Dmod_Free(real_path);

    if (ok)
        replyf(c, 257, "\"%s\" created", virtual_path);
    else
        reply(c, 550, "Create directory failed");
    Dmod_Free(virtual_path);
}

static void cmd_rmd(ftpd_connection_t* c, const char* arg)
{
    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;
    Dmod_Free(virtual_path);

    bool ok = (Dmod_RemoveDir(real_path) == 0);
    Dmod_Free(real_path);
    reply(c, ok ? 250 : 550, ok ? "Directory removed" : "Remove directory failed");
}

static void cmd_size(ftpd_connection_t* c, const char* arg)
{
    char* virtual_path;
    char* real_path;
    resolve_paths(c, arg, &virtual_path, &real_path);
    if (virtual_path == NULL)
        return;
    Dmod_Free(virtual_path);

    void* file = Dmod_FileOpen(real_path, "r");
    Dmod_Free(real_path);
    if (file == NULL)
    {
        reply(c, 550, "File not found");
        return;
    }

    unsigned long size = (unsigned long)Dmod_FileSize(file);
    Dmod_FileClose(file);
    replyf(c, 213, "%lu", size);
}

static void cmd_abor(ftpd_connection_t* c)
{
    dmtcp_conn_t victim = c->data_conn;
    finish_transfer(c, 226, "ABOR command successful");
    if (victim != NULL)
        dmtcp_abort(victim);
}

void ftpd_handle_command(libftp_t engine, const char* verb, const char* arg, void* user_data)
{
    (void)engine;
    ftpd_connection_t* c = user_data;

    DMOD_LOG_INFO("ftpd: <%p> -> %s %s\n", (void*)c, verb, arg);

    /* Allowed before login, per RFC 959. */
    if (strcmp(verb, "USER") == 0) { cmd_user(c, arg); return; }
    if (strcmp(verb, "PASS") == 0) { cmd_pass(c, arg); return; }
    if (strcmp(verb, "QUIT") == 0) { cmd_quit(c); return; }
    if (strcmp(verb, "NOOP") == 0) { reply(c, 200, "NOOP ok"); return; }
    if (strcmp(verb, "SYST") == 0) { reply(c, 215, "UNIX Type: L8"); return; }

    if (!c->logged_in)
    {
        reply(c, 530, "Please login with USER and PASS");
        return;
    }

    if (strcmp(verb, "PWD") == 0 || strcmp(verb, "XPWD") == 0) { cmd_pwd(c); return; }
    if (strcmp(verb, "CWD") == 0) { cmd_cwd(c, arg); return; }
    if (strcmp(verb, "CDUP") == 0) { cmd_cwd(c, ".."); return; }
    if (strcmp(verb, "TYPE") == 0) { cmd_type(c, arg); return; }
    if (strcmp(verb, "PASV") == 0) { cmd_pasv(c); return; }
    if (strcmp(verb, "PORT") == 0) { cmd_port(c, arg); return; }
    if (strcmp(verb, "LIST") == 0) { cmd_list(c, arg, ftpd_data_op_list); return; }
    if (strcmp(verb, "NLST") == 0) { cmd_list(c, arg, ftpd_data_op_nlst); return; }
    if (strcmp(verb, "RETR") == 0) { cmd_retr(c, arg); return; }
    if (strcmp(verb, "STOR") == 0) { cmd_stor(c, arg); return; }
    if (strcmp(verb, "DELE") == 0) { cmd_dele(c, arg); return; }
    if (strcmp(verb, "MKD") == 0 || strcmp(verb, "XMKD") == 0) { cmd_mkd(c, arg); return; }
    if (strcmp(verb, "RMD") == 0 || strcmp(verb, "XRMD") == 0) { cmd_rmd(c, arg); return; }
    if (strcmp(verb, "SIZE") == 0) { cmd_size(c, arg); return; }
    if (strcmp(verb, "ABOR") == 0) { cmd_abor(c); return; }

    reply(c, 502, "Command not implemented");
}
