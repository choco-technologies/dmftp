#ifndef DMFTP_INTERNAL_H
#define DMFTP_INTERNAL_H

#include "dmftp.h"
#include "dmtcp.h"
#include "dmosi.h"
#include "dmlist.h"

/**
 * @file dmftp_internal.h
 * @brief Private state and cross-file plumbing for dmftp
 *
 * The public opaque handles (dmftp_server_t, dmftp_session_t,
 * dmftp_client_t) are defined here, not in include/dmftp.h - callers only
 * ever get a pointer, never the layout (opaque-handle + magic-guard
 * pattern; worked example in dm_sw_ring, and the same shape
 * dmtcp_internal.h/dmdhcp_internal.h use).
 *
 * File map:
 *
 *  - dmftp_registrations.c  DMOD_ENABLE_REGISTRATION - must stand alone,
 *                           see that file's own comment
 *  - dmftp.c                dmod_init()/_deinit(), the module-wide lock,
 *                           and the buffer/string helpers everything else
 *                           is built out of
 *  - dmftp_wire.c           the control-line codec (public API):
 *                           _parse_command/_format_reply/_parse_reply/
 *                           _format_host_port/_parse_host_port
 *  - dmftp_path.c           jail-safe path resolution - the one place a
 *                           remote-supplied path becomes a real filesystem
 *                           path
 *  - dmftp_net.c            EVERY dmtcp interaction that passes a function
 *                           pointer, plus the listener registry - see the
 *                           "One file owns every dmtcp callback" note below
 *  - dmftp_xfer.c           the transfer engine: file/directory sources,
 *                           file/line sinks, and the on_writable-driven pump
 *  - dmftp_server.c         server + session lifecycle and the public
 *                           accessors
 *  - dmftp_server_cmd.c     the command dispatch table and every command
 *                           that needs no data connection
 *  - dmftp_server_xfer.c    PASV/PORT plus LIST/NLST/RETR/STOR/APPE - the
 *                           commands that do need one
 *  - dmftp_client.c         the client half, its reply state machine, and
 *                           its public API
 *
 * @par One file owns every dmtcp callback
 * dmtcp's own docs (docs/dmtcp.md, "A loader constraint") record that this
 * loader mis-resolves a callback whose address is taken in one .c file and
 * handed to another module's registration API from a different .c file of
 * the same module - the call lands on an unrelocated address. Every
 * dmtcp_listen()/_listen_any()/_connect()/_conn_set_callbacks() call in
 * dmftp therefore lives in dmftp_net.c, next to the handlers it registers;
 * the rest of the module reaches dmtcp through the dmftp_net_*() helpers
 * below and never touches a dmtcp registration API directly.
 *
 * @par Threading: one recursive module lock, no dmftp thread
 * dmftp has no thread of its own. Control lines, inbound data and
 * connection events all arrive inline on whatever thread pumps the
 * interface (dmtcp's documented delivery context), which for several
 * interfaces means several threads. One recursive mutex, taken at the top
 * of every dmtcp callback and every public entry point, serializes all of
 * it.
 *
 * Unlike dmtcp/dmdhcp, dmftp deliberately holds that lock across user
 * callbacks (on_auth, on_list, on_done, ...) instead of snapshotting and
 * releasing first. Two reasons: the lock is recursive, so the thing those
 * callbacks most want to do - call straight back into dmftp
 * (dmftp_session_set_root() from on_auth, dmftp_client_get() from
 * on_ready) - just works; and dropping it mid-command would expose every
 * caller to a session being torn down underneath it by another interface's
 * thread, which is a much harder class of bug than the rule it replaces.
 * That rule, inherited unchanged from dmtcp: a dmftp callback must not
 * block.
 */

/* ============================================================================
 *                      Magic guards and tunables
 * ========================================================================== */

#define DMFTP_SERVER_MAGIC  0x46545053u /* "FTPS" */
#define DMFTP_SESSION_MAGIC 0x46545045u /* "FTPE" - sEssion */
#define DMFTP_CLIENT_MAGIC  0x46545043u /* "FTPC" */
#define DMFTP_XFER_MAGIC    0x46545058u /* "FTPX" */

/** @brief Module name reported to Dmod_MallocEx for dmftp's allocations */
#define DMFTP_ALLOCATOR_NAME "dmftp"

