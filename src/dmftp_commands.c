/* DMOD_ENABLE_REGISTRATION is deliberately NOT set here - see
 * src/dmftp_server.c's top comment for why. */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <stdarg.h>

/**
 * RFC 959 command handling and PASV data transfer for dmftp - the "policy"
 * layer on top of the transport-agnostic engine in dmftp.h/src/dmftp.c and
 * the dmtcp/config wiring in src/dmftp_server.c. See dmftp_internal.h for
 * the shared connection/context structs, and dmftp_server.c's top comment
 * for this server's documented scope (PASV only, binary transfers only,
 * no REST/APPE/rename).
 */

static void reply(dmftp_connection_t* c, int code, const char* text)
{
    dmftp_reply(c->engine, code, text);
}

static void replyf(dmftp_connection_t* c, int code, const char* fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    Dmod_VSnPrintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    dmftp_reply(c->engine, code, buffer);
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
    char raw[DMFTP_MAX_LINE_LEN];

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
 * src/dmftp_server.c's normalize_root()).
 *
 * @return A newly heap-allocated real path, or NULL on allocation failure
 */
static char* build_real_path(const char* virtual_path)
{
    const char* root = g_dmftp_context->root;
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
static void resolve_paths(dmftp_connection_t* c, const char* arg, char** out_virtual, char** out_real)
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

static void finish_transfer(dmftp_connection_t* c, int code, const char* text)
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
    c->data_op = dmftp_data_op_none;
    c->data_conn = NULL;
    c->transfer_ok = false;

    reply(c, code, text);
}

static void send_list_chunk(dmftp_connection_t* c)
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
            return; /* Wait for dmftp_data_on_writable() */

        size_t remaining = c->list_len - c->list_sent;
        size_t want = ((size_t)space < remaining) ? (size_t)space : remaining;

        int sent = dmtcp_send(c->data_conn, c->list_buffer + c->list_sent, want);
        if (sent <= 0)
            return;

        c->list_sent += (size_t)sent;
        if ((size_t)sent < want)
            return; /* Short write - wait for dmftp_data_on_writable() */
    }
}

