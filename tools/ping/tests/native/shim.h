#ifndef PING_TEST_SHIM_H
#define PING_TEST_SHIM_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DMARP_DEFAULT_TIMEOUT_MS 1000u
typedef enum { dmip_family_none, dmip_family_v4, dmip_family_v6 } dmip_family_t;
typedef struct { dmip_family_t family; union { uint8_t v4[4]; uint8_t v6[16]; } addr; } dmip_addr_t;
typedef dmip_addr_t dmroute_addr_t;
typedef void* dmosi_semaphore_t;
typedef void (*dmicmp_echo_reply_handler_t)(const dmip_addr_t*, uint16_t, uint16_t, const uint8_t*, size_t);
int Dmod_Printf(const char* format, ...);
#define Dmod_SnPrintf snprintf
void* Dmod_Malloc(size_t size);
void Dmod_Free(void* p);
int dmdns_resolve(const char*, dmip_family_t, dmip_addr_t**, size_t*, uint32_t);
int dmicmp_register_echo_listener(uint16_t, dmicmp_echo_reply_handler_t);
int dmicmp_unregister_echo_listener(uint16_t);
int dmicmp_v4_send_echo_request(const dmip_addr_t*, uint16_t, uint16_t, const uint8_t*, size_t, uint32_t);
dmosi_semaphore_t dmosi_semaphore_create(uint32_t, uint32_t);
void dmosi_semaphore_destroy(dmosi_semaphore_t);
int dmosi_semaphore_wait(dmosi_semaphore_t, uint32_t, int32_t);
int dmosi_semaphore_post(dmosi_semaphore_t, uint32_t);
uint32_t dmosi_get_tick_count(void);
void dmosi_thread_sleep(uint32_t);
#endif
