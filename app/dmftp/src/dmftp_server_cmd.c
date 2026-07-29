/**
 * @file dmftp_server_cmd.c
 * @brief The command dispatch table, and every command that needs no data
 *        connection
 *
 * One table, one lookup, one place where "does this need a login" and
 * "does this need write permission" are decided - so a new command cannot
 * accidentally skip either check, which is the usual way an FTP server
 * grows a hole.
 *
 * @par A loader constraint: no pointers in static tables
 * The obvious way to write this is an array of
 * `{ const char* verb, handler_fn, flags }`. It does not work on this
 * loader: a pointer sitting inside a static initialized aggregate is never
 * relocated, so the first `strcmp(entry->verb, ...)` dereferences a link-
 * time address and the module dies with a SIGSEGV that points nowhere near
 * the table. (dmtcp's docs record a sibling of this rule for callbacks
 * registered across translation units; this is the data-side one, and the
 * reason dmell registers its own command handlers with runtime calls
 * instead of a static table.)
 *
 * The table below is therefore pure integers - the verb packed into a
 * uint32_t, the flags, and an enum id - which need no relocation at all.
 * Dispatch is a switch on that id. The lookup stays table-driven, so the
 * "one place decides the access rules" property survives intact.
 */
#include "dmod.h"
#include "dmftp_internal.h"
#include <string.h>
#include <errno.h>

/** @brief Reject with 530 unless the session has completed its login */
#define DMFTP_CMD_AUTH  0x01u
/** @brief Reject with 550 on a read-only session */
#define DMFTP_CMD_WRITE 0x02u
/** @brief Reject with 501 when the command arrives with no argument */
#define DMFTP_CMD_ARG   0x04u

/**
 * @brief Pack a four-character verb into one integer, NUL-padded
 *
 * An FTP verb is at most DMFTP_VERB_MAX characters (RFC 959), so it fits
 * exactly - which is what lets the table hold verbs without holding
 * pointers to them.
 */
#define DMFTP_VERB(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/** @brief Every command dmftp answers */
typedef enum
{
    dmftp_cmd_id_user = 0,
    dmftp_cmd_id_pass,
    dmftp_cmd_id_acct,
    dmftp_cmd_id_quit,
    dmftp_cmd_id_noop,
    dmftp_cmd_id_syst,
    dmftp_cmd_id_feat,
    dmftp_cmd_id_opts,
    dmftp_cmd_id_help,
    dmftp_cmd_id_stat,
    dmftp_cmd_id_type,
    dmftp_cmd_id_mode,
    dmftp_cmd_id_stru,
    dmftp_cmd_id_allo,
    dmftp_cmd_id_rest,
    dmftp_cmd_id_pwd,
    dmftp_cmd_id_cwd,
    dmftp_cmd_id_cdup,
    dmftp_cmd_id_pasv,
    dmftp_cmd_id_port,
    dmftp_cmd_id_list,
    dmftp_cmd_id_nlst,
    dmftp_cmd_id_retr,
    dmftp_cmd_id_stor,
    dmftp_cmd_id_appe,
    dmftp_cmd_id_abor,
    dmftp_cmd_id_dele,
    dmftp_cmd_id_mkd,
    dmftp_cmd_id_rmd,
    dmftp_cmd_id_rnfr,
    dmftp_cmd_id_rnto,
    dmftp_cmd_id_size,
    dmftp_cmd_id_mdtm,
} dmftp_cmd_id_t;

/** @brief One row of the dispatch table - integers only, see the file comment */
struct dmftp_cmd_entry
{
    uint32_t key;
    uint8_t  id;
    uint8_t  flags;
};

/**
 * @brief Copy a command's argument into a NUL-terminated string
 *
 * dmftp_command_t::arg is borrowed and length-delimited (trailing
 * whitespace was trimmed off the line, so it is not necessarily terminated
 * where the argument ends) - every handler that passes it to a
 * string-taking API needs its own copy.
 *
 * @return A new string the caller frees, or NULL if there was no argument
 */
static char* dup_arg(const dmftp_command_t* cmd)
{
    if (cmd->arg == NULL || cmd->arg_len == 0)
        return NULL;

    return dmftp_str_ndup(cmd->arg, cmd->arg_len);
}

