#ifndef STM_ESP_HOSTED_LWIP_H
#define STM_ESP_HOSTED_LWIP_H
#include "stm_esp_hosted.h"
#include "lwip/netif.h"
typedef struct {
    esp_hosted_handle_t hosted;
    struct netif *netif;
    uint8_t mac[6];
    uint8_t dhcp_running;
    struct netif *ap_netif;
    uint8_t ap_mac[6];
} esp_hosted_lwip_t;
/** @brief 在 netif_add() 前填入；mac 应来自 eh_wifi_get_mac(handle, EH_WIFI_IF_STA, mac)。 */
stm_err_t esp_hosted_lwip_prepare(esp_hosted_lwip_t *adapter,
                                  esp_hosted_handle_t hosted, struct netif *netif,
                                  const uint8_t mac[6]);
/** @brief 传给 netif_add() 的 init 回调，state 应指向 adapter。 */
err_t esp_hosted_lwip_netif_init(struct netif *netif);
/** @brief netif_add() 成功后调用，绑定收包和上下线事件。 */
stm_err_t esp_hosted_lwip_attach(esp_hosted_lwip_t *adapter);
/** Call after esp_hosted_poll(): follows STA link, starts/stops DHCP and clears old IPv4 address. */
stm_err_t esp_hosted_lwip_sta_update(esp_hosted_lwip_t *adapter);
/** Stop DHCP and clear the STA IPv4 address, including during application shutdown. */
stm_err_t esp_hosted_lwip_sta_stop(esp_hosted_lwip_t *adapter);
/** True only while DHCP is active and an address has been supplied. */
uint8_t esp_hosted_lwip_sta_has_address(const esp_hosted_lwip_t *adapter);
/* Optional AP netif; call prepare before netif_add and attach after it. */
stm_err_t esp_hosted_lwip_ap_prepare(esp_hosted_lwip_t *adapter,
                                     struct netif *ap_netif, const uint8_t mac[6]);
err_t esp_hosted_lwip_ap_netif_init(struct netif *netif);
stm_err_t esp_hosted_lwip_ap_attach(esp_hosted_lwip_t *adapter);
#endif
