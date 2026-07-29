#ifndef DMFTP_H
#define DMFTP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmip.h"
#include "dmftp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmftp.h
 * @brief DMOD FTP - Public API (RFC 959 server and client)
 *
 * dmftp speaks File Transfer Protocol over dmtcp: a server that exposes
 * this device's filesystem, and a client that talks to somebody else's.
 * Both halves share one control-line codec, one jail-safe path resolver,
 * and one transfer engine, so a fix to any of the three lands in both.
 *
 * @par Files come from the DMOD SAL, not from dmvfs
 * Every filesystem operation goes through dmod's own SAL
 * (Dmod_FileOpen()/_FileRead()/_FileWrite()/_OpenDir()/_ReadDirEx()/
 * _MakeDir()/_RemoveDir()/_FileRemove()/_Rename()/_Access()) - the same
 * calls the dmell commands (ls, cat, cp) use, and for the same reason:
 * whatever dmvfs has mounted is reachable through them with no dependency
 * on dmvfs itself (which publishes no dmf-get package and therefore cannot
 * be a dmod_link_modules() dependency). One consequence is visible in the
 * protocol: the SAL reports a directory entry's name and type but not its
 * size or timestamp, so LIST stats each entry by opening it, and MDTM has
 * no timestamp to report at all - see docs/dmftp.md's "What the SAL does
 * not give us" section, and note that MDTM is deliberately absent from the
 * FEAT reply because of it.
 *
 * @par Everything is callback-driven; nothing here blocks
 * There is no dmftp thread. Control lines and inbound data arrive inline
 * on whatever thread pumps the interface (dmtcp's documented delivery
 * context), and outbound file data is pushed from dmtcp's on_writable
 * callback as the peer's ACKs free buffer space. A RETR of a 4 MB file
 * therefore costs no thread and no timer - see docs/dmftp.md's "The
 * transfer pump" section.
 *
 * @par IPv4 only, inherited from dmtcp
 * dmtcp cannot originate an IPv6 segment (no NDP module exists yet), so
 * neither can dmftp: PASV/PORT carry IPv4 host-port tuples, and EPSV/EPRT
 * (RFC 2428) are answered 502. This is a limitation of the layer below,
 * not a scope decision here.
 */

/* ============================================================================
 *                      Well-known ports and limits
 * ========================================================================== */

/** @brief RFC 959 control port */
#define DMFTP_PORT_CONTROL 21u

/**
 * @brief RFC 959 §3.2 default server-side data port (L-1)
 *
 * Only used for an active-mode (PORT) data connection, and only when a
 * server is configured to ask for it - see dmftp_server_config_t's
 * `active_data_port`, which defaults to an ephemeral port instead.
 */
#define DMFTP_PORT_DATA_DEFAULT 20u

/**
 * @brief Longest command verb RFC 959 defines (four characters: RETR,
 *        STOR, XMKD, ...)
 */
#define DMFTP_VERB_MAX 4u

/**
 * @brief Longest control line dmftp will accept, CRLF included
 *
 * A line longer than this is answered 500 and discarded rather than
 * buffered - an unbounded control line is the one place a remote peer
 * could otherwise drive this module's heap use directly.
 */
#define DMFTP_LINE_MAX 512u

/* ============================================================================
 *                      Shared protocol types
 * ========================================================================== */

/**
 * @brief RFC 959 §3.1.1 representation type
 *
 * dmftp_type_ascii really does translate: a download converts a bare LF to
 * CRLF on the wire, an upload converts CRLF back to a bare LF. Clients that
 * issue `TYPE I` (all of them, for anything but plain text) get the bytes
 * through untouched.
 */
typedef enum
{
    dmftp_type_ascii = 0,
    dmftp_type_image,
} dmftp_type_t;

/**
 * @brief How the data connection for a transfer gets established
 *
 * Passive: the server listens and the client connects to it (PASV/227).
 * Active: the client listens and tells the server where to connect back to
 * (PORT/200). Both are supported in both roles.
 */
