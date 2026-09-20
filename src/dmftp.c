#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmftp.h"
#include <errno.h>

/**
 * dmftp's transport-agnostic control-connection engine - see the header's
 * top comment. dmod_init()/dmod_deinit() (the actual dmtcp/filesystem-
 * backed FTP *server*) live in src/dmftp_server.c, not here.
 */
struct dmftp
{
    dmftp_callbacks_t callbacks;
    void* user_data;

    char   line[DMFTP_MAX_LINE_LEN];
    size_t line_len;
    bool   line_overflowed; /**< See dmftp_recv()'s doc comment on truncation */
};

static char to_upper_ascii(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/**
 * Splits `session->line` (NUL-terminated by the caller) into an upper-cased
 * verb and a left-trimmed argument, then invokes on_command. Never called
 * with an empty line (dmftp_recv() skips those - a bare CRLF is simply
 * ignored, matching real FTP clients/servers keeping the connection alive
 * with blank lines).
 */
static void dispatch_line(dmftp_t session)
{
    char* line = session->line;

    size_t verb_len = 0;
    while (line[verb_len] != '\0' && line[verb_len] != ' ')
    {
        line[verb_len] = to_upper_ascii(line[verb_len]);
        verb_len++;
    }

    char saved = line[verb_len];
    line[verb_len] = '\0';
    const char* verb = line;

    const char* arg = "";
    if (saved == ' ')
    {
        arg = line + verb_len + 1;
        while (*arg == ' ')
            arg++;
    }

    if (session->callbacks.on_command != NULL)
    {
        session->callbacks.on_command(session, verb, arg, session->user_data);
    }
}

dmod_dmftp_api_declaration(1.0, dmftp_t, _create, ( const dmftp_callbacks_t* callbacks, void* user_data ))
{
    if (callbacks == NULL || callbacks->on_send == NULL)
    {
        return NULL;
    }

    struct dmftp* session = Dmod_Malloc(sizeof(*session));
    if (session == NULL)
    {
        return NULL;
    }

    session->callbacks = *callbacks;
    session->user_data = user_data;
    session->line_len = 0;
    session->line_overflowed = false;

    return session;
}

dmod_dmftp_api_declaration(1.0, void, _destroy, ( dmftp_t session ))
{
    Dmod_Free(session);
}

dmod_dmftp_api_declaration(1.0, int, _recv, ( dmftp_t session, const uint8_t* data, size_t data_len ))
{
    if (session == NULL || (data == NULL && data_len > 0))
    {
        return -EINVAL;
    }

    for (size_t i = 0; i < data_len; i++)
    {
        char byte = (char)data[i];

        if (byte == '\r')
        {
            /* NVT-style line endings use CR LF; the trailing LF (handled
             * below) is what actually terminates the line, so a lone CR
             * is simply dropped rather than stored. */
            continue;
        }

        if (byte == '\n')
        {
            if (session->line_len > 0 && !session->line_overflowed)
            {
                session->line[session->line_len] = '\0';
                dispatch_line(session);
            }
            session->line_len = 0;
            session->line_overflowed = false;
            continue;
        }

        if (session->line_len < DMFTP_MAX_LINE_LEN - 1)
        {
            session->line[session->line_len++] = byte;
        }
        else
        {
            /* Line longer than DMFTP_MAX_LINE_LEN - see the header's doc
             * comment: the overflow is dropped, not merged into the next
             * line, by discarding everything buffered for this line once
             * its terminator finally arrives. */
            session->line_overflowed = true;
        }
    }

    return 0;
}

dmod_dmftp_api_declaration(1.0, int, _reply, ( dmftp_t session, int code, const char* text ))
{
    if (session == NULL || text == NULL || code < 100 || code > 559)
    {
        return -EINVAL;
    }

    char buffer[DMFTP_MAX_LINE_LEN];
    int len = Dmod_SnPrintf(buffer, sizeof(buffer), "%d %s\r\n", code, text);
    if (len < 0)
    {
        return -EINVAL;
    }
    if ((size_t)len >= sizeof(buffer))
    {
        len = (int)sizeof(buffer) - 1; /* Truncated - see the header's doc comment */
    }

    session->callbacks.on_send(session, (const uint8_t*)buffer, (size_t)len, session->user_data);
    return 0;
}
