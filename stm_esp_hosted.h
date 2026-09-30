/**
 * @file    stm_esp_hosted.h
 * @brief   ESP-Hosted SPI Full-Duplex 主机传输层。
 */
#ifndef STM_ESP_HOSTED_H
#define STM_ESP_HOSTED_H

#include <stddef.h>
#include <stdint.h>
#include "stm_err.h"

#ifndef STM_ESP_HOSTED_HAL_HEADER
#define STM_ESP_HOSTED_HAL_HEADER "stm32h7xx_hal.h"
#endif
#include STM_ESP_HOSTED_HAL_HEADER

#ifdef __cplusplus
extern "C" {
#endif

#define STM_ESP_HOSTED_VERSION       "0.5.0"
#define ESP_HOSTED_FRAME_SIZE        1600U
#define ESP_HOSTED_FRAME_HEADER_SIZE  12U
#define ESP_HOSTED_FRAME_CHECKSUM_OFFSET 6U
#define ESP_HOSTED_FRAME_PAYLOAD_OFFSET 12U
#define ESP_HOSTED_DMA_ALIGNMENT     32U
#define ESP_HOSTED_DUMMY_IF_TYPE     8U
#define ESP_HOSTED_STA_IF_TYPE       1U
#define ESP_HOSTED_AP_IF_TYPE        2U
#define ESP_HOSTED_PRIV_IF_TYPE      5U
#define ESP_HOSTED_STA_MTU           1500U
#define ESP_HOSTED_LEGACY_DUMMY_IF_TYPE 6U

typedef struct esp_hosted_context *esp_hosted_handle_t;

typedef struct {
    SPI_HandleTypeDef *spi;
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;
    GPIO_TypeDef *reset_port;
    uint16_t reset_pin;
    GPIO_TypeDef *handshake_port;
    uint16_t handshake_pin;
    GPIO_TypeDef *data_ready_port;
    uint16_t data_ready_pin;
    uint8_t *tx_buffer;
    uint8_t *rx_buffer;
    size_t buffer_size;
    uint32_t transfer_timeout_ms;
    /**
     * @brief V1 dummy 帧使用的接口类型；填 0 使用 ESP_HOSTED_DUMMY_IF_TYPE。
     *
     * 旧版线协议可设置为 ESP_HOSTED_LEGACY_DUMMY_IF_TYPE。
     */
    uint8_t dummy_if_type;
    /**
     * @brief 是否生成和校验 V1 帧校验和；0 表示关闭。
     */
    uint8_t checksum_enabled;
} esp_hosted_config_t;

typedef struct {
    GPIO_PinState handshake;
    GPIO_PinState data_ready;
} esp_hosted_signals_t;

typedef struct {
    uint8_t if_type;
    uint8_t if_num;
    uint8_t flags;
    uint16_t payload_length;
    uint16_t payload_offset;
    uint16_t checksum;
    uint16_t sequence;
    uint8_t throttle_command;
    uint8_t packet_type;
    const uint8_t *payload;
} esp_hosted_frame_t;

typedef enum {
    ESP_HOSTED_FRAME_OK = 0,
    ESP_HOSTED_FRAME_DUMMY = 1,
    ESP_HOSTED_FRAME_CORRUPT = 2,
    ESP_HOSTED_FRAME_TOO_BIG = 3,
    ESP_HOSTED_FRAME_INVALID = 4,
} esp_hosted_frame_result_t;

typedef struct {
    uint32_t frame_size;
    uint32_t transfer_count;
    uint32_t last_transfer_tick;
    HAL_StatusTypeDef last_hal_status;
    uint16_t last_rpc_id;
    uint32_t last_rpc_status;
    uint8_t last_rpc_status_present;
    uint8_t ready;
} esp_hosted_info_t;

/**
 * @brief 创建主机传输对象；不会初始化 SPI 外设或 GPIO 时钟。
 */
stm_err_t esp_hosted_create(const esp_hosted_config_t *config,
                            esp_hosted_handle_t *out_handle);

/**
 * @brief 释放对象；不会反初始化调用者持有的 HAL 外设。
 */
stm_err_t esp_hosted_delete(esp_hosted_handle_t *handle);

/**
 * @brief 复位 ESP 从机并等待其启动。
 */
stm_err_t esp_hosted_reset(esp_hosted_handle_t handle,
                           uint32_t low_time_ms,
                           uint32_t boot_time_ms);

/**
 * @brief 查询 Handshake 和 Data Ready 电平。
 */
stm_err_t esp_hosted_get_signals(esp_hosted_handle_t handle,
                                  esp_hosted_signals_t *signals);

/**
 * @brief 等待 ESP-Hosted Handshake 有效。
 */
stm_err_t esp_hosted_wait_handshake(esp_hosted_handle_t handle,
                                     uint32_t timeout_ms);

/**
 * @brief 等待 ESP-Hosted Data Ready 有效。
 */
stm_err_t esp_hosted_wait_data_ready(esp_hosted_handle_t handle,
                                      uint32_t timeout_ms);

/**
 * @brief 交换一个固定 1600 字节 Full-Duplex 帧。
 *
 * tx_frame 或 rx_frame 可以为 NULL；此时使用 create 配置中的 DMA 缓冲区。
 * tx_frame 为 NULL 时发送合法的 V1 dummy 帧，而不是全零缓冲区；此时要求
 * Data Ready 有效，因为主机没有待发数据。配置中的 tx/rx 缓冲区必须位于
 * DMA 可访问内存，且至少 32 字节对齐。
 */
stm_err_t esp_hosted_transfer(esp_hosted_handle_t handle,
                              const uint8_t *tx_frame,
                              uint8_t *rx_frame);

/**
 * @brief 解析 ESP-Hosted v3 INIT TLV，构造主机能力回包载荷。
 *
 * 接收载荷以事件头 0x22 开始，返回 17 或 20 字节能力 TLV。
 * 仅对端声明 RPC v2 时回显版本 0x1A；不在此函数中发送 SPI。
 */
stm_err_t esp_hosted_build_host_caps(const uint8_t *init_payload,
                                     size_t init_length,
                                     uint8_t *caps_payload,
                                     size_t caps_capacity,
                                     size_t *caps_length);

/**
 * @brief 将载荷编码为固定长度 V1 帧；frame 不得与 payload 重叠。
 *
 * 编码只写入 frame，发送时调用 esp_hosted_transfer()。序列号由调用者维护。
 */
stm_err_t esp_hosted_encode_frame(esp_hosted_handle_t handle,
                                  uint8_t if_type,
                                  uint8_t packet_type,
                                  uint16_t sequence,
                                  const uint8_t *payload,
                                  size_t payload_length,
                                  uint8_t *frame,
                                  size_t frame_size);

/**
 * @brief 判断收到的 V1 帧是否为 dummy 帧。
 *
 * 该函数只识别空闲帧；数据帧的 payload 由 esp_hosted_decode_frame() 返回，
 * 不在本组件中解释 RPC、Wi-Fi 或 HCI 内容。
 */
uint8_t esp_hosted_is_dummy_frame(esp_hosted_handle_t handle,
                                   const uint8_t *frame,
                                   size_t frame_size);

/**
 * @brief 计算 ESP-Hosted V1 帧的 16 位字节和。
 *
 * 计算时会跳过 V1 头部中的 checksum 字段；调用者应先将该字段清零。
 */
uint16_t esp_hosted_frame_checksum(const uint8_t *frame, size_t frame_size);

/**
 * @brief 解析并校验一个 ESP-Hosted V1 帧。
 *
 * 返回的 payload 指针仍指向调用者提供的 frame，不会分配内存。
 */
esp_hosted_frame_result_t esp_hosted_decode_frame(esp_hosted_handle_t handle,
                                                   const uint8_t *frame,
                                                   size_t frame_size,
                                                   esp_hosted_frame_t *decoded);

/**
 * @brief 查询传输统计信息。
 */
stm_err_t esp_hosted_get_info(esp_hosted_handle_t handle,
                              esp_hosted_info_t *info);

/** @brief 接收到完整 STA Ethernet 帧；仅在 esp_hosted_poll() 内调用，不能保留指针。 */
typedef void (*esp_hosted_rx_fn)(void *user, const uint8_t *frame, size_t length);
/** @brief Wi-Fi 关联状态变化回调。 */
typedef void (*esp_hosted_link_fn)(void *user, uint8_t connected);

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} esp_hosted_version_t;

