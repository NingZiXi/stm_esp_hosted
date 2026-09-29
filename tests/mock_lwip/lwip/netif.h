#ifndef MOCK_LWIP_NETIF_H
#define MOCK_LWIP_NETIF_H
#include <stdint.h>
struct pbuf;
typedef struct { uint8_t byte[4]; } ip4_addr_t;
typedef int err_t;
typedef uint16_t u16_t;
#define ERR_OK 0
#define ERR_BUF -1
#define ERR_IF -2
#define ERR_ARG -3
#define NETIF_FLAG_BROADCAST 1U
#define NETIF_FLAG_ETHARP 2U
#define NETIF_FLAG_ETHERNET 4U
struct netif {
    void *state;
    char name[2];
    uint8_t hwaddr_len, hwaddr[6], flags, link_up;
    uint16_t mtu;
    ip4_addr_t ip_addr, netmask, gw;
    err_t (*input)(struct pbuf *, struct netif *);
    err_t (*output)(struct netif *, struct pbuf *, const void *);
    err_t (*linkoutput)(struct netif *, struct pbuf *);
};
#define netif_ip4_addr(n) (&(n)->ip_addr)
#define netif_ip4_netmask(n) (&(n)->netmask)
#define ip4_addr1(a) ((a)->byte[0])
#define ip4_addr2(a) ((a)->byte[1])
#define ip4_addr3(a) ((a)->byte[2])
#define ip4_addr4(a) ((a)->byte[3])
void netif_set_link_up(struct netif *n);
void netif_set_link_down(struct netif *n);
int netif_is_link_up(const struct netif *n);
void netif_set_addr(struct netif *n, const ip4_addr_t *ip,
                    const ip4_addr_t *mask, const ip4_addr_t *gw);
#endif