/**
 * @brief Turn a command's argument into a filesystem path inside the jail
 *
 * @param out_virtual Optional: also receives the virtual (client-visible)
 *                     path, which the caller frees
 *
 * @return The OS path (caller frees), or NULL after replying 550 itself
 */
static char* resolve_arg(struct dmftp_session* session, const dmftp_command_t* cmd, char** out_virtual)
{
    char* arg = dup_arg(cmd);
    char* os_path = dmftp_path_resolve(session->root, session->cwd, arg, out_virtual);
    Dmod_Free(arg);

    if (os_path == NULL)
    {
        dmftp_server_reply(session, 550, "Out of memory.");
    }
    return os_path;
}

/**
 * @brief Reply 250/550 for a command whose whole result is "did it work"
 */
static void reply_result(struct dmftp_session* session, bool ok, const char* verb)
{
    if (ok)
    {
        dmftp_server_reply(session, 250, "Command successful.");
        return;
    }

    char* text = dmftp_str_join(verb, ": operation failed.", NULL);
    dmftp_server_reply(session, 550, text != NULL ? text : "Operation failed.");
    Dmod_Free(text);
}

/* ============================================================================
 *                      Access control
 * ========================================================================== */

static void cmd_user(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    /* A second USER restarts the login sequence, which is what a client
     * does after a 530 rather than opening a fresh connection. */
    Dmod_Free(session->user);
    session->user = dup_arg(cmd);
    session->login = session->user != NULL ? dmftp_login_user : dmftp_login_none;

    if (session->user == NULL)
    {
        dmftp_server_reply(session, 501, "USER requires a user name.");
        return;
    }
    dmftp_server_reply(session, 331, "Password required.");
}

static void cmd_pass(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    if (session->login != dmftp_login_user)
    {
        dmftp_server_reply(session, 503, "Login with USER first.");
        return;
    }

    char* password = dup_arg(cmd);
    struct dmftp_server* server = session->server;

    /* The handler may call dmftp_session_set_root()/_set_read_only() from
     * inside this call - the module lock is recursive precisely so that
     * works, and both take effect before the 230 below. */
    bool accepted = server->callbacks.on_auth(session, session->user,
                                               password != NULL ? password : "", server->user_data);
    Dmod_Free(password);

    if (!accepted)
    {
        session->login = dmftp_login_none;
        dmftp_server_reply(session, 530, "Login incorrect.");
        return;
    }

    session->login = dmftp_login_done;
    dmftp_server_reply(session, 230, "Login successful.");
}

static void cmd_acct(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    /* dmftp has no notion of an account beyond the user - 202 is RFC 959's
     * "understood, but not needed here". */
    dmftp_server_reply(session, 202, "ACCT not required.");
}

static void cmd_quit(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    dmftp_server_reply(session, 221, "Goodbye.");
    dmftp_server_session_close(session);
}

/* ============================================================================
 *                      Session parameters
 * ========================================================================== */

static void cmd_noop(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    dmftp_server_reply(session, 200, "NOOP ok.");
}

static void cmd_syst(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    /* Clients switch their listing parser on this string; "UNIX Type: L8"
     * is what makes them expect the `ls -l` format dmftp_xfer.c emits. */
    dmftp_server_reply(session, 215, "UNIX Type: L8");
}

static void cmd_type(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char type = dmftp_str_upper(cmd->arg[0]);

    if (type == 'I' || type == 'L')
    {
        session->type = dmftp_type_image;
        dmftp_server_reply(session, 200, "Type set to I.");
        return;
    }
    if (type == 'A')
    {
        session->type = dmftp_type_ascii;
        dmftp_server_reply(session, 200, "Type set to A.");
        return;
    }
    dmftp_server_reply(session, 504, "Only TYPE A and TYPE I are supported.");
}

static void cmd_mode(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    /* Stream mode only - block and compressed mode (RFC 959 §3.4.2/3.4.3)
     * exist in no client anyone still runs. */
    if (dmftp_str_upper(cmd->arg[0]) == 'S')
    {
        dmftp_server_reply(session, 200, "Mode set to S.");
        return;
    }
    dmftp_server_reply(session, 504, "Only stream mode is supported.");
}

