#ifndef STM_ESP_HOSTED_LWIP_H
#define STM_ESP_HOSTED_LWIP_H
#include "stm_esp_hosted.h"
#include "lwip/netif.h"
typedef struct {
    esp_hosted_handle_t hosted;
    struct netif *netif;
    uint8_t mac[6];
} esp_hosted_lwip_t;
/** @brief 在 netif_add() 前填入；mac 必须来自 esp_hosted_get_sta_mac()。 */
stm_err_t esp_hosted_lwip_prepare(esp_hosted_lwip_t *adapter,
                                  esp_hosted_handle_t hosted, struct netif *netif,
                                  const uint8_t mac[6]);
/** @brief 传给 netif_add() 的 init 回调，state 应指向 adapter。 */
err_t esp_hosted_lwip_netif_init(struct netif *netif);
/** @brief netif_add() 成功后调用，绑定收包和上下线事件。 */
stm_err_t esp_hosted_lwip_attach(esp_hosted_lwip_t *adapter);
#endif
