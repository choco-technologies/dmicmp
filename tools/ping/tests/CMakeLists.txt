set(DMOD_MODULE_NAME        test_ping)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    ping_test.c
)

# `ping` itself is never linked as a dependency here - it's invoked on
# demand through Dmod_RunModule() (see ping_test.c), which loads/runs/
# unloads it per call instead of holding it resident for the whole test
# run.
# dmroute/dmarp: seed a fixture route and ARP cache entry so a send can be
#                exercised up to (but not through) a real driver, the same
#                pattern dmicmp's own tests/dmicmp_test.c uses.
# dmnetif: the "/dev/null"-backed fixture interface the route points at.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmnetif
    dmroute
    dmarp
)