static void cmd_stru(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    if (dmftp_str_upper(cmd->arg[0]) == 'F')
    {
        dmftp_server_reply(session, 200, "Structure set to F.");
        return;
    }
    dmftp_server_reply(session, 504, "Only file structure is supported.");
}

static void cmd_feat(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    /* RFC 2389's multi-line form. MDTM is deliberately absent: the SAL
     * reports no modification time, so advertising it would make clients
     * ask a question dmftp cannot answer (see dmftp.h). */
    dmftp_server_reply_raw(session, "211-Features:");
    dmftp_server_reply_raw(session, " SIZE");
    dmftp_server_reply_raw(session, " REST STREAM");
    dmftp_server_reply_raw(session, " UTF8");
    dmftp_server_reply_raw(session, "211 End");
}

static void cmd_opts(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* arg = dup_arg(cmd);

    /* Paths are passed through as the bytes the client sent, so UTF-8 is
     * on in the only sense dmftp can promise it. */
    bool utf8 = arg != NULL && (dmftp_str_iequal(arg, "UTF8 ON") || dmftp_str_iequal(arg, "UTF8"));
    Dmod_Free(arg);

    if (utf8)
    {
        dmftp_server_reply(session, 200, "UTF8 set to on.");
        return;
    }
    dmftp_server_reply(session, 501, "Option not understood.");
}

static void cmd_allo(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    dmftp_server_reply(session, 202, "ALLO not required.");
}

static void cmd_rest(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    uint32_t offset = 0;
    if (!dmftp_str_to_u32(cmd->arg, cmd->arg_len, &offset))
    {
        dmftp_server_reply(session, 501, "REST requires a byte offset.");
        return;
    }

    session->rest_offset = offset;
    dmftp_server_reply(session, 350, "Restart position accepted.");
}

/* ============================================================================
 *                      Navigation
 * ========================================================================== */

static void cmd_pwd(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    char* text = dmftp_str_join("\"", session->cwd, "\" is the current directory.");
    dmftp_server_reply(session, 257, text != NULL ? text : "\"/\"");
    Dmod_Free(text);
}

/**
 * @brief Move the session's working directory to `arg`, resolved as usual
 *
 * Shared by CWD and CDUP, which differ only in where the argument comes
 * from. Frees nothing the caller owns and replies for every outcome.
 */
static void change_dir(struct dmftp_session* session, const char* arg)
{
    char* virtual_path = NULL;
    char* os_path = dmftp_path_resolve(session->root, session->cwd, arg, &virtual_path);
    if (os_path == NULL)
    {
        Dmod_Free(virtual_path);
        dmftp_server_reply(session, 550, "Out of memory.");
        return;
    }

    if (dmftp_path_is_dir(os_path))
    {
        Dmod_Free(session->cwd);
        session->cwd = virtual_path; /* ownership moves into the session */
        dmftp_server_reply(session, 250, "Directory changed.");
    }
    else
    {
        Dmod_Free(virtual_path);
        dmftp_server_reply(session, 550, "No such directory.");
    }

    Dmod_Free(os_path);
}

static void cmd_cwd(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* arg = dup_arg(cmd);
    change_dir(session, arg);
    Dmod_Free(arg);
}

static void cmd_cdup(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    change_dir(session, "..");
}

/* ============================================================================
 *                      Filesystem mutation
 * ========================================================================== */

static void cmd_dele(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* os_path = resolve_arg(session, cmd, NULL);
    if (os_path == NULL)
        return;

    reply_result(session, Dmod_FileRemove(os_path) == 0, "DELE");
    Dmod_Free(os_path);
}

static void cmd_mkd(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* virtual_path = NULL;
    char* os_path = resolve_arg(session, cmd, &virtual_path);
    if (os_path == NULL)
        return;

    if (Dmod_MakeDir(os_path, 0755) == 0)
    {
        /* RFC 959 §4.2's 257 form quotes the path that was created. */
        char* text = dmftp_str_join("\"", virtual_path, "\" created.");
        dmftp_server_reply(session, 257, text != NULL ? text : "Directory created.");
        Dmod_Free(text);
    }
    else
    {
        dmftp_server_reply(session, 550, "MKD: could not create directory.");
    }

    Dmod_Free(virtual_path);
    Dmod_Free(os_path);
}

