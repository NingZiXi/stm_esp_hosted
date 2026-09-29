#ifndef MOCK_LWIP_UDP_H
#define MOCK_LWIP_UDP_H
#include "lwip/ip.h"
#include "lwip/pbuf.h"
#define IPADDR_TYPE_V4 0
#define IP_ANY_TYPE ((const ip_addr_t *)0)
struct udp_pcb { struct netif *netif; void (*receive)(void *, struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t); void *arg; };
struct udp_pcb *udp_new_ip_type(int type);
void udp_bind_netif(struct udp_pcb *pcb, struct netif *netif);
err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port);
void udp_recv(struct udp_pcb *pcb, void (*receive)(void *, struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t), void *arg);
err_t udp_sendto_if_src(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst, u16_t port, struct netif *netif, const ip_addr_t *src);
void udp_remove(struct udp_pcb *pcb);
#endif
