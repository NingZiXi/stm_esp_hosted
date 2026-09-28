#ifndef MOCK_LWIP_NETIF_H
#define MOCK_LWIP_NETIF_H
#include <stdint.h>
struct pbuf;
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
    err_t (*input)(struct pbuf *, struct netif *);
    err_t (*output)(struct netif *, struct pbuf *, const void *);
    err_t (*linkoutput)(struct netif *, struct pbuf *);
};
void netif_set_link_up(struct netif *n);
void netif_set_link_down(struct netif *n);
int netif_is_link_up(const struct netif *n);
#endif