/**
 * @brief Staging buffer size for one file<->socket hop
 *
 * Sized against dmtcp's own 4096-byte send buffer: big enough that a full
 * buffer is filled in a handful of Dmod_FileRead() calls, small enough that
 * a device with several concurrent transfers is not paying kilobytes per
 * idle session. ASCII mode can expand LF to CRLF, so the wire-side buffer
 * is twice this - see dmftp_xfer.c.
 */
#define DMFTP_CHUNK_LEN 1024u

/** @brief Initial capacity of a growable byte buffer (replies, lines) */
#define DMFTP_BUF_INITIAL 128u

/* ============================================================================
 *                      Growable byte buffer
 * ========================================================================== */

/**
 * @brief A heap byte buffer that grows on demand
 *
 * Used for two things: the pending-output queue of a control connection
 * (replies that dmtcp_send() could not take yet), and the partial-line
 * accumulator of an inbound stream. Neither has a length a fixed-size
 * array could honestly bound, which is exactly the case
 * dmod-coding-conventions says to heap-allocate.
 */
typedef struct
{
    uint8_t* data;
    size_t   len;
    size_t   cap;
} dmftp_buf_t;

void dmftp_buf_init(dmftp_buf_t* buf);
void dmftp_buf_free(dmftp_buf_t* buf);

/**
 * @brief Append `len` bytes, growing the buffer if needed
 *
 * @return 0 on success, -ENOMEM if the buffer could not grow
 */
int dmftp_buf_append(dmftp_buf_t* buf, const void* data, size_t len);

/** @brief Drop the first `len` bytes, keeping the rest */
void dmftp_buf_consume(dmftp_buf_t* buf, size_t len);

/**
 * @brief Take the next complete line out of an inbound stream buffer
 *
 * Both control channels - the server reading commands and the client
 * reading replies - face the same problem, since TCP is a byte stream: a
 * line can arrive in any number of pieces and two lines can share one
 * segment. This is that reassembly, in one place.
 *
 * The line's terminating LF (and the CR before it, if present) is consumed
 * and stripped.
 *
 * @param out_len Optional: receives the line's length
 *
 * @return A new string the caller frees, or NULL if `buf` holds no complete
 *         line yet
 */
char* dmftp_buf_take_line(dmftp_buf_t* buf, size_t* out_len);

/* ---- Small string helpers (dmod modules have no libc beyond
 *      dmod/src/module/string.c's minimal set) ---- */

/**
 * @brief Concatenate up to three parts into one freshly allocated string
 *
 * Any part may be NULL (treated as empty).
 *
 * @return The new string, or NULL on allocation failure
 */
char* dmftp_str_join(const char* a, const char* b, const char* c);

/**
 * @brief Copy exactly `len` bytes and NUL-terminate
 *
 * @return The new string, or NULL on allocation failure
 */
char* dmftp_str_ndup(const char* str, size_t len);

/**
 * @brief Parse a decimal unsigned integer spanning exactly `len` bytes
 *
 * @return true if every byte was a digit and the value fit in uint32_t
 */
bool dmftp_str_to_u32(const char* text, size_t len, uint32_t* out);

/** @brief ASCII upper-case, locale-free */
char dmftp_str_upper(char c);

/** @brief Case-insensitive comparison of NUL-terminated strings */
bool dmftp_str_iequal(const char* a, const char* b);

/* ---- The module-wide recursive lock (see this file's header comment) ---- */

void dmftp_lock(void);
void dmftp_unlock(void);

/* ============================================================================
 *                      Path resolution (dmftp_path.c)
 * ========================================================================== */

/**
 * @brief Normalize `arg` against `cwd` into an absolute virtual path
 *
 * The result always starts with '/', never ends with one (except for "/"
 * itself), and contains no "." or ".." component: a ".." at the top is
 * dropped, which is what confines a session to its root. `arg` may be NULL
 * or empty, meaning "cwd itself".
 *
 * @return A new string the caller frees, or NULL on allocation failure
 */
char* dmftp_path_virtual(const char* cwd, const char* arg);

/**
 * @brief Turn a virtual path into the real filesystem path under `root`
 *
 * @return A new string the caller frees, or NULL on allocation failure
 */
