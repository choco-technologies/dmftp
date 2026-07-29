/**
 * @file ftpd.c
 * @brief `ftpd` - the FTP service front-end over the dmftp library
 *
 * A thin Application module: it turns a unit file's `args=` line and an INI
 * configuration into a dmftp_server_config_t, owns the user table that
 * backs dmftp's authentication callback, and starts the server. It holds no
 * protocol logic of its own - that all lives in the `dmftp` Library, which
 * anything else embedding an FTP server links against directly.
 *
 * @par main() returns; the server keeps running
 * dmftp is entirely callback-driven, so once the control port is listening
 * there is nothing for a thread to do here. main() performs the
 * configure-and-start pass and returns, exactly as `systemd` does for the
 * units it starts, and dmod_deinit() shuts the server down when the module
 * is unloaded.
 *
 * @par Configuration
 * The INI file supplies the defaults and the user table; command-line flags
 * override the INI, so a unit can share one configuration file and still
 * differ in a single setting. See docs/configuration.md.
 */
#include "dmod.h"
#include "dmftp.h"
#include "dmini.h"
#include "dmlist.h"
#include <string.h>
#include <errno.h>

#define FTPD_ALLOCATOR_NAME "ftpd"
#define FTPD_DEFAULT_CONFIG "/etc/ftpd.ini"

/** @brief One account ftpd will accept a login for */
struct ftpd_user
{
    char* name;
    char* password; /**< empty string means "any password" */
    char* home;     /**< NULL keeps the server-wide root */
    bool  read_only;
};

/** @brief Everything the command line and the INI file can set */
struct ftpd_config
{
    uint16_t port;
    char*    root;
    char*    banner;
    bool     read_only;
    bool     passive;
    bool     active;
    uint16_t active_data_port;
    uint32_t max_sessions;
    bool     anonymous;
};

static dmlist_context_t* g_users = NULL;
static dmftp_server_t    g_server = NULL;

/* ============================================================================
 *                      The user table
 * ========================================================================== */

static void user_free(struct ftpd_user* user)
{
    Dmod_Free(user->name);
    Dmod_Free(user->password);
    Dmod_Free(user->home);
    Dmod_Free(user);
}

static void users_clear(void)
{
    if (g_users == NULL)
        return;

    struct ftpd_user* user;
    while ((user = dmlist_pop_front(g_users)) != NULL)
    {
        user_free(user);
    }
}

static int compare_user_name(const void* data, const void* user_data)
{
    const struct ftpd_user* user = (const struct ftpd_user*)data;
    return strcmp(user->name, (const char*)user_data) == 0 ? 0 : -1;
}

static struct ftpd_user* find_user(const char* name)
{
    if (g_users == NULL || name == NULL)
        return NULL;

    return (struct ftpd_user*)dmlist_find(g_users, name, compare_user_name);
}

/**
 * @brief Add (or replace) an account
 *
 * A later definition wins, which is what makes a `--user` flag override the
 * same name coming out of the INI file.
 *
 * @return 0 on success, -ENOMEM
 */
static int user_add(const char* name, const char* password, const char* home, bool read_only)
{
    struct ftpd_user* existing = find_user(name);
    if (existing != NULL)
    {
        dmlist_remove(g_users, existing, compare_user_name);
        user_free(existing);
    }

    struct ftpd_user* user = Dmod_Malloc(sizeof(*user));
    if (user == NULL)
        return -ENOMEM;

    memset(user, 0, sizeof(*user));
    user->name = Dmod_StrDup(name);
    user->password = Dmod_StrDup(password != NULL ? password : "");
    user->home = (home != NULL && home[0] != '\0') ? Dmod_StrDup(home) : NULL;
    user->read_only = read_only;

    if (user->name == NULL || user->password == NULL || !dmlist_push_back(g_users, user))
    {
        user_free(user);
        return -ENOMEM;
    }
    return 0;
}

