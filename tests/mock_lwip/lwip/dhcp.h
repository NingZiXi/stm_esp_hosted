#ifndef MOCK_LWIP_DHCP_H
#define MOCK_LWIP_DHCP_H
#include "lwip/netif.h"
err_t dhcp_start(struct netif *netif);
void dhcp_stop(struct netif *netif);
uint8_t dhcp_supplied_address(const struct netif *netif);
extern unsigned mock_dhcp_starts, mock_dhcp_stops;
extern int mock_dhcp_error;
#endif