static void cmd_rmd(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* os_path = resolve_arg(session, cmd, NULL);
    if (os_path == NULL)
        return;

    reply_result(session, Dmod_RemoveDir(os_path) == 0, "RMD");
    Dmod_Free(os_path);
}

static void cmd_rnfr(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* os_path = resolve_arg(session, cmd, NULL);
    if (os_path == NULL)
        return;

    if (!Dmod_FileAvailable(os_path) && !dmftp_path_is_dir(os_path))
    {
        Dmod_Free(os_path);
        dmftp_server_reply(session, 550, "No such file or directory.");
        return;
    }

    Dmod_Free(session->rename_from);
    session->rename_from = os_path; /* held until the RNTO that follows */
    dmftp_server_reply(session, 350, "Ready for RNTO.");
}

static void cmd_rnto(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    if (session->rename_from == NULL)
    {
        dmftp_server_reply(session, 503, "RNFR required first.");
        return;
    }

    char* os_path = resolve_arg(session, cmd, NULL);
    if (os_path == NULL)
        return;

    bool ok = Dmod_Rename(session->rename_from, os_path) == 0;

    /* A rename pair is consumed whether or not it worked - leaving the
     * RNFR armed would let a later stray RNTO rename the wrong thing. */
    Dmod_Free(session->rename_from);
    session->rename_from = NULL;
    Dmod_Free(os_path);

    reply_result(session, ok, "RNTO");
}

/* ============================================================================
 *                      Metadata
 * ========================================================================== */

static void cmd_size(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* os_path = resolve_arg(session, cmd, NULL);
    if (os_path == NULL)
        return;

    uint32_t size = 0;
    if (!dmftp_path_file_size(os_path, &size))
    {
        Dmod_Free(os_path);
        dmftp_server_reply(session, 550, "Could not determine size.");
        return;
    }
    Dmod_Free(os_path);

    char text[16];
    if (Dmod_SnPrintf(text, sizeof(text), "%u", (unsigned)size) > 0)
    {
        dmftp_server_reply(session, 213, text);
    }
    else
    {
        dmftp_server_reply(session, 550, "Could not determine size.");
    }
}

static void cmd_mdtm(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    char* os_path = resolve_arg(session, cmd, NULL);
    if (os_path == NULL)
        return;

    bool exists = Dmod_FileAvailable(os_path) || dmftp_path_is_dir(os_path);
    Dmod_Free(os_path);

    /* Answered honestly rather than with a fabricated timestamp: the SAL
     * exposes no mtime (see dmftp.h), which is also why MDTM is missing
     * from the FEAT list and no client should be asking in the first place. */
    dmftp_server_reply(session, 550, exists ? "Modification time is not available."
                                            : "No such file or directory.");
}

/**
 * @brief STAT: session status with no argument, a control-channel listing
 *        with one (RFC 959 §4.1.3)
 */
static void cmd_stat(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    if (cmd->arg == NULL)
    {
        dmftp_server_reply_raw(session, "211-Status:");
        dmftp_server_reply_raw(session, session->login == dmftp_login_done ? " Logged in" : " Not logged in");
        dmftp_server_reply_raw(session, session->type == dmftp_type_image ? " TYPE: I" : " TYPE: A");
        dmftp_server_reply_raw(session, session->read_only ? " Access: read-only" : " Access: read-write");
        dmftp_server_reply_raw(session, "211 End of status");
        return;
    }

    char* os_path = resolve_arg(session, cmd, NULL);
    if (os_path == NULL)
        return;

    void* dir = Dmod_OpenDir(os_path);
    if (dir == NULL)
    {
        Dmod_Free(os_path);
        dmftp_server_reply(session, 550, "No such directory.");
        return;
    }

    dmftp_server_reply_raw(session, "213-Status follows:");

    dmftp_buf_t listing;
    dmftp_buf_init(&listing);

    const Dmod_DirEntry_t* entry;
    while ((entry = Dmod_ReadDirEx(dir)) != NULL)
    {
        if (entry->name[0] == '.' &&
            (entry->name[1] == '\0' || (entry->name[1] == '.' && entry->name[2] == '\0')))
            continue;

        if (dmftp_xfer_format_entry(&listing, os_path, entry->name,
                                     entry->type == Dmod_DirEntryType_Dir, true) != 0)
            break;
    }

    dmftp_net_send(session->control, &session->out, listing.data, listing.len);
    dmftp_buf_free(&listing);
    Dmod_CloseDir(dir);
    Dmod_Free(os_path);

    dmftp_server_reply_raw(session, "213 End of status");
}

