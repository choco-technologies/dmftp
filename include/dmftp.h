#ifndef DMFTP_H
#define DMFTP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmftp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * dmftp - a transport-agnostic FTP (RFC 959) control-connection engine.
 *
 * dmftp never touches a socket or a filesystem itself: it only knows how to
 * split a byte stream into CRLF (or bare LF, tolerated for lenient clients)
 * terminated command lines, split each line into a verb/argument pair, and
 * format a numeric reply line back out - the same "engine fed bytes, calls
 * callbacks, caller owns policy" shape dmtelnet uses for Telnet framing (see
 * https://github.com/choco-technologies/dmtelnet). Everything that actually
 * makes this a working FTP *server* - dmtcp control/data connections,
 * authentication, the virtual filesystem root, PASV data transfer, LIST
 * rendering - lives in this same module's src/dmftp_server.c/
 * src/dmftp_commands.c, layered on top of this engine exactly like
 * telnetd.c layers dmtcp/dmtty on top of dmtelnet.
 *
 * A command line is capped at DMFTP_MAX_LINE_LEN bytes - generous for every
 * real FTP command (even a deep RETR/STOR path), and a fixed, tightly
 * bounded size rather than an attacker-growable heap buffer. A line that
 * exceeds the cap has its overflow silently dropped (the buffered prefix is
 * still parsed once the terminator arrives) rather than being merged with
 * whatever comes next.
 */

/* Opaque handle - the real struct is defined in src/dmftp.c */
typedef struct dmftp* dmftp_t;

/**
 * Maximum number of bytes buffered for one not-yet-terminated command line.
 * RFC 959 does not fix a limit; this matches the conventional bound used by
 * vsftpd/Postfix-style line-oriented protocol servers - generous for any
 * real path/argument, small enough to bound per-connection RAM use.
 */
#define DMFTP_MAX_LINE_LEN 512u

/**
 * Fires once per complete command line: `verb` is upper-cased (FTP verbs
 * are case-insensitive per RFC 959, real clients almost always send
 * upper-case already) and NUL-terminated; `arg` is the remainder of the
 * line with leading spaces stripped, NUL-terminated, and never NULL (an
 * argument-less command reports "" ). Both are borrowed, valid only for the
 * duration of the call - copy out anything to keep.
 */
typedef void (*dmftp_command_handler_t)(dmftp_t session, const char* verb, const char* arg, void* user_data);

/**
 * The only way dmftp ever produces wire bytes - dmftp_reply() ends up
 * calling this once per call. `data`/`data_len` are borrowed, valid only
 * for the duration of the call (e.g. hand them straight to dmtcp_send()).
 *
 * Required: dmftp_create() fails if this is NULL, since a session with no
 * way to emit bytes cannot do anything useful.
 */
typedef void (*dmftp_send_handler_t)(dmftp_t session, const uint8_t* data, size_t data_len, void* user_data);

/**
 * Callbacks for one control-connection session. on_command may be left
 * NULL (a session that only ever gets replied to from outside), in which
 * case complete command lines are simply dropped; on_send is required.
 */
typedef struct
{
    dmftp_command_handler_t on_command;
    dmftp_send_handler_t    on_send;
} dmftp_callbacks_t;

/**
 * Create a new FTP control-connection session.
 *
 * @param callbacks Copied - the pointer need not outlive this call.
 *                  callbacks->on_send must be non-NULL.
 * @param user_data Opaque pointer passed back to every callback.
 *
 * @return A valid handle on success, or NULL if callbacks is NULL,
 *         callbacks->on_send is NULL, or on allocation failure.
 */
dmod_dmftp_api(1.0, dmftp_t, _create, ( const dmftp_callbacks_t* callbacks, void* user_data ));

/**
 * Destroy a session created by dmftp_create(). Safe to call with NULL.
 */
dmod_dmftp_api(1.0, void, _destroy, ( dmftp_t session ));

/**
 * Feed newly-received raw bytes into the session. Synchronously invokes
 * on_command zero or more times before returning, once per complete
 * (CRLF- or bare-LF-terminated) line found in `data`.
 *
 * @return 0 on success, -EINVAL if `session` is NULL, or if `data` is NULL
 *         while `data_len` is nonzero.
 */
dmod_dmftp_api(1.0, int, _recv, ( dmftp_t session, const uint8_t* data, size_t data_len ));

/**
 * Format and send a standard "CODE text\r\n" reply line (RFC 959 §4.2),
 * e.g. dmftp_reply(session, 230, "Login successful").
 *
 * The formatted line is capped at DMFTP_MAX_LINE_LEN bytes (a `text` long
 * enough to overflow that, e.g. a pathological ~500-byte path in a PWD
 * reply, is truncated rather than growing the reply arbitrarily).
 *
 * @return 0 on success, -EINVAL if `session`/`text` is NULL or `code` is
 *         not a 3-digit reply code (100-559)
 */
dmod_dmftp_api(1.0, int, _reply, ( dmftp_t session, int code, const char* text ));

#ifdef __cplusplus
}
#endif

#endif // DMFTP_H
