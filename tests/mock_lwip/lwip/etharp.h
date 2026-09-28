#ifndef MOCK_LWIP_ETHARP_H
#define MOCK_LWIP_ETHARP_H
#include "lwip/netif.h"
err_t etharp_output(struct netif *n, struct pbuf *p, const void *addr);
#endif
