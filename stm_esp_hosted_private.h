#ifndef STM_ESP_HOSTED_PRIVATE_H
#define STM_ESP_HOSTED_PRIVATE_H
#include "stm_esp_hosted.h"
struct esp_hosted_context {
    esp_hosted_config_t config;
    esp_hosted_info_t info;
    esp_hosted_rx_fn receive;
    esp_hosted_link_fn link;
    void *user;
    uint16_t sequence;
    uint32_t uid;
    uint32_t response_uid;
    uint16_t response_id;
    uint8_t response_data[128];
    size_t response_length;
    esp_hosted_version_t version;
    uint8_t mac[6];
    uint8_t initialized;
    uint8_t negotiated;
    uint8_t connected;
    uint8_t wifi_initialized;
    uint8_t response_ready;
};
#endif