typedef enum
{
    dmftp_data_passive = 0,
    dmftp_data_active,
} dmftp_data_mode_t;

/**
 * @brief One parsed control-channel command line
 *
 * `verb` is upper-cased during parsing, so a comparison against a literal
 * ("RETR") needs no case handling. `arg` points into the caller's own line
 * buffer - it is borrowed, not owned, and is NULL when the command had no
 * argument.
 */
typedef struct
{
    char        verb[DMFTP_VERB_MAX + 1];
    const char* arg;
    size_t      arg_len;
} dmftp_command_t;

/* ============================================================================
 *                      Control-line codec
 * ========================================================================== */

/**
 * @brief Parse one control line (CRLF already stripped) into verb + argument
 *
 * Also strips the Telnet IAC sequences a strict client may prefix an ABOR
 * with (RFC 959 §4.1.3.5), so the verb is found even behind them.
 *
 * @param line   Line contents, without its terminating CRLF
 * @param length Number of valid bytes in `line`
 * @param out    Output: the parsed command
 *
 * @return 0 on success, -EINVAL on a NULL argument, -EPROTO for an empty
 *         line or a verb longer than DMFTP_VERB_MAX
 */
dmod_dmftp_api(1.0, int, _parse_command, ( const char* line, size_t length, dmftp_command_t* out ));

/**
 * @brief Format one single-line reply ("214 Help OK.\r\n") into `buffer`
 *
 * Single-line only, by design: RFC 959 §4.2's multi-line form (FEAT, HELP)
 * is composed by the caller out of several of these plus its own
 * continuation markers, rather than hidden behind a text-scanning special
 * case here.
 *
 * @param buffer     Output buffer
 * @param buffer_len Size of `buffer` in bytes
 * @param code       Three-digit reply code (100..599)
 * @param text       Reply text, with no CRLF of its own
 * @param out_len    Output: bytes written, CRLF included (may be NULL)
 *
 * @return 0 on success, -EINVAL on a NULL argument or out-of-range `code`,
 *         -ENOSPC if the formatted reply would not fit in `buffer`
 */
dmod_dmftp_api(1.0, int, _format_reply, ( char* buffer, size_t buffer_len, int code, const char* text, size_t* out_len ));

/**
 * @brief Parse one reply line from a server
 *
 * @param line      Line contents, without its terminating CRLF
 * @param length    Number of valid bytes in `line`
 * @param out_code  Output: the three-digit reply code
 * @param out_final Output: false for a `250-` continuation line, true for
 *                   the `250 ` line that ends the reply
 * @param out_text  Output: borrowed pointer to the text after the code
 *
 * @return 0 on success, -EINVAL on a NULL argument, -EPROTO if the line
 *         does not start with three digits followed by a space or '-'
 */
dmod_dmftp_api(1.0, int, _parse_reply, ( const char* line, size_t length, int* out_code, bool* out_final, const char** out_text ));

/**
 * @brief Format an IPv4 address + port as RFC 959's `h1,h2,h3,h4,p1,p2`
 *
 * @param buffer     Output buffer (24 bytes is always enough)
 * @param buffer_len Size of `buffer` in bytes
 * @param addr       Address to encode (family must be dmip_family_v4)
 * @param port       Port to encode
 * @param out_len    Output: bytes written, NUL terminator excluded (may be
 *                    NULL)
 *
 * @return 0 on success, -EINVAL on a NULL/non-IPv4 argument, -ENOSPC if
 *         `buffer` is too small
 */
dmod_dmftp_api(1.0, int, _format_host_port, ( char* buffer, size_t buffer_len, const dmip_addr_t* addr, uint16_t port, size_t* out_len ));

