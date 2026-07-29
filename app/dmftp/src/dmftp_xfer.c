/**
 * @file dmftp_xfer.c
 * @brief The transfer engine - one data connection's worth of bytes moving
 *        between a file (or a generated directory listing) and dmtcp
 *
 * @par The pump
 * Outbound transfers are driven entirely by dmtcp's on_writable callback.
 * dmftp_xfer_pump() fills `scratch` from the source, hands it to
 * dmtcp_send(), and stops the moment dmtcp short-writes - which is exactly
 * when dmtcp arms its edge-triggered on_writable latch, so the next
 * space-reclaiming ACK calls straight back in here. A 4 MB RETR therefore
 * needs no thread, no timer and no polling loop; it advances at precisely
 * the rate the peer acknowledges data.
 *
 * Inbound transfers need none of that: dmtcp hands the bytes over as they
 * arrive and dmftp_xfer_receive() writes them through.
 *
 * @par One transfer, one owner, one completion
 * dmftp_xfer_finish() is idempotent, because a transfer can end in several
 * ways at once - the source runs out, the peer sends FIN, a RST arrives,
 * the session is torn down. The first one wins and the owner's completion
 * hook fires exactly once; the owner is what frees the transfer.
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

/**
 * @brief Cap on an unterminated line in a client-side listing
 *
 * A server that never sends a newline would otherwise grow `scratch`
 * without bound. Past this, whatever has accumulated is delivered as a
 * line - malformed input costs a strange-looking listing entry, not the
 * heap.
 */
#define DMFTP_LIST_LINE_CAP (DMFTP_LINE_MAX * 4u)

struct dmftp_xfer* dmftp_xfer_create(void* owner, dmftp_owner_kind_t kind, bool to_peer)
{
    struct dmftp_xfer* xfer = Dmod_Malloc(sizeof(*xfer));
    if (xfer == NULL)
        return NULL;

    memset(xfer, 0, sizeof(*xfer));
    xfer->magic = DMFTP_XFER_MAGIC;
    xfer->owner_kind = kind;
    xfer->owner = owner;
    xfer->to_peer = to_peer;
    xfer->result = 0;
    dmftp_buf_init(&xfer->scratch);
    return xfer;
}

void dmftp_xfer_destroy(struct dmftp_xfer* xfer)
{
    if (xfer == NULL || xfer->magic != DMFTP_XFER_MAGIC)
        return;

    if (xfer->file != NULL)
    {
        Dmod_FileClose(xfer->file);
    }
    if (xfer->dir != NULL)
    {
        Dmod_CloseDir(xfer->dir);
    }
    Dmod_Free(xfer->dir_path);
    Dmod_Free(xfer->raw);
    dmftp_buf_free(&xfer->scratch);

    xfer->magic = 0;
    Dmod_Free(xfer);
}

void dmftp_xfer_set_file(struct dmftp_xfer* xfer, void* file, bool ascii)
{
    xfer->file = file;
    xfer->ascii = ascii;

    if (xfer->to_peer)
    {
        xfer->src = dmftp_src_file;
        xfer->raw = Dmod_Malloc(DMFTP_CHUNK_LEN);
    }
    else
    {
        xfer->dst = dmftp_dst_file;
    }
}

void dmftp_xfer_set_dir(struct dmftp_xfer* xfer, void* dir, char* dir_path, bool long_format)
{
    xfer->src = dmftp_src_dir;
    xfer->dir = dir;
    xfer->dir_path = dir_path; /* ownership moves here */
    xfer->long_format = long_format;
}

void dmftp_xfer_set_lines(struct dmftp_xfer* xfer)
{
    xfer->dst = dmftp_dst_lines;
}

/* ============================================================================
 *                      Directory listing format
 * ========================================================================== */