static void send_retr_chunk(dmftp_connection_t* c)
{
    uint8_t chunk[DMFTP_DATA_CHUNK_SIZE];

    for (;;)
    {
        int space = dmtcp_send_space(c->data_conn);
        if (space <= 0)
            return; /* Wait for dmftp_data_on_writable() */

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

void dmftp_data_begin(dmftp_connection_t* c)
{
    switch (c->data_op)
    {
    case dmftp_data_op_list:
    case dmftp_data_op_nlst:
        send_list_chunk(c);
        break;
    case dmftp_data_op_retr:
        send_retr_chunk(c);
        break;
    case dmftp_data_op_stor:
        break; /* Nothing to do yet - wait for dmftp_data_on_data() */
    case dmftp_data_op_none:
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

void dmftp_data_on_data(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data)
{
    dmftp_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    if (data == NULL)
    {
        /* Peer's FIN - the upload finished cleanly. */
        c->transfer_ok = true;
        dmtcp_close(conn);
        return;
    }

    if (c->data_op == dmftp_data_op_stor && c->transfer_file != NULL)
    {
        Dmod_FileWrite(data, 1, data_len, c->transfer_file);
    }
}

void dmftp_data_on_writable(dmtcp_conn_t conn, size_t space, void* user_data)
{
    (void)space;
    dmftp_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    if (c->data_op == dmftp_data_op_list || c->data_op == dmftp_data_op_nlst)
        send_list_chunk(c);
    else if (c->data_op == dmftp_data_op_retr)
        send_retr_chunk(c);
}

void dmftp_data_on_closed(dmtcp_conn_t conn, void* user_data)
{
    dmftp_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    if (c->transfer_ok)
        finish_transfer(c, 226, "Transfer complete");
    else
        finish_transfer(c, 426, "Connection closed; transfer aborted");
}

void dmftp_data_on_reset(dmtcp_conn_t conn, void* user_data)
{
    dmftp_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    finish_transfer(c, 426, "Connection reset; transfer aborted");
}

void dmftp_data_on_error(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)error;
    dmftp_connection_t* c = user_data;
    if (c->data_conn != conn)
        return;

    finish_transfer(c, 451, "Local error; transfer aborted");
}

/* ============================================================================
 *                      RFC 959 commands
 * ========================================================================== */

static void cmd_user(dmftp_connection_t* c, const char* arg)
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

static void cmd_pass(dmftp_connection_t* c, const char* arg)
{
    if (!c->user_received)
    {
        reply(c, 503, "Login with USER first");
        return;
    }

    const struct dmftp_context* ctx = g_dmftp_context;
    bool ok;
    if (dmftp_is_anonymous_user(ctx->user))
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

static void cmd_quit(dmftp_connection_t* c)
{
    reply(c, 221, "Goodbye");
    if (c->control_conn != NULL)
        dmtcp_close(c->control_conn);
}

static void cmd_pwd(dmftp_connection_t* c)
{
    replyf(c, 257, "\"%s\" is the current directory", c->cwd);
}

static void cmd_type(dmftp_connection_t* c, const char* arg)
{
    if (strcmp(arg, "I") == 0 || strcmp(arg, "L8") == 0)
    {
        c->binary_mode = true;
        reply(c, 200, "Type set to I");
    }
    else if (strcmp(arg, "A") == 0)
    {
        /* Accepted but not actually translated - see this file's top
         * comment / dmftp_server.c's known-limitations list. */
        c->binary_mode = false;
        reply(c, 200, "Type set to A");
    }
    else
    {
        reply(c, 504, "Type not supported");
    }
}

static void cmd_cwd(dmftp_connection_t* c, const char* arg)
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

static void cmd_pasv(dmftp_connection_t* c)
{
    if (c->data_conn != NULL && c->data_op == dmftp_data_op_none)
    {
        /* A previous PASV's data connection arrived but was never claimed
         * by a LIST/RETR/STOR - drop it rather than leaking it. */
        dmtcp_conn_t stale = c->data_conn;
        c->data_conn = NULL;
        dmtcp_abort(stale);
    }

    uint16_t port;
    if (dmftp_server_start_pasv(c, &port) != 0)
    {
        reply(c, 425, "Cannot open passive connection");
        return;
    }

    dmip_addr_t local_addr;
    uint16_t local_port;
    if (dmtcp_conn_get_local_endpoint(c->control_conn, &local_addr, &local_port) != 0 ||
        local_addr.family != dmip_family_v4)
    {
        dmftp_server_stop_pasv(c);
        reply(c, 425, "Cannot open passive connection");
        return;
    }

    replyf(c, 227, "Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
        (unsigned)local_addr.addr.v4[0], (unsigned)local_addr.addr.v4[1],
        (unsigned)local_addr.addr.v4[2], (unsigned)local_addr.addr.v4[3],
        (unsigned)(port >> 8), (unsigned)(port & 0xFFu));
}

static void cmd_list(dmftp_connection_t* c, const char* arg, dmftp_data_op_t op)
{
    if (c->data_conn == NULL && !c->pasv_pending)
    {
        reply(c, 425, "Use PASV first");
        return;
    }
    if (c->data_op != dmftp_data_op_none)
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

    uint8_t* buffer = NULL;
    size_t len = 0;
    bool ok = render_listing(real_path, op == dmftp_data_op_nlst, &buffer, &len);
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
    reply(c, 150, "Here comes the directory listing");

    /* The data connection may already be sitting there, idle, from a PASV
     * accept that arrived before this command did (the common client
     * ordering - see dmftp_data_begin()'s doc comment). If so, kick off
     * sending right away instead of waiting for an accept that already
     * happened. */
    if (c->data_conn != NULL)
        dmftp_data_begin(c);
}

static void cmd_retr(dmftp_connection_t* c, const char* arg)
{
    if (c->data_conn == NULL && !c->pasv_pending)
    {
        reply(c, 425, "Use PASV first");
        return;
    }
    if (c->data_op != dmftp_data_op_none)
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

    c->data_op = dmftp_data_op_retr;
    c->transfer_file = file;
    reply(c, 150, "Opening binary mode data connection for file transfer");

    /* See cmd_list()'s own comment on the data connection possibly already
     * being there. */
    if (c->data_conn != NULL)
        dmftp_data_begin(c);
}

static void cmd_stor(dmftp_connection_t* c, const char* arg)
{
    if (c->data_conn == NULL && !c->pasv_pending)
    {
        reply(c, 425, "Use PASV first");
        return;
    }
    if (c->data_op != dmftp_data_op_none)
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

    c->data_op = dmftp_data_op_stor;
    c->transfer_file = file;
    reply(c, 150, "Ready to receive file");
}

static void cmd_dele(dmftp_connection_t* c, const char* arg)
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

static void cmd_mkd(dmftp_connection_t* c, const char* arg)
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

static void cmd_rmd(dmftp_connection_t* c, const char* arg)
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

static void cmd_size(dmftp_connection_t* c, const char* arg)
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

static void cmd_abor(dmftp_connection_t* c)
{
    dmtcp_conn_t victim = c->data_conn;
    finish_transfer(c, 226, "ABOR command successful");
    if (victim != NULL)
        dmtcp_abort(victim);
}

void dmftp_handle_command(dmftp_t engine, const char* verb, const char* arg, void* user_data)
{
    (void)engine;
    dmftp_connection_t* c = user_data;

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
    if (strcmp(verb, "PORT") == 0) { reply(c, 502, "Active mode (PORT) is not implemented - use PASV"); return; }
    if (strcmp(verb, "LIST") == 0) { cmd_list(c, arg, dmftp_data_op_list); return; }
    if (strcmp(verb, "NLST") == 0) { cmd_list(c, arg, dmftp_data_op_nlst); return; }
    if (strcmp(verb, "RETR") == 0) { cmd_retr(c, arg); return; }
    if (strcmp(verb, "STOR") == 0) { cmd_stor(c, arg); return; }
    if (strcmp(verb, "DELE") == 0) { cmd_dele(c, arg); return; }
    if (strcmp(verb, "MKD") == 0 || strcmp(verb, "XMKD") == 0) { cmd_mkd(c, arg); return; }
    if (strcmp(verb, "RMD") == 0 || strcmp(verb, "XRMD") == 0) { cmd_rmd(c, arg); return; }
    if (strcmp(verb, "SIZE") == 0) { cmd_size(c, arg); return; }
    if (strcmp(verb, "ABOR") == 0) { cmd_abor(c); return; }

    reply(c, 502, "Command not implemented");
}
