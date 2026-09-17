# Name of the module
set(DMOD_MODULE_NAME        ping)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/ping.dmr)

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/ping.c
)

# dmicmp is this same repository's own top-level module, already built as
# part of this CMake configure by the time tools/ is added (see the root
# CMakeLists.txt) - link its dmicmp_if target directly, exactly like
# tests/CMakeLists.txt does, instead of fetching dmicmp's own published
# headers back via dmod_link_modules() as if it were an external module.
target_link_libraries(${DMOD_MODULE_NAME} dmicmp_if)

# dmip: dmicmp.h #includes dmip.h directly (dmip_addr_t, dmip_send()'s
#       family) - needed for that header to resolve even though ping.c
#       itself never calls a dmip_* function (same reasoning dmicmp's own
#       CMakeLists.txt gives for linking dmroute/dmnetif header-only).
# dmroute: dmip.h needs dmroute_addr_t (re-exported as dmip_addr_t)
#          directly - dmicmp_if alone doesn't carry dmroute's own include
#          dir.
# dmnetif: dmip.h needs dmnetif_iface_t for dmip_protocol_handler_t: not
#          called from ping.c, just part of the header chain dmicmp.h
#          pulls in.
# dmarp: DMARP_DEFAULT_TIMEOUT_MS, the ARP-resolution timeout forwarded to
#        dmicmp_v4_send_echo_request().
# dmosi: dmosi_semaphore_*() to block for the matching Echo Reply (with a
#        timeout), dmosi_get_tick_count() for round-trip time, and
#        dmosi_thread_sleep() for the delay between requests.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmroute
    dmnetif
    dmarp
    dmosi
)

add_subdirectory(tests)