int dmftp_xfer_format_entry(dmftp_buf_t* out, const char* dir_os_path, const char* name, bool is_dir, bool long_format)
{
    if (!long_format)
    {
        int result = dmftp_buf_append(out, name, strlen(name));
        return result != 0 ? result : dmftp_buf_append(out, "\r\n", 2u);
    }

    uint32_t size = 0;
    if (!is_dir)
    {
        char* entry_path = dmftp_str_join(dir_os_path, "/", name);
        if (entry_path != NULL)
        {
            /* Best effort: an entry that cannot be opened (a device node, a
             * file another module holds exclusively) is still listed, just
             * with a size of 0 - dropping it would be worse. */
            (void)dmftp_path_file_size(entry_path, &size);
            Dmod_Free(entry_path);
        }
    }

    /* `ls -l`-shaped, which is the dialect every FTP client knows how to
     * parse. The timestamp is a fixed placeholder: the SAL reports no
     * mtime (see dmftp.h), and clients treat the field as advisory. */
    char prefix[64];
    int written = Dmod_SnPrintf(prefix, sizeof(prefix), "%s 1 ftp ftp %10u Jan  1  1970 ",
                                 is_dir ? "drwxr-xr-x" : "-rw-r--r--", (unsigned)size);
    if (written <= 0 || (size_t)written >= sizeof(prefix))
        return -ENOSPC;

    int result = dmftp_buf_append(out, prefix, (size_t)written);
    if (result != 0)
        return result;

    result = dmftp_buf_append(out, name, strlen(name));
    if (result != 0)
        return result;

    return dmftp_buf_append(out, "\r\n", 2u);
}

/* ============================================================================
 *                      Sources
 * ========================================================================== */

/**
 * @brief Copy `len` bytes into `scratch`, turning every bare LF into CRLF
 *
 * RFC 959 §3.1.1.1's ASCII type. A CR that is already there is passed
 * through untouched, so a file stored with CRLF endings does not come back
 * with doubled CRs.
 */
static int append_ascii(dmftp_buf_t* scratch, const uint8_t* data, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (data[i] != '\n' || (i > 0 && data[i - 1u] == '\r'))
            continue;

        int result = dmftp_buf_append(scratch, data + start, i - start);
        if (result != 0)
            return result;

        result = dmftp_buf_append(scratch, "\r", 1u);
        if (result != 0)
            return result;

        start = i;
    }
    return dmftp_buf_append(scratch, data + start, len - start);
}

/**
 * @brief Read the next block of the source file into `scratch`
 *
 * @return 0 on success (which includes "nothing left", flagged through
 *         source_eof), -EIO on a read error, -ENOMEM
 */
static int fill_from_file(struct dmftp_xfer* xfer)
{
    if (xfer->raw == NULL)
        return -ENOMEM;

    size_t read = Dmod_FileRead(xfer->raw, 1u, DMFTP_CHUNK_LEN, xfer->file);
    if (read == 0)
    {
        xfer->source_eof = true;
        return 0;
    }
    if (read < DMFTP_CHUNK_LEN)
    {
        xfer->source_eof = true; /* a short read is end-of-file for the SAL */
    }

    return xfer->ascii ? append_ascii(&xfer->scratch, xfer->raw, read)
                       : dmftp_buf_append(&xfer->scratch, xfer->raw, read);
}

/**
 * @brief Format the next directory entries into `scratch`
 *
 * Generated a block at a time rather than rendered up front: a directory
 * with thousands of entries would otherwise have to fit in RAM in one
 * piece before a single byte reached the wire.
 */
static int fill_from_dir(struct dmftp_xfer* xfer)
{
    while (xfer->scratch.len < DMFTP_CHUNK_LEN)
    {
        const Dmod_DirEntry_t* entry = Dmod_ReadDirEx(xfer->dir);
        if (entry == NULL)
        {
            xfer->source_eof = true;
            return 0;
        }

        /* "." and ".." are navigation, not content - real servers omit
         * them from LIST and clients synthesize their own. */
        if (entry->name[0] == '.' &&
            (entry->name[1] == '\0' || (entry->name[1] == '.' && entry->name[2] == '\0')))
            continue;

        int result = dmftp_xfer_format_entry(&xfer->scratch, xfer->dir_path, entry->name,
                                              entry->type == Dmod_DirEntryType_Dir, xfer->long_format);
        if (result != 0)
            return result;
    }
    return 0;
}

