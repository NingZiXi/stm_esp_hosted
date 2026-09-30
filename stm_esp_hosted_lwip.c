#include "stm_esp_hosted_lwip.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
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
    a->hosted = h; a->netif = netif; a->dhcp_running = 0U; memcpy(a->mac, mac, 6U);
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
    eh_wifi_status_t status;
    stm_err_t err = eh_wifi_get_status(a->hosted, &status);
    if (err == STM_OK) { err = esp_hosted_set_callbacks(a->hosted, sta_receive, sta_link, a); }
    /* Association may have completed before the netif was attached. */
    if (err == STM_OK) { sta_link(a, status.sta_connected); }
    return err;
}

stm_err_t esp_hosted_lwip_sta_stop(esp_hosted_lwip_t *a)
{
    if (!a || !a->netif || a->netif->state != a) { return STM_ERR_INVALID_ARG; }
    if (a->dhcp_running) { dhcp_stop(a->netif); a->dhcp_running = 0U; }
    netif_set_addr(a->netif, NULL, NULL, NULL);
    return STM_OK;
}
stm_err_t esp_hosted_lwip_sta_update(esp_hosted_lwip_t *a)
{
    if (!a || !a->netif || a->netif->state != a) { return STM_ERR_INVALID_ARG; }
    if (!netif_is_link_up(a->netif)) {
        if (a->dhcp_running) { return esp_hosted_lwip_sta_stop(a); }
        return STM_OK;
    }
    if (!a->dhcp_running) {
        if (dhcp_start(a->netif) != ERR_OK) { return STM_ERR_IO; }
        a->dhcp_running = 1U;
    }
    return STM_OK;
}
uint8_t esp_hosted_lwip_sta_has_address(const esp_hosted_lwip_t *a)
{
    return (uint8_t)(a && a->netif && a->netif->state == a && a->dhcp_running &&
                     netif_is_link_up(a->netif) && dhcp_supplied_address(a->netif));
}

static err_t ap_output(struct netif *netif, struct pbuf *p)
{
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)netif->state;
    uint8_t frame[ESP_HOSTED_STA_MTU + 14U];
    if (!a || !p || p->tot_len < 14U || p->tot_len > sizeof(frame) ||
        pbuf_copy_partial(p, frame, p->tot_len, 0U) != p->tot_len) { return ERR_BUF; }
    return eh_wifi_ap_send(a->hosted, frame, p->tot_len) == STM_OK ? ERR_OK : ERR_IF;
}
static void ap_receive(void *user, const uint8_t *frame, size_t length)
{
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)user;
    if (!a || !a->ap_netif || !netif_is_link_up(a->ap_netif) ||
        length < 14U || length > ESP_HOSTED_STA_MTU + 14U) { return; }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
    if (!p) { return; }
    if (pbuf_take(p, frame, (u16_t)length) != ERR_OK ||
        a->ap_netif->input(p, a->ap_netif) != ERR_OK) { pbuf_free(p); }
}
static void ap_link(void *user, uint8_t up)
{
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)user;
    if (a && a->ap_netif) {
        if (up) { netif_set_link_up(a->ap_netif); }
        else { netif_set_link_down(a->ap_netif); }
    }
}
stm_err_t esp_hosted_lwip_ap_prepare(esp_hosted_lwip_t *a,
                                     struct netif *netif, const uint8_t mac[6])
{
    if (!a || !a->hosted || !netif || !mac || (mac[0] & 1U)) { return STM_ERR_INVALID_ARG; }
    a->ap_netif = netif; memcpy(a->ap_mac, mac, 6U);
    return STM_OK;
}
err_t esp_hosted_lwip_ap_netif_init(struct netif *netif)
{
    if (!netif || !netif->state) { return ERR_ARG; }
    esp_hosted_lwip_t *a = (esp_hosted_lwip_t *)netif->state;
    netif->name[0] = 'a'; netif->name[1] = 'p';
    netif->hwaddr_len = 6U; memcpy(netif->hwaddr, a->ap_mac, 6U);
    netif->mtu = ESP_HOSTED_STA_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = ap_output;
    return ERR_OK;
}
stm_err_t esp_hosted_lwip_ap_attach(esp_hosted_lwip_t *a)
{
    if (!a || !a->hosted || !a->ap_netif || a->ap_netif->state != a) { return STM_ERR_INVALID_ARG; }
    eh_wifi_status_t status;
    stm_err_t err = eh_wifi_get_status(a->hosted, &status);
    if (err == STM_OK) { err = eh_wifi_set_ap_rx_callback(a->hosted, ap_receive, a); }
    if (err == STM_OK) { err = eh_wifi_set_ap_link_callback(a->hosted, ap_link, a); }
    /* AP_START can arrive while startup RPCs are still being processed. */
    if (err == STM_OK) { ap_link(a, status.ap_started); }
    return err;
}