/**
 * @brief Parse RFC 959's `h1,h2,h3,h4,p1,p2` back into an address + port
 *
 * Tolerates the spaces some clients insert after the commas.
 *
 * @param text     NUL-terminated tuple text
 * @param out_addr Output: the decoded IPv4 address
 * @param out_port Output: the decoded port
 *
 * @return 0 on success, -EINVAL on a NULL argument, -EPROTO if the text is
 *         not six decimal fields in range
 */
dmod_dmftp_api(1.0, int, _parse_host_port, ( const char* text, dmip_addr_t* out_addr, uint16_t* out_port ));

/* ============================================================================
 *                      Server
 * ========================================================================== */

/**
 * @brief Opaque handle to one FTP server (one listening control port)
 */
typedef struct dmftp_server* dmftp_server_t;

/**
 * @brief Opaque handle to one client's control connection to a server
 *
 * @par Handle lifetime
 * Valid from the dmftp_session_handler_t that announces it until the
 * matching on_session_close returns, at which point it is freed. The
 * handle passed to dmftp_auth_handler_t is live for the duration of that
 * call and afterwards, since authentication happens mid-session.
 */
typedef struct dmftp_session* dmftp_session_t;

/**
 * @brief Decide whether a USER/PASS pair may log in
 *
 * Called once per PASS command, on the thread delivering the control line.
 * A handler is free to call dmftp_session_set_root()/_set_read_only() from
 * inside it to give this particular user their own home directory and
 * write permission - those calls take effect before the 230 reply goes
 * out.
 *
 * `password` is NULL when the client sent USER and then something other
 * than PASS - only reachable for a server that wants to allow anonymous
 * access, where answering true lets the session in without a password.
 *
 * @return true to accept the login (230), false to reject it (530)
 */
typedef bool (*dmftp_auth_handler_t)( dmftp_session_t session, const char* user, const char* password, void* user_data );

/**
 * @brief Announces a session opening or closing
 *
 * on_session_open fires as soon as the control connection is established,
 * before the 220 greeting and long before any login. on_session_close
 * fires immediately before the session is freed - the handle must not be
 * used after it returns.
 */
typedef void (*dmftp_session_handler_t)( dmftp_session_t session, void* user_data );

/**
 * @brief Server-wide callbacks - only on_auth is required
 */
typedef struct
{
    dmftp_auth_handler_t    on_auth;
    dmftp_session_handler_t on_session_open;
    dmftp_session_handler_t on_session_close;
} dmftp_server_callbacks_t;

/**
 * @brief Server configuration, copied by dmftp_server_create()
 *
 * Every string member is deep-copied, so nothing here needs to outlive the
 * create call.
 */
typedef struct
{
    /** @brief Control port to listen on; 0 means DMFTP_PORT_CONTROL */
    uint16_t port;

    /**
     * @brief Filesystem path every session is confined to; NULL means "/"
     *
     * A session can never resolve a path outside this subtree: a `..` that
     * would climb past it is clamped to the root, the same thing a real
     * chrooted FTP server does. dmftp_session_set_root() can narrow it
     * further per user from inside on_auth.
     */
    const char* root;

    /** @brief Reject every mutating command (STOR/DELE/MKD/...) with 550 */
    bool read_only;

    /** @brief Answer PASV; both modes are enabled when neither is set */
    bool allow_passive;

    /** @brief Answer PORT; both modes are enabled when neither is set */
    bool allow_active;

    /**
     * @brief Local port for an active-mode data connection; 0 (the
     *        default) picks an ephemeral one
     *
     * Set to DMFTP_PORT_DATA_DEFAULT for RFC 959's classic behaviour, which
     * some firewalls still expect to see.
     */
    uint16_t active_data_port;

    /** @brief Maximum concurrent sessions; 0 means unlimited */
    uint32_t max_sessions;

    /** @brief Text of the 220 greeting; NULL uses a built-in default */
    const char* banner;
} dmftp_server_config_t;