char* dmftp_path_to_os(const char* root, const char* virtual_path);

/**
 * @brief dmftp_path_virtual() followed by dmftp_path_to_os() - the normal
 *        way a command turns its argument into something to open
 *
 * @param root    Session root (an OS path)
 * @param cwd     Session working directory (a virtual path)
 * @param arg     The command's argument, or NULL
 * @param out_virtual Optional: receives the intermediate virtual path,
 *                     which the caller also frees
 *
 * @return A new OS path the caller frees, or NULL on allocation failure
 */
char* dmftp_path_resolve(const char* root, const char* cwd, const char* arg, char** out_virtual);

/** @brief Whether an OS path names an existing directory */
bool dmftp_path_is_dir(const char* os_path);

/**
 * @brief Size of an existing file in bytes
 *
 * The SAL has no stat(), so this opens the file, asks Dmod_FileSize() and
 * closes it again - see dmftp.h's note on what the SAL does not provide.
 *
 * @return true on success (`out_size` set), false if the path could not be
 *         opened as a file
 */
bool dmftp_path_file_size(const char* os_path, uint32_t* out_size);

/* ============================================================================
 *                      Transfer engine (dmftp_xfer.c)
 * ========================================================================== */

/** @brief Who owns a transfer - selects which completion hook fires */
typedef enum
{
    dmftp_owner_session = 0,
    dmftp_owner_client,
} dmftp_owner_kind_t;

/** @brief Where an outbound transfer's bytes come from */
typedef enum
{
    dmftp_src_none = 0,
    dmftp_src_file, /**< RETR / a client's STOR */
    dmftp_src_dir,  /**< LIST / NLST generated entry by entry */
} dmftp_src_kind_t;

/** @brief Where an inbound transfer's bytes go */
typedef enum
{
    dmftp_dst_none = 0,
    dmftp_dst_file,  /**< STOR / a client's RETR */
    dmftp_dst_lines, /**< a client's LIST/NLST, split into on_list calls */
} dmftp_dst_kind_t;

/**
 * @brief One data-connection transfer, in either direction
 *
 * Created when a command that needs a data connection is accepted, and
 * destroyed once that connection has finished. Outbound transfers are
 * driven entirely by dmtcp's on_writable (see dmftp_xfer_pump()); inbound
 * ones by on_data (see dmftp_xfer_receive()).
 */
struct dmftp_xfer
{
    uint32_t           magic;
    dmftp_owner_kind_t owner_kind;
    void*              owner;

    bool               to_peer; /**< true: we send; false: we receive */
    dmftp_src_kind_t   src;
    dmftp_dst_kind_t   dst;

    void* file;     /**< Dmod_FileOpen() handle for src/dst _file */
    void* dir;      /**< Dmod_OpenDir() handle for dmftp_src_dir */
    char* dir_path; /**< OS path of that directory, to size its entries */
    bool  long_format; /**< LIST rather than NLST */

    bool ascii;      /**< TYPE A: translate line endings in flight */
    bool pending_cr; /**< inbound ASCII: last byte seen was a CR */

    dmtcp_conn_t conn;

    /**
     * @brief File-read staging, allocated only for dmftp_src_file
     *
     * A directory source formats straight into `scratch`, and both sinks
     * write through without staging, so this is the one path that needs a
     * separate landing buffer for Dmod_FileRead().
     */
    uint8_t* raw;

    /**
     * @brief The bytes still owed to (or received from) the wire
     *
     * Outbound: whatever the source produced that dmtcp_send() has not
     * taken yet. Inbound with dmftp_dst_lines: the partial trailing line.
     * The two uses never overlap - a transfer has a source or a sink,
     * never both.
     */
    dmftp_buf_t scratch;

    bool     source_eof;
    bool     finished;
    int      result;
    uint64_t bytes;
};

struct dmftp_xfer* dmftp_xfer_create(void* owner, dmftp_owner_kind_t kind, bool to_peer);
void               dmftp_xfer_destroy(struct dmftp_xfer* xfer);

/** @brief Point an outbound/inbound transfer at an already-open file */
void dmftp_xfer_set_file(struct dmftp_xfer* xfer, void* file, bool ascii);