/** @brief 复位、完成 INIT/RPC v2 协商并核对 ESP-Hosted CP 固件 3.0.9。 */
stm_err_t esp_hosted_start(esp_hosted_handle_t handle, uint32_t timeout_ms);
/** @brief 轮询一次 SPI；没有待收帧时立即返回。 */
stm_err_t esp_hosted_poll(esp_hosted_handle_t handle);
/** @brief 注册 STA 接收及链路回调；可传 NULL 取消。 */
stm_err_t esp_hosted_set_callbacks(esp_hosted_handle_t handle,
                                   esp_hosted_rx_fn receive,
                                   esp_hosted_link_fn link,
                                   void *user);
/** @brief 查询协处理器版本（先完成 esp_hosted_start）。 */
stm_err_t esp_hosted_get_version(esp_hosted_handle_t handle, esp_hosted_version_t *version);

typedef enum {
    EH_WIFI_MODE_NULL = 0,
    EH_WIFI_MODE_STA = 1,
    EH_WIFI_MODE_AP = 2,
    EH_WIFI_MODE_APSTA = 3,
} eh_wifi_mode_t;
typedef enum { EH_WIFI_IF_STA = 0, EH_WIFI_IF_AP = 1 } eh_wifi_if_t;
/** Secondary-channel position reported by the coprocessor. */
typedef enum {
    EH_WIFI_SECOND_CHAN_NONE = 0,
    EH_WIFI_SECOND_CHAN_ABOVE = 1,
    EH_WIFI_SECOND_CHAN_BELOW = 2,
} eh_wifi_second_chan_t;
/** Associated AP client snapshot; IPv4 addresses belong to the DHCP/lwIP layer. */
typedef struct {
    uint8_t mac[6];
    int8_t rssi;
} eh_wifi_sta_record_t;
/** Optional STA reconnect policy. max_attempts=0 means unlimited. */
typedef struct {
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint32_t association_timeout_ms;
    uint32_t rpc_timeout_ms;
    uint16_t max_attempts;
    uint8_t enabled;
} eh_wifi_reconnect_config_t;
typedef struct {
    char ssid[33];
    char password[65];
} eh_wifi_sta_config_t;
typedef struct {
    char ssid[33];
    char password[65];
    uint8_t channel;
    uint8_t hidden;
    uint8_t max_connections;
} eh_wifi_ap_config_t;
typedef union {
    eh_wifi_sta_config_t sta;
    eh_wifi_ap_config_t ap;
} eh_wifi_config_t;
/** Configuration read back from the CP. Credentials are deliberately excluded. */
typedef struct {
    char ssid[33];
    uint8_t channel;         /* AP only. */
    uint8_t hidden;          /* AP only. */
    uint8_t max_connections; /* AP only. */
    uint8_t authmode;        /* AP only; ESP-IDF wifi_auth_mode_t value. */
} eh_wifi_config_info_t;