/**
 * @brief Create a server - does not listen yet, see dmftp_server_start()
 *
 * @param config    Configuration (copied). NULL uses every default.
 * @param callbacks Callbacks (copied). Must be non-NULL with a non-NULL
 *                   on_auth - a server that cannot decide who may log in
 *                   would have to let everybody in, which is never the
 *                   safe default.
 * @param user_data Opaque pointer handed back to every callback
 *
 * @return The new server, or NULL on a bad argument or allocation failure
 */
dmod_dmftp_api(1.0, dmftp_server_t, _server_create, ( const dmftp_server_config_t* config, const dmftp_server_callbacks_t* callbacks, void* user_data ));

/**
 * @brief Start listening on the configured control port
 *
 * @return 0 on success, -EINVAL if `server` is NULL/invalid, -EALREADY if
 *         it is already running, or whatever dmtcp_listen() returned
 *         (-EEXIST if the port is taken, -ENOMEM)
 */
dmod_dmftp_api(1.0, int, _server_start, ( dmftp_server_t server ));

/**
 * @brief Stop accepting new connections and close every live session
 *
 * Idempotent. A session's on_session_close fires for each one closed here.
 *
 * @return 0 on success, -EINVAL if `server` is NULL/invalid
 */
dmod_dmftp_api(1.0, int, _server_stop, ( dmftp_server_t server ));

/**
 * @brief Stop (if running) and free a server
 */
dmod_dmftp_api(1.0, void, _server_destroy, ( dmftp_server_t server ));

/**
 * @return The control port the server listens on, or 0 if `server` is
 *         NULL/invalid
 */
dmod_dmftp_api(1.0, uint16_t, _server_get_port, ( dmftp_server_t server ));

/**
 * @return How many sessions are currently open, or 0 if `server` is
 *         NULL/invalid
 */
dmod_dmftp_api(1.0, size_t, _server_get_session_count, ( dmftp_server_t server ));

/* ---- Per-session accessors ---- */

/**
 * @return The name given by USER, or NULL before USER arrives / if
 *         `session` is NULL/invalid. Borrowed - valid until the session
 *         closes.
 */
dmod_dmftp_api(1.0, const char*, _session_get_user, ( dmftp_session_t session ));

/**
 * @brief Read the address/port the client connected from
 *
 * @return 0 on success, -EINVAL on a NULL/invalid argument
 */
dmod_dmftp_api(1.0, int, _session_get_peer, ( dmftp_session_t session, dmip_addr_t* out_addr, uint16_t* out_port ));

/**
 * @brief Confine this one session to `root` (typically the user's home)
 *
 * Meant to be called from dmftp_auth_handler_t. `root` is resolved inside
 * the server-wide root, so a session can only ever be narrowed, never
 * widened - passing "/" restores the server-wide root, and a path escaping
 * it is clamped to it. Resets the session's working directory to "/".
 *
 * @return 0 on success, -EINVAL on a NULL/invalid argument, -ENOMEM
 */
dmod_dmftp_api(1.0, int, _session_set_root, ( dmftp_session_t session, const char* root ));

/**
 * @brief Grant or revoke write access for this one session
 *
 * Can only restrict, never widen: on a server configured `read_only`, a
 * `false` here is ignored.
 *
 * @return 0 on success, -EINVAL on a NULL/invalid argument
 */
dmod_dmftp_api(1.0, int, _session_set_read_only, ( dmftp_session_t session, bool read_only ));

/** @brief Attach an opaque per-session pointer */
dmod_dmftp_api(1.0, int, _session_set_user_data, ( dmftp_session_t session, void* user_data ));

/** @brief Read back the pointer set by dmftp_session_set_user_data() */
dmod_dmftp_api(1.0, void*, _session_get_user_data, ( dmftp_session_t session ));

/**
 * @brief Close a session from the outside (sends 421, then FIN)
 *
 * Safe to call from any callback except on_session_close itself.
 */
dmod_dmftp_api(1.0, void, _session_close, ( dmftp_session_t session ));

/* ============================================================================
 *                      Client
 * ========================================================================== */

/**
 * @brief Opaque handle to one client connection to a remote FTP server
 */
