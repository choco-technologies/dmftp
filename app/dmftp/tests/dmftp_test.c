/**
 * @file dmftp_test.c
 * @brief Test steps for dmftp
 *
 * Two layers get exercised here, for two different reasons.
 *
 * The control-line codec (dmftp_parse_command/_format_reply/_parse_reply/
 * _format_host_port/_parse_host_port) is pure - no connection, no session -
 * so it is tested directly, exhaustively, including the malformed input a
 * remote peer gets to choose.
 *
 * The server is tested through its real public API against a real dmtcp
 * listener. It is deliberately not driven end-to-end with a live client:
 * this environment has no interface with a driver behind it (the same limit
 * dmtcp's own tests document - dmtcp_connect() always ends in -EIO here), so
 * a handshake to localhost cannot complete. What the server tests therefore
 * cover is everything reachable without a peer: lifecycle, port reservation,
 * configuration handling, session accounting, and argument validation.
 */
#define ENABLE_DIF_REGISTRATIONS ON
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmftp.h"
#include "dmtcp.h"
#include "dmroute.h"
#include "dmarp.h"
#include "dmnetbridge.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

/* Well clear of anything dmtcp's own ephemeral allocator hands out. */
#define TEST_PORT_BASE 2100u

static dmip_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmip_addr_t ip = { 0 };
    ip.family = dmip_family_v4;
    ip.addr.v4[0] = a;
    ip.addr.v4[1] = b;
    ip.addr.v4[2] = c;
    ip.addr.v4[3] = d;
    return ip;
}

/* dmod modules have no libc memcmp() (see dmod/src/module/string.c's minimal
 * replacement set) - a small manual comparison stands in for it, the same
 * pattern dmtcp's own tests use. */
static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

static bool text_equal(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

#define TEST_DEVICE_PATH "/dev/null"

static dmnetif_iface_t g_iface = NULL;

void dmod_test_setup(void)
{
    g_iface = dmnetif_register("test0", TEST_DEVICE_PATH);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface);
    g_iface = NULL;
}

/* ============================================================================
 *                      dmftp_parse_command
 * ========================================================================== */