typedef enum {
    EH_WIFI_PS_NONE = 0,
    EH_WIFI_PS_MIN_MODEM = 1,
    EH_WIFI_PS_MAX_MODEM = 2,
} eh_wifi_ps_t;

typedef struct {
    const char *ssid; /* NULL scans all SSIDs. */
    uint8_t channel;  /* 0 scans all channels. */
    uint8_t show_hidden;
} eh_wifi_scan_config_t;
typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t authmode;
} eh_wifi_ap_record_t;
/** Snapshot maintained by RPC replies and asynchronous Wi-Fi events; no RPC is sent. */
typedef struct {
    eh_wifi_mode_t mode;
    uint8_t started;
    uint8_t sta_connected;
    uint8_t ap_started;
    uint8_t scan_pending;
    uint32_t last_disconnect_reason;
    uint16_t reconnect_attempts;
    uint8_t reconnect_pending;
    uint8_t reconnect_enabled;
} eh_wifi_status_t;
typedef enum {
    EH_WIFI_EVENT_SCAN_DONE,
    EH_WIFI_EVENT_STA_CONNECTED,
    EH_WIFI_EVENT_STA_DISCONNECTED,
    EH_WIFI_EVENT_AP_STARTED,
    EH_WIFI_EVENT_AP_STOPPED,
    EH_WIFI_EVENT_AP_CLIENT_CONNECTED,
    EH_WIFI_EVENT_AP_CLIENT_DISCONNECTED,
} eh_wifi_event_id_t;
typedef struct {
    eh_wifi_event_id_t id;
    uint32_t reason;
    uint32_t scan_count;
    uint32_t scan_status; /* CP status: zero means success. */
    uint8_t client_mac[6];
    uint16_t aid;
} eh_wifi_event_t;
typedef void (*eh_wifi_event_fn)(void *user, const eh_wifi_event_t *event);
typedef void (*eh_wifi_ap_rx_fn)(void *user, const uint8_t *frame, size_t length);
typedef void (*eh_wifi_ap_link_fn)(void *user, uint8_t up);

/* Wi-Fi operations require esp_hosted_start() first. connect/scan_start initiate
 * asynchronous operations; call esp_hosted_poll() frequently to receive events. */
stm_err_t eh_wifi_init(esp_hosted_handle_t handle, uint32_t timeout_ms);
stm_err_t eh_wifi_set_mode(esp_hosted_handle_t handle, eh_wifi_mode_t mode, uint32_t timeout_ms);
/** Query the CP's actual mode (unlike the cached eh_wifi_get_status snapshot). */
stm_err_t eh_wifi_get_mode(esp_hosted_handle_t handle, eh_wifi_mode_t *mode, uint32_t timeout_ms);
stm_err_t eh_wifi_set_config(esp_hosted_handle_t handle, eh_wifi_if_t iface,
                             const eh_wifi_config_t *config, uint32_t timeout_ms);
/** Query STA/AP configuration, omitting credentials and unsupported advanced fields. */
stm_err_t eh_wifi_get_config(esp_hosted_handle_t handle, eh_wifi_if_t iface,
                             eh_wifi_config_info_t *info, uint32_t timeout_ms);
