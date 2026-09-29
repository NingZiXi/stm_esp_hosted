#include "stm_esp_hosted_dhcps.h"
#include "lwip/sys.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct netif *incoming;
static struct udp_pcb *active;
static uint32_t now;
static uint8_t last_reply[576];
static size_t reply_size;
static unsigned sends;
struct netif *ip_current_input_netif(void) { return incoming; }
uint32_t sys_now(void) { return now; }
struct udp_pcb *udp_new_ip_type(int type) { (void)type; return calloc(1, sizeof(struct udp_pcb)); }
void udp_bind_netif(struct udp_pcb *p, struct netif *n) { p->netif=n; }
err_t udp_bind(struct udp_pcb *p, const ip_addr_t *a, u16_t port) { (void)p; (void)a; assert(port==67); return ERR_OK; }
void udp_recv(struct udp_pcb *p, void (*fn)(void *, struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t), void *arg) { p->receive=fn; p->arg=arg; active=p; }
err_t udp_sendto_if_src(struct udp_pcb *p, struct pbuf *b, const ip_addr_t *to, u16_t port, struct netif *n, const ip_addr_t *from)
{
    assert(p==active && n==incoming && port==68 && to->byte[0]==255 && from->byte[0]==192);
    assert(b->tot_len<=sizeof(last_reply));
    reply_size=b->tot_len; memcpy(last_reply,b->payload,reply_size); ++sends; return ERR_OK;
}
void udp_remove(struct udp_pcb *p) { assert(p==active); active=NULL; free(p); }
static void packet(uint8_t type, const uint8_t mac[6], uint8_t wanted, int bad)
{
    uint8_t bytes[300]={0}; bytes[0]=bytes[1]=1; bytes[2]=6; bytes[4]=0x12; bytes[7]=0x34;
    memcpy(bytes+28,mac,6); bytes[236]=99; bytes[237]=130; bytes[238]=83; bytes[239]=99;
    bytes[240]=53; bytes[241]=1; bytes[242]=type;
    bytes[243]=50; bytes[244]=4; bytes[245]=192; bytes[246]=168; bytes[247]=40; bytes[248]=wanted;
    bytes[249]=255;
    if (type==7) { bytes[12]=192; bytes[13]=168; bytes[14]=40; bytes[15]=wanted; }
    if (bad) bytes[244]=255;
    struct pbuf *b=pbuf_alloc(PBUF_TRANSPORT,sizeof(bytes),PBUF_RAM);
    assert(b && pbuf_take(b,bytes,sizeof(bytes))==ERR_OK);
    active->receive(active->arg,active,b,NULL,68);
}
int main(void)
{
    struct netif ap={0}; esp_hosted_lwip_dhcps_t dhcp={0};
    ap.ip_addr=(ip4_addr_t){{192,168,40,1}}; ap.netmask=(ip4_addr_t){{255,255,255,0}};
    incoming=&ap; ap.link_up=1;
    assert(esp_hosted_lwip_dhcps_start(&dhcp,&ap)==STM_OK);
    const uint8_t a[6]={2,1,1,1,1,1}, b[6]={2,2,2,2,2,2};
    packet(1,a,0,0); assert(sends==1 && reply_size==300 && last_reply[242]==2 && last_reply[19]==100);
    packet(3,a,100,0); assert(sends==2 && last_reply[242]==5 && dhcp.leases[0].last_octet==100);
    packet(3,b,100,0); assert(sends==2); /* another client's active lease is protected */
    packet(1,b,0,0); assert(sends==3 && last_reply[19]==101);
    packet(3,b,101,0); assert(sends==4 && last_reply[242]==5);
    packet(3,a,102,0); assert(sends==5 && last_reply[242]==6 && dhcp.leases[0].last_octet==100);
    packet(1,a,0,1); assert(sends==5);
    incoming=NULL; packet(1,a,0,0); assert(sends==5); incoming=&ap;
    packet(7,a,100,0); assert(dhcp.leases[0].last_octet==0);
    now=3600001U; packet(3,b,100,0); assert(sends==6 && last_reply[242]==5);
    ap.link_up=0;
    assert(esp_hosted_lwip_dhcps_update(&dhcp,&ap)==STM_OK && !active);
    assert(esp_hosted_lwip_dhcps_update(&dhcp,&ap)==STM_OK && !active);
    ap.link_up=1;
    assert(esp_hosted_lwip_dhcps_update(&dhcp,&ap)==STM_OK && active);
    assert(esp_hosted_lwip_dhcps_update(&dhcp,&ap)==STM_OK && active);
    esp_hosted_lwip_dhcps_stop(&dhcp); assert(!active);
    puts("stm_esp_hosted DHCP tests: PASS");
    return 0;
}
