/**
 * @file    stm_esp_hosted.c
 * @brief   ESP-Hosted SPI Full-Duplex 主机传输层实现。
 */
#include "stm_esp_hosted.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stm_esp_hosted_private.h"

static uint8_t dummy_if_type(const struct esp_hosted_context *context)
{
    return context->config.dummy_if_type != 0U
               ? context->config.dummy_if_type
               : ESP_HOSTED_DUMMY_IF_TYPE;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}


static uint16_t checksum_zeroed(const uint8_t *frame, size_t frame_size)
{
    uint16_t checksum = 0U;
    size_t index;

    for (index = 0U; index < frame_size; ++index) {
        if (index == ESP_HOSTED_FRAME_CHECKSUM_OFFSET ||
            index == ESP_HOSTED_FRAME_CHECKSUM_OFFSET + 1U) {
            continue;
        }
        checksum = (uint16_t)(checksum + frame[index]);
    }
    return checksum;
}

static void encode_dummy_frame(const struct esp_hosted_context *context,
                               uint8_t *frame)
{
    memset(frame, 0, ESP_HOSTED_FRAME_SIZE);
    /* V1: if_type occupies the low nibble; if_num=0xF marks a dummy frame. */
    frame[0] = (uint8_t)((dummy_if_type(context) & 0x0FU) | 0xF0U);
    /* ESP-Hosted V1 dummy headers leave offset and checksum zero. */
}

static int is_aligned(const void *address)
{
    return address != NULL && (((uintptr_t)address & (ESP_HOSTED_DMA_ALIGNMENT - 1U)) == 0U);
}