/** @brief Point an outbound transfer at an already-open directory */
void dmftp_xfer_set_dir(struct dmftp_xfer* xfer, void* dir, char* dir_path, bool long_format);

/** @brief Deliver inbound bytes into a client's on_list handler */
void dmftp_xfer_set_lines(struct dmftp_xfer* xfer);

/**
 * @brief The data connection is up - start moving bytes
 *
 * Takes ownership of `conn` for the rest of the transfer.
 */
void dmftp_xfer_attach(struct dmftp_xfer* xfer, dmtcp_conn_t conn);

/** @brief dmtcp on_writable: push more of an outbound transfer */
void dmftp_xfer_pump(struct dmftp_xfer* xfer);

/** @brief dmtcp on_data: absorb bytes of an inbound transfer */
void dmftp_xfer_receive(struct dmftp_xfer* xfer, const uint8_t* data, size_t len);

/**
 * @brief End a transfer and tell its owner
 *
 * Idempotent - the first call wins, so the several ways a data connection
 * can end (our own EOF, the peer's FIN, a RST, the owner giving up) all
 * funnel into exactly one completion.
 */
void dmftp_xfer_finish(struct dmftp_xfer* xfer, int result);

/* Completion hooks the engine calls back into, one per owner kind. */
void dmftp_server_xfer_done(struct dmftp_session* session, int result, uint64_t bytes);
void dmftp_client_xfer_done(struct dmftp_client* client, int result, uint64_t bytes);

/** @brief dmftp_dst_lines delivery hook - one complete line, CRLF stripped */
void dmftp_client_xfer_line(struct dmftp_client* client, const char* line);

/* ============================================================================
 *                      dmtcp plumbing (dmftp_net.c)
 * ========================================================================== */

/**
 * @brief Which set of dmtcp callbacks a connection gets
 *
 * Every connection dmftp owns is one of these four; dmftp_net.c keeps one
 * callback set per role and dispatches into the owning session/client/
 * transfer from there.
 */
typedef enum
{
    dmftp_role_server_control = 0,
    dmftp_role_server_data,
    dmftp_role_client_control,
    dmftp_role_client_data,
} dmftp_role_t;

/**
 * @brief What an accepted connection should be handed to
 *
 * dmtcp_accept_handler_t carries no user_data, so a listener registry maps
 * the local port back to whoever reserved it - see dmftp_net_listen().
 */
typedef enum
{
    dmftp_listen_server_control = 0, /**< owner is a struct dmftp_server* */
    dmftp_listen_server_data,        /**< owner is a struct dmftp_session* */
    dmftp_listen_client_data,        /**< owner is a struct dmftp_client* */
} dmftp_listen_kind_t;

int  dmftp_net_init(void);
void dmftp_net_deinit(void);

/**
 * @brief Reserve `port` (or, for _any, the first free ephemeral one) and
 *        remember who to hand accepted connections to
 *
 * @return 0 on success, -EEXIST if the port is taken, -ENOMEM,
 *         -EADDRNOTAVAIL if the ephemeral range is exhausted
 */
int  dmftp_net_listen(uint16_t port, dmftp_listen_kind_t kind, void* owner);
int  dmftp_net_listen_any(dmftp_listen_kind_t kind, void* owner, uint16_t* out_port);

/** @brief Release a port reserved above - a no-op for an unknown port */
void dmftp_net_unlisten(uint16_t port);

/** @brief Drop every listener whose owner is `owner` (used during teardown) */
void dmftp_net_unlisten_owner(void* owner);

/**
 * @brief Actively open a connection and install `role`'s callbacks on it
 *
 * @return 0 on success (`*out_conn` set), otherwise dmtcp_connect()'s error
 */
int dmftp_net_connect(const dmip_addr_t* dst, uint16_t dst_port, uint16_t src_port,
                       dmftp_role_t role, void* owner, dmtcp_conn_t* out_conn);

/** @brief Install `role`'s callbacks on an already-accepted connection */
int dmftp_net_attach(dmtcp_conn_t conn, dmftp_role_t role, void* owner);

/**
 * @brief Clear a connection's callbacks so dmtcp stops calling into us
 *
 * Must be done before freeing whatever was that connection's `owner`: a
 * dmtcp_close() is graceful, so the TCB survives the call and fires its
 * terminal callback later - at which point the owner pointer would be
 * dangling. See the implementation's comment.
 */
