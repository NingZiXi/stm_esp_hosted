#ifndef MOCK_LWIP_PBUF_H
#define MOCK_LWIP_PBUF_H
#include <stdint.h>
#include "lwip/netif.h"
#define PBUF_RAW 0
#define PBUF_POOL 1
struct pbuf { struct pbuf *next; void *payload; uint16_t len, tot_len; };
struct pbuf *pbuf_alloc(int layer, uint16_t size, int type);
uint16_t pbuf_copy_partial(const struct pbuf *p, void *target, uint16_t size, uint16_t offset);
err_t pbuf_take(struct pbuf *p, const void *source, uint16_t size);
uint8_t pbuf_free(struct pbuf *p);
#endif
