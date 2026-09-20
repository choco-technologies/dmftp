#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmftp.h"
#include <string.h>

/* dmod modules have no libc memcpy()/memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - a small manual loop stands in for it, same
 * convention as dmtcp/dmudp/dmicmp/dmtelnet's own tests. */
static void bytes_copy(char* dst, const char* src, size_t len)
{
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];
}

static bool str_equal(const char* a, const char* b)
{
    size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return a[i] == b[i];
}

static dmftp_t g_session = NULL;

/* on_command capture */
#define CAPTURE_MAX 256
static char g_verb[CAPTURE_MAX];
static char g_arg[CAPTURE_MAX];
static int  g_command_calls;

/* on_send capture */
static char   g_send_buf[CAPTURE_MAX];
static size_t g_send_len;

static void reset_captures(void)
{
    g_verb[0] = '\0';
    g_arg[0] = '\0';
    g_command_calls = 0;
    g_send_len = 0;
}

static void on_command(dmftp_t session, const char* verb, const char* arg, void* user_data)
{
    (void)session;
    (void)user_data;
    bytes_copy(g_verb, verb, strlen(verb) + 1);
    bytes_copy(g_arg, arg, strlen(arg) + 1);
    g_command_calls++;
}

static void on_send(dmftp_t session, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)session;
    (void)user_data;
    if (g_send_len + data_len <= sizeof(g_send_buf))
    {
        bytes_copy(g_send_buf + g_send_len, (const char*)data, data_len);
        g_send_len += data_len;
    }
}

void dmod_test_setup(void)
{
    reset_captures();

    dmftp_callbacks_t callbacks = {
        .on_command = on_command,
        .on_send    = on_send,
    };
    g_session = dmftp_create(&callbacks, NULL);
}

void dmod_test_teardown(void)
{
    dmftp_destroy(g_session);
    g_session = NULL;
}

DMOD_TEST_STEP(dmftp_create_requires_on_send)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_session);

    dmftp_callbacks_t no_send = { 0 };
    DMOD_TEST_EXPECT_NULL(dmftp_create(&no_send, NULL));
    DMOD_TEST_EXPECT_NULL(dmftp_create(NULL, NULL));
}

DMOD_TEST_STEP(dmftp_recv_splits_verb_and_arg)
{
    const uint8_t input[] = "USER anonymous\r\n";
    int ret = dmftp_recv(g_session, input, sizeof(input) - 1);

    DMOD_TEST_EXPECT_EQ(ret, 0);
    DMOD_TEST_EXPECT_EQ(g_command_calls, 1);
    DMOD_TEST_EXPECT_TRUE(str_equal(g_verb, "USER"));
    DMOD_TEST_EXPECT_TRUE(str_equal(g_arg, "anonymous"));
}

DMOD_TEST_STEP(dmftp_recv_upper_cases_the_verb)
{
    const uint8_t input[] = "user bob\r\n";
    dmftp_recv(g_session, input, sizeof(input) - 1);

    DMOD_TEST_EXPECT_TRUE(str_equal(g_verb, "USER"));
    DMOD_TEST_EXPECT_TRUE(str_equal(g_arg, "bob"));
}

DMOD_TEST_STEP(dmftp_recv_command_with_no_argument)
{
    const uint8_t input[] = "NOOP\r\n";
    dmftp_recv(g_session, input, sizeof(input) - 1);

    DMOD_TEST_EXPECT_TRUE(str_equal(g_verb, "NOOP"));
    DMOD_TEST_EXPECT_TRUE(str_equal(g_arg, ""));
}

DMOD_TEST_STEP(dmftp_recv_tolerates_bare_lf)
{
    /* Real clients always send CRLF, but a bare LF (no preceding CR)
     * should still terminate the line - see dmftp_recv()'s doc comment. */
    const uint8_t input[] = "PWD\n";
    dmftp_recv(g_session, input, sizeof(input) - 1);

    DMOD_TEST_EXPECT_EQ(g_command_calls, 1);
    DMOD_TEST_EXPECT_TRUE(str_equal(g_verb, "PWD"));
}

DMOD_TEST_STEP(dmftp_recv_across_multiple_calls)
{
    /* A command line split across two dmftp_recv() calls (e.g. two TCP
     * segments) must still be reported as one command once complete. */
    const uint8_t part1[] = "CWD /so";
    const uint8_t part2[] = "me/dir\r\n";

    dmftp_recv(g_session, part1, sizeof(part1) - 1);
    DMOD_TEST_EXPECT_EQ(g_command_calls, 0);

    dmftp_recv(g_session, part2, sizeof(part2) - 1);
    DMOD_TEST_EXPECT_EQ(g_command_calls, 1);
    DMOD_TEST_EXPECT_TRUE(str_equal(g_verb, "CWD"));
    DMOD_TEST_EXPECT_TRUE(str_equal(g_arg, "/some/dir"));
}

DMOD_TEST_STEP(dmftp_recv_multiple_lines_in_one_call)
{
    const uint8_t input[] = "TYPE I\r\nPWD\r\n";
    dmftp_recv(g_session, input, sizeof(input) - 1);

    /* Only the *last* command's verb/arg remain captured, but both must
     * have fired. */
    DMOD_TEST_EXPECT_EQ(g_command_calls, 2);
    DMOD_TEST_EXPECT_TRUE(str_equal(g_verb, "PWD"));
}

DMOD_TEST_STEP(dmftp_recv_ignores_blank_lines)
{
    const uint8_t input[] = "\r\n\r\nNOOP\r\n";
    dmftp_recv(g_session, input, sizeof(input) - 1);

    DMOD_TEST_EXPECT_EQ(g_command_calls, 1);
}

DMOD_TEST_STEP(dmftp_recv_rejects_null_data_with_nonzero_len)
{
    int ret = dmftp_recv(g_session, NULL, 5);
    DMOD_TEST_EXPECT_EQ(ret, -22 /* EINVAL */);
}

DMOD_TEST_STEP(dmftp_reply_formats_code_and_text)
{
    int ret = dmftp_reply(g_session, 230, "Login successful");

    DMOD_TEST_EXPECT_EQ(ret, 0);
    g_send_buf[g_send_len] = '\0';
    DMOD_TEST_EXPECT_TRUE(str_equal(g_send_buf, "230 Login successful\r\n"));
}

DMOD_TEST_STEP(dmftp_reply_rejects_bad_code)
{
    DMOD_TEST_EXPECT_EQ(dmftp_reply(g_session, 99, "x"), -22 /* EINVAL */);
    DMOD_TEST_EXPECT_EQ(dmftp_reply(g_session, 560, "x"), -22 /* EINVAL */);
}

DMOD_TEST_STEP(dmftp_reply_rejects_null_args)
{
    DMOD_TEST_EXPECT_EQ(dmftp_reply(NULL, 200, "x"), -22 /* EINVAL */);
    DMOD_TEST_EXPECT_EQ(dmftp_reply(g_session, 200, NULL), -22 /* EINVAL */);
}

DMOD_TEST_STEP(dmftp_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmftp_destroy(NULL);
}