/** CP power-save mode: no implicit change to its default setting. */
stm_err_t eh_wifi_set_ps(esp_hosted_handle_t handle, eh_wifi_ps_t mode, uint32_t timeout_ms);
stm_err_t eh_wifi_get_ps(esp_hosted_handle_t handle, eh_wifi_ps_t *mode, uint32_t timeout_ms);
stm_err_t eh_wifi_start(esp_hosted_handle_t handle, uint32_t timeout_ms);
stm_err_t eh_wifi_stop(esp_hosted_handle_t handle, uint32_t timeout_ms);
stm_err_t eh_wifi_connect(esp_hosted_handle_t handle, uint32_t timeout_ms);
stm_err_t eh_wifi_disconnect(esp_hosted_handle_t handle, uint32_t timeout_ms);
/** Set optional automatic STA reconnect policy; disabled by default. Does not initiate a first connection. */
stm_err_t eh_wifi_set_reconnect(esp_hosted_handle_t handle,
                                 const eh_wifi_reconnect_config_t *config);
/** Call after esp_hosted_poll() and outside callbacks; attempts a due reconnect using the configured RPC timeout. */
stm_err_t eh_wifi_reconnect_update(esp_hosted_handle_t handle);
uint8_t eh_wifi_is_connected(esp_hosted_handle_t handle);
/** Read cached state without sending an RPC. */
stm_err_t eh_wifi_get_status(esp_hosted_handle_t handle, eh_wifi_status_t *status);
/** Query the currently associated AP; requires a STA connection. */
stm_err_t eh_wifi_sta_get_ap_info(esp_hosted_handle_t handle, eh_wifi_ap_record_t *record,
                                  uint32_t timeout_ms);
/** Query STA signal strength in dBm; requires an associated STA.
 * On failure the output remains unchanged. */
stm_err_t eh_wifi_sta_get_rssi(esp_hosted_handle_t handle, int8_t *rssi, uint32_t timeout_ms);
/** Query the current radio channel; requires Wi-Fi to be started.
 * On failure both outputs remain unchanged. */
stm_err_t eh_wifi_get_channel(esp_hosted_handle_t handle, uint8_t *primary,
                             eh_wifi_second_chan_t *second, uint32_t timeout_ms);
/** Query AP clients. NULL/zero capacity queries count only. Insufficient capacity
 * returns STM_ERR_OUT_OF_RANGE with required count and leaves records unchanged.
 * All other failures leave both outputs unchanged. Requires an active AP. */
stm_err_t eh_wifi_ap_get_sta_list(esp_hosted_handle_t handle, eh_wifi_sta_record_t *records,
                                 size_t capacity, size_t *count, uint32_t timeout_ms);
/** Query a client's association ID by its unicast MAC; requires an active AP.
 * On failure the output remains unchanged. */
stm_err_t eh_wifi_ap_get_sta_aid(esp_hosted_handle_t handle, const uint8_t mac[6],
                                uint16_t *aid, uint32_t timeout_ms);
/** Request disconnection of one AP client (AID 1..2007). Success means accepted;
 * confirm actual departure through events or a fresh client-list query. */
stm_err_t eh_wifi_deauth_sta(esp_hosted_handle_t handle, uint16_t aid, uint32_t timeout_ms);
stm_err_t eh_wifi_scan_start(esp_hosted_handle_t handle,
                             const eh_wifi_scan_config_t *config, uint32_t timeout_ms);
stm_err_t eh_wifi_scan_stop(esp_hosted_handle_t handle, uint32_t timeout_ms);
stm_err_t eh_wifi_scan_get_results(esp_hosted_handle_t handle, eh_wifi_ap_record_t *records,
                                   size_t capacity, size_t *count, uint32_t timeout_ms);
stm_err_t eh_wifi_set_event_callback(esp_hosted_handle_t handle, eh_wifi_event_fn callback, void *user);
stm_err_t eh_wifi_set_ap_rx_callback(esp_hosted_handle_t handle, eh_wifi_ap_rx_fn callback, void *user);
stm_err_t eh_wifi_set_ap_link_callback(esp_hosted_handle_t handle, eh_wifi_ap_link_fn callback, void *user);
stm_err_t eh_wifi_get_mac(esp_hosted_handle_t handle, eh_wifi_if_t iface, uint8_t mac[6]);
stm_err_t eh_wifi_ap_send(esp_hosted_handle_t handle, const uint8_t *frame, size_t length);

/** @brief 发送完整 Ethernet 帧；长度不得超过 ESP_HOSTED_STA_MTU + 14。 */
stm_err_t esp_hosted_send(esp_hosted_handle_t handle,
                          const uint8_t *frame, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* STM_ESP_HOSTED_H */
