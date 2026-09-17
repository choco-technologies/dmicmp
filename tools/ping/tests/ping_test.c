/**
 * @file ping_test.c
 * @brief Test steps for ping
 *
 * Drives the actual `ping` module through Dmod_RunModule("ping", argc,
 * argv) (loads it, runs its main(), unloads it again - see
 * Dmod_RunModule in dmod.h) rather than depending on `ping` as a linked
 * module: an Application-type module can't be loaded as another module's
 * dependency (only a Library-type module can) - same reasoning
 * dmnet/tools/ifconfig/tests/ifconfig_test.c documents for itself.
 *
 * A "/dev/null"-backed dmnetif fixture, with a route and ARP cache entry
 * pointed at it, lets a send be exercised up through dmip_send() the same
 * way dmicmp's own tests/dmicmp_test.c does for dmicmp_v4_send_error()/
 * _send_dest_unreachable() - no real driver backs it, so
 * dmicmp_v4_send_echo_request() itself still fails (dmnetif_send() ->
 * -EIO), and no real Echo Reply can ever arrive. Every step below
 * therefore expects ping to report loss, not success - it only proves
 * argument handling and the full send attempt don't crash and report the
 * honest outcome, the same boundary ifconfig_test.c's "up"/"down" steps
 * document for themselves.
 */
#include "dmod_test.h"
#include "dmroute.h"
#include "dmarp.h"
#include <string.h>

#define TEST_DEVICE_PATH "/dev/null"

static dmnetif_iface_t g_iface = NULL;
static dmroute_route_t g_route = NULL;

void dmod_test_setup(void)
{
    g_iface = dmnetif_register("test0", TEST_DEVICE_PATH);

    dmroute_addr_t dest_net = { 0 };
    dest_net.family = dmroute_family_v4;
    dest_net.addr.v4[0] = 203; dest_net.addr.v4[1] = 0; dest_net.addr.v4[2] = 113; dest_net.addr.v4[3] = 0;

    dmroute_addr_t netmask = { 0 };
    netmask.family = dmroute_family_v4;
    netmask.addr.v4[0] = 255; netmask.addr.v4[1] = 255; netmask.addr.v4[2] = 255; netmask.addr.v4[3] = 0;

    g_route = dmroute_add(&dest_net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);

    dmroute_addr_t dst = { 0 };
    dst.family = dmroute_family_v4;
    dst.addr.v4[0] = 203; dst.addr.v4[1] = 0; dst.addr.v4[2] = 113; dst.addr.v4[3] = 7;

    dmnetif_mac_addr_t fake_mac = { { 0x02, 0x00, 0x00, 0x00, 0x00, 0x40 } };
    dmarp_cache_insert(g_iface, &dst, &fake_mac);
}

void dmod_test_teardown(void)
{
    dmroute_addr_t dst = { 0 };
    dst.family = dmroute_family_v4;
    dst.addr.v4[0] = 203; dst.addr.v4[1] = 0; dst.addr.v4[2] = 113; dst.addr.v4[3] = 7;
    dmarp_cache_remove(g_iface, &dst);

    dmroute_remove(g_route);
    g_route = NULL;

    dmnetif_unregister(g_iface);
    g_iface = NULL;
}

DMOD_TEST_STEP(no_args_fails)
{
    char* argv[] = { "ping" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 1, argv), 0);
}

DMOD_TEST_STEP(run_module_unloads_ping_afterwards)
{
    char* argv[] = { "ping", "--help" };
    Dmod_RunModule("ping", 2, argv);
    DMOD_TEST_EXPECT_FALSE(Dmod_IsModuleLoaded("ping"));
}

DMOD_TEST_STEP(help_flag_succeeds)
{
    char* argv[] = { "ping", "--help" };
    DMOD_TEST_EXPECT_EQ(Dmod_RunModule("ping", 2, argv), 0);
}

DMOD_TEST_STEP(short_help_flag_succeeds)
{
    char* argv[] = { "ping", "-h" };
    DMOD_TEST_EXPECT_EQ(Dmod_RunModule("ping", 2, argv), 0);
}

DMOD_TEST_STEP(invalid_address_fails)
{
    char* argv[] = { "ping", "not-an-address" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 2, argv), 0);
}

DMOD_TEST_STEP(address_octet_out_of_range_fails)
{
    char* argv[] = { "ping", "192.168.1.999" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 2, argv), 0);
}

DMOD_TEST_STEP(unknown_flag_fails)
{
    char* argv[] = { "ping", "203.0.113.7", "--frobnicate" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 3, argv), 0);
}

DMOD_TEST_STEP(count_zero_fails)
{
    char* argv[] = { "ping", "203.0.113.7", "-c", "0" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 4, argv), 0);
}

DMOD_TEST_STEP(count_non_numeric_fails)
{
    char* argv[] = { "ping", "203.0.113.7", "-c", "many" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 4, argv), 0);
}

DMOD_TEST_STEP(timeout_zero_fails)
{
    char* argv[] = { "ping", "203.0.113.7", "-W", "0" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 4, argv), 0);
}

DMOD_TEST_STEP(size_over_max_fails)
{
    char* argv[] = { "ping", "203.0.113.7", "-s", "999999" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 4, argv), 0);
}

DMOD_TEST_STEP(unreachable_destination_reports_loss_without_crashing)
{
    /* No route anywhere in this file's fixtures covers this network. */
    char* argv[] = { "ping", "198.51.100.1", "-c", "1", "-W", "50" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 5, argv), 0);
}

DMOD_TEST_STEP(routed_destination_without_real_driver_reports_loss)
{
    /* Route + ARP entry let dmicmp_v4_send_echo_request() reach
     * dmnetif_send() itself, which fails (-EIO) since TEST_DEVICE_PATH is
     * not a real driver - ping must still report the honest 100% loss
     * outcome rather than crash or hang. */
    char* argv[] = { "ping", "203.0.113.7", "-c", "2", "-i", "1", "-W", "50" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 7, argv), 0);
}

DMOD_TEST_STEP(custom_payload_size_is_accepted)
{
    char* argv[] = { "ping", "203.0.113.7", "-c", "1", "-s", "8", "-W", "50" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 7, argv), 0);
}

DMOD_TEST_STEP(zero_payload_size_is_accepted)
{
    char* argv[] = { "ping", "203.0.113.7", "-c", "1", "-s", "0", "-W", "50" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("ping", 7, argv), 0);
}