static int fill_scratch(struct dmftp_xfer* xfer)
{
    switch (xfer->src)
    {
        case dmftp_src_file:
            return fill_from_file(xfer);
        case dmftp_src_dir:
            return fill_from_dir(xfer);
        case dmftp_src_none:
        default:
            xfer->source_eof = true;
            return 0;
    }
}

/* ============================================================================
 *                      The pump
 * ========================================================================== */

void dmftp_xfer_pump(struct dmftp_xfer* xfer)
{
    if (xfer == NULL || xfer->magic != DMFTP_XFER_MAGIC)
        return;
    if (!xfer->to_peer || xfer->finished || xfer->conn == NULL)
        return;

    while (true)
    {
        if (xfer->scratch.len == 0)
        {
            if (xfer->source_eof)
            {
                dmftp_xfer_finish(xfer, 0);
                return;
            }

            int result = fill_scratch(xfer);
            if (result != 0)
            {
                dmftp_xfer_finish(xfer, result);
                return;
            }
            if (xfer->scratch.len == 0)
            {
                /* Nothing produced: either the source is done (finish on
                 * the next turn of the loop) or it genuinely had nothing
                 * to give this time, and on_writable will call back. */
                if (!xfer->source_eof)
                    return;
                continue;
            }
        }

        int sent = dmtcp_send(xfer->conn, xfer->scratch.data, xfer->scratch.len);
        if (sent < 0)
        {
            dmftp_xfer_finish(xfer, sent);
            return;
        }

        size_t taken = (size_t)sent;
        bool   short_write = taken < xfer->scratch.len;
        xfer->bytes += taken;
        dmftp_buf_consume(&xfer->scratch, taken);

        /* dmtcp armed on_writable on that short write - stop here and let
         * the peer's next ACK drive the rest. */
        if (short_write)
            return;
    }
}

/* ============================================================================
 *                      Sinks
 * ========================================================================== */

/**
 * @brief Write `len` bytes to the sink file, undoing ASCII CRLF pairs
 *
 * The mirror of append_ascii(). `pending_cr` carries the state across
 * segment boundaries, since a CRLF can be split between two of them.
 *
 * @return 0 on success, -EIO on a short write
 */
static int write_ascii(struct dmftp_xfer* xfer, const uint8_t* data, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (data[i] != '\r')
            continue;

        /* Drop the CR only if an LF really follows it; a lone CR is data. */
        bool lf_follows = (i + 1u < len) ? (data[i + 1u] == '\n') : true;
        if (!lf_follows)
            continue;

        if (i > start && Dmod_FileWrite(data + start, 1u, i - start, xfer->file) != i - start)
            return -EIO;

        start = i + 1u;
        if (i + 1u == len)
        {
            xfer->pending_cr = true; /* resolve against the next segment */
        }
    }

    size_t remaining = len - start;
    if (remaining > 0 && Dmod_FileWrite(data + start, 1u, remaining, xfer->file) != remaining)
        return -EIO;

    return 0;
}

/**
 * @brief Emit every complete line accumulated in `scratch` to the client's
 *        on_list handler
 */
static void drain_lines(struct dmftp_xfer* xfer)
{
    struct dmftp_client* client = (struct dmftp_client*)xfer->owner;

    for (;;)
    {
        size_t newline = 0;
        bool   found = false;
        for (size_t i = 0; i < xfer->scratch.len; i++)
        {
            if (xfer->scratch.data[i] == '\n')
            {
                newline = i;
                found = true;
                break;
            }
        }

        if (!found)
        {
            if (xfer->scratch.len < DMFTP_LIST_LINE_CAP)
                return;
            newline = xfer->scratch.len; /* over the cap - deliver what we have */
        }

        size_t text_len = newline;
        if (text_len > 0 && xfer->scratch.data[text_len - 1u] == '\r')
        {
            text_len--;
        }

        char* line = dmftp_str_ndup((const char*)xfer->scratch.data, text_len);
        if (line != NULL)
        {
            dmftp_client_xfer_line(client, line);
            Dmod_Free(line);
        }

        dmftp_buf_consume(&xfer->scratch, found ? newline + 1u : newline);
    }
}