static void cmd_help(struct dmftp_session* session, const dmftp_command_t* cmd)
{
    (void)cmd;
    dmftp_server_reply_raw(session, "214-The following commands are recognized:");
    dmftp_server_reply_raw(session, " USER PASS ACCT QUIT NOOP SYST FEAT OPTS HELP STAT");
    dmftp_server_reply_raw(session, " TYPE MODE STRU ALLO REST PWD  CWD  CDUP");
    dmftp_server_reply_raw(session, " PASV PORT LIST NLST RETR STOR APPE ABOR");
    dmftp_server_reply_raw(session, " DELE MKD  RMD  RNFR RNTO SIZE MDTM");
    dmftp_server_reply_raw(session, "214 Help OK.");
}

/* ============================================================================
 *                      Dispatch
 * ========================================================================== */

/**
 * @brief Every command dmftp answers, as integers only
 *
 * The X-prefixed spellings (XPWD, XCWD, XMKD, XRMD, XCUP) are RFC 775's
 * older names for the same operations; some embedded clients still emit
 * them, and aliasing them costs one row each.
 *
 * No pointers here on purpose - see this file's header comment for the
 * loader constraint that rules them out.
 */
static const struct dmftp_cmd_entry g_commands[] = {
    { DMFTP_VERB('U','S','E','R'), dmftp_cmd_id_user, 0 },
    { DMFTP_VERB('P','A','S','S'), dmftp_cmd_id_pass, 0 },
    { DMFTP_VERB('A','C','C','T'), dmftp_cmd_id_acct, 0 },
    { DMFTP_VERB('Q','U','I','T'), dmftp_cmd_id_quit, 0 },
    { DMFTP_VERB('N','O','O','P'), dmftp_cmd_id_noop, 0 },
    { DMFTP_VERB('S','Y','S','T'), dmftp_cmd_id_syst, 0 },
    { DMFTP_VERB('F','E','A','T'), dmftp_cmd_id_feat, 0 },
    { DMFTP_VERB('O','P','T','S'), dmftp_cmd_id_opts, DMFTP_CMD_ARG },
    { DMFTP_VERB('H','E','L','P'), dmftp_cmd_id_help, 0 },
    { DMFTP_VERB('S','T','A','T'), dmftp_cmd_id_stat, DMFTP_CMD_AUTH },

    { DMFTP_VERB('T','Y','P','E'), dmftp_cmd_id_type, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('M','O','D','E'), dmftp_cmd_id_mode, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('S','T','R','U'), dmftp_cmd_id_stru, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('A','L','L','O'), dmftp_cmd_id_allo, DMFTP_CMD_AUTH },
    { DMFTP_VERB('R','E','S','T'), dmftp_cmd_id_rest, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },

    { DMFTP_VERB('P','W','D','\0'), dmftp_cmd_id_pwd,  DMFTP_CMD_AUTH },
    { DMFTP_VERB('X','P','W','D'),   dmftp_cmd_id_pwd,  DMFTP_CMD_AUTH },
    { DMFTP_VERB('C','W','D','\0'), dmftp_cmd_id_cwd,  DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('X','C','W','D'),   dmftp_cmd_id_cwd,  DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('C','D','U','P'),   dmftp_cmd_id_cdup, DMFTP_CMD_AUTH },
    { DMFTP_VERB('X','C','U','P'),   dmftp_cmd_id_cdup, DMFTP_CMD_AUTH },

    { DMFTP_VERB('P','A','S','V'), dmftp_cmd_id_pasv, DMFTP_CMD_AUTH },
    { DMFTP_VERB('P','O','R','T'), dmftp_cmd_id_port, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('L','I','S','T'), dmftp_cmd_id_list, DMFTP_CMD_AUTH },
    { DMFTP_VERB('N','L','S','T'), dmftp_cmd_id_nlst, DMFTP_CMD_AUTH },
    { DMFTP_VERB('R','E','T','R'), dmftp_cmd_id_retr, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('S','T','O','R'), dmftp_cmd_id_stor, DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('A','P','P','E'), dmftp_cmd_id_appe, DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('A','B','O','R'), dmftp_cmd_id_abor, DMFTP_CMD_AUTH },

    { DMFTP_VERB('D','E','L','E'),   dmftp_cmd_id_dele, DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('M','K','D','\0'), dmftp_cmd_id_mkd,  DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('X','M','K','D'),   dmftp_cmd_id_mkd,  DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('R','M','D','\0'), dmftp_cmd_id_rmd,  DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('X','R','M','D'),   dmftp_cmd_id_rmd,  DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('R','N','F','R'),   dmftp_cmd_id_rnfr, DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },
    { DMFTP_VERB('R','N','T','O'),   dmftp_cmd_id_rnto, DMFTP_CMD_AUTH | DMFTP_CMD_ARG | DMFTP_CMD_WRITE },

    { DMFTP_VERB('S','I','Z','E'), dmftp_cmd_id_size, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
    { DMFTP_VERB('M','D','T','M'), dmftp_cmd_id_mdtm, DMFTP_CMD_AUTH | DMFTP_CMD_ARG },
};

/**
 * @brief Pack a parsed verb the same way the table does
 *
 * The verb is already upper-cased and at most DMFTP_VERB_MAX long
 * (dmftp_parse_command() guarantees both), so this is exact - shorter verbs
 * pad with NULs, matching the DMFTP_VERB() spellings above.
 */
static uint32_t verb_key(const char* verb)
{
    uint32_t key = 0;
    bool     ended = false;

    for (size_t i = 0; i < DMFTP_VERB_MAX; i++)
    {
        ended = ended || verb[i] == '\0';
        key = (key << 8) | (ended ? 0u : (uint32_t)(uint8_t)verb[i]);
    }
    return key;
}

static const struct dmftp_cmd_entry* find_command(const char* verb)
{
    uint32_t key = verb_key(verb);
    for (size_t i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); i++)
    {
        if (g_commands[i].key == key)
            return &g_commands[i];
    }
    return NULL;
}