void dmftp_net_detach(dmtcp_conn_t conn);

/**
 * @brief Queue bytes on a connection, buffering whatever dmtcp could not
 *        take yet
 *
 * The control channel's counterpart to the transfer engine: replies are
 * appended to `out` and flushed as far as dmtcp_send() allows, with the
 * remainder pushed again from on_writable.
 *
 * @return 0 on success, -ENOMEM, or dmtcp_send()'s error
 */
int dmftp_net_send(dmtcp_conn_t conn, dmftp_buf_t* out, const void* data, size_t len);

/** @brief Push whatever is still pending in `out` - called from on_writable */
int dmftp_net_flush(dmtcp_conn_t conn, dmftp_buf_t* out);

/* ============================================================================
 *                      Server (dmftp_server.c)
 * ========================================================================== */

/** @brief Where a session is in its login sequence */
typedef enum
{
    dmftp_login_none = 0, /**< no USER yet */
    dmftp_login_user,     /**< USER seen, waiting for PASS */
    dmftp_login_done,     /**< authenticated */
} dmftp_login_state_t;

struct dmftp_session
{
    uint32_t              magic;
    struct dmftp_server*  server;
    dmtcp_conn_t          control;
    dmip_addr_t           peer_addr;
    uint16_t              peer_port;
    dmip_addr_t           local_addr;
    dmnetif_iface_t       iface;

    dmftp_buf_t out; /**< replies dmtcp_send() has not taken yet */
    dmftp_buf_t in;  /**< partial command line */

    dmftp_login_state_t login;
    char*               user;
    char*               root; /**< OS path this session is confined to */
    char*               cwd;  /**< virtual path, always starts with '/' */
    bool                read_only;
    dmftp_type_t        type;

    /* Data connection setup, valid between PASV/PORT and the transfer */
    dmftp_data_mode_t   data_mode;
    uint16_t            pasv_port; /**< 0 when no PASV listener is reserved */
    dmip_addr_t         active_addr;
    uint16_t            active_port;
    struct dmftp_xfer*  xfer;

    /**
     * @brief A PASV connection the client opened before sending its
     *        transfer command
     *
     * Clients routinely connect the instant they read the 227, several
     * milliseconds before RETR/STOR arrives, so the accepted connection has
     * to wait somewhere until there is a transfer to give it to.
     */
    dmtcp_conn_t pending_data_conn;

    uint32_t rest_offset;
    char*    rename_from; /**< OS path captured by RNFR */

    void* user_data;
    bool  closing;
};

struct dmftp_server
{
    uint32_t                 magic;
    uint16_t                 port;
    char*                    root;
    char*                    banner;
    bool                     read_only;
    bool                     allow_passive;
    bool                     allow_active;
    uint16_t                 active_data_port;
    uint32_t                 max_sessions;
    dmftp_server_callbacks_t callbacks;
    void*                    user_data;
    bool                     running;
    dmlist_context_t*        sessions;
};

/* Events dmftp_net.c routes into the server half. */
void dmftp_server_on_accept(struct dmftp_server* server, dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface);
void dmftp_server_on_control_data(struct dmftp_session* session, const uint8_t* data, size_t len);
void dmftp_server_on_control_closed(struct dmftp_session* session);
void dmftp_server_on_data_accept(struct dmftp_session* session, dmtcp_conn_t conn);
void dmftp_server_on_data_established(struct dmftp_session* session, dmtcp_conn_t conn);

/** @brief Queue one reply line on a session's control connection */
void dmftp_server_reply(struct dmftp_session* session, int code, const char* text);

/** @brief Queue a raw, already-formatted line (the multi-line FEAT/HELP form) */
void dmftp_server_reply_raw(struct dmftp_session* session, const char* line);

/** @brief Dispatch one complete command line (dmftp_server_cmd.c) */
void dmftp_server_dispatch(struct dmftp_session* session, const char* line, size_t len);

/** @brief Tear a session down and free it */
void dmftp_server_session_close(struct dmftp_session* session);

/**
 * @brief Release any half-set-up data connection state (a reserved PASV
 *        port, a pending transfer)
 *
 * Called between commands and during teardown, so a failed or abandoned
 * transfer never leaves a port reserved.
 */
