#ifndef STM_ESP_HOSTED_DHCPS_H
#define STM_ESP_HOSTED_DHCPS_H
#include "stm_esp_hosted_lwip.h"
#include "lwip/udp.h"
/* Four leases on the AP /24 subnet; call sys_check_timeouts() in the main loop. */
typedef struct {
    struct netif *ap_netif;
    struct udp_pcb *pcb;
    struct {
        uint8_t mac[6];
        uint8_t last_octet;
        uint32_t expires_at;
    } leases[4];
} esp_hosted_lwip_dhcps_t;
stm_err_t esp_hosted_lwip_dhcps_start(esp_hosted_lwip_dhcps_t *server, struct netif *ap_netif);
void esp_hosted_lwip_dhcps_stop(esp_hosted_lwip_dhcps_t *server);
/** Call after esp_hosted_poll() to follow AP link; four leases only. */
stm_err_t esp_hosted_lwip_dhcps_update(esp_hosted_lwip_dhcps_t *server,
                                       struct netif *ap_netif);
#endif