typedef struct dmftp_client* dmftp_client_t;

/**
 * @brief The client finished connecting and logging in - commands may now
 *        be issued
 */
typedef void (*dmftp_client_ready_handler_t)( dmftp_client_t client, void* user_data );

/**
 * @brief Every final (non-continuation) reply the server sends
 *
 * `text` is borrowed for the duration of the call. Fires for replies to
 * the client's own commands as well as for the 220 greeting.
 */
typedef void (*dmftp_client_reply_handler_t)( dmftp_client_t client, int code, const char* text, void* user_data );

/**
 * @brief One line of a LIST/NLST response
 *
 * `line` is borrowed, NUL-terminated, and has its CRLF stripped. Fires
 * once per line; the listing as a whole ends with the on_done that follows
 * it.
 */
typedef void (*dmftp_client_list_handler_t)( dmftp_client_t client, const char* line, void* user_data );

/**
 * @brief A transfer started by dmftp_client_get()/_put()/_append()/_list()
 *        finished
 *
 * @param result 0 on success, or a negative errno (-EIO if the local file
 *                could not be read/written, -ECONNRESET if the data
 *                connection died mid-transfer, -EPERM if the server
 *                refused the command)
 * @param bytes  How many payload bytes crossed the data connection
 */
typedef void (*dmftp_client_done_handler_t)( dmftp_client_t client, int result, uint64_t bytes, void* user_data );

/**
 * @brief TERMINAL: the control connection is gone (QUIT completed, the
 *        server dropped it, or a transport error) - the handle must not be
 *        used after this returns
 *
 * @param error 0 for an orderly finish, otherwise a negative errno
 */
typedef void (*dmftp_client_closed_handler_t)( dmftp_client_t client, int error, void* user_data );

/**
 * @brief Client callbacks - all optional
 */
typedef struct
{
    dmftp_client_ready_handler_t  on_ready;
    dmftp_client_reply_handler_t  on_reply;
    dmftp_client_list_handler_t   on_list;
    dmftp_client_done_handler_t   on_done;
    dmftp_client_closed_handler_t on_closed;
} dmftp_client_callbacks_t;

/**
 * @brief Client configuration, copied by dmftp_client_create()
 */
typedef struct
{
    /** @brief Server address (family must be dmip_family_v4) */
    dmip_addr_t host;

    /** @brief Server control port; 0 means DMFTP_PORT_CONTROL */
    uint16_t port;

    /** @brief Login name; NULL means "anonymous" */
    const char* user;

    /** @brief Password; NULL means "dmftp@" (the usual anonymous form) */
    const char* password;

    /**
     * @brief How data connections are established
     *
     * dmftp_data_passive (the default) issues PASV and connects out, which
     * is what works from behind NAT. dmftp_data_active listens locally and
     * sends PORT.
     */
    dmftp_data_mode_t data_mode;

    /** @brief Representation type to request; defaults to dmftp_type_image */
    dmftp_type_t type;
} dmftp_client_config_t;

/**
 * @brief Create a client - does not connect yet, see dmftp_client_connect()
 *
 * @return The new client, or NULL on a bad argument or allocation failure
 */
dmod_dmftp_api(1.0, dmftp_client_t, _client_create, ( const dmftp_client_config_t* config, const dmftp_client_callbacks_t* callbacks, void* user_data ));

/**
 * @brief Open the control connection and log in
 *
 * Non-blocking: returns as soon as the TCP connect is under way, and
 * announces the result later through on_ready (logged in and idle) or
 * on_closed (it failed).
 *
 * @return 0 on success, -EINVAL if `client` is NULL/invalid, -EALREADY if
 *         it is already connected, or whatever dmtcp_connect() returned
 */
dmod_dmftp_api(1.0, int, _client_connect, ( dmftp_client_t client ));