/* ============================================================================
 *                      Authentication
 * ========================================================================== */

/**
 * @brief Constant-ish string equality for passwords
 *
 * Not a timing-hardened comparison - dmftp offers no rate limiting either,
 * and on a device where the control channel is plaintext FTP the threat
 * model does not stop at timing. Documented rather than pretended away; see
 * docs/configuration.md's security note.
 */
static bool password_matches(const struct ftpd_user* user, const char* password)
{
    if (user->password[0] == '\0')
        return true; /* an empty configured password accepts any */

    return password != NULL && strcmp(user->password, password) == 0;
}

/**
 * @brief dmftp_auth_handler_t: look the name up and apply that user's home
 *        and permissions
 */
static bool ftpd_authenticate(dmftp_session_t session, const char* user, const char* password, void* user_data)
{
    struct ftpd_config* config = (struct ftpd_config*)user_data;

    if (user == NULL)
        return false;

    struct ftpd_user* account = find_user(user);
    if (account == NULL)
    {
        /* Anonymous access, when enabled, covers the two names clients
         * actually use for it and is always read-only unless the config
         * defines a real account by that name instead. */
        bool anonymous = config->anonymous && (strcmp(user, "anonymous") == 0 || strcmp(user, "ftp") == 0);
        if (!anonymous)
        {
            Dmod_Printf("ftpd: login refused for '%s'\n", user);
            return false;
        }

        dmftp_session_set_read_only(session, true);
        return true;
    }

    if (!password_matches(account, password))
    {
        Dmod_Printf("ftpd: bad password for '%s'\n", user);
        return false;
    }

    if (account->home != NULL)
    {
        dmftp_session_set_root(session, account->home);
    }
    dmftp_session_set_read_only(session, account->read_only);
    return true;
}

/* ============================================================================
 *                      INI configuration
 * ========================================================================== */

/**
 * @brief Parse a decimal option value
 *
 * The SAL has no atoi()/strtol() (see dmod/src/module/string.c's minimal
 * replacement set), and Dmod_Sscanf() would be a heavyweight way to read
 * one port number.
 *
 * @return The value, or `fallback` if `text` is not all digits
 */
static uint32_t parse_u32(const char* text, uint32_t fallback)
{
    if (text == NULL || text[0] == '\0')
        return fallback;

    uint32_t value = 0;
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return fallback;

        value = value * 10u + (uint32_t)(text[i] - '0');
    }
    return value;
}

static bool parse_bool(const char* text, bool fallback)
{
    if (text == NULL || text[0] == '\0')
        return fallback;

    if (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "on") == 0)
        return true;
    if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 || strcmp(text, "no") == 0 || strcmp(text, "off") == 0)
        return false;

    return fallback;
}

/**
 * @brief Replace a heap-owned config string
 */
static void set_string(char** slot, const char* value)
{
    if (value == NULL || value[0] == '\0')
        return;

    char* copy = Dmod_StrDup(value);
    if (copy == NULL)
        return;

    Dmod_Free(*slot);
    *slot = copy;
}

/**
 * @brief Read the `[server]` section into `config`
 */
static void load_ini_server(dmini_context_t ini, struct ftpd_config* config)
{
    config->port = (uint16_t)dmini_get_int(ini, "server", "port", (int)config->port);
    config->active_data_port = (uint16_t)dmini_get_int(ini, "server", "active_data_port", (int)config->active_data_port);
    config->max_sessions = (uint32_t)dmini_get_int(ini, "server", "max_sessions", (int)config->max_sessions);

    set_string(&config->root, dmini_get_string(ini, "server", "root", NULL));
    set_string(&config->banner, dmini_get_string(ini, "server", "banner", NULL));

    config->read_only = parse_bool(dmini_get_string(ini, "server", "readonly", NULL), config->read_only);
    config->passive = parse_bool(dmini_get_string(ini, "server", "passive", NULL), config->passive);
    config->active = parse_bool(dmini_get_string(ini, "server", "active", NULL), config->active);
    config->anonymous = parse_bool(dmini_get_string(ini, "server", "anonymous", NULL), config->anonymous);
}

