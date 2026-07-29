#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmftp.h"

static dmftp_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmftp_create();
}

void dmod_test_teardown(void)
{
    dmftp_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmftp_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmftp_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmftp_is_valid(g_handle));
}

DMOD_TEST_STEP(dmftp_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmftp_destroy(NULL);
}
