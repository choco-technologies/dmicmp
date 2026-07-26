#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmicmp.h"

static dmicmp_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmicmp_create();
}

void dmod_test_teardown(void)
{
    dmicmp_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmicmp_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmicmp_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmicmp_is_valid(g_handle));
}

DMOD_TEST_STEP(dmicmp_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmicmp_destroy(NULL);
}