/**
 * @brief Read the `[users]` section, plus each account's optional `[home]`
 *        and `[readonly]` entry
 *
 * Three flat sections keyed by user name rather than one `[user.name]`
 * section each: it keeps the common case (a name and a password) to a
 * single readable line.
 */
static void load_ini_users(dmini_context_t ini, const struct ftpd_config* config)
{
    int count = dmini_key_count(ini, "users");
    for (int i = 0; i < count; i++)
    {
        const char* name = dmini_key_name(ini, "users", i);
        if (name == NULL || name[0] == '\0')
            continue;

        const char* password = dmini_get_string(ini, "users", name, "");
        const char* home = dmini_get_string(ini, "home", name, NULL);
        bool read_only = parse_bool(dmini_get_string(ini, "readonly", name, NULL), config->read_only);

        if (user_add(name, password, home, read_only) != 0)
        {
            Dmod_Printf("ftpd: out of memory adding user '%s'\n", name);
            return;
        }
    }
}

/**
 * @return 0 on success, -ENOENT if the file could not be parsed
 */
static int load_ini(const char* path, struct ftpd_config* config)
{
    dmini_context_t ini = dmini_create();
    if (ini == NULL)
        return -ENOMEM;

    int result = dmini_parse_file(ini, path);
    if (result != 0)
    {
        dmini_destroy(ini);
        return -ENOENT;
    }

    load_ini_server(ini, config);
    load_ini_users(ini, config);
    dmini_destroy(ini);
    return 0;
}

/* ============================================================================
 *                      Command line
 * ========================================================================== */

static void print_usage(const char* program)
{
    Dmod_Printf("Usage: %s [options]\n", program);
    Dmod_Printf("\nOptions:\n");
    Dmod_Printf("  -c, --config PATH        INI file to read (default %s)\n", FTPD_DEFAULT_CONFIG);
    Dmod_Printf("  -p, --port N             control port (default %u)\n", (unsigned)DMFTP_PORT_CONTROL);
    Dmod_Printf("  -r, --root PATH          directory every session is confined to\n");
    Dmod_Printf("      --banner TEXT        text of the 220 greeting\n");
    Dmod_Printf("      --max-sessions N     concurrent session limit (0 = unlimited)\n");
    Dmod_Printf("      --active-data-port N local port for PORT transfers (0 = ephemeral)\n");
    Dmod_Printf("      --read-only          refuse every mutating command\n");
    Dmod_Printf("      --no-passive         refuse PASV\n");
    Dmod_Printf("      --no-active          refuse PORT\n");
    Dmod_Printf("      --anonymous          allow anonymous/ftp logins (read-only)\n");
    Dmod_Printf("      --user NAME:PASS[:HOME[:ro]]  define an account\n");
    Dmod_Printf("  -h, --help               show this message and exit\n");
}

/**
 * @brief Add an account from a `NAME:PASSWORD[:HOME[:ro]]` flag
 *
 * @return 0 on success, -EINVAL on a malformed field, -ENOMEM
 */