void dmftp_server_reset_data(struct dmftp_session* session);

/* Commands that need a data connection (dmftp_server_xfer.c). */
void dmftp_server_cmd_pasv(struct dmftp_session* session, const dmftp_command_t* cmd);
void dmftp_server_cmd_port(struct dmftp_session* session, const dmftp_command_t* cmd);
void dmftp_server_cmd_list(struct dmftp_session* session, const dmftp_command_t* cmd);
void dmftp_server_cmd_nlst(struct dmftp_session* session, const dmftp_command_t* cmd);
void dmftp_server_cmd_retr(struct dmftp_session* session, const dmftp_command_t* cmd);
void dmftp_server_cmd_stor(struct dmftp_session* session, const dmftp_command_t* cmd);
void dmftp_server_cmd_appe(struct dmftp_session* session, const dmftp_command_t* cmd);
void dmftp_server_cmd_abor(struct dmftp_session* session, const dmftp_command_t* cmd);

/**
 * @brief Render one LIST/NLST entry into `out`
 *
 * Shared by the server's own listing generator and exposed here so the
 * format lives in exactly one place.
 */
int dmftp_xfer_format_entry(dmftp_buf_t* out, const char* dir_os_path, const char* name, bool is_dir, bool long_format);

/* ============================================================================
 *                      Client (dmftp_client.c)
 * ========================================================================== */

/** @brief What the client is waiting for a reply to */
typedef enum
{
    dmftp_cstate_idle = 0,
    dmftp_cstate_greeting, /**< connected, waiting for 220 */
    dmftp_cstate_user,     /**< USER sent */
    dmftp_cstate_pass,     /**< PASS sent */
    dmftp_cstate_type,     /**< TYPE sent, last step before ready */
    dmftp_cstate_ready,    /**< logged in, no command outstanding */
    dmftp_cstate_setup,    /**< PASV/PORT sent for a pending transfer */
    dmftp_cstate_xfer_cmd, /**< RETR/STOR/LIST sent, waiting for 1xx */
    dmftp_cstate_xfer,     /**< transfer running, waiting for 226 */
    dmftp_cstate_simple,   /**< a one-shot command is outstanding */
    dmftp_cstate_rnfr,     /**< RNFR sent, RNTO queued behind it */
    dmftp_cstate_quit,     /**< QUIT sent */
    dmftp_cstate_closed,
} dmftp_client_state_t;

struct dmftp_client
{
    uint32_t                 magic;
    dmip_addr_t              host;
    uint16_t                 port;
    char*                    user;
    char*                    password;
    dmftp_data_mode_t        data_mode;
    dmftp_type_t             type;
    dmftp_client_callbacks_t callbacks;
    void*                    user_data;

    dmtcp_conn_t         control;
    dmftp_buf_t          out;
    dmftp_buf_t          in;
    dmftp_client_state_t state;

    /* The transfer being set up or run. The command is held back until the
     * data connection has been arranged (after the 227 or the 200), which
     * is the order every server expects. */
    char*              pending_verb;
    char*              pending_arg;
    char*              rename_to;
    struct dmftp_xfer* xfer;
    uint16_t           active_port; /**< our PORT listener, 0 if none */

    /**
     * @brief A transfer finishes on two channels, and on_done waits for
     *        both
     *
     * The data connection closing says "the bytes are all here"; the
     * server's 226 says "and I agree it worked". Reporting on whichever
     * arrives first would mean announcing success before the server had a
     * chance to say otherwise, so `data_done` and `control_done` are
     * tracked separately and on_done fires once, when both are in.
     */
    bool     data_done;
    bool     control_done;
    bool     xfer_reported;
    int      xfer_result;
    uint64_t xfer_bytes;
};

/* Events dmftp_net.c routes into the client half. */
void dmftp_client_on_established(struct dmftp_client* client);
void dmftp_client_on_control_data(struct dmftp_client* client, const uint8_t* data, size_t len);
void dmftp_client_on_control_closed(struct dmftp_client* client, int error);
void dmftp_client_on_data_accept(struct dmftp_client* client, dmtcp_conn_t conn);
void dmftp_client_on_data_established(struct dmftp_client* client, dmtcp_conn_t conn);

#endif // DMFTP_INTERNAL_H
