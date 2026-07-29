/**
 * @file ftpd_test.c
 * @brief Test steps for the ftpd service module
 *
 * ftpd is an Application module: its logic is main(), which a test module
 * cannot call (a test provides its own main - see dmod_add_test). What is
 * testable without duplicating that entry point is the contract ftpd
 * depends on: that a dmftp server built exactly the way start_server()
 * builds one behaves as ftpd assumes, and that the INI file shipped in
 * examples/ parses into the values documented in docs/configuration.md.
 *
 * The service is exercised end to end by running it, not from here - see
 * app/ftpd/docs/README.md.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmftp.h"
#include "dmini.h"
#include <string.h>
#include <errno.h>

#define TEST_FTPD_PORT 2121u

/** @brief The example configuration, inlined so the test needs no fixture file */
static const char* g_example_ini =
    "[server]\n"
    "port = 21\n"
    "root = /srv/ftp\n"
    "readonly = false\n"
    "banner = dmftp ready\n"
    "max_sessions = 4\n"
    "passive = true\n"
    "active = true\n"
    "anonymous = false\n"
    "\n"
    "[users]\n"
    "admin = s3cret\n"
    "guest =\n"
    "\n"
    "[home]\n"
    "guest = /srv/ftp/pub\n"
    "\n"
    "[readonly]\n"
    "guest = true\n";

static bool text_equal(const char* a, const char* b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static bool g_auth_answer;
static bool g_auth_called;

static bool test_auth(dmftp_session_t session, const char* user, const char* password, void* user_data)
{
    (void)session; (void)user; (void)password; (void)user_data;
    g_auth_called = true;
    return g_auth_answer;
}

void dmod_test_setup(void)
{
    g_auth_called = false;
    g_auth_answer = true;
}

void dmod_test_teardown(void)
{
}

/* ============================================================================
 *                      The configuration file
 * ========================================================================== */

DMOD_TEST_STEP(example_ini_parses_the_server_section)
{
    dmini_context_t ini = dmini_create();
    DMOD_TEST_EXPECT_NOT_NULL(ini);
    DMOD_TEST_EXPECT_EQ(dmini_parse_string(ini, g_example_ini), 0);

    DMOD_TEST_EXPECT_EQ(dmini_get_int(ini, "server", "port", 0), 21);
    DMOD_TEST_EXPECT_EQ(dmini_get_int(ini, "server", "max_sessions", 0), 4);
    DMOD_TEST_EXPECT_TRUE(text_equal(dmini_get_string(ini, "server", "root", NULL), "/srv/ftp"));
    DMOD_TEST_EXPECT_TRUE(text_equal(dmini_get_string(ini, "server", "banner", NULL), "dmftp ready"));

    /* A key ftpd does not set must fall through to the caller's default
     * rather than to zero - that is what makes flags able to override the
     * file and the file able to override the built-in defaults. */
    DMOD_TEST_EXPECT_EQ(dmini_get_int(ini, "server", "active_data_port", 7), 7);

    dmini_destroy(ini);
}

DMOD_TEST_STEP(example_ini_lists_users_in_order)
{
    dmini_context_t ini = dmini_create();
    DMOD_TEST_EXPECT_EQ(dmini_parse_string(ini, g_example_ini), 0);

    /* ftpd builds its account table by iterating [users]; a key count of 0
     * here would mean every login silently failed. */
    DMOD_TEST_EXPECT_EQ(dmini_key_count(ini, "users"), 2);
    DMOD_TEST_EXPECT_TRUE(text_equal(dmini_key_name(ini, "users", 0), "admin"));
    DMOD_TEST_EXPECT_TRUE(text_equal(dmini_key_name(ini, "users", 1), "guest"));
    DMOD_TEST_EXPECT_TRUE(text_equal(dmini_get_string(ini, "users", "admin", NULL), "s3cret"));

    dmini_destroy(ini);
}

DMOD_TEST_STEP(example_ini_carries_per_user_home_and_permissions)
{
    dmini_context_t ini = dmini_create();
    DMOD_TEST_EXPECT_EQ(dmini_parse_string(ini, g_example_ini), 0);

    DMOD_TEST_EXPECT_TRUE(text_equal(dmini_get_string(ini, "home", "guest", NULL), "/srv/ftp/pub"));
    DMOD_TEST_EXPECT_TRUE(text_equal(dmini_get_string(ini, "readonly", "guest", NULL), "true"));

    /* An account with no [home] entry keeps the server-wide root, which is
     * signalled by the lookup returning the caller's default. */
    DMOD_TEST_EXPECT_NULL(dmini_get_string(ini, "home", "admin", NULL));

    dmini_destroy(ini);
}

/* ============================================================================
 *                      The server ftpd builds
 * ========================================================================== */

DMOD_TEST_STEP(ftpd_shaped_server_starts_and_stops)
{
    /* The same construction start_server() performs, with the values the
     * example INI file would have produced. */
    dmftp_server_config_t config = { 0 };
    config.port = TEST_FTPD_PORT;
    config.root = "/tmp";
    config.banner = "dmftp ready";
    config.read_only = false;
    config.allow_passive = true;
    config.allow_active = true;
    config.max_sessions = 4;

    dmftp_server_callbacks_t callbacks = { 0 };
    callbacks.on_auth = test_auth;

    dmftp_server_t server = dmftp_server_create(&config, &callbacks, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(server);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_port(server), (uint16_t)TEST_FTPD_PORT);
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(server), 0);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(server), (size_t)0);

    dmftp_server_destroy(server);

    /* Destroying released the port - a fresh server can take it. */
    dmftp_server_t again = dmftp_server_create(&config, &callbacks, NULL);
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(again), 0);
    dmftp_server_destroy(again);
}

DMOD_TEST_STEP(ftpd_refuses_a_server_without_authentication)
{
    /* ftpd always installs on_auth; this pins the reason it has to. */
    dmftp_server_config_t config = { 0 };
    config.port = TEST_FTPD_PORT + 1u;

    dmftp_server_callbacks_t callbacks = { 0 };
    DMOD_TEST_EXPECT_NULL(dmftp_server_create(&config, &callbacks, NULL));
}