DMOD_TEST_STEP(parse_command_rejects_bad_arguments)
{
    dmftp_command_t cmd;

    DMOD_TEST_EXPECT_EQ(dmftp_parse_command(NULL, 4u, &cmd), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_command("NOOP", 4u, NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_command("", 0u, &cmd), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_command("    ", 4u, &cmd), -EPROTO);
}

DMOD_TEST_STEP(parse_command_rejects_overlong_verb)
{
    dmftp_command_t cmd;
    const char* line = "TOOLONG /x";

    /* Five characters or more is not an FTP verb - accepting it would mean
     * the dispatch table could never match it anyway. */
    DMOD_TEST_EXPECT_EQ(dmftp_parse_command(line, strlen(line), &cmd), -EPROTO);
}

DMOD_TEST_STEP(parse_command_upcases_verb_and_splits_argument)
{
    dmftp_command_t cmd;
    const char* line = "retr /pub/file.bin";

    DMOD_TEST_EXPECT_EQ(dmftp_parse_command(line, strlen(line), &cmd), 0);
    DMOD_TEST_EXPECT_TRUE(text_equal(cmd.verb, "RETR"));
    DMOD_TEST_EXPECT_EQ(cmd.arg_len, (size_t)13);
    DMOD_TEST_EXPECT_TRUE(bytes_equal((const uint8_t*)cmd.arg, (const uint8_t*)"/pub/file.bin", 13u));
}

DMOD_TEST_STEP(parse_command_without_argument_reports_null)
{
    dmftp_command_t cmd;
    const char* line = "PASV";

    DMOD_TEST_EXPECT_EQ(dmftp_parse_command(line, strlen(line), &cmd), 0);
    DMOD_TEST_EXPECT_TRUE(text_equal(cmd.verb, "PASV"));
    DMOD_TEST_EXPECT_NULL(cmd.arg);
    DMOD_TEST_EXPECT_EQ(cmd.arg_len, (size_t)0);
}

DMOD_TEST_STEP(parse_command_keeps_inner_spaces_in_argument)
{
    dmftp_command_t cmd;
    const char* line = "STOR my file.txt   ";

    /* A filename may contain spaces; only trailing whitespace is trimmed. */
    DMOD_TEST_EXPECT_EQ(dmftp_parse_command(line, strlen(line), &cmd), 0);
    DMOD_TEST_EXPECT_EQ(cmd.arg_len, (size_t)11);
    DMOD_TEST_EXPECT_TRUE(bytes_equal((const uint8_t*)cmd.arg, (const uint8_t*)"my file.txt", 11u));
}

DMOD_TEST_STEP(parse_command_skips_telnet_prefix)
{
    dmftp_command_t cmd;
    /* IAC IP, IAC DM - what a strict client puts in front of ABOR
     * (RFC 959 4.1.3.5). */
    const char line[] = { (char)0xFF, (char)0xF4, (char)0xFF, (char)0xF2, 'A', 'B', 'O', 'R' };

    DMOD_TEST_EXPECT_EQ(dmftp_parse_command(line, sizeof(line), &cmd), 0);
    DMOD_TEST_EXPECT_TRUE(text_equal(cmd.verb, "ABOR"));
}

DMOD_TEST_STEP(parse_command_skips_telnet_option_negotiation)
{
    dmftp_command_t cmd;
    /* IAC WILL <option> is three bytes, not two - getting that wrong would
     * leave the option byte at the front of the verb. */
    const char line[] = { (char)0xFF, (char)0xFB, (char)0x01, 'N', 'O', 'O', 'P' };

    DMOD_TEST_EXPECT_EQ(dmftp_parse_command(line, sizeof(line), &cmd), 0);
    DMOD_TEST_EXPECT_TRUE(text_equal(cmd.verb, "NOOP"));
}

/* ============================================================================
 *                      dmftp_format_reply / _parse_reply
 * ========================================================================== */

DMOD_TEST_STEP(format_reply_rejects_bad_arguments)
{
    char buffer[32];
    size_t written = 0;

    DMOD_TEST_EXPECT_EQ(dmftp_format_reply(NULL, sizeof(buffer), 200, "ok", &written), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_format_reply(buffer, sizeof(buffer), 200, NULL, &written), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_format_reply(buffer, sizeof(buffer), 99, "ok", &written), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_format_reply(buffer, sizeof(buffer), 600, "ok", &written), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_format_reply(buffer, 4u, 200, "ok", &written), -ENOSPC);
}

DMOD_TEST_STEP(format_reply_writes_code_text_and_crlf)
{
    char buffer[32];
    size_t written = 0;

    DMOD_TEST_EXPECT_EQ(dmftp_format_reply(buffer, sizeof(buffer), 226, "Transfer complete.", &written), 0);
    DMOD_TEST_EXPECT_EQ(written, (size_t)24);
    DMOD_TEST_EXPECT_TRUE(bytes_equal((const uint8_t*)buffer, (const uint8_t*)"226 Transfer complete.\r\n", 24u));
}

DMOD_TEST_STEP(parse_reply_rejects_bad_arguments)
{
    int code = 0;
    bool final = false;
    const char* text = NULL;

    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply(NULL, 5u, &code, &final, &text), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply("200 ", 4u, NULL, &final, &text), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply("20", 2u, &code, &final, &text), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply("2x0 hi", 6u, &code, &final, &text), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply("200x hi", 7u, &code, &final, &text), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply("099 hi", 6u, &code, &final, &text), -EPROTO);
}

DMOD_TEST_STEP(parse_reply_reads_final_line)
{
    int code = 0;
    bool final = false;
    const char* text = NULL;
    const char* line = "230 Login successful.";

    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply(line, strlen(line), &code, &final, &text), 0);
    DMOD_TEST_EXPECT_EQ(code, 230);
    DMOD_TEST_EXPECT_TRUE(final);
    DMOD_TEST_EXPECT_TRUE(text_equal(text, "Login successful."));
}

DMOD_TEST_STEP(parse_reply_marks_continuation_line)
{
    int code = 0;
    bool final = true;
    const char* text = NULL;
    const char* line = "211-Features:";

    /* A '-' is what distinguishes a banner line from the verdict - a client
     * that acted on continuation lines would react to a FEAT header. */
    DMOD_TEST_EXPECT_EQ(dmftp_parse_reply(line, strlen(line), &code, &final, &text), 0);
    DMOD_TEST_EXPECT_EQ(code, 211);
    DMOD_TEST_EXPECT_FALSE(final);
}