/**
 * @brief Download `remote_path` into `local_path` (RETR)
 *
 * One command may be in flight at a time - FTP's control channel is
 * strictly serial, so this returns -EBUSY rather than queueing. Completion
 * arrives via on_done.
 *
 * @return 0 once the command is on its way, -EINVAL on a NULL/invalid
 *         argument, -ENOTCONN if not logged in yet, -EBUSY if another
 *         command is still running, -EIO if `local_path` could not be
 *         opened for writing, -ENOMEM
 */
dmod_dmftp_api(1.0, int, _client_get, ( dmftp_client_t client, const char* remote_path, const char* local_path ));

/**
 * @brief Upload `local_path` to `remote_path` (STOR)
 *
 * @return Same set as dmftp_client_get(), with -EIO for a `local_path`
 *         that could not be opened for reading
 */
dmod_dmftp_api(1.0, int, _client_put, ( dmftp_client_t client, const char* local_path, const char* remote_path ));

/**
 * @brief Append `local_path` to `remote_path` (APPE)
 *
 * @return Same set as dmftp_client_put()
 */
dmod_dmftp_api(1.0, int, _client_append, ( dmftp_client_t client, const char* local_path, const char* remote_path ));

/**
 * @brief List a directory - LIST (`long_format` true) or NLST (false)
 *
 * Each line arrives via on_list, the listing ends with on_done.
 *
 * @param remote_path Directory to list; NULL lists the current working
 *                     directory
 *
 * @return Same set as dmftp_client_get()
 */
dmod_dmftp_api(1.0, int, _client_list, ( dmftp_client_t client, const char* remote_path, bool long_format ));

/* ---- Single-command operations; the outcome arrives via on_reply ---- */

dmod_dmftp_api(1.0, int, _client_cwd,    ( dmftp_client_t client, const char* path ));
dmod_dmftp_api(1.0, int, _client_delete, ( dmftp_client_t client, const char* path ));
dmod_dmftp_api(1.0, int, _client_mkdir,  ( dmftp_client_t client, const char* path ));
dmod_dmftp_api(1.0, int, _client_rmdir,  ( dmftp_client_t client, const char* path ));
dmod_dmftp_api(1.0, int, _client_size,   ( dmftp_client_t client, const char* path ));

/**
 * @brief Rename `from` to `to` (RNFR followed by RNTO)
 *
 * Two commands, one call: the RNTO is issued automatically once the RNFR
 * is accepted with 350. on_reply sees both.
 */
dmod_dmftp_api(1.0, int, _client_rename, ( dmftp_client_t client, const char* from, const char* to ));

/**
 * @brief Send an arbitrary command - the escape hatch for anything this
 *        API does not wrap
 *
 * @param verb Command verb (case-insensitive, at most DMFTP_VERB_MAX chars)
 * @param arg  Argument, or NULL for a bare command
 *
 * @return 0 once the command is on its way, otherwise as dmftp_client_get()
 */
dmod_dmftp_api(1.0, int, _client_command, ( dmftp_client_t client, const char* verb, const char* arg ));

/**
 * @brief Send QUIT and close the control connection
 *
 * on_closed fires once the connection is actually gone.
 *
 * @return 0 on success, -EINVAL if `client` is NULL/invalid
 */
dmod_dmftp_api(1.0, int, _client_quit, ( dmftp_client_t client ));

/**
 * @brief Abandon any connection and free the client
 *
 * Does not wait for a graceful QUIT - use dmftp_client_quit() for that and
 * destroy from on_closed.
 */
dmod_dmftp_api(1.0, void, _client_destroy, ( dmftp_client_t client ));

/** @brief Read back the opaque pointer given to dmftp_client_create() */
dmod_dmftp_api(1.0, void*, _client_get_user_data, ( dmftp_client_t client ));

/**
 * @brief Whether the client is connected and logged in (on_ready has fired
 *        and no terminal callback has)
 */
dmod_dmftp_api(1.0, bool, _client_is_ready, ( dmftp_client_t client ));

#ifdef __cplusplus
}
#endif

#endif // DMFTP_H
