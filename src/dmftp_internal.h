#ifndef DMFTP_INTERNAL_H
#define DMFTP_INTERNAL_H

#include "dmod.h"
#include "dmftp.h"
#include "dmtcp.h"
#include "dmosi.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * Private, cross-file state for the dmftp *server* (dmtcp/filesystem glue
 * on top of the transport-agnostic engine in dmftp.h/src/dmftp.c) - shared
 * between src/dmftp_server.c (dmod_init/dmod_deinit, dmtcp wiring, config)
 * and src/dmftp_commands.c (RFC 959 command handling, path resolution,
 * data-connection transfer). Never installed into include/ - nothing
 * outside this module ever needs it.
 */

#define DMFTP_MAX_CONNECTIONS   4u
#define DMFTP_DEFAULT_PORT      21u
#define DMFTP_DATA_CHUNK_SIZE   512u

/** Conventional FTP data-connection source port (RFC 959 §3.2) for an
 * active-mode (PORT) transfer - see dmftp_server_connect_port(). */
#define DMFTP_ACTIVE_SRC_PORT   20u

typedef enum
{
    dmftp_data_op_none,
    dmftp_data_op_list,
    dmftp_data_op_nlst,
    dmftp_data_op_retr,
    dmftp_data_op_stor,
} dmftp_data_op_t;

typedef struct
{
    bool in_use;

    dmtcp_conn_t control_conn;
    dmftp_t      engine;

    bool  logged_in;
    bool  user_received;     /**< USER was sent - PASS is now expected */
    char* username;          /**< Dmod_StrDup of the name from USER, or NULL */
    char* cwd;                /**< Virtual absolute path, always starts with '/', no trailing slash except root itself */
    bool  binary_mode;        /**< TYPE I vs TYPE A - see dmftp_commands.c's TYPE handler doc comment */

    /* PASV listener reserved for this session's *next* data connection -
     * see dmftp_server_start_pasv()/_stop_pasv(). */
    bool     pasv_pending;
    uint16_t pasv_port;

    /* PORT target for this session's *next* data connection - the address
     * is validated against the control connection's own peer when PORT is
     * received (see dmftp_commands.c's cmd_port()), so it is always safe
     * to actually connect to later. */
    bool        port_pending;
    dmip_addr_t port_addr;
    uint16_t    port_port;

    /* At most one data connection/transfer in flight at a time. */
    dmftp_data_op_t data_op;
    dmtcp_conn_t     data_conn;
    void*            transfer_file;   /**< Dmod_FileOpen() handle, for RETR/STOR */
    uint8_t*         list_buffer;     /**< Rendered LIST/NLST text, for those two ops */
    size_t           list_len;
    size_t           list_sent;
    bool             transfer_ok;     /**< Set once a transfer has legitimately finished - see the data terminal callbacks */
} dmftp_connection_t;

struct dmftp_context
{
    uint16_t control_port;
    char*    root;   /**< Configured virtual filesystem root, e.g. "/" - trailing slash stripped except for "/" itself */
    char*    user;   /**< Configured username; "anonymous" (case-insensitive) means any USER/PASS is accepted */
    char*    pass;   /**< Configured password; "" means any password is accepted for `user` */

    dmosi_mutex_t mutex; /**< Guards `connections` and each connection's pasv/data_conn fields below */
    dmftp_connection_t connections[DMFTP_MAX_CONNECTIONS];
};

/**
 * The single running server instance - valid between dmod_init() and
 * dmod_deinit(), NULL otherwise. Defined in src/dmftp_server.c.
 */
extern struct dmftp_context* g_dmftp_context;

/* ---- src/dmftp_commands.c: command handling and data transfer ---- */

/** The dmftp_command_handler_t installed on every session's engine. */
void dmftp_handle_command(dmftp_t engine, const char* verb, const char* arg, void* user_data);

/** The dmftp_send_handler_t installed on every session's engine (queues bytes on the control dmtcp_conn_t). */
void dmftp_handle_engine_send(dmftp_t engine, const uint8_t* data, size_t data_len, void* user_data);

/** dmtcp_conn_callbacks_t for a data connection (PASV accept or PORT connect) - see
 * src/dmftp_server.c's pasv_on_accept()/dmftp_server_connect_port(). */
void dmftp_data_on_data(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data);
void dmftp_data_on_writable(dmtcp_conn_t conn, size_t space, void* user_data);
void dmftp_data_on_closed(dmtcp_conn_t conn, void* user_data);
void dmftp_data_on_reset(dmtcp_conn_t conn, void* user_data);
void dmftp_data_on_error(dmtcp_conn_t conn, int error, void* user_data);

/** PORT-mode only: fires once dmftp_server_connect_port()'s active open completes. */
void dmftp_data_on_established(dmtcp_conn_t conn, void* user_data);

/** Called once a data connection is accepted, to kick off LIST/NLST/RETR sending. */
void dmftp_data_begin(dmftp_connection_t* c);

/** Frees everything a connection slot owns and clears it back to unused - safe to call more than once. */
void dmftp_connection_release(dmftp_connection_t* c);

/* ---- src/dmftp_server.c: dmtcp wiring, config, connection table ---- */

/** Case-insensitive check for the literal string "anonymous". */
bool dmftp_is_anonymous_user(const char* user);

/**
 * Reserve an ephemeral port for `c`'s next data connection via
 * dmtcp_listen_any(), remembering it in the context's port lookup so the
 * shared pasv_on_accept() handler can find `c` back (dmtcp_accept_handler_t
 * has no user_data parameter - see dmtcp.h).
 *
 * @return 0 on success (*out_port set), negative dmtcp_listen_any() error otherwise
 */
int dmftp_server_start_pasv(dmftp_connection_t* c, uint16_t* out_port);

/** Undo dmftp_server_start_pasv() - safe to call when no PASV is pending (a no-op). */
void dmftp_server_stop_pasv(dmftp_connection_t* c);

/**
 * Actively open `c`'s PORT-requested data connection (dmtcp_connect() from
 * DMFTP_ACTIVE_SRC_PORT to c->port_addr:c->port_port) - see cmd_port()'s
 * doc comment for the peer-address validation that makes this safe to call.
 * On success, clears c->port_pending and sets c->data_conn; the connection
 * is not necessarily ESTABLISHED yet (dmftp_data_on_established() reports
 * that once it happens).
 *
 * @return 0 on success, -EINVAL if `c` has no PORT pending, or whatever
 *         dmtcp_connect() itself returned on failure
 */
int dmftp_server_connect_port(dmftp_connection_t* c);

#endif // DMFTP_INTERNAL_H
