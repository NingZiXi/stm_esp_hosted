#ifndef MOCK_LWIP_IP_H
#define MOCK_LWIP_IP_H
#include "lwip/netif.h"
typedef ip4_addr_t ip_addr_t;
#define IP_ADDR4(dst,a,b,c,d) do { (dst)->byte[0]=(a); (dst)->byte[1]=(b); (dst)->byte[2]=(c); (dst)->byte[3]=(d); } while (0)
#define ip_addr_copy_from_ip4(dst, src) ((dst)=(src))
struct netif *ip_current_input_netif(void);
#endif