/**
 * @brief Apply an entry's flags, replying itself if the command is refused
 *
 * @return true if the handler may run
 */
static bool check_flags(struct dmftp_session* session, const struct dmftp_cmd_entry* entry, const dmftp_command_t* cmd)
{
    if ((entry->flags & DMFTP_CMD_AUTH) != 0 && session->login != dmftp_login_done)
    {
        dmftp_server_reply(session, 530, "Please login with USER and PASS.");
        return false;
    }
    if ((entry->flags & DMFTP_CMD_ARG) != 0 && (cmd->arg == NULL || cmd->arg_len == 0))
    {
        dmftp_server_reply(session, 501, "Syntax error: argument required.");
        return false;
    }
    if ((entry->flags & DMFTP_CMD_WRITE) != 0 && session->read_only)
    {
        dmftp_server_reply(session, 550, "Permission denied: read-only access.");
        return false;
    }
    return true;
}

/**
 * @brief Login, session parameters and the informational commands
 *
 * @return true if `id` was handled here
 */
static bool invoke_session_command(struct dmftp_session* session, uint8_t id, const dmftp_command_t* cmd)
{
    switch (id)
    {
        case dmftp_cmd_id_user: cmd_user(session, cmd); return true;
        case dmftp_cmd_id_pass: cmd_pass(session, cmd); return true;
        case dmftp_cmd_id_acct: cmd_acct(session, cmd); return true;
        case dmftp_cmd_id_quit: cmd_quit(session, cmd); return true;
        case dmftp_cmd_id_noop: cmd_noop(session, cmd); return true;
        case dmftp_cmd_id_syst: cmd_syst(session, cmd); return true;
        case dmftp_cmd_id_feat: cmd_feat(session, cmd); return true;
        case dmftp_cmd_id_opts: cmd_opts(session, cmd); return true;
        case dmftp_cmd_id_help: cmd_help(session, cmd); return true;
        case dmftp_cmd_id_stat: cmd_stat(session, cmd); return true;
        case dmftp_cmd_id_type: cmd_type(session, cmd); return true;
        case dmftp_cmd_id_mode: cmd_mode(session, cmd); return true;
        case dmftp_cmd_id_stru: cmd_stru(session, cmd); return true;
        case dmftp_cmd_id_allo: cmd_allo(session, cmd); return true;
        case dmftp_cmd_id_rest: cmd_rest(session, cmd); return true;
        default: return false;
    }
}