static void cache_clean(void *address, size_t length)
{
    uintptr_t start;
    uintptr_t end;

    if (address == NULL || length == 0U || (SCB->CCR & SCB_CCR_DC_Msk) == 0U) {
        return;
    }
    start = (uintptr_t)address & ~(uintptr_t)(ESP_HOSTED_DMA_ALIGNMENT - 1U);
    end = ((uintptr_t)address + length + ESP_HOSTED_DMA_ALIGNMENT - 1U) &
          ~(uintptr_t)(ESP_HOSTED_DMA_ALIGNMENT - 1U);
    SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

static void cache_invalidate(void *address, size_t length)
{
    uintptr_t start;
    uintptr_t end;

    if (address == NULL || length == 0U || (SCB->CCR & SCB_CCR_DC_Msk) == 0U) {
        return;
    }
    start = (uintptr_t)address & ~(uintptr_t)(ESP_HOSTED_DMA_ALIGNMENT - 1U);
    end = ((uintptr_t)address + length + ESP_HOSTED_DMA_ALIGNMENT - 1U) &
          ~(uintptr_t)(ESP_HOSTED_DMA_ALIGNMENT - 1U);
    SCB_InvalidateDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

static stm_err_t map_hal_status(HAL_StatusTypeDef status)
{
    if (status == HAL_OK) {
        return STM_OK;
    }
    if (status == HAL_TIMEOUT) {
        return STM_ERR_TIMEOUT;
    }
    return STM_ERR_IO;
}

stm_err_t esp_hosted_create(const esp_hosted_config_t *config,
                            esp_hosted_handle_t *out_handle)
{
    struct esp_hosted_context *context;

    if (config == NULL || out_handle == NULL || *out_handle != NULL ||
        config->spi == NULL || config->cs_port == NULL || config->reset_port == NULL ||
        config->handshake_port == NULL || config->data_ready_port == NULL ||
        config->tx_buffer == NULL || config->rx_buffer == NULL ||
        config->tx_buffer == config->rx_buffer ||
        config->buffer_size < ESP_HOSTED_FRAME_SIZE ||
        !is_aligned(config->tx_buffer) || !is_aligned(config->rx_buffer)) {
        return STM_ERR_INVALID_ARG;
    }
    if (config->transfer_timeout_ms == 0U ||
        (config->dummy_if_type != 0U && config->dummy_if_type > 0x0FU)) {
        return STM_ERR_INVALID_CONFIG;
    }

    context = (struct esp_hosted_context *)calloc(1U, sizeof(*context));
    if (context == NULL) {
        return STM_ERR_NO_MEM;
    }
    context->config = *config;
    context->info.frame_size = ESP_HOSTED_FRAME_SIZE;
    context->info.last_hal_status = HAL_OK;
    *out_handle = context;
    return STM_OK;
}

stm_err_t esp_hosted_delete(esp_hosted_handle_t *handle)
{
    if (handle == NULL) {
        return STM_ERR_INVALID_ARG;
    }
    if (*handle != NULL) {
        if ((*handle)->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
        if ((*handle)->request_active || (*handle)->recovery_phase) { return STM_ERR_INVALID_STATE; }
        free(*handle);
        *handle = NULL;
    }
    return STM_OK;
}

stm_err_t esp_hosted_reset(esp_hosted_handle_t handle,
                           uint32_t low_time_ms,
                           uint32_t boot_time_ms)
{
    if (handle == NULL || low_time_ms == 0U || boot_time_ms == 0U) {
        return STM_ERR_INVALID_ARG;
    }
    if (handle->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (handle->request_active || handle->recovery_phase) { return STM_ERR_INVALID_STATE; }
    esp_hosted_invalidate(handle, ESP_HOSTED_FAULT_RESET, STM_ERR_CANCELLED,
                          ESP_HOSTED_STATE_NOT_READY);
    HAL_GPIO_WritePin(handle->config.cs_port, handle->config.cs_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(handle->config.reset_port, handle->config.reset_pin, GPIO_PIN_RESET);
    HAL_Delay(low_time_ms);
    HAL_GPIO_WritePin(handle->config.reset_port, handle->config.reset_pin, GPIO_PIN_SET);
    HAL_Delay(boot_time_ms);
    return STM_OK;
}

stm_err_t esp_hosted_get_signals(esp_hosted_handle_t handle,
                                  esp_hosted_signals_t *signals)
{
    if (handle == NULL || signals == NULL) {
        return STM_ERR_INVALID_ARG;
    }
    signals->handshake = HAL_GPIO_ReadPin(handle->config.handshake_port,
                                           handle->config.handshake_pin);
    signals->data_ready = HAL_GPIO_ReadPin(handle->config.data_ready_port,
                                           handle->config.data_ready_pin);
    return STM_OK;
}

static stm_err_t wait_signal(GPIO_TypeDef *port,
                             uint16_t pin,
                             uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while (HAL_GPIO_ReadPin(port, pin) != GPIO_PIN_SET) {
        if ((HAL_GetTick() - start) >= timeout_ms) {
            return STM_ERR_TIMEOUT;
        }
    }
    return STM_OK;
}

stm_err_t esp_hosted_wait_handshake(esp_hosted_handle_t handle,
                                     uint32_t timeout_ms)
{
    if (handle == NULL || timeout_ms == 0U) {
        return STM_ERR_INVALID_ARG;
    }
    return wait_signal(handle->config.handshake_port,
                       handle->config.handshake_pin,
                       timeout_ms);
}

stm_err_t esp_hosted_wait_data_ready(esp_hosted_handle_t handle,
                                      uint32_t timeout_ms)
{
    if (handle == NULL || timeout_ms == 0U) {
        return STM_ERR_INVALID_ARG;
    }
    return wait_signal(handle->config.data_ready_port,
                       handle->config.data_ready_pin,
                       timeout_ms);
}

stm_err_t esp_hosted_transfer(esp_hosted_handle_t handle,
                              const uint8_t *tx_frame,
                              uint8_t *rx_frame)
{
    HAL_StatusTypeDef hal_status;
    uint8_t *tx_buffer;
    uint8_t *rx_buffer;

    if (handle == NULL) {
        return STM_ERR_INVALID_ARG;
    }
    if (handle->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (HAL_GPIO_ReadPin(handle->config.handshake_port,
                         handle->config.handshake_pin) != GPIO_PIN_SET) {
        return STM_ERR_INVALID_STATE;
    }
    /* With no host payload, a transaction is allowed only when the slave has data. */
    if (tx_frame == NULL &&
        HAL_GPIO_ReadPin(handle->config.data_ready_port,
                         handle->config.data_ready_pin) != GPIO_PIN_SET) {
        return STM_ERR_INVALID_STATE;
    }

    tx_buffer = handle->config.tx_buffer;
    rx_buffer = handle->config.rx_buffer;
    if (tx_frame != NULL && tx_frame != tx_buffer) {
        memcpy(tx_buffer, tx_frame, ESP_HOSTED_FRAME_SIZE);
    } else if (tx_frame == NULL) {
        encode_dummy_frame(handle, tx_buffer);
    }
    if (rx_frame != NULL && rx_frame != rx_buffer) {
        memset(rx_buffer, 0, ESP_HOSTED_FRAME_SIZE);
    }
    cache_clean(tx_buffer, ESP_HOSTED_FRAME_SIZE);
    cache_invalidate(rx_buffer, ESP_HOSTED_FRAME_SIZE);

    HAL_GPIO_WritePin(handle->config.cs_port, handle->config.cs_pin, GPIO_PIN_RESET);
    hal_status = HAL_SPI_TransmitReceive(handle->config.spi,
                                          tx_buffer,
                                          rx_buffer,
                                          ESP_HOSTED_FRAME_SIZE,
                                          handle->config.transfer_timeout_ms);
    HAL_GPIO_WritePin(handle->config.cs_port, handle->config.cs_pin, GPIO_PIN_SET);
    cache_invalidate(rx_buffer, ESP_HOSTED_FRAME_SIZE);
    handle->info.last_hal_status = hal_status;
    if (hal_status != HAL_OK) {
        esp_hosted_count(&handle->diagnostics.spi_failures);
        esp_hosted_record_fault(handle, ESP_HOSTED_FAULT_SPI, map_hal_status(hal_status));
        return map_hal_status(hal_status);
    }
    if (rx_frame != NULL && rx_frame != rx_buffer) {
        memcpy(rx_frame, rx_buffer, ESP_HOSTED_FRAME_SIZE);
    }
    handle->info.transfer_count++;
    handle->info.last_transfer_tick = HAL_GetTick();
    handle->info.ready = 1U;
    return STM_OK;
}

stm_err_t esp_hosted_build_host_caps(const uint8_t *init_payload,
                                     size_t init_length,
                                     uint8_t *caps_payload,
                                     size_t caps_capacity,
                                     size_t *caps_length)
{
    uint8_t chip_id = 0U;
    uint8_t rpc_version = 0U;
    size_t end;
    size_t i;
    static const uint8_t base_caps[17] = {
        0x22U, 15U, 0x44U, 1U, 0U, 0x45U, 1U, 0U, 0x46U, 1U, 0U,
        0x47U, 1U, 0U, 0x48U, 1U, 0U
    };
    if (init_payload == NULL || caps_payload == NULL || caps_length == NULL ||
        init_length < 2U || init_payload[0] != 0x22U ||
        (size_t)init_payload[1] + 2U > init_length || caps_capacity < 20U) {
        return STM_ERR_INVALID_ARG;
    }
    end = (size_t)init_payload[1] + 2U;
    for (i = 2U; i < end;) {
        uint8_t length;
        if (i + 2U > end) { return STM_ERR_VERIFY; }
        length = init_payload[i + 1U];
        if (i + 2U + length > end) { return STM_ERR_VERIFY; }
        if (init_payload[i] == 0x12U && length >= 1U) {
            chip_id = init_payload[i + 2U];
        } else if (init_payload[i] == 0x1AU && length >= 1U) {
            rpc_version = init_payload[i + 2U];
        }
        i += 2U + length;
    }
    if (chip_id == 0U || (rpc_version != 0U && rpc_version != 2U)) {
        return STM_ERR_INVALID_CONFIG;
    }
    memcpy(caps_payload, base_caps, sizeof(base_caps));
    caps_payload[7] = chip_id;
    *caps_length = sizeof(base_caps);
    if (rpc_version == 2U) {
        caps_payload[1] = 18U;
        caps_payload[17] = 0x1AU;
        caps_payload[18] = 1U;
        caps_payload[19] = 2U;
        *caps_length = 20U;
    }
    return STM_OK;
}

stm_err_t esp_hosted_encode_frame(esp_hosted_handle_t handle,
                                  uint8_t if_type,
                                  uint8_t packet_type,
                                  uint16_t sequence,
                                  const uint8_t *payload,
                                  size_t payload_length,
                                  uint8_t *frame,
                                  size_t frame_size)
{
    if (handle == NULL || frame == NULL || payload == NULL ||
        if_type >= 0x0FU || payload_length == 0U ||
        payload_length > ESP_HOSTED_FRAME_SIZE - ESP_HOSTED_FRAME_HEADER_SIZE ||
        frame_size < ESP_HOSTED_FRAME_SIZE ||
        ((uintptr_t)payload < (uintptr_t)frame + ESP_HOSTED_FRAME_SIZE &&
         (uintptr_t)frame < (uintptr_t)payload + payload_length)) {
        return STM_ERR_INVALID_ARG;
    }
    memset(frame, 0, ESP_HOSTED_FRAME_SIZE);
    frame[0] = if_type;
    frame[2] = (uint8_t)payload_length;
    frame[3] = (uint8_t)(payload_length >> 8U);
    frame[4] = ESP_HOSTED_FRAME_HEADER_SIZE;
    frame[8] = (uint8_t)sequence;
    frame[9] = (uint8_t)(sequence >> 8U);
    frame[11] = packet_type;
    memcpy(frame + ESP_HOSTED_FRAME_HEADER_SIZE, payload, payload_length);
    if (handle->config.checksum_enabled != 0U) {
        uint16_t checksum = esp_hosted_frame_checksum(
            frame, ESP_HOSTED_FRAME_HEADER_SIZE + payload_length);
        frame[ESP_HOSTED_FRAME_CHECKSUM_OFFSET] = (uint8_t)checksum;
        frame[ESP_HOSTED_FRAME_CHECKSUM_OFFSET + 1U] = (uint8_t)(checksum >> 8U);
    }
    return STM_OK;
}

uint16_t esp_hosted_frame_checksum(const uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size < ESP_HOSTED_FRAME_CHECKSUM_OFFSET + 2U) {
        return 0U;
    }
    return checksum_zeroed(frame, frame_size);
}

uint8_t esp_hosted_is_dummy_frame(esp_hosted_handle_t handle,
                                   const uint8_t *frame,
                                   size_t frame_size)
{
    if (handle == NULL || frame == NULL || frame_size < ESP_HOSTED_FRAME_HEADER_SIZE) {
        return 0U;
    }
    return (uint8_t)(((frame[0] & 0x0FU) == (dummy_if_type(handle) & 0x0FU)) &&
                      ((frame[0] >> 4U) == 0x0FU) &&
                      read_le16(&frame[2]) == 0U);
}

esp_hosted_frame_result_t esp_hosted_decode_frame(esp_hosted_handle_t handle,
                                                   const uint8_t *frame,
                                                   size_t frame_size,
                                                   esp_hosted_frame_t *decoded)
{
    uint16_t payload_length;
    uint16_t payload_offset;
    uint32_t total_length;
    uint16_t stored_checksum;

    if (handle == NULL || frame == NULL || decoded == NULL ||
        frame_size < ESP_HOSTED_FRAME_HEADER_SIZE) {
        return ESP_HOSTED_FRAME_INVALID;
    }
    memset(decoded, 0, sizeof(*decoded));
    payload_length = read_le16(&frame[2]);
    payload_offset = read_le16(&frame[4]);
    decoded->if_type = (uint8_t)(frame[0] & 0x0FU);
    decoded->if_num = (uint8_t)(frame[0] >> 4U);
    decoded->flags = frame[1];
    decoded->payload_length = payload_length;
    decoded->payload_offset = payload_offset;
    decoded->checksum = read_le16(&frame[ESP_HOSTED_FRAME_CHECKSUM_OFFSET]);
    decoded->sequence = read_le16(&frame[8]);
    decoded->throttle_command = (uint8_t)(frame[10] & 0x03U);
    decoded->packet_type = frame[11];

    if (esp_hosted_is_dummy_frame(handle, frame, frame_size)) {
        /* ESP-Hosted does not checksum payload-free dummy frames. */
        return ESP_HOSTED_FRAME_DUMMY;
    }
    if (payload_offset < ESP_HOSTED_FRAME_HEADER_SIZE ||
        payload_offset > ESP_HOSTED_FRAME_HEADER_SIZE + 3U) {
        return ESP_HOSTED_FRAME_INVALID;
    }
    total_length = (uint32_t)payload_offset + payload_length;
    if (total_length > handle->config.buffer_size || total_length > frame_size) {
        return total_length > ESP_HOSTED_FRAME_SIZE
                   ? ESP_HOSTED_FRAME_TOO_BIG
                   : ESP_HOSTED_FRAME_INVALID;
    }
    if (handle->config.checksum_enabled != 0U) {
        stored_checksum = decoded->checksum;
        if (stored_checksum != esp_hosted_frame_checksum(frame, (size_t)total_length)) {
            return ESP_HOSTED_FRAME_CORRUPT;
        }
    }
    decoded->payload = &frame[payload_offset];
    return ESP_HOSTED_FRAME_OK;
}

stm_err_t esp_hosted_get_info(esp_hosted_handle_t handle,
                              esp_hosted_info_t *info)
{
    if (handle == NULL || info == NULL) {
        return STM_ERR_INVALID_ARG;
    }
    *info = handle->info;
    return STM_OK;
}
