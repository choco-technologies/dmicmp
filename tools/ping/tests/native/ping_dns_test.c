#include "shim.h"
#include <stdarg.h>
#include <errno.h>

#define main ping_main
#include "../../src/ping.c"
#undef main

/* Fail even in Release builds, where assert() would disappear. */
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static int dns_error, dns_calls, sends, allocations;
static size_t dns_count = 2;
static bool fail_allocation;
static dmicmp_echo_reply_handler_t listener;
static char output[4096];
static size_t output_len;
static const dmip_addr_t first = { .family = dmip_family_v4, .addr.v4 = { 203, 0, 113, 7 } };

int Dmod_Printf(const char* format, ...)
{
    va_list args; va_start(args, format);
    int n = vsnprintf(output + output_len, sizeof(output) - output_len, format, args);
    va_end(args);
    CHECK(n >= 0 && (size_t)n < sizeof(output) - output_len);
    output_len += (size_t)n;
    return n;
}
void* Dmod_Malloc(size_t size)
{
    if (fail_allocation) return NULL;
    void* p = malloc(size); CHECK(p); ++allocations; return p;
}
void Dmod_Free(void* p) { if (p) { --allocations; free(p); } }
int dmdns_resolve(const char* name, dmip_family_t family, dmip_addr_t** addrs, size_t* count, uint32_t timeout)
{
    CHECK(strcmp(name, "example.test") == 0 || strcmp(name, "203.0.113.7") == 0);
    CHECK(family == dmip_family_v4 && timeout == 0);
    ++dns_calls;
    *addrs = NULL; *count = 0;
    if (dns_error) return dns_error;
    if (!dns_count) return 0;
    *addrs = Dmod_Malloc(2 * sizeof(**addrs));
    if (!*addrs) return -ENOMEM;
    (*addrs)[0] = first;
    (*addrs)[1] = first; (*addrs)[1].addr.v4[3] = 8;
    *count = dns_count;
    return 0;
}
int dmicmp_register_echo_listener(uint16_t id, dmicmp_echo_reply_handler_t cb)
{ (void)id; listener = cb; return 0; }
int dmicmp_unregister_echo_listener(uint16_t id)
{ (void)id; listener = NULL; return 0; }
int dmicmp_v4_send_echo_request(const dmip_addr_t* dst, uint16_t id, uint16_t seq, const uint8_t* data, size_t len, uint32_t timeout)
{
    (void)timeout;
    CHECK(dst->family == first.family && memcmp(dst->addr.v4, first.addr.v4, 4) == 0);
    ++sends; CHECK(listener); listener(dst, id, seq, data, len); return 0;
}
dmosi_semaphore_t dmosi_semaphore_create(uint32_t initial, uint32_t max)
{ (void)initial; (void)max; return Dmod_Malloc(1); }
void dmosi_semaphore_destroy(dmosi_semaphore_t sem) { Dmod_Free(sem); }
int dmosi_semaphore_wait(dmosi_semaphore_t sem, uint32_t count, int32_t timeout)
{ (void)sem; (void)count; (void)timeout; return 0; }
int dmosi_semaphore_post(dmosi_semaphore_t sem, uint32_t count)
{ (void)sem; (void)count; return 0; }
uint32_t dmosi_get_tick_count(void) { return 42; }
void dmosi_thread_sleep(uint32_t ms) { (void)ms; }

static int run(int argc, char** argv)
{
    dns_calls = sends = 0; output_len = 0; output[0] = '\0';
    int rc = ping_main(argc, argv);
    CHECK(allocations == 0 && listener == NULL);
    return rc;
}
int main(void)
{
    char* name[] = { "ping", "example.test", "-c", "2", "-i", "0" };
    CHECK(run(6, name) == 0 && dns_calls == 1 && sends == 2);
    CHECK(strstr(output, "PING example.test (203.0.113.7)"));
    CHECK(strstr(output, "bytes from 203.0.113.7"));
    CHECK(strstr(output, "2 packets received, 0% packet loss"));
    char* literal[] = { "ping", "203.0.113.7", "-c", "1" };
    CHECK(run(4, literal) == 0 && sends == 1);
    int errors[] = { -ENOENT, -ENODATA, -ETIMEDOUT, -EDESTADDRREQ, -EINVAL };
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        dns_error = errors[i];
        CHECK(run(6, name) == 1 && dns_calls == 1 && sends == 0);
        CHECK(strstr(output, "cannot resolve 'example.test' to IPv4"));
    }
    dns_error = 0; dns_count = 0;
    CHECK(run(6, name) == 1 && sends == 0);
    dns_count = 2; fail_allocation = true;
    CHECK(run(6, name) == 1 && sends == 0);
    fail_allocation = false;
    char* invalid[] = { "ping", "example.test", "-c", "0" };
    CHECK(run(4, invalid) == 1 && dns_calls == 0);
    char* help[] = { "ping", "--help" };
    CHECK(run(2, help) == 0 && dns_calls == 0);
    puts("PASS: hostname, first IPv4, single resolution, literal, DNS failures, cleanup, argument validation");
    return 0;
}