/**
 * @brief Navigation, filesystem mutation and metadata
 *
 * @return true if `id` was handled here
 */
static bool invoke_file_command(struct dmftp_session* session, uint8_t id, const dmftp_command_t* cmd)
{
    switch (id)
    {
        case dmftp_cmd_id_pwd:  cmd_pwd(session, cmd);  return true;
        case dmftp_cmd_id_cwd:  cmd_cwd(session, cmd);  return true;
        case dmftp_cmd_id_cdup: cmd_cdup(session, cmd); return true;
        case dmftp_cmd_id_dele: cmd_dele(session, cmd); return true;
        case dmftp_cmd_id_mkd:  cmd_mkd(session, cmd);  return true;
        case dmftp_cmd_id_rmd:  cmd_rmd(session, cmd);  return true;
        case dmftp_cmd_id_rnfr: cmd_rnfr(session, cmd); return true;
        case dmftp_cmd_id_rnto: cmd_rnto(session, cmd); return true;
        case dmftp_cmd_id_size: cmd_size(session, cmd); return true;
        case dmftp_cmd_id_mdtm: cmd_mdtm(session, cmd); return true;
        default: return false;
    }
}

/**
 * @brief The commands that need a data connection (dmftp_server_xfer.c)
 */
static void invoke_data_command(struct dmftp_session* session, uint8_t id, const dmftp_command_t* cmd)
{
    switch (id)
    {
        case dmftp_cmd_id_pasv: dmftp_server_cmd_pasv(session, cmd); break;
        case dmftp_cmd_id_port: dmftp_server_cmd_port(session, cmd); break;
        case dmftp_cmd_id_list: dmftp_server_cmd_list(session, cmd); break;
        case dmftp_cmd_id_nlst: dmftp_server_cmd_nlst(session, cmd); break;
        case dmftp_cmd_id_retr: dmftp_server_cmd_retr(session, cmd); break;
        case dmftp_cmd_id_stor: dmftp_server_cmd_stor(session, cmd); break;
        case dmftp_cmd_id_appe: dmftp_server_cmd_appe(session, cmd); break;
        case dmftp_cmd_id_abor: dmftp_server_cmd_abor(session, cmd); break;
        default: dmftp_server_reply(session, 502, "Command not implemented."); break;
    }
}

void dmftp_server_dispatch(struct dmftp_session* session, const char* line, size_t len)
{
    dmftp_command_t cmd;
    if (dmftp_parse_command(line, len, &cmd) != 0)
    {
        dmftp_server_reply(session, 500, "Syntax error, command unrecognized.");
        return;
    }

    const struct dmftp_cmd_entry* entry = find_command(cmd.verb);
    if (entry == NULL)
    {
        /* EPSV/EPRT land here on purpose: RFC 2428's extended tuples exist
         * for IPv6, and dmtcp cannot originate an IPv6 segment at all (see
         * dmftp.h), so 502 is the truthful answer rather than a 500. */
        dmftp_server_reply(session, 502, "Command not implemented.");
        return;
    }

    if (!check_flags(session, entry, &cmd))
        return;

    if (invoke_session_command(session, entry->id, &cmd))
        return;
    if (invoke_file_command(session, entry->id, &cmd))
        return;

    invoke_data_command(session, entry->id, &cmd);
}
