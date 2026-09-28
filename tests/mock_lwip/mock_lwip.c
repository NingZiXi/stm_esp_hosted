#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include <stdlib.h>
#include <string.h>
struct pbuf *pbuf_alloc(int layer, uint16_t size, int type)
{
    (void)layer; (void)type;
    struct pbuf *p = calloc(1, sizeof(*p));
    if (!p) { return NULL; }
    p->payload = malloc(size);
    if (!p->payload) { free(p); return NULL; }
    p->len = p->tot_len = size; return p;
}
uint16_t pbuf_copy_partial(const struct pbuf *p, void *dst, uint16_t size, uint16_t offset)
{
    uint16_t copied = 0;
    while (p && copied < size) {
        if (offset >= p->len) { offset -= p->len; p = p->next; continue; }
        uint16_t n = p->len - offset;
        if (n > size - copied) { n = size - copied; }
        memcpy((uint8_t *)dst + copied, (uint8_t *)p->payload + offset, n);
        copied += n; offset = 0; p = p->next;
    }
    return copied;
}
err_t pbuf_take(struct pbuf *p, const void *src, uint16_t size)
{
    for (const uint8_t *b = src; p && size; p = p->next) {
        uint16_t n = p->len < size ? p->len : size;
        memcpy(p->payload, b, n); b += n; size -= n;
    }
    return size ? ERR_BUF : ERR_OK;
}
uint8_t pbuf_free(struct pbuf *p)
{
    uint8_t n = 0;
    while (p) { struct pbuf *next = p->next; free(p->payload); free(p); p = next; ++n; }
    return n;
}
void netif_set_link_up(struct netif *n) { n->link_up = 1; }
void netif_set_link_down(struct netif *n) { n->link_up = 0; }
int netif_is_link_up(const struct netif *n) { return n->link_up; }
err_t etharp_output(struct netif *n, struct pbuf *p, const void *addr)
{ (void)addr; return n->linkoutput(n, p); }
