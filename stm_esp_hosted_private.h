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
    uint8_t response_data[512];
    size_t response_length;
    esp_hosted_version_t version;
    uint8_t mac[6];
    uint8_t initialized;
    uint8_t negotiated;
    uint8_t connected;
    uint8_t wifi_initialized;
    uint8_t wifi_started;
    uint8_t wifi_mode;
    uint8_t scan_pending;
    uint8_t scan_done;
    uint32_t last_disconnect_reason;
    eh_wifi_reconnect_config_t reconnect_config;
    uint32_t reconnect_since;
    uint32_t reconnect_delay;
    uint16_t reconnect_attempts;
    uint8_t reconnect_armed;
    uint8_t reconnect_waiting;
    uint8_t connect_pending;
    eh_wifi_event_fn wifi_event;
    void *wifi_event_user;
    eh_wifi_ap_link_fn ap_link;
    void *ap_link_user;
    uint8_t ap_up;
    eh_wifi_ap_rx_fn ap_receive;
    void *ap_user;
    uint8_t response_ready;
    esp_hosted_diagnostics_t diagnostics;
    esp_hosted_monitor_config_t monitor;
    uint32_t heartbeat_tick;
    uint32_t session_epoch;
    uint32_t request_epoch, request_tick, request_timeout;
    uint16_t request_id;
    uint8_t request_active, request_sent, callback_depth;
    stm_err_t request_error;
    uint8_t recovery_phase, starting;
    uint32_t recovery_tick, recovery_timeout, phase_tick;
    uint8_t rx_callback, queued_count, queued_head;
    uint8_t queued_iface[2];
    uint16_t queued_length[2];
    uint8_t queued_frame[2][ESP_HOSTED_STA_MTU + 14U];
};
void esp_hosted_invalidate(struct esp_hosted_context *ctx, esp_hosted_fault_t reason,
                           stm_err_t error, esp_hosted_state_t state);
void esp_hosted_record_fault(struct esp_hosted_context *ctx, esp_hosted_fault_t reason,
                             stm_err_t error);
void esp_hosted_count(uint32_t *counter);
#endif