static int parse_user_flag(const char* spec, bool default_read_only)
{
    char* copy = Dmod_StrDup(spec);
    if (copy == NULL)
        return -ENOMEM;

    char* fields[4] = { copy, NULL, NULL, NULL };
    size_t count = 1;
    for (char* cursor = copy; *cursor != '\0' && count < 4; cursor++)
    {
        if (*cursor == ':')
        {
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
    }

    int result = -EINVAL;
    if (fields[0][0] != '\0')
    {
        bool read_only = (fields[3] != NULL) ? parse_bool(fields[3], true) || strcmp(fields[3], "ro") == 0
                                             : default_read_only;
        result = user_add(fields[0], fields[1], fields[2], read_only);
    }

    Dmod_Free(copy);
    return result;
}

/**
 * @brief Whether `arg` is one of two spellings of the same option
 */
static bool is_option(const char* arg, const char* short_form, const char* long_form)
{
    return (short_form != NULL && strcmp(arg, short_form) == 0) || strcmp(arg, long_form) == 0;
}

/**
 * @brief Apply an option that takes the following argv entry as its value
 *
 * @return 2 (the option plus its value) if `arg` was one of these,
 *         -EINVAL if it was but the value is missing, -ENOENT if it was not
 */
static int apply_value_option(const char* arg, const char* value, struct ftpd_config* config)
{
    bool known = is_option(arg, "-p", "--port") || is_option(arg, "-r", "--root")
              || is_option(arg, NULL, "--banner") || is_option(arg, NULL, "--max-sessions")
              || is_option(arg, NULL, "--active-data-port") || is_option(arg, NULL, "--user");
    if (!known)
        return -ENOENT;
    if (value == NULL)
        return -EINVAL;

    if (is_option(arg, "-p", "--port"))
    {
        config->port = (uint16_t)parse_u32(value, config->port);
    }
    else if (is_option(arg, "-r", "--root"))
    {
        set_string(&config->root, value);
    }
    else if (is_option(arg, NULL, "--banner"))
    {
        set_string(&config->banner, value);
    }
    else if (is_option(arg, NULL, "--max-sessions"))
    {
        config->max_sessions = parse_u32(value, config->max_sessions);
    }
    else if (is_option(arg, NULL, "--active-data-port"))
    {
        config->active_data_port = (uint16_t)parse_u32(value, config->active_data_port);
    }
    else if (parse_user_flag(value, config->read_only) != 0)
    {
        return -EINVAL;
    }
    return 2;
}

/**
 * @brief Apply a standalone flag
 *
 * @return 1 if `arg` was one of these, -ENOENT otherwise
 */
static int apply_flag_option(const char* arg, struct ftpd_config* config)
{
    if (is_option(arg, NULL, "--read-only"))
    {
        config->read_only = true;
    }
    else if (is_option(arg, NULL, "--no-passive"))
    {
        config->passive = false;
    }
    else if (is_option(arg, NULL, "--no-active"))
    {
        config->active = false;
    }
    else if (is_option(arg, NULL, "--anonymous"))
    {
        config->anonymous = true;
    }
    else
    {
        return -ENOENT;
    }
    return 1;
}

/**
 * @brief Apply one option; `value` is the next argv entry (may be NULL)
 *
 * @return How many argv entries were consumed (1 or 2), or a negative errno
 */
static int apply_option(const char* arg, const char* value, struct ftpd_config* config)
{
    int consumed = apply_value_option(arg, value, config);
    if (consumed != -ENOENT)
        return consumed;

    return apply_flag_option(arg, config);
}

/**
 * @brief Find `--config`/`-c` before anything else
 *
 * The INI file has to be read first so that later flags can override what
 * it set, which means one pass to locate it and a second for the rest.
 *
 * @return The path given on the command line, or FTPD_DEFAULT_CONFIG
 */
static const char* find_config_path(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; i++)
    {
        if (is_option(argv[i], "-c", "--config"))
            return argv[i + 1];
    }
    return FTPD_DEFAULT_CONFIG;
}

/**
 * @return 0 on success, -EINVAL on a malformed option, 1 if usage was
 *         requested
 */
static int parse_args(int argc, char** argv, struct ftpd_config* config)
{
    int index = 1;
    while (index < argc)
    {
        if (is_option(argv[index], "-h", "--help"))
            return 1;

        if (is_option(argv[index], "-c", "--config"))
        {
            index += 2; /* already handled by find_config_path() */
            continue;
        }

        const char* value = (index + 1 < argc) ? argv[index + 1] : NULL;
        int consumed = apply_option(argv[index], value, config);
        if (consumed < 0)
        {
            Dmod_Printf("ftpd: unrecognized or incomplete option '%s'\n", argv[index]);
            return -EINVAL;
        }
        index += consumed;
    }
    return 0;
}