/* ============================================================================
 *                      Host-port tuples
 * ========================================================================== */

DMOD_TEST_STEP(format_host_port_rejects_bad_arguments)
{
    char buffer[24];
    dmip_addr_t addr = make_v4(10, 0, 0, 1);
    dmip_addr_t v6 = { 0 };
    v6.family = dmip_family_v6;

    DMOD_TEST_EXPECT_EQ(dmftp_format_host_port(NULL, sizeof(buffer), &addr, 21u, NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_format_host_port(buffer, sizeof(buffer), NULL, 21u, NULL), -EINVAL);
    /* RFC 959's tuple has no IPv6 form; dmtcp could not originate one anyway. */
    DMOD_TEST_EXPECT_EQ(dmftp_format_host_port(buffer, sizeof(buffer), &v6, 21u, NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_format_host_port(buffer, 8u, &addr, 21u, NULL), -ENOSPC);
}

DMOD_TEST_STEP(format_host_port_splits_port_into_two_bytes)
{
    char buffer[24];
    size_t written = 0;
    dmip_addr_t addr = make_v4(192, 168, 1, 10);

    /* 50000 = 195*256 + 80 */
    DMOD_TEST_EXPECT_EQ(dmftp_format_host_port(buffer, sizeof(buffer), &addr, 50000u, &written), 0);
    DMOD_TEST_EXPECT_TRUE(text_equal(buffer, "192,168,1,10,195,80"));
    DMOD_TEST_EXPECT_EQ(written, strlen("192,168,1,10,195,80"));
}

DMOD_TEST_STEP(parse_host_port_round_trips)
{
    char buffer[24];
    dmip_addr_t addr = make_v4(10, 20, 30, 40);
    dmip_addr_t parsed = { 0 };
    uint16_t port = 0;

    DMOD_TEST_EXPECT_EQ(dmftp_format_host_port(buffer, sizeof(buffer), &addr, 49321u, NULL), 0);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_host_port(buffer, &parsed, &port), 0);
    DMOD_TEST_EXPECT_EQ(parsed.family, dmip_family_v4);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(parsed.addr.v4, addr.addr.v4, DMIP_IPV4_ADDR_LEN));
    DMOD_TEST_EXPECT_EQ(port, (uint16_t)49321);
}

DMOD_TEST_STEP(parse_host_port_tolerates_spaces_after_commas)
{
    dmip_addr_t parsed = { 0 };
    uint16_t port = 0;

    DMOD_TEST_EXPECT_EQ(dmftp_parse_host_port("127, 0, 0, 1, 4, 1", &parsed, &port), 0);
    DMOD_TEST_EXPECT_EQ(parsed.addr.v4[0], (uint8_t)127);
    DMOD_TEST_EXPECT_EQ(port, (uint16_t)1025);
}

DMOD_TEST_STEP(parse_host_port_rejects_malformed_tuples)
{
    dmip_addr_t parsed = { 0 };
    uint16_t port = 0;

    DMOD_TEST_EXPECT_EQ(dmftp_parse_host_port(NULL, &parsed, &port), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_parse_host_port("10,0,0,1,4", &parsed, &port), -EPROTO);       /* too few fields */
    DMOD_TEST_EXPECT_EQ(dmftp_parse_host_port("10,0,0,1,4,1,9", &parsed, &port), 0);          /* extra fields ignored */
    DMOD_TEST_EXPECT_EQ(dmftp_parse_host_port("10,0,0,256,4,1", &parsed, &port), -EPROTO);    /* octet out of range */
    DMOD_TEST_EXPECT_EQ(dmftp_parse_host_port("10,0,0,1,4,x", &parsed, &port), -EPROTO);      /* not a number */
}

/* ============================================================================
 *                      Server lifecycle
 * ========================================================================== */

static bool g_auth_called;
static bool g_auth_answer;

static bool test_auth(dmftp_session_t session, const char* user, const char* password, void* user_data)
{
    (void)session; (void)user; (void)password; (void)user_data;
    g_auth_called = true;
    return g_auth_answer;
}