void dmftp_xfer_receive(struct dmftp_xfer* xfer, const uint8_t* data, size_t len)
{
    if (xfer == NULL || xfer->magic != DMFTP_XFER_MAGIC || xfer->finished || len == 0)
        return;

    xfer->bytes += len;

    if (xfer->dst == dmftp_dst_file)
    {
        int result;
        if (!xfer->ascii)
        {
            result = (Dmod_FileWrite(data, 1u, len, xfer->file) == len) ? 0 : -EIO;
        }
        else
        {
            /* A CR that ended the previous segment is only a line ending if
             * this one starts with LF; otherwise it was real data. */
            if (xfer->pending_cr)
            {
                xfer->pending_cr = false;
                if (data[0] != '\n' && Dmod_FileWrite("\r", 1u, 1u, xfer->file) != 1u)
                {
                    dmftp_xfer_finish(xfer, -EIO);
                    return;
                }
            }
            result = write_ascii(xfer, data, len);
        }

        if (result != 0)
        {
            dmftp_xfer_finish(xfer, result);
        }
        return;
    }

    if (xfer->dst == dmftp_dst_lines)
    {
        if (dmftp_buf_append(&xfer->scratch, data, len) != 0)
        {
            dmftp_xfer_finish(xfer, -ENOMEM);
            return;
        }
        drain_lines(xfer);
    }
}

/* ============================================================================
 *                      Attach and finish
 * ========================================================================== */

void dmftp_xfer_attach(struct dmftp_xfer* xfer, dmtcp_conn_t conn)
{
    if (xfer == NULL || xfer->magic != DMFTP_XFER_MAGIC || xfer->finished)
        return;

    xfer->conn = conn;
    if (xfer->to_peer)
    {
        dmftp_xfer_pump(xfer);
    }
}

/**
 * @brief Deliver whatever is left of an inbound listing that never ended
 *        with a newline
 */
static void flush_trailing_line(struct dmftp_xfer* xfer)
{
    if (xfer->dst != dmftp_dst_lines || xfer->scratch.len == 0)
        return;

    char* line = dmftp_str_ndup((const char*)xfer->scratch.data, xfer->scratch.len);
    if (line != NULL)
    {
        dmftp_client_xfer_line((struct dmftp_client*)xfer->owner, line);
        Dmod_Free(line);
    }
    xfer->scratch.len = 0;
}

void dmftp_xfer_finish(struct dmftp_xfer* xfer, int result)
{
    if (xfer == NULL || xfer->magic != DMFTP_XFER_MAGIC || xfer->finished)
        return;

    xfer->finished = true;
    xfer->result = result;

    flush_trailing_line(xfer);

    if (xfer->file != NULL)
    {
        Dmod_FileClose(xfer->file);
        xfer->file = NULL;
    }
    if (xfer->dir != NULL)
    {
        Dmod_CloseDir(xfer->dir);
        xfer->dir = NULL;
    }

    /* Closing the data connection is how FTP signals end-of-file in stream
     * mode (RFC 959 §3.4). A NULL conn means dmtcp is already tearing the
     * TCB down and told us so - see dmftp_net.c's data_on_terminal(). */
    if (xfer->conn != NULL)
    {
        dmftp_net_detach(xfer->conn); /* the owner may be freed before the close completes */
        dmtcp_close(xfer->conn);
        xfer->conn = NULL;
    }

    /* The owner reports the outcome on the control channel and then frees
     * this transfer - nothing may touch `xfer` past this call. */
    if (xfer->owner_kind == dmftp_owner_session)
    {
        dmftp_server_xfer_done((struct dmftp_session*)xfer->owner, result, xfer->bytes);
    }
    else
    {
        dmftp_client_xfer_done((struct dmftp_client*)xfer->owner, result, xfer->bytes);
    }
}