/* ============================================================================
 *                      Entry point
 * ========================================================================== */

static struct ftpd_config g_config;

static void config_defaults(struct ftpd_config* config)
{
    memset(config, 0, sizeof(*config));
    config->port = DMFTP_PORT_CONTROL;
    config->passive = true;
    config->active = true;
}

static void config_free(struct ftpd_config* config)
{
    Dmod_Free(config->root);
    Dmod_Free(config->banner);
    config->root = NULL;
    config->banner = NULL;
}

/**
 * @brief Build the server out of `g_config` and start listening
 *
 * @return 0 on success, otherwise the dmftp error that stopped it
 */
static int start_server(void)
{
    dmftp_server_config_t server_config = { 0 };
    server_config.port = g_config.port;
    server_config.root = g_config.root;
    server_config.banner = g_config.banner;
    server_config.read_only = g_config.read_only;
    server_config.allow_passive = g_config.passive;
    server_config.allow_active = g_config.active;
    server_config.active_data_port = g_config.active_data_port;
    server_config.max_sessions = g_config.max_sessions;

    dmftp_server_callbacks_t callbacks = { 0 };
    callbacks.on_auth = ftpd_authenticate;

    g_server = dmftp_server_create(&server_config, &callbacks, &g_config);
    if (g_server == NULL)
        return -ENOMEM;

    int result = dmftp_server_start(g_server);
    if (result != 0)
    {
        dmftp_server_destroy(g_server);
        g_server = NULL;
    }
    return result;
}

int main(int argc, char** argv)
{
    const char* program = (argc > 0) ? argv[0] : "ftpd";

    g_users = dmlist_create(FTPD_ALLOCATOR_NAME);
    if (g_users == NULL)
        return -ENOMEM;

    config_defaults(&g_config);

    const char* config_path = find_config_path(argc, argv);
    if (load_ini(config_path, &g_config) != 0)
    {
        /* Not fatal: a unit may configure everything through flags, and a
         * missing default path is the normal case for that. */
        Dmod_Printf("ftpd: no usable configuration at '%s', continuing with defaults\n", config_path);
    }

    int parsed = parse_args(argc, argv, &g_config);
    if (parsed != 0)
    {
        print_usage(program);
        users_clear();
        config_free(&g_config);
        return parsed > 0 ? 0 : parsed;
    }

    if (dmlist_size(g_users) == 0 && !g_config.anonymous)
    {
        Dmod_Printf("ftpd: no accounts configured and anonymous access is off - nobody could log in\n");
        users_clear();
        config_free(&g_config);
        return -EINVAL;
    }

    int result = start_server();
    if (result != 0)
    {
        Dmod_Printf("ftpd: could not start on port %u (%d)\n", (unsigned)g_config.port, result);
        users_clear();
        config_free(&g_config);
        return result;
    }

    Dmod_Printf("ftpd: listening on port %u, root '%s'%s\n",
                 (unsigned)g_config.port,
                 g_config.root != NULL ? g_config.root : "/",
                 g_config.read_only ? " (read-only)" : "");
    return 0;
}

/**
 * @brief Stop the server when the module is unloaded
 *
 * main() returned long ago - this is the only place the listening port and
 * the live sessions get released.
 */
int dmod_deinit(void)
{
    if (g_server != NULL)
    {
        dmftp_server_destroy(g_server);
        g_server = NULL;
    }

    users_clear();
    if (g_users != NULL)
    {
        dmlist_destroy(g_users);
        g_users = NULL;
    }
    config_free(&g_config);
    return 0;
}
