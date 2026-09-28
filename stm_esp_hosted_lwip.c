#include "stm_esp_hosted_lwip.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include <string.h>

static err_t sta_output(struct netif *netif, struct pbuf *p)
{
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)netif->state;
    uint8_t frame[ESP_HOSTED_STA_MTU + 14U];
    if (!a || !p || p->tot_len < 14U || p->tot_len > sizeof(frame) ||
        pbuf_copy_partial(p, frame, p->tot_len, 0U) != p->tot_len) { return ERR_BUF; }
    return esp_hosted_send(a->hosted, frame, p->tot_len) == STM_OK ? ERR_OK : ERR_IF;
}
static void sta_receive(void *user, const uint8_t *frame, size_t length)
{
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)user;
    if (!a || !a->netif || !netif_is_link_up(a->netif) ||
        length < 14U || length > ESP_HOSTED_STA_MTU + 14U) { return; }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
    if (!p) { return; }
    if (pbuf_take(p, frame, (u16_t)length) != ERR_OK ||
        a->netif->input(p, a->netif) != ERR_OK) { pbuf_free(p); }
}
static void sta_link(void *user, uint8_t connected)
{
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)user;
    if (a && a->netif) {
        if (connected) { netif_set_link_up(a->netif); }
        else { netif_set_link_down(a->netif); }
    }
}
stm_err_t esp_hosted_lwip_prepare(esp_hosted_lwip_t *a, esp_hosted_handle_t h,
                                  struct netif *netif, const uint8_t mac[6])
{
    if (!a || !h || !netif || !mac || (mac[0] & 1U)) { return STM_ERR_INVALID_ARG; }
    a->hosted = h; a->netif = netif; memcpy(a->mac, mac, 6U);
    return STM_OK;
}
err_t esp_hosted_lwip_netif_init(struct netif *netif)
{
    if (!netif || !netif->state) { return ERR_ARG; }
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)netif->state;
    netif->name[0] = 'e'; netif->name[1] = 'h';
    netif->hwaddr_len = 6U; memcpy(netif->hwaddr, a->mac, 6U);
    netif->mtu = ESP_HOSTED_STA_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = sta_output;
    return ERR_OK;
}
stm_err_t esp_hosted_lwip_attach(esp_hosted_lwip_t *a)
{
    if (!a || !a->hosted || !a->netif || a->netif->state != a) { return STM_ERR_INVALID_ARG; }
    return esp_hosted_set_callbacks(a->hosted, sta_receive, sta_link, a);
}