DMOD_TEST_STEP(server_create_requires_an_auth_callback)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE;

    /* A server with no way to decide who may log in would have to let
     * everybody in - refusing to build one is the safe default. */
    DMOD_TEST_EXPECT_NULL(dmftp_server_create(&config, NULL, NULL));

    dmftp_server_callbacks_t empty = { 0 };
    DMOD_TEST_EXPECT_NULL(dmftp_server_create(&config, &empty, NULL));
}

DMOD_TEST_STEP(server_create_applies_defaults)
{
    dmftp_server_callbacks_t callbacks = { 0 };
    callbacks.on_auth = test_auth;

    /* A NULL config must produce a usable server on the standard port. */
    dmftp_server_t server = dmftp_server_create(NULL, &callbacks, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(server);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_port(server), (uint16_t)DMFTP_PORT_CONTROL);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(server), (size_t)0);

    dmftp_server_destroy(server);
}

DMOD_TEST_STEP(server_start_reserves_the_control_port)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 1u;
    config.root = "/tmp";

    dmftp_server_callbacks_t callbacks = { 0 };
    callbacks.on_auth = test_auth;

    dmftp_server_t server = dmftp_server_create(&config, &callbacks, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(server);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_port(server), (uint16_t)(TEST_PORT_BASE + 1u));

    DMOD_TEST_EXPECT_EQ(dmftp_server_start(server), 0);
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(server), -EALREADY);

    /* A second server on the same port must be refused while the first
     * holds it, and accepted once it lets go. */
    dmftp_server_t other = dmftp_server_create(&config, &callbacks, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(other);
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(other), -EEXIST);

    DMOD_TEST_EXPECT_EQ(dmftp_server_stop(server), 0);
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(other), 0);

    dmftp_server_destroy(other);
    dmftp_server_destroy(server);
}

DMOD_TEST_STEP(server_stop_is_idempotent)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 2u;

    dmftp_server_callbacks_t callbacks = { 0 };
    callbacks.on_auth = test_auth;

    dmftp_server_t server = dmftp_server_create(&config, &callbacks, NULL);
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(server), 0);
    DMOD_TEST_EXPECT_EQ(dmftp_server_stop(server), 0);
    DMOD_TEST_EXPECT_EQ(dmftp_server_stop(server), 0);

    /* And it can be started again afterwards. */
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(server), 0);
    dmftp_server_destroy(server);
}

