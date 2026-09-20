/**
 * @file ftpd.c
 * @brief ftpd - FTP server Application, argv-configured, one process per instance
 *
 * Parses --port/--root/--user/--pass from argv (the dmsystem unit's own
 * `args=` key - see configs/ftpd.ini), starts listening via
 * ftpd_server_start(), then blocks for the rest of the process's
 * lifetime.
 *
 * That last part deliberately does NOT pump the network interface itself
 * (no dmnetbridge_handle_netif_rx() call, unlike networkd/dmtcpecho):
 * dmnetbridge documents itself as allowing only one pump per interface,
 * and on a real board `networkd@eth0` already owns that job (see
 * dmnetif's device-rule wiring) - two competing pumps on the same
 * interface would be a bug, not a feature. ftpd only needs to stay
 * loaded (so its callbacks registered with dmtcp_listen() remain valid
 * code to jump to) while whichever thread *is* pumping the interface -
 * networkd's - is what actually invokes them, exactly like every other
 * dmtcp-based listener in this ecosystem that isn't itself the pump
 * owner (dmicmp, telnetd, ftpd's own control connection). A plain sleep
 * loop is the whole job.
 */
#include "dmod.h"
#include "ftpd_internal.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

#define FTPD_IDLE_SLEEP_MS 60000u

static void print_usage(const char* prog)
{
    Dmod_Printf("Usage: %s [--port N] [--root PATH] [--user NAME] [--pass SECRET]\n", prog);
    Dmod_Printf("  --port N       TCP port to listen on (default: %u)\n", (unsigned)FTPD_DEFAULT_PORT);
    Dmod_Printf("  --root PATH    Virtual filesystem root to serve (default: /)\n");
    Dmod_Printf("  --user NAME    Required username, or \"anonymous\" to accept any (default: anonymous)\n");
    Dmod_Printf("  --pass SECRET  Required password when --user isn't anonymous (default: empty, accepts any)\n");
}

int main(int argc, char *argv[])
{
    uint16_t port = (uint16_t)FTPD_DEFAULT_PORT;
    const char* root = "/";
    const char* user = "anonymous";
    const char* pass = "";

    for (int i = 1; i < argc; i++)
    {
        bool have_value = (i + 1 < argc);

        if (strcmp(argv[i], "--port") == 0 && have_value)
        {
            unsigned int value;
            if (Dmod_Sscanf(argv[++i], "%u", &value) == 1 && value <= 65535u)
                port = (uint16_t)value;
        }
        else if (strcmp(argv[i], "--root") == 0 && have_value)
        {
            root = argv[++i];
        }
        else if (strcmp(argv[i], "--user") == 0 && have_value)
        {
            user = argv[++i];
        }
        else if (strcmp(argv[i], "--pass") == 0 && have_value)
        {
            pass = argv[++i];
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            DMOD_LOG_ERROR("ftpd: unrecognized argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return -EINVAL;
        }
    }

    int ret = ftpd_server_start(port, root, user, pass);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("ftpd: failed to start (error %d)\n", ret);
        return ret;
    }

    for (;;)
        dmosi_thread_sleep(FTPD_IDLE_SLEEP_MS);
}