DMOD_TEST_STEP(server_accessors_reject_invalid_handles)
{
    DMOD_TEST_EXPECT_EQ(dmftp_server_start(NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_server_stop(NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_port(NULL), (uint16_t)0);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(NULL), (size_t)0);

    /* Destroying NULL must not crash. */
    dmftp_server_destroy(NULL);
}

DMOD_TEST_STEP(session_accessors_reject_invalid_handles)
{
    dmip_addr_t addr = { 0 };
    uint16_t port = 0;

    DMOD_TEST_EXPECT_NULL(dmftp_session_get_user(NULL));
    DMOD_TEST_EXPECT_EQ(dmftp_session_get_peer(NULL, &addr, &port), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_session_set_root(NULL, "/"), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_session_set_read_only(NULL, true), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_session_set_user_data(NULL, NULL), -EINVAL);
    DMOD_TEST_EXPECT_NULL(dmftp_session_get_user_data(NULL));

    dmftp_session_close(NULL);
}

/* ============================================================================
 *                      Client
 * ========================================================================== */

DMOD_TEST_STEP(client_create_rejects_bad_configuration)
{
    DMOD_TEST_EXPECT_NULL(dmftp_client_create(NULL, NULL, NULL));

    /* IPv6 is unreachable through dmtcp (no dmip_v6_send() yet), so a
     * client configured for it is refused at creation rather than failing
     * later, halfway through a connect. */
    dmftp_client_config_t config = { 0 };
    config.host.family = dmip_family_v6;
    DMOD_TEST_EXPECT_NULL(dmftp_client_create(&config, NULL, NULL));
}

DMOD_TEST_STEP(client_starts_disconnected_and_rejects_commands)
{
    dmftp_client_config_t config = { 0 };
    config.host = make_v4(203, 0, 113, 7); /* TEST-NET-3: no route exists here */

    dmftp_client_t client = dmftp_client_create(&config, NULL, (void*)0x1234);
    DMOD_TEST_EXPECT_NOT_NULL(client);
    DMOD_TEST_EXPECT_FALSE(dmftp_client_is_ready(client));
    DMOD_TEST_EXPECT_EQ(dmftp_client_get_user_data(client), (void*)0x1234);

    /* Nothing may be issued before the login completes. */
    DMOD_TEST_EXPECT_EQ(dmftp_client_cwd(client, "/pub"), -ENOTCONN);
    DMOD_TEST_EXPECT_EQ(dmftp_client_get(client, "a", "b"), -ENOTCONN);
    DMOD_TEST_EXPECT_EQ(dmftp_client_list(client, NULL, true), -ENOTCONN);

    dmftp_client_destroy(client);
}

DMOD_TEST_STEP(client_connect_without_a_route_fails_synchronously)
{
    dmftp_client_config_t config = { 0 };
    config.host = make_v4(203, 0, 113, 8);

    dmftp_client_t client = dmftp_client_create(&config, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(client);

    /* dmtcp reports the first SYN's failure synchronously; dmftp passes it
     * straight through rather than pretending the connect is under way. */
    DMOD_TEST_EXPECT_EQ(dmftp_client_connect(client), -ENETUNREACH);

    dmftp_client_destroy(client);
}

DMOD_TEST_STEP(client_accessors_reject_invalid_handles)
{
    DMOD_TEST_EXPECT_EQ(dmftp_client_connect(NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_client_quit(NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_client_cwd(NULL, "/"), -EINVAL);
    DMOD_TEST_EXPECT_FALSE(dmftp_client_is_ready(NULL));
    DMOD_TEST_EXPECT_NULL(dmftp_client_get_user_data(NULL));

    dmftp_client_destroy(NULL);
}

DMOD_TEST_STEP(client_command_rejects_an_overlong_verb)
{
    dmftp_client_config_t config = { 0 };
    config.host = make_v4(203, 0, 113, 9);

    dmftp_client_t client = dmftp_client_create(&config, NULL, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(client);

    /* Checked before the connection state, so the caller learns the verb is
     * the problem rather than being told it is not connected. */
    DMOD_TEST_EXPECT_EQ(dmftp_client_command(client, "TOOLONG", NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_client_command(client, "", NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmftp_client_command(client, NULL, NULL), -EINVAL);

    dmftp_client_destroy(client);
}

/* ============================================================================
 *                      End-to-end control channel
 *
 * Everything above tests dmftp's pieces in isolation. This section drives a
 * real client session all the way through the stack: a hand-built,
 * correctly-checksummed IPv4/TCP frame goes into dmnetbridge's real
 * packet_received DIF (found the same way dmnetbridge_handle_netif_rx()
 * finds it), through dmip, through dmtcp, and out into dmftp's accept
 * handler and command dispatcher. Same fixture shape dmtcp's own tests use.
 *
 * What cannot be checked here is what dmftp *replies*: the fixture
 * interface is backed by "/dev/null" with no driver behind it, so outbound
 * segments go nowhere (the limit dmtcp's tests document for themselves).
 * The assertions are therefore about observable server state - which
 * sessions exist, who logged in, what the auth handler was told - rather
 * than about reply codes, which the codec tests above cover directly.
 * ========================================================================== */

#define TEST_ETH_HEADER_LEN 14u
#define TEST_ETHERTYPE_IPV4 0x0800u
#define TEST_MAX_PAYLOAD_LEN 64u
#define TEST_V4_PSEUDO_HEADER_LEN 12u
#define TEST_WINDOW 65535u

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

/**
 * @brief Wrap a complete IP packet in a minimal Ethernet frame and hand it
 *        to every packet_received DIF implementor (dmip's, in practice)
 */
static void feed_frame(dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len)
{
    size_t frame_len = TEST_ETH_HEADER_LEN + packet_len;
    uint8_t* frame = Dmod_Malloc(frame_len);
    if (frame == NULL)
        return;

    memset(frame, 0, TEST_ETH_HEADER_LEN);
    write_u16_be(&frame[12], TEST_ETHERTYPE_IPV4);
    memcpy(frame + TEST_ETH_HEADER_LEN, packet, packet_len);

    Dmod_Context_t* implementor = NULL;
    while ((implementor = Dmod_GetNextDifModule(dmod_dmnetbridge_packet_received_sig, implementor)) != NULL)
    {
        dmod_dmnetbridge_packet_received_t fn =
            (dmod_dmnetbridge_packet_received_t)Dmod_GetDifFunction(implementor, dmod_dmnetbridge_packet_received_sig);
        if (fn != NULL)
        {
            fn(iface, frame, frame_len);
        }
    }

    Dmod_Free(frame);
}

/**
 * @brief Build one correctly-checksummed TCP-over-IPv4 packet and feed it in
 */
static void feed_segment(dmip_addr_t src, dmip_addr_t dst, uint16_t src_port, uint16_t dst_port,
                          uint32_t seq, uint32_t ack, uint8_t flags, const char* payload)
{
    size_t payload_len = payload != NULL ? strlen(payload) : 0;
    size_t seg_len = DMTCP_HEADER_LEN + payload_len;

    uint8_t segment[DMTCP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
    dmtcp_header_t header = { .src_port = src_port, .dst_port = dst_port, .seq_num = seq,
                              .ack_num = ack, .flags = flags, .window = TEST_WINDOW };
    dmtcp_build_header(segment, seg_len, &header);
    if (payload_len > 0)
    {
        memcpy(segment + DMTCP_HEADER_LEN, payload, payload_len);
    }

    uint8_t pseudo[TEST_V4_PSEUDO_HEADER_LEN + DMTCP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
    memcpy(&pseudo[0], src.addr.v4, DMIP_IPV4_ADDR_LEN);
    memcpy(&pseudo[4], dst.addr.v4, DMIP_IPV4_ADDR_LEN);
    pseudo[8] = 0;
    pseudo[9] = DMIP_PROTO_TCP;
    write_u16_be(&pseudo[10], (uint16_t)seg_len);
    memcpy(&pseudo[TEST_V4_PSEUDO_HEADER_LEN], segment, seg_len);
    write_u16_be(&segment[16], dmip_checksum(pseudo, TEST_V4_PSEUDO_HEADER_LEN + seg_len));

    dmip_v4_header_t ip = { 0 };
    ip.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + seg_len);
    ip.ttl = DMIP_DEFAULT_TTL;
    ip.protocol = DMIP_PROTO_TCP;
    ip.src = src;
    ip.dst = dst;

    uint8_t packet[DMIP_V4_HEADER_LEN + DMTCP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
    dmip_v4_build_header(packet, sizeof(packet), &ip);
    memcpy(packet + DMIP_V4_HEADER_LEN, segment, seg_len);

    feed_frame(g_iface, packet, DMIP_V4_HEADER_LEN + seg_len);
}

/* ---- What the server told us, recorded by its callbacks ---- */

static dmftp_session_t g_session;
static bool            g_session_opened;
static bool            g_session_closed;
static char            g_auth_user[32];
static char            g_auth_password[32];

static void copy_into(char* dst, size_t dst_len, const char* src)
{
    size_t len = (src != NULL) ? strlen(src) : 0;
    if (len >= dst_len)
    {
        len = dst_len - 1u;
    }
    if (len > 0)
    {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static bool recording_auth(dmftp_session_t session, const char* user, const char* password, void* user_data)
{
    (void)session; (void)user_data;
    g_auth_called = true;
    copy_into(g_auth_user, sizeof(g_auth_user), user);
    copy_into(g_auth_password, sizeof(g_auth_password), password);
    return g_auth_answer;
}

static void recording_session_open(dmftp_session_t session, void* user_data)
{
    (void)user_data;
    g_session_opened = true;
    g_session = session;
}

static void recording_session_close(dmftp_session_t session, void* user_data)
{
    (void)session; (void)user_data;
    g_session_closed = true;
    g_session = NULL;
}

static void reset_recording(void)
{
    g_session = NULL;
    g_session_opened = false;
    g_session_closed = false;
    g_auth_called = false;
    g_auth_answer = true;
    g_auth_user[0] = '\0';
    g_auth_password[0] = '\0';
}

/**
 * @brief A running server plus the sequence-number bookkeeping of one
 *        client connection to it
 */
struct test_session
{
    dmftp_server_t server;
    dmip_addr_t    client;
    dmip_addr_t    server_addr;
    uint16_t       client_port;
    uint16_t       server_port;
    uint32_t       client_seq; /**< next byte we will send */
    uint32_t       server_seq; /**< the server's ISS + 1 */
};

/**
 * @brief Start a server and drive a full three-way handshake into it
 *
 * dmtcp derives its initial sequence number straight from
 * dmosi_get_tick_count() at SYN-arrival time (a documented, deliberately
 * testable simplification), so sampling the same counter here predicts it -
 * with a second candidate for the case where the tick advanced in between.
 *
 * @return true if the handshake completed and a session exists
 */
static bool open_session(struct test_session* fixture, uint16_t port, uint16_t client_port,
                          const dmftp_server_config_t* config)
{
    reset_recording();

    dmftp_server_callbacks_t callbacks = { 0 };
    callbacks.on_auth = recording_auth;
    callbacks.on_session_open = recording_session_open;
    callbacks.on_session_close = recording_session_close;

    fixture->server = dmftp_server_create(config, &callbacks, NULL);
    if (fixture->server == NULL || dmftp_server_start(fixture->server) != 0)
        return false;

    fixture->client = make_v4(10, 30, 0, 1);
    fixture->server_addr = make_v4(10, 30, 0, 2);
    fixture->client_port = client_port;
    fixture->server_port = port;
    fixture->client_seq = 7000;

    /* Bracket dmtcp's own sample: the SYN is processed synchronously inside
     * feed_segment(), so the ISS it picked is somewhere in [before, after].
     * An ACK carrying the wrong ack_num is simply ignored (it acknowledges
     * something never sent), so walking the range costs nothing but is the
     * difference between a reliable fixture and one that works only while
     * the tick happens not to advance. */
    uint32_t before = dmosi_get_tick_count();
    feed_segment(fixture->client, fixture->server_addr, client_port, port,
                  fixture->client_seq, 0, DMTCP_FLAG_SYN, NULL);
    uint32_t after = dmosi_get_tick_count();
    fixture->client_seq += 1u; /* the SYN consumes one sequence number */

    for (uint32_t iss = before; iss <= after && !g_session_opened; iss++)
    {
        feed_segment(fixture->client, fixture->server_addr, client_port, port,
                      fixture->client_seq, iss + 1u, DMTCP_FLAG_ACK, NULL);
        if (g_session_opened)
        {
            fixture->server_seq = iss + 1u;
        }
    }
    return g_session_opened;
}

/** @brief Send one command line on the control connection */
static void send_line(struct test_session* fixture, const char* line)
{
    feed_segment(fixture->client, fixture->server_addr, fixture->client_port, fixture->server_port,
                  fixture->client_seq, fixture->server_seq, DMTCP_FLAG_ACK, line);
    fixture->client_seq += (uint32_t)strlen(line);
}

/**
 * @brief End a fixture session and free everything dmtcp is holding for it
 *
 * The RST matters: dmftp_server_destroy() closes gracefully, and a graceful
 * close against a peer that never answers leaves the TCB (and both of its
 * dmosi timers, each with a worker thread on the posix backend) alive for a
 * retransmission cycle plus TIME_WAIT. Steps run back to back, so those
 * would accumulate across the whole file and eventually starve a later
 * handshake. A RST makes dmtcp tear the connection down on the spot, which
 * keeps each step's cost to itself.
 */
static void close_session(struct test_session* fixture)
{
    if (fixture->server == NULL)
        return;

    feed_segment(fixture->client, fixture->server_addr, fixture->client_port, fixture->server_port,
                  fixture->client_seq, fixture->server_seq, DMTCP_FLAG_RST, NULL);
    dmftp_server_destroy(fixture->server);
    fixture->server = NULL;
}

DMOD_TEST_STEP(handshake_creates_a_session)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 10u;
    config.root = "/tmp";

    struct test_session fixture = { 0 };
    DMOD_TEST_EXPECT_TRUE(open_session(&fixture, config.port, 7100u, &config));
    DMOD_TEST_EXPECT_NOT_NULL(g_session);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(fixture.server), (size_t)1);

    /* The session knows where the client came from - the same 4-tuple the
     * handshake used. */
    dmip_addr_t peer = { 0 };
    uint16_t peer_port = 0;
    DMOD_TEST_EXPECT_EQ(dmftp_session_get_peer(g_session, &peer, &peer_port), 0);
    DMOD_TEST_EXPECT_EQ(peer_port, (uint16_t)7100);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(peer.addr.v4, fixture.client.addr.v4, DMIP_IPV4_ADDR_LEN));

    /* No USER has arrived yet. */
    DMOD_TEST_EXPECT_NULL(dmftp_session_get_user(g_session));

    close_session(&fixture);
}

DMOD_TEST_STEP(user_and_pass_reach_the_auth_handler)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 11u;
    config.root = "/tmp";

    struct test_session fixture = { 0 };
    DMOD_TEST_EXPECT_TRUE(open_session(&fixture, config.port, 7101u, &config));

    send_line(&fixture, "USER admin\r\n");
    DMOD_TEST_EXPECT_TRUE(text_equal(dmftp_session_get_user(g_session), "admin"));
    DMOD_TEST_EXPECT_FALSE(g_auth_called); /* nothing to authenticate against yet */

    send_line(&fixture, "PASS s3cret\r\n");
    DMOD_TEST_EXPECT_TRUE(g_auth_called);
    DMOD_TEST_EXPECT_TRUE(text_equal(g_auth_user, "admin"));
    DMOD_TEST_EXPECT_TRUE(text_equal(g_auth_password, "s3cret"));

    close_session(&fixture);
}

DMOD_TEST_STEP(commands_split_across_segments_are_reassembled)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 12u;
    config.root = "/tmp";

    struct test_session fixture = { 0 };
    DMOD_TEST_EXPECT_TRUE(open_session(&fixture, config.port, 7102u, &config));

    /* TCP is a byte stream: a command can arrive in as many pieces as the
     * network feels like, and two commands can share one segment. */
    send_line(&fixture, "USER ad");
    DMOD_TEST_EXPECT_NULL(dmftp_session_get_user(g_session)); /* no CRLF yet */

    send_line(&fixture, "min\r\nPASS pw\r\n");
    DMOD_TEST_EXPECT_TRUE(text_equal(dmftp_session_get_user(g_session), "admin"));
    DMOD_TEST_EXPECT_TRUE(g_auth_called);
    DMOD_TEST_EXPECT_TRUE(text_equal(g_auth_password, "pw"));

    close_session(&fixture);
}

DMOD_TEST_STEP(quit_closes_the_session)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 13u;
    config.root = "/tmp";

    struct test_session fixture = { 0 };
    DMOD_TEST_EXPECT_TRUE(open_session(&fixture, config.port, 7103u, &config));
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(fixture.server), (size_t)1);

    send_line(&fixture, "QUIT\r\n");
    DMOD_TEST_EXPECT_TRUE(g_session_closed);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(fixture.server), (size_t)0);

    close_session(&fixture);
}

DMOD_TEST_STEP(rejected_login_leaves_the_session_unauthenticated)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 14u;
    config.root = "/tmp";

    struct test_session fixture = { 0 };
    DMOD_TEST_EXPECT_TRUE(open_session(&fixture, config.port, 7104u, &config));
    g_auth_answer = false;

    send_line(&fixture, "USER intruder\r\n");
    send_line(&fixture, "PASS guess\r\n");
    DMOD_TEST_EXPECT_TRUE(g_auth_called);

    /* A refused login must not close the connection - a client is allowed
     * to try again with another USER - but it must not be logged in either,
     * which the session staying open and unauthenticated encodes. */
    DMOD_TEST_EXPECT_FALSE(g_session_closed);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(fixture.server), (size_t)1);

    /* A second attempt reaches the handler again rather than being cached. */
    g_auth_called = false;
    g_auth_answer = true;
    send_line(&fixture, "USER admin\r\n");
    send_line(&fixture, "PASS ok\r\n");
    DMOD_TEST_EXPECT_TRUE(g_auth_called);
    DMOD_TEST_EXPECT_TRUE(text_equal(g_auth_user, "admin"));

    close_session(&fixture);
}

DMOD_TEST_STEP(server_stop_closes_live_sessions)
{
    dmftp_server_config_t config = { 0 };
    config.port = TEST_PORT_BASE + 15u;
    config.root = "/tmp";

    struct test_session fixture = { 0 };
    DMOD_TEST_EXPECT_TRUE(open_session(&fixture, config.port, 7105u, &config));

    DMOD_TEST_EXPECT_EQ(dmftp_server_stop(fixture.server), 0);
    DMOD_TEST_EXPECT_TRUE(g_session_closed);
    DMOD_TEST_EXPECT_EQ(dmftp_server_get_session_count(fixture.server), (size_t)0);

    close_session(&fixture);
}
